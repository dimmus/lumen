module;

// Bodies of `:capture_engine`. See capture_engine.cppm for the model.

#include <poll.h>
#include <unistd.h>

module lx.compositor;

import std;

import lx.foundation;
import lx.gfx;
import lx.trace;
import :capture;
import :capture_engine;

namespace {

constexpr lx::trace::category log_capture{"compositor.capture"};

} // namespace

namespace lx::compositor {

namespace {

void close_fd(int& fd) {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

/// The rects of `r` (full: the whole `size`) clipped to `size`.
lx::gfx::damage_region clipped(const lx::gfx::damage_region& r, lx::size2i size) {
    lx::gfx::damage_region out = r;
    out.clip({0, 0, size.width, size.height});
    return out;
}

lx::rect2i intersect(lx::rect2i a, lx::rect2i b) {
    const int x0 = a.x > b.x ? a.x : b.x;
    const int y0 = a.y > b.y ? a.y : b.y;
    const long long ax1 = static_cast<long long>(a.x) + a.width;
    const long long bx1 = static_cast<long long>(b.x) + b.width;
    const long long ay1 = static_cast<long long>(a.y) + a.height;
    const long long by1 = static_cast<long long>(b.y) + b.height;
    const long long x1 = ax1 < bx1 ? ax1 : bx1;
    const long long y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, static_cast<int>(x1 - x0), static_cast<int>(y1 - y0)};
}

/// The job's buffer fits the source as it is now: its size, or its crop's inside it.
bool fits_source(const capture_job& job, lx::size2i size) {
    if (job.crop.width > 0 && job.crop.height > 0)
        return job.crop.x >= 0 && job.crop.y >= 0 &&
               job.crop.x + job.crop.width <= size.width &&
               job.crop.y + job.crop.height <= size.height &&
               job.target.width == static_cast<unsigned>(job.crop.width) &&
               job.target.height == static_cast<unsigned>(job.crop.height);
    return job.target.width == static_cast<unsigned>(size.width) &&
           job.target.height == static_cast<unsigned>(size.height);
}

bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// The pointer as shown now differs from what the session's last frame painted.
bool cursor_moved(const capture_cursor_memo& memo, const capture_cursor& now) {
    const bool shown = now.visible && now.pixels;
    if (!memo.known)
        return shown;
    return shown != memo.painted ||
           (shown && (!same_rect(now.rect, memo.rect) || now.generation != memo.generation));
}

/// Fills BGRA rows with opaque black (the curtain).
void fill_black(unsigned char* pixels, unsigned stride, unsigned width, unsigned height) {
    for (unsigned y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(pixels + static_cast<std::size_t>(y) * stride);
        for (unsigned x = 0; x < width; ++x)
            row[x] = 0xFF000000u;
    }
}

} // namespace

// ── shm guards ───────────────────────────────────────────────────────────────

unsigned capture_shm_guards::add(void* shm_buffer, void* shm_pool, unsigned char* pixels,
                                 unsigned stride, unsigned width, unsigned height, bool opaque) {
    for (unsigned i = 0; i < k_max; ++i) {
        entry& e = entries_[i];
        if (e.used)
            continue;
        std::lock_guard lock{e.mutex};
        e.shm = shm_buffer;
        e.pool = shm_pool;
        e.pixels = pixels;
        e.stride = stride;
        e.width = width;
        e.height = height;
        e.opaque = opaque;
        e.cancelled = false;
        e.used = true;
        ++used_;
        return i + 1;
    }
    return 0;
}

void capture_shm_guards::cancel(unsigned handle) {
    if (handle == 0 || handle > k_max)
        return;
    entry& e = entries_[handle - 1];
    std::lock_guard lock{e.mutex};
    e.cancelled = true;
    e.shm = nullptr;
    e.pixels = nullptr;
}

void capture_shm_guards::release(unsigned handle) {
    if (handle == 0 || handle > k_max)
        return;
    entry& e = entries_[handle - 1];
    void* pool = nullptr;
    {
        std::lock_guard lock{e.mutex};
        if (!e.used)
            return;
        pool = e.pool;
        e.used = false;
        e.shm = e.pool = nullptr;
        e.pixels = nullptr;
        e.cancelled = false;
    }
    --used_;
    if (pool && hooks_.unref_pool)
        hooks_.unref_pool(pool);
}

// ── channel ──────────────────────────────────────────────────────────────────

bool capture_channel::post(const capture_job& job) {
    std::lock_guard lock{mutex_};
    if (job_count_ >= k_jobs)
        return false;
    jobs_[job_count_++] = job;
    return true;
}

unsigned capture_channel::take(capture_job* out, unsigned cap) {
    std::lock_guard lock{mutex_};
    const unsigned n = job_count_ < cap ? job_count_ : cap;
    for (unsigned i = 0; i < n; ++i)
        out[i] = jobs_[i];
    for (unsigned i = n; i < job_count_; ++i)
        jobs_[i - n] = jobs_[i];
    job_count_ -= n;
    return n;
}

bool capture_channel::complete(const capture_completion& c) {
    std::lock_guard lock{mutex_};
    if (completion_count_ >= k_completions)
        return false;
    completions_[completion_count_++] = c;
    completions_ready_.store(true, std::memory_order_release);
    return true;
}

unsigned capture_channel::drain(capture_completion* out, unsigned cap) {
    std::lock_guard lock{mutex_};
    const unsigned n = completion_count_ < cap ? completion_count_ : cap;
    for (unsigned i = 0; i < n; ++i)
        out[i] = completions_[i];
    for (unsigned i = n; i < completion_count_; ++i)
        completions_[i - n] = completions_[i];
    completion_count_ -= n;
    completions_ready_.store(completion_count_ > 0, std::memory_order_release);
    return n;
}

// ── renderer ─────────────────────────────────────────────────────────────────

bool capture_renderer::ensure_gl() {
    if (gl_capture_.ready())
        return true;
    if (gl_tried_ || !gl_ || !egl_)
        return false;
    gl_tried_ = true;
    if (auto ok = gl_capture_.initialize(*egl_); !ok) {
        log_capture.error(ok.get_error());
        return false;
    }
    return true;
}

void capture_renderer::reset(session_state& st, unsigned generation) {
    st.generation = generation;
    st.doomed = false;
    st.read_bo = nullptr;
    st.read_bo_w = st.read_bo_h = 0;
    st.read_fence = -1;
    st.read_done = {};
    st.read_out = nullptr;
    st.read_region.clear();
    st.read_src_w = st.read_src_h = 0;
    st.read_dx = st.read_dy = 0;
    st.read_cursor = false;
}

capture_renderer::session_state& capture_renderer::state(const capture_job& job) {
    session_state& s = sessions_[(job.session - 1) % k_capture_sessions];
    s.owner = this;
    // Never while a worker read of the slot is in flight (callers wait for it first).
    if (s.generation != job.generation && !s.busy.load(std::memory_order_acquire))
        reset(s, job.generation);
    return s;
}

void capture_renderer::reap() {
    for (unsigned i = 0; i < k_capture_sessions; ++i) {
        session_state& st = sessions_[i];
        if (st.doomed && !st.busy.load(std::memory_order_acquire)) {
            gl_capture_.forget_owner(i + 1);
            if (vk_)
                vk_->read_back_free(i);
            reset(st, st.generation);
        }
    }
}

void capture_renderer::worker_read(void* arg) {
    auto* st = static_cast<session_state*>(arg);
    capture_renderer* self = st->owner;
    if (st->read_fence >= 0) {
        // The blit's completion: waited for here, never on render.
        pollfd p{st->read_fence, POLLIN, 0};
        (void)::poll(&p, 1, 2000);
        close_fd(st->read_fence);
    }
    capture_completion c = st->read_done;
    bool ok = false;
    const bool written = self->guards_ && self->guards_->write(
        c.shm, [&](unsigned char* pixels, unsigned stride, unsigned w, unsigned h, bool opaque) {
            ok = lx::gfx::gl_capture::read_bo(st->read_bo, st->read_bo_w, st->read_bo_h,
                                              st->read_region, pixels, stride, 0, 0, w, h,
                                              opaque);
            if (ok && st->read_cursor)
                capture_blend_argb(pixels, w, h, stride, st->cursor_copy.data(),
                                   st->read_cursor_w, st->read_cursor_h, st->read_cursor_w,
                                   st->read_cursor_rect.x, st->read_cursor_rect.y);
        });
    if (!written || !ok) {
        c.result = capture_result::failed;
        c.reason = capture_failure::unknown;
    }
    // The queue holds a completion per job in flight; a full one (it cannot be) waits.
    for (int i = 0; i < 2000 && !st->read_out->complete(c); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    st->busy.store(false, std::memory_order_release);
    if (self->notify_)
        self->notify_(self->notify_user_);
}

void capture_renderer::worker_read_vk(void* arg) {
    auto* st = static_cast<session_state*>(arg);
    capture_renderer* self = st->owner;
    // The copy's completion: waited for here, never on render.
    const unsigned char* rows = self->vk_ ? self->vk_->read_back_wait(st->vk_slot, 2'000'000'000ull)
                                          : nullptr;
    capture_completion c = st->read_done;
    const bool written = rows && self->guards_ && self->guards_->write(
        c.shm, [&](unsigned char* pixels, unsigned stride, unsigned tw, unsigned th, bool) {
            const lx::gfx::damage_region& r = st->read_region;
            for (unsigned i = 0; i < r.count; ++i) {
                const lx::rect2i rc = intersect(
                    r.rects[i], {st->read_dx, st->read_dy, static_cast<int>(tw),
                                 static_cast<int>(th)});
                for (int y = rc.y; y < rc.y + rc.height; ++y) {
                    const unsigned char* sp =
                        rows + (static_cast<std::size_t>(y) * st->read_src_w +
                                static_cast<unsigned>(rc.x)) * 4u;
                    unsigned char* d = pixels + static_cast<std::size_t>(y - st->read_dy) * stride +
                                       static_cast<std::size_t>(rc.x - st->read_dx) * 4u;
                    // B, G, R as they are; the padding byte opaque (an output frame is).
                    for (int x = 0; x < rc.width; ++x) {
                        std::uint32_t px;
                        std::memcpy(&px, sp + static_cast<std::size_t>(x) * 4u, 4);
                        px |= 0xFF000000u;
                        std::memcpy(d + static_cast<std::size_t>(x) * 4u, &px, 4);
                    }
                }
            }
            if (st->read_cursor)
                capture_blend_argb(pixels, tw, th, stride, st->cursor_copy.data(),
                                   st->read_cursor_w, st->read_cursor_h, st->read_cursor_w,
                                   st->read_cursor_rect.x, st->read_cursor_rect.y);
        });
    if (!written) {
        c.result = capture_result::failed;
        c.reason = capture_failure::unknown;
    }
    for (int i = 0; i < 2000 && !st->read_out->complete(c); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    st->busy.store(false, std::memory_order_release);
    if (self->notify_)
        self->notify_(self->notify_user_);
}

void capture_renderer::read_on_worker(session_state& st, const capture_job& job,
                                      capture_completion c, unsigned w, unsigned h,
                                      const lx::gfx::damage_region& rects,
                                      const capture_cursor* cursor, lx::rect2i cursor_at,
                                      capture_channel& out) {
    st.read_bo = gl_capture_.offscreen_bo(job.session);
    st.read_bo_w = w;
    st.read_bo_h = h;
    st.read_region = rects;
    st.read_cursor = cursor != nullptr;
    if (cursor) {
        const std::size_t px = static_cast<std::size_t>(cursor->width) * cursor->height;
        if (st.cursor_copy.size() < px)
            st.cursor_copy.resize(px);
        for (unsigned y = 0; y < cursor->height; ++y)
            std::memcpy(&st.cursor_copy[static_cast<std::size_t>(y) * cursor->width],
                        cursor->pixels + static_cast<std::size_t>(y) * cursor->stride_px,
                        cursor->width * 4u);
        st.read_cursor_rect = cursor_at;
        st.read_cursor_w = cursor->width;
        st.read_cursor_h = cursor->height;
    }
    c.result = capture_result::ready;
    c.fence_fd = -1;
    st.read_done = c;
    st.read_out = &out;
    st.read_fence = gl_capture_.fence();
    st.busy.store(true, std::memory_order_release);
    if (!post_ || !post_(&capture_renderer::worker_read, &st))
        worker_read(&st); // no worker: here (the map waits for the blit)
}

void capture_renderer::complete(capture_channel& out, const capture_completion& c) {
    if (overflow_count_ == 0 && out.complete(c))
        return;
    if (overflow_count_ < capture_channel::k_completions) {
        overflow_[overflow_count_++] = c;
        return;
    }
    // Cannot happen with one frame per session; never leak the fence.
    capture_completion lost = c;
    close_fd(lost.fence_fd);
    log_capture.warn("completion queue full: a capture frame is lost");
}

void capture_renderer::flush(capture_channel& out) {
    unsigned kept = 0;
    for (unsigned i = 0; i < overflow_count_; ++i)
        if (!out.complete(overflow_[i]))
            overflow_[kept++] = overflow_[i];
    overflow_count_ = kept;
}

int capture_renderer::take_sample_fence(bool wanted) {
    if (!sampled_textures_)
        return -1;
    sampled_textures_ = false;
    return wanted && gl_ ? gl_capture_.fence() : -1;
}

void capture_renderer::close_job(capture_job& job) {
    close_fd(job.target.fd);
}

void capture_renderer::fail(capture_channel& out, const capture_job& job, capture_failure reason) {
    capture_completion c{};
    c.session = job.session;
    c.generation = job.generation;
    c.serial = job.serial;
    c.shm = job.target.shm;
    c.lock_epoch = job.lock_epoch;
    c.result = capture_result::failed;
    c.reason = reason;
    complete(out, c);
}

void capture_renderer::drop_pending(unsigned index, capture_channel& out) {
    const capture_job& job = pending_[index];
    capture_completion c{};
    c.session = job.session;
    c.generation = job.generation;
    c.serial = job.serial;
    c.shm = job.target.shm;
    c.lock_epoch = job.lock_epoch;
    c.result = capture_result::cancelled;
    complete(out, c);
    remove_pending(index);
}

void capture_renderer::remove_pending(unsigned index) {
    close_job(pending_[index]);
    for (unsigned i = index + 1; i < pending_count_; ++i)
        pending_[i - 1] = pending_[i];
    --pending_count_;
}

void capture_renderer::take(capture_channel& in) {
    reap();
    capture_job jobs[capture_channel::k_jobs];
    const unsigned n = in.take(jobs, capture_channel::k_jobs);
    for (unsigned i = 0; i < n; ++i) {
        capture_job& job = jobs[i];
        switch (job.kind) {
        case capture_job_kind::forget_buffer:
            gl_capture_.forget_dmabuf(job.target.buffer_key);
            break;
        case capture_job_kind::forget_session: {
            session_state& s = sessions_[(job.session - 1) % k_capture_sessions];
            if (s.busy.load(std::memory_order_acquire)) {
                s.doomed = true; // freed by `reap` once the worker let go
            } else {
                gl_capture_.forget_owner(job.session);
                if (vk_)
                    vk_->read_back_free((job.session - 1) % k_capture_sessions);
                reset(s, s.generation);
            }
            for (unsigned k = pending_count_; k-- > 0;)
                if (pending_[k].session == job.session)
                    drop_pending(k, in);
            break;
        }
        case capture_job_kind::finish:
            run_finish(job, in);
            break;
        default:
            // One frame per session: a newer job of the slot replaces whatever is left of an
            // older one (its frame is gone on the UI side).
            for (unsigned k = pending_count_; k-- > 0;)
                if (pending_[k].session == job.session)
                    drop_pending(k, in);
            if (pending_count_ < capture_channel::k_jobs) {
                pending_[pending_count_++] = job;
            } else {
                close_job(job);
                fail(in, job, capture_failure::unknown);
            }
            break;
        }
    }
}

bool capture_renderer::paint_cursor_gl(unsigned framebuffer, lx::size2i size,
                                       const capture_cursor& cursor) {
    if (!gl_ || !cursor.visible || cursor.in_frame || !cursor.pixels || cursor.width == 0)
        return false;
    if (cursor.generation != cursor_uploaded_gen_ || cursor_uploaded_gen_ == 0) {
        lx::result<void> up{};
        if (gl_->supports_client_upload()) {
            up = gl_->upload_client_region(k_capture_cursor_texture, cursor.width, cursor.height,
                                           {}, reinterpret_cast<const unsigned char*>(cursor.pixels),
                                           cursor.stride_px * 4u, false);
        } else {
            std::vector<unsigned char> rgba(static_cast<std::size_t>(cursor.width) * cursor.height * 4u);
            for (unsigned y = 0; y < cursor.height; ++y)
                for (unsigned x = 0; x < cursor.width; ++x) {
                    const std::uint32_t p = cursor.pixels[static_cast<std::size_t>(y) * cursor.stride_px + x];
                    unsigned char* d = &rgba[(static_cast<std::size_t>(y) * cursor.width + x) * 4u];
                    d[0] = static_cast<unsigned char>(p >> 16);
                    d[1] = static_cast<unsigned char>(p >> 8);
                    d[2] = static_cast<unsigned char>(p);
                    d[3] = static_cast<unsigned char>(p >> 24);
                }
            up = gl_->upload_rgba(k_capture_cursor_texture, cursor.width, cursor.height, rgba.data());
        }
        if (!up) {
            log_capture.error(up.get_error());
            return false;
        }
        cursor_uploaded_gen_ = cursor.generation;
    }
    lx::gfx::blit_command cmd{};
    cmd.texture_id = k_capture_cursor_texture;
    cmd.dst = cursor.rect;
    cmd.blend = lx::blend_mode::premultiplied;
    lx::gfx::damage_region where{};
    where.add(cursor.rect);
    where.clip({0, 0, size.width, size.height});
    if (where.empty())
        return false;
    auto drawn = gl_->composite_into(framebuffer, static_cast<unsigned>(size.width),
                                     static_cast<unsigned>(size.height),
                                     lx::color::rgb(0.f, 0.f, 0.f), &cmd, 1, where, false, false);
    return static_cast<bool>(drawn);
}

bool capture_renderer::recomposite_cpu(const capture_frame_source& src,
                                       const lx::gfx::pixel_surface& dst, lx::color clear,
                                       const lx::gfx::blit_command* blits, lx::point2i offset,
                                       const lx::gfx::damage_region& region) {
    if (src.paint.cpu)
        return src.paint.cpu(src.paint.user, dst, clear, blits, src.blit_count, offset, region);
    return static_cast<bool>(cpu_->composite_region(dst, clear, blits, src.blit_count, region));
}

bool capture_renderer::recomposite_gl(const capture_frame_source& src, unsigned framebuffer,
                                      unsigned width, unsigned height, lx::color clear,
                                      const lx::gfx::blit_command* blits, lx::point2i offset,
                                      const lx::gfx::damage_region& region) {
    if (src.paint.gl)
        return src.paint.gl(src.paint.user, framebuffer, width, height, clear, blits,
                            src.blit_count, offset, region);
    return static_cast<bool>(
        gl_->composite_into(framebuffer, width, height, clear, blits, src.blit_count, region, true,
                            false));
}

bool capture_renderer::run_output(capture_job& job, const capture_frame_source& src,
                                  bool composited_now, capture_channel& out) {
    (void)composited_now;
    session_state& st = state(job);
    if (st.busy.load(std::memory_order_acquire))
        return false; // its previous read is still on the worker
    capture_damage_ring& ring = rings_[job.output_slot % 16];
    const lx::size2i size = src.size;
    // The part of the source copied, to the buffer's origin (wlr-screencopy regions); the whole
    // source otherwise.
    const lx::rect2i crop = job.crop.width > 0 && job.crop.height > 0
                                ? job.crop
                                : lx::rect2i{0, 0, size.width, size.height};
    const lx::size2i out_size{crop.width, crop.height};
    const bool hw_cursor = job.paint_cursors && src.cursor.visible && !src.cursor.in_frame &&
                           src.cursor.pixels;
    lx::rect2i cursor_at = src.cursor.rect; // in the buffer
    cursor_at.x -= crop.x;
    cursor_at.y -= crop.y;

    // What changed for this session (source pixels): the composites since its last copy
    // (everything when the ring no longer reaches back), and the pointer's old and new place
    // when it paints it.
    lx::gfx::damage_region changed{};
    if (job.full || !ring.since(job.since_seq, changed))
        changed.set_full();
    if (job.paint_cursors && !changed.full) {
        if (job.cursor.painted)
            changed.add(job.cursor.rect);
        if (hw_cursor)
            changed.add(src.cursor.rect);
    }
    if (changed.full) {
        changed.clear();
        changed.add(crop);
    } else {
        changed.clip(crop);
        // A crop (a screencopy region) waits for a change inside it; what changed outside it
        // until now is of no interest to it.
        if (changed.empty()) {
            job.since_seq = ring.seq();
            return false;
        }
    }
    // The copy: that plus the client's damage (buffer pixels, so moved by the crop).
    lx::gfx::damage_region region = changed;
    {
        lx::gfx::damage_region client = job.client_damage;
        if (client.full) {
            region.clear();
            region.add(crop);
        } else {
            for (unsigned k = 0; k < client.count; ++k)
                region.add({client.rects[k].x + crop.x, client.rects[k].y + crop.y,
                            client.rects[k].width, client.rects[k].height});
            region.clip(crop);
        }
    }

    capture_completion c{};
    c.session = job.session;
    c.generation = job.generation;
    c.serial = job.serial;
    c.shm = job.target.shm;
    c.lock_epoch = job.lock_epoch;
    // Reported in buffer pixels.
    for (unsigned k = 0; k < changed.count; ++k)
        c.damage.add({changed.rects[k].x - crop.x, changed.rects[k].y - crop.y,
                      changed.rects[k].width, changed.rects[k].height});
    c.seq = ring.seq();
    c.frame = src.frame;
    c.time_ns = src.time_ns;
    c.result = capture_result::ready;

    // The software pointer is in the frame: a session without paint_cursors gets the frame
    // composited again without it (its draws are at hand on every path that has them).
    const bool hide_pointer = src.cursor.in_frame && !job.paint_cursors && src.blits != nullptr;
    static thread_local std::vector<lx::gfx::blit_command> scratch;
    // The frame's draws as the buffer sees them: the pointer left out when it must be, moved
    // by the crop.
    const auto draws = [&]() -> const lx::gfx::blit_command* {
        if (!hide_pointer && crop.x == 0 && crop.y == 0)
            return src.blits;
        scratch.assign(src.blits, src.blits + src.blit_count);
        for (auto& b : scratch) {
            if (hide_pointer && b.texture_id == k_capture_hidden_texture)
                b.occluded = true;
            b.dst.x -= crop.x;
            b.dst.y -= crop.y;
            if (b.clip.width > 0 && b.clip.height > 0) {
                b.clip.x -= crop.x;
                b.clip.y -= crop.y;
            }
            for (unsigned k = 0; k < b.visible_count; ++k) {
                b.visible[k].x -= crop.x;
                b.visible[k].y -= crop.y;
            }
            b.blur_box.x -= crop.x;
            b.blur_box.y -= crop.y;
        }
        return scratch.data();
    };
    // `region` in the buffer's pixels.
    lx::gfx::damage_region out_region{};
    for (unsigned k = 0; k < region.count; ++k)
        out_region.add({region.rects[k].x - crop.x, region.rects[k].y - crop.y,
                        region.rects[k].width, region.rects[k].height});

    const unsigned w = static_cast<unsigned>(out_size.width);
    const unsigned h = static_cast<unsigned>(out_size.height);
    if (gl_ && src.gl_framebuffer != 0) {
        if (!ensure_gl()) {
            fail(out, job, capture_failure::unknown);
            return true;
        }
        const unsigned sw = static_cast<unsigned>(size.width);
        const unsigned sh = static_cast<unsigned>(size.height);
        if (job.target.dmabuf) {
            auto dst = gl_capture_.dmabuf_framebuffer(job.target.buffer_key, job.target.fd,
                                                      job.target.offset, job.target.stride, w, h,
                                                      job.target.format, job.target.modifier);
            if (!dst) {
                log_capture.error(dst.get_error());
                fail(out, job, capture_failure::buffer_constraints);
                return true;
            }
            bool ok = true;
            if (hide_pointer) {
                sampled_textures_ = true;
                ok = recomposite_gl(src, dst.value(), w, h, src.clear, draws(), {crop.x, crop.y},
                                    out_region);
            } else if (!region.empty()) {
                ok = static_cast<bool>(gl_capture_.blit(src.gl_framebuffer, dst.value(), sw, sh,
                                                        region, crop.x, crop.y));
            }
            if (ok && hw_cursor) {
                capture_cursor moved = src.cursor;
                moved.rect = cursor_at;
                (void)paint_cursor_gl(dst.value(), out_size, moved);
            }
            if (!ok) {
                fail(out, job, capture_failure::unknown);
                return true;
            }
            c.fence_fd = gl_capture_.fence();
            c.result = c.fence_fd >= 0 ? capture_result::fence : capture_result::ready;
        } else {
            if (post_) {
                // The frame's rects into this session's mappable buffer — one blit (or the
                // re-composite without the pointer), fenced — and the read off render.
                auto off = gl_capture_.offscreen_framebuffer(job.session, w, h);
                void* bo = off ? gl_capture_.offscreen_bo(job.session) : nullptr;
                if (bo) {
                    sampled_textures_ = sampled_textures_ || hide_pointer;
                    const bool drew =
                        hide_pointer
                            ? recomposite_gl(src, off.value(), w, h, src.clear, draws(),
                                             {crop.x, crop.y}, out_region)
                            : region.empty() || static_cast<bool>(gl_capture_.blit(
                                                    src.gl_framebuffer, off.value(), sw, sh,
                                                    region, crop.x, crop.y));
                    if (!drew) {
                        fail(out, job, capture_failure::unknown);
                        return true;
                    }
                    c.cursor = {true, hw_cursor, hw_cursor ? src.cursor.rect : lx::rect2i{},
                                src.cursor.generation};
                    read_on_worker(st, job, c, w, h, out_region, hw_cursor ? &src.cursor : nullptr,
                                   cursor_at, out);
                    return true;
                }
            }
            unsigned from = src.gl_framebuffer;
            unsigned from_w = sw;
            unsigned from_h = sh;
            lx::gfx::damage_region read = region;
            int dx = crop.x;
            int dy = crop.y;
            if (hide_pointer) {
                // Composited at the buffer's size: read with no crop.
                auto off = gl_capture_.offscreen_framebuffer(job.session, w, h);
                sampled_textures_ = true;
                if (!off || !recomposite_gl(src, off.value(), w, h, src.clear, draws(),
                                            {crop.x, crop.y}, out_region)) {
                    fail(out, job, capture_failure::unknown);
                    return true;
                }
                from = off.value();
                from_w = w;
                from_h = h;
                read = out_region;
                dx = dy = 0;
            }
            if (auto r = gl_capture_.read_async(job.session, from, from_w, from_h, read); !r) {
                log_capture.error(r.get_error());
                fail(out, job, capture_failure::unknown);
                return true;
            }
            st.read_region = read;
            st.read_src_w = from_w;
            st.read_src_h = from_h;
            st.read_dx = dx;
            st.read_dy = dy;
            st.read_cursor = hw_cursor;
            if (hw_cursor) {
                // The pointer as it is now, blended once the read lands.
                const std::size_t px = static_cast<std::size_t>(src.cursor.width) * src.cursor.height;
                if (st.cursor_copy.size() < px)
                    st.cursor_copy.resize(px);
                for (unsigned y = 0; y < src.cursor.height; ++y)
                    std::memcpy(&st.cursor_copy[static_cast<std::size_t>(y) * src.cursor.width],
                                src.cursor.pixels + static_cast<std::size_t>(y) * src.cursor.stride_px,
                                src.cursor.width * 4u);
                st.read_cursor_rect = cursor_at;
                st.read_cursor_w = src.cursor.width;
                st.read_cursor_h = src.cursor.height;
            }
            c.cursor = {true, hw_cursor, hw_cursor ? src.cursor.rect : lx::rect2i{},
                        src.cursor.generation};
            c.fence_fd = gl_capture_.fence();
            c.result = capture_result::readback;
            complete(out, c);
            if (c.fence_fd < 0) {
                // No fence to wait on: the copy-out runs now (the map waits for the read).
                capture_job fin = job;
                fin.kind = capture_job_kind::finish;
                run_finish(fin, out);
            }
            return true;
        }
    } else if (cpu_) {
        const bool written = guards_ && guards_->write(
            job.target.shm, [&](unsigned char* pixels, unsigned stride, unsigned tw, unsigned th,
                                bool opaque) {
                lx::gfx::pixel_surface dst{};
                dst.pixels = pixels;
                dst.width = tw;
                dst.height = th;
                dst.stride = stride;
                dst.format = static_cast<lx::fourcc>(opaque ? lx::pixel_format::xrgb8888
                                                            : lx::pixel_format::argb8888);
                if (src.blits && !out_region.empty())
                    (void)recomposite_cpu(src, dst, src.clear, draws(), {crop.x, crop.y},
                                          out_region);
                if (hw_cursor)
                    capture_blend_argb(pixels, tw, th, stride, src.cursor.pixels, src.cursor.width,
                                       src.cursor.height, src.cursor.stride_px, cursor_at.x,
                                       cursor_at.y);
            });
        if (!written) {
            fail(out, job, capture_failure::unknown);
            return true;
        }
    } else if (vk_ && src.vk_target) {
        // One copy of the changed rects into the session's persistent mapped buffer, submitted
        // without waiting; the worker waits for it and writes the client's rows (BGRA is
        // already wl_shm's byte order) — render neither allocates nor stalls per frame.
        const unsigned slot = static_cast<unsigned>(&st - sessions_);
        if (auto read = vk_->read_back_async(slot, *src.vk_target, region.rects, region.count);
            !read) {
            log_capture.error(read.get_error());
            fail(out, job, capture_failure::unknown);
            return true;
        }
        st.read_region = region;
        st.read_src_w = static_cast<unsigned>(size.width);
        st.read_src_h = static_cast<unsigned>(size.height);
        st.read_dx = crop.x;
        st.read_dy = crop.y;
        st.read_cursor = hw_cursor;
        if (hw_cursor) {
            const std::size_t px = static_cast<std::size_t>(src.cursor.width) * src.cursor.height;
            if (st.cursor_copy.size() < px)
                st.cursor_copy.resize(px);
            for (unsigned y = 0; y < src.cursor.height; ++y)
                std::memcpy(&st.cursor_copy[static_cast<std::size_t>(y) * src.cursor.width],
                            src.cursor.pixels + static_cast<std::size_t>(y) * src.cursor.stride_px,
                            src.cursor.width * 4u);
            st.read_cursor_rect = cursor_at;
            st.read_cursor_w = src.cursor.width;
            st.read_cursor_h = src.cursor.height;
        }
        c.cursor = {true, hw_cursor, hw_cursor ? src.cursor.rect : lx::rect2i{},
                    src.cursor.generation};
        st.vk_slot = slot;
        st.read_done = c;
        st.read_out = &out;
        st.busy.store(true, std::memory_order_release);
        if (!post_ || !post_(&capture_renderer::worker_read_vk, &st))
            worker_read_vk(&st); // no worker: here (the wait blocks render)
        return true;
    } else {
        fail(out, job, capture_failure::unknown);
        return true;
    }
    c.cursor = {true, hw_cursor, hw_cursor ? src.cursor.rect : lx::rect2i{},
                src.cursor.generation};
    complete(out, c);
    return true;
}

void capture_renderer::run_finish(const capture_job& job, capture_channel& out) {
    session_state& st = state(job);
    capture_completion c{};
    c.session = job.session;
    c.generation = job.generation;
    c.serial = job.serial;
    c.shm = job.target.shm;
    c.lock_epoch = job.lock_epoch;
    c.result = capture_result::ready;
    c.finished_read = true;
    bool ok = false;
    const bool written = guards_ && guards_->write(
        job.target.shm, [&](unsigned char* pixels, unsigned stride, unsigned w, unsigned h,
                            bool opaque) {
            ok = static_cast<bool>(gl_capture_.finish_read(job.session, pixels, stride,
                                                           st.read_src_w, st.read_src_h,
                                                           st.read_region, st.read_dx,
                                                           st.read_dy, w, h));
            if (ok && st.read_cursor)
                capture_blend_argb(pixels, w, h, stride, st.cursor_copy.data(), st.read_cursor_w,
                                   st.read_cursor_h, st.read_cursor_w, st.read_cursor_rect.x,
                                   st.read_cursor_rect.y);
            if (ok && opaque) {
                // XRGB: the padding byte is the client's to ignore, but a reader that takes it
                // as alpha must see an opaque frame (the offscreen target of a window has
                // alpha).
                const lx::gfx::damage_region& r = st.read_region;
                for (unsigned i = 0; i < r.count; ++i) {
                    const lx::rect2i rc = intersect(
                        {r.rects[i].x - st.read_dx, r.rects[i].y - st.read_dy, r.rects[i].width,
                         r.rects[i].height},
                        {0, 0, static_cast<int>(w), static_cast<int>(h)});
                    for (int y = rc.y; y < rc.y + rc.height; ++y) {
                        unsigned char* d = pixels + static_cast<std::size_t>(y) * stride +
                                           static_cast<std::size_t>(rc.x) * 4u + 3u;
                        for (int x = 0; x < rc.width; ++x, d += 4)
                            *d = 0xFF;
                    }
                }
            }
        });
    if (!written || !ok) {
        c.result = capture_result::failed;
        c.reason = capture_failure::unknown;
    }
    complete(out, c);
}

void capture_renderer::output_composited(unsigned slot, const lx::gfx::damage_region& damage,
                                         const capture_frame_source& src, capture_channel& out) {
    if (slot >= 16)
        return;
    (void)rings_[slot].record(damage);
    const unsigned epoch = out.lock_epoch.load(std::memory_order_acquire);
    for (unsigned i = 0; i < pending_count_;) {
        capture_job& job = pending_[i];
        if (job.kind != capture_job_kind::output || job.output_slot != slot) {
            ++i;
            continue;
        }
        if (job.lock_epoch != epoch) {
            drop_pending(i, out);
            continue;
        }
        if (job.has_min_frame && static_cast<int>(src.frame - job.min_frame) < 0) {
            ++i;
            continue;
        }
        if (!fits_source(job, src.size)) {
            fail(out, job, capture_failure::buffer_constraints);
            remove_pending(i);
            continue;
        }
        if (run_output(job, src, true, out))
            remove_pending(i);
        else
            ++i;
    }
}

bool capture_renderer::wants_front(unsigned slot, const capture_cursor& cursor) const {
    if (slot >= 16)
        return false;
    for (unsigned i = 0; i < pending_count_; ++i) {
        const capture_job& job = pending_[i];
        if (job.kind != capture_job_kind::output || job.output_slot != slot)
            continue;
        if (job.full || rings_[slot].seq() != job.since_seq)
            return true;
        if (job.paint_cursors && !cursor.in_frame && cursor_moved(job.cursor, cursor))
            return true;
    }
    return false;
}

void capture_renderer::output_front(unsigned slot, const capture_frame_source& src,
                                    capture_channel& out) {
    if (slot >= 16)
        return;
    const unsigned epoch = out.lock_epoch.load(std::memory_order_acquire);
    for (unsigned i = 0; i < pending_count_;) {
        capture_job& job = pending_[i];
        if (job.kind != capture_job_kind::output || job.output_slot != slot) {
            ++i;
            continue;
        }
        if (job.lock_epoch != epoch) {
            drop_pending(i, out);
            continue;
        }
        bool due = job.full || rings_[slot].seq() != job.since_seq;
        if (!due && job.paint_cursors && !src.cursor.in_frame)
            due = cursor_moved(job.cursor, src.cursor);
        if (!due || (job.has_min_frame && static_cast<int>(src.frame - job.min_frame) < 0)) {
            ++i;
            continue;
        }
        if (!fits_source(job, src.size)) {
            fail(out, job, capture_failure::buffer_constraints);
            remove_pending(i);
            continue;
        }
        if (run_output(job, src, false, out))
            remove_pending(i);
        else
            ++i;
    }
}

void capture_renderer::output_reset(unsigned slot) {
    if (slot < 16)
        rings_[slot].reset();
}

void capture_renderer::run_curtains(capture_channel& out) {
    for (unsigned i = 0; i < pending_count_;) {
        capture_job& job = pending_[i];
        if (job.kind != capture_job_kind::curtain) {
            ++i;
            continue;
        }
        capture_completion c{};
        c.session = job.session;
        c.generation = job.generation;
        c.serial = job.serial;
        c.shm = job.target.shm;
        c.lock_epoch = job.lock_epoch;
        c.result = capture_result::ready;
        c.damage.set_full();
        bool ok = false;
        if (job.target.dmabuf) {
            if (ensure_gl()) {
                auto dst = gl_capture_.dmabuf_framebuffer(
                    job.target.buffer_key, job.target.fd, job.target.offset, job.target.stride,
                    job.target.width, job.target.height, job.target.format, job.target.modifier);
                if (dst && gl_capture_.clear(dst.value(), job.target.width, job.target.height,
                                             lx::color::rgb(0.f, 0.f, 0.f, 1.f))) {
                    ok = true;
                    c.fence_fd = gl_capture_.fence();
                    c.result = c.fence_fd >= 0 ? capture_result::fence : capture_result::ready;
                }
            }
        } else {
            ok = guards_ && guards_->write(job.target.shm, [](unsigned char* pixels, unsigned stride,
                                                              unsigned w, unsigned h, bool) {
                fill_black(pixels, stride, w, h);
            });
        }
        if (!ok) {
            c.result = capture_result::failed;
            c.reason = capture_failure::unknown;
        }
        // The pointer is not on a curtain: the next real frame repaints where it was.
        c.cursor = {true, false, {}, 0};
        complete(out, c);
        remove_pending(i);
    }
}

unsigned capture_renderer::toplevel_jobs(capture_toplevel_job* out, unsigned cap) const {
    unsigned n = 0;
    for (unsigned i = 0; i < pending_count_ && n < cap; ++i) {
        const capture_job& j = pending_[i];
        if (j.kind != capture_job_kind::toplevel)
            continue;
        out[n].view = j.view;
        out[n].frame = j.view_frame;
        out[n].paint_cursors = j.paint_cursors;
        out[n].origin = j.view_origin;
        out[n].scale = j.view_scale;
        out[n].size = {static_cast<int>(j.target.width), static_cast<int>(j.target.height)};
        ++n;
    }
    return n;
}

void capture_renderer::toplevel_composited(unsigned view, const lx::gfx::damage_region& damage,
                                           const capture_frame_source& src, capture_channel& out) {
    const unsigned epoch = out.lock_epoch.load(std::memory_order_acquire);
    // Every job of this view up to its frame runs (or is dropped) now.
    if (static_cast<long long>(src.frame) > view_frame_done_)
        view_frame_done_ = src.frame;
    for (unsigned i = 0; i < pending_count_;) {
        capture_job& job = pending_[i];
        if (job.kind != capture_job_kind::toplevel || job.view != view) {
            ++i;
            continue;
        }
        if (job.lock_epoch != epoch) {
            drop_pending(i, out);
            continue;
        }
        if (static_cast<int>(src.frame - job.view_frame) < 0) {
            ++i; // the view's publish of this job has not been seen yet
            continue;
        }
        // The view was configured at the buffer's size when the job was posted (UI checked
        // the buffer against the constraints the view was built for).
        const lx::size2i size{static_cast<int>(job.target.width),
                              static_cast<int>(job.target.height)};
        session_state& st = state(job);
        if (st.busy.load(std::memory_order_acquire)) {
            ++i; // its previous read is still on the worker
            continue;
        }
        const bool hw_cursor = job.paint_cursors && src.cursor.visible && src.cursor.pixels;
        lx::gfx::damage_region changed{};
        if (job.full)
            changed.set_full();
        else
            changed = damage;
        if (job.paint_cursors && !changed.full) {
            if (job.cursor.painted)
                changed.add(job.cursor.rect);
            if (hw_cursor)
                changed.add(src.cursor.rect);
        }
        changed = clipped(changed, size);
        lx::gfx::damage_region region = changed;
        region.add_region(clipped(job.client_damage, size));

        capture_completion c{};
        c.session = job.session;
        c.generation = job.generation;
        c.serial = job.serial;
        c.shm = job.target.shm;
        c.lock_epoch = job.lock_epoch;
        c.damage = changed;
        c.frame = src.frame;
        c.time_ns = src.time_ns;
        c.result = capture_result::ready;
        // A window has no backdrop: what it does not cover is transparent.
        const lx::color clear = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
        bool ok = true;
        const unsigned w = static_cast<unsigned>(size.width);
        const unsigned h = static_cast<unsigned>(size.height);
        if (gl_) {
            sampled_textures_ = sampled_textures_ || !region.empty();
            if (!ensure_gl()) {
                ok = false;
            } else if (job.target.dmabuf) {
                auto dst = gl_capture_.dmabuf_framebuffer(job.target.buffer_key, job.target.fd,
                                                          job.target.offset, job.target.stride, w,
                                                          h, job.target.format, job.target.modifier);
                ok = dst && (region.empty() ||
                             recomposite_gl(src, dst.value(), w, h, clear, src.blits, {}, region));
                if (ok && hw_cursor)
                    (void)paint_cursor_gl(dst.value(), size, src.cursor);
                if (ok) {
                    c.fence_fd = gl_capture_.fence();
                    c.result = c.fence_fd >= 0 ? capture_result::fence : capture_result::ready;
                }
            } else {
                auto off = gl_capture_.offscreen_framebuffer(job.session, w, h);
                ok = off && (region.empty() ||
                             recomposite_gl(src, off.value(), w, h, clear, src.blits, {}, region));
                if (ok && post_ && gl_capture_.offscreen_bo(job.session)) {
                    // Read off render (see run_output).
                    c.cursor = {true, hw_cursor, hw_cursor ? src.cursor.rect : lx::rect2i{},
                                src.cursor.generation};
                    read_on_worker(st, job, c, w, h, region, hw_cursor ? &src.cursor : nullptr,
                                   src.cursor.rect, out);
                    remove_pending(i);
                    continue;
                }
                if (ok)
                    ok = static_cast<bool>(
                        gl_capture_.read_async(job.session, off.value(), w, h, region));
                if (ok) {
                    st.read_region = region;
                    st.read_src_w = w;
                    st.read_src_h = h;
                    st.read_dx = 0;
                    st.read_dy = 0;
                    st.read_cursor = hw_cursor;
                    if (hw_cursor) {
                        const std::size_t px =
                            static_cast<std::size_t>(src.cursor.width) * src.cursor.height;
                        if (st.cursor_copy.size() < px)
                            st.cursor_copy.resize(px);
                        for (unsigned y = 0; y < src.cursor.height; ++y)
                            std::memcpy(&st.cursor_copy[static_cast<std::size_t>(y) * src.cursor.width],
                                        src.cursor.pixels +
                                            static_cast<std::size_t>(y) * src.cursor.stride_px,
                                        src.cursor.width * 4u);
                        st.read_cursor_rect = src.cursor.rect;
                        st.read_cursor_w = src.cursor.width;
                        st.read_cursor_h = src.cursor.height;
                    }
                    c.fence_fd = gl_capture_.fence();
                    c.result = capture_result::readback;
                }
            }
        } else if (cpu_) {
            ok = guards_ && guards_->write(
                job.target.shm, [&](unsigned char* pixels, unsigned stride, unsigned tw,
                                    unsigned th, bool opaque) {
                    lx::gfx::pixel_surface dst{};
                    dst.pixels = pixels;
                    dst.width = tw;
                    dst.height = th;
                    dst.stride = stride;
                    dst.format = static_cast<lx::fourcc>(opaque ? lx::pixel_format::xrgb8888
                                                                : lx::pixel_format::argb8888);
                    if (!region.empty())
                        (void)recomposite_cpu(src, dst, clear, src.blits, {}, region);
                    if (hw_cursor)
                        capture_blend_argb(pixels, tw, th, stride, src.cursor.pixels,
                                           src.cursor.width, src.cursor.height,
                                           src.cursor.stride_px, src.cursor.rect.x,
                                           src.cursor.rect.y);
                });
        } else {
            ok = false;
        }
        if (!ok) {
            fail(out, job, capture_failure::unknown);
            remove_pending(i);
            continue;
        }
        c.cursor = {true, hw_cursor, hw_cursor ? src.cursor.rect : lx::rect2i{},
                    src.cursor.generation};
        complete(out, c);
        if (c.result == capture_result::readback && c.fence_fd < 0) {
            capture_job fin = job;
            fin.kind = capture_job_kind::finish;
            run_finish(fin, out);
        }
        remove_pending(i);
    }
}

} // namespace lx::compositor
