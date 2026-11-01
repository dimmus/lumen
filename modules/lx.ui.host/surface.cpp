module;

// Bodies of lx.ui.host:surface: create/destroy, configure and scale, buffers, the frame loop.

#include <time.h>

module lx.ui.host;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.trace;
import lx.ui;
import lx.wayland.client;

namespace lx::ui::host {

namespace {

constexpr lx::trace::category log_surface{"ui.host.surface"};

[[nodiscard]] lx::gfx::ui_stream stream_of(const lx::scene::draw_list& d) {
    lx::gfx::ui_stream s{};
    s.refs = d.refs();
    s.batches = d.batches();
    s.quads = d.quad_instances();
    s.shadows = d.shadow_instances();
    s.sprites = d.sprite_instances();
    s.underlines = d.underline_instances();
    return s;
}

[[nodiscard]] int ceil_scaled(int logical, float scale) {
    if (scale == 1.f)
        return logical;
    // The tolerance keeps an exact product (640 × 1.25) from rounding up a pixel on float
    // error.
    return static_cast<int>(std::ceil(static_cast<double>(logical) * scale - 1e-6));
}

} // namespace

/// The surface as an lx::runtime::event_source: due when a frame should be drawn now; no fd,
/// no timeout of its own (GTK's frame clock is a GSource the same way).
struct surface::frame_source final : public lx::runtime::event_source {
    explicit frame_source(surface& s) : owner{s} {}
    bool prepare(lx::runtime::clock_time& timeout) override {
        const bool due = owner.frame_due();
        timeout = due ? 0 : -1;
        return due;
    }
    bool check() override { return owner.frame_due(); }
    bool dispatch() override {
        owner.run_frame();
        return true;
    }
    surface& owner;
};

surface::surface() = default;

surface::~surface() { destroy(); }

std::uint64_t surface::thread_cpu_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint64_t surface::now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

lx::result<void> surface::create(context& ctx, const surface_options& options) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (created_)
        return lx::invalid_argument("lx::ui::host::surface::create: already created");
    if (!options.describe)
        return lx::invalid_argument("lx::ui::host::surface::create: no describe function");
    if (!ctx.started())
        return lx::invalid_argument("lx::ui::host::surface::create: the context is not started");
    lx::wayland::display& d = ctx.display();
    if (!d.connected())
        return lx::make_error(lx::wayland_err::connect_failed,
                              "lx::ui::host::surface::create: the display is not connected");
    ctx_ = &ctx;
    options_ = options;
    presentation_feedback_ = options.presentation_feedback;
    timeline_ = {};
    timeline_.opened_ns = now_ns();
    stats_ = {};
    ring_next_ = 0;
    created_ = true;
    if (!ctx.attach(*this)) {
        destroy();
        return lx::invalid_argument("lx::ui::host::surface::create: the context's surface table "
                                    "is full (k_max_surfaces)");
    }
    registered_ = true;

    if (auto failed = surface_.create(d); !failed) {
        destroy();
        return failed.get_error();
    }
    surface_.set_handler(this);
    // Fractional scale needs both: the scale and a viewport to put the larger buffer in the
    // logical size (otherwise integer buffer_scale).
    if (options.fractional_scale && d.has(lx::wayland::global_kind::fractional_scale) &&
        d.has(lx::wayland::global_kind::viewporter)) {
        fractional_ = surface_.enable_fractional_scale().ok() && surface_.enable_viewport().ok();
        viewport_ = fractional_;
    }

    // The UI: a root on its own ui_context (damage, overlays, timers, reducers); the text and
    // icon caches are the context's (lx.ui's resources, installed by it).
    arena_ = std::make_unique<lx::runtime::memory_arena>(static_cast<unsigned>(options.arena_bytes));
    ui_ = std::make_unique<lx::ui::ui_context>();
    root_ = std::make_unique<lx::ui::ui_root>(*arena_, *ui_);
    root_->set_describer(options.describe);
    // A client keeps its last frame (the buffer's age is known): emit only what it repaints.
    root_->set_emit_scope(lx::ui::emit_scope::damage);
    draws_ = std::make_unique<lx::scene::draw_list>();
    if (options.frame_records) {
        if (options.frame_ring.size() == k_frame_ring) {
            ring_ = options.frame_ring.data();
            for (frame_record& r : options.frame_ring)
                r = {};
        } else {
            ring_owned_ = std::make_unique<frame_record[]>(k_frame_ring);
            ring_ = ring_owned_.get();
        }
    }

    input_.bind(d, surface_, *root_, ctx.text_input());
    source_ = std::make_unique<frame_source>(*this);
    ctx.loop().add_source(source_.get());
    return {};
}

lx::result<void> surface::show() {
    if (!created_)
        return lx::invalid_argument("lx::ui::host::surface::show: not created");
    // The initial commit (no buffer) goes out first: the compositor's first configure travels
    // while the GPU tier initialises (EGL / Vulkan device creation, tens of ms).
    (void)surface_.commit();
    ++stats_.commits;
    (void)ctx_->display().flush();
    if (pixel_path_)
        return {};
    const std::uint64_t t0 = now_ns();
    if (auto made = ctx_->ensure_gpu(options_.opaque); !made) {
        log_surface.error(made.get_error());
        return made;
    }
    format_ = {};
    if (ctx_->gpu().tier() != lx::gfx::client_tier::cpu) {
        auto chosen = ctx_->format_for(options_.opaque);
        if (!chosen) {
            log_surface.error(chosen.get_error());
            return chosen.get_error();
        }
        format_ = chosen.value();
    }
    swapchain_.set_listener(this);
    swapchain_.set_owner_affinity(lx::runtime::affinity::ui);
    pixel_path_ = true;
    log_surface.debug("pixel path: {} in {} µs", lx::gfx::client_tier_name(ctx_->gpu().tier()),
                      (now_ns() - t0) / 1000);
    return {};
}

void surface::destroy() {
    if (!created_)
        return;
    if (registered_)
        ctx_->detach(*this);
    registered_ = false;
    if (source_)
        ctx_->loop().remove_source(source_.get());
    if (release_source_)
        ctx_->loop().remove_source(release_source_.get());
    source_.reset();
    release_source_.reset();
    // The role object goes before the wl_surface it is the role of.
    if (surface_role* r = role_) {
        role_ = nullptr;
        r->role_surface_destroyed();
    }
    input_.unbind();
    // The shared renderer may still read this surface's images: Vulkan waits for its queue
    // (GL's draws are fenced in the ring; the images' EGLImages go with the swapchain).
    if (swapchain_.valid())
        if (lx::gfx::ui_vk_renderer* vk = ctx_->vk_renderer())
            vk->wait_idle();
    last_image_ = nullptr;
    swapchain_.destroy();
    for (buffer_slot& b : buffers_) {
        b.buffer.destroy();
        b.timeline.destroy();
        b.id = 0;
        b.pool_id = 0;
    }
    for (shm_pool_slot& p : pools_) {
        p.pool.destroy();
        p.id = 0;
        p.refs = 0;
    }
    surface_.destroy();
    root_.reset();
    ui_.reset();
    arena_.reset();
    draws_.reset();
    ring_ = nullptr;
    ring_owned_.reset();
    ring_next_ = 0;
    role_ = nullptr;
    created_ = false;
    pixel_path_ = false;
    configured_ = false;
    configure_pending_ = false;
    scale_pending_ = false;
    waiting_release_ = false;
    frame_requested_ns_ = 0;
    syncobj_ = false;
    broken_ = false;
    opaque_region_set_ = false;
    fractional_ = viewport_ = false;
    logical_ = buffer_ = viewport_dst_ = {};
    scale_ = 1.f;
    buffer_scale_ = 1;
    ctx_ = nullptr;
}

void surface::configure(const surface_configure& c) {
    ++stats_.configures;
    if (timeline_.first_configure_ns == 0)
        timeline_.first_configure_ns = now_ns();
    pending_cfg_ = c;
    configure_pending_ = true;
}

void surface::commit_state() {
    if (!created_)
        return;
    (void)surface_.commit();
    ++stats_.commits;
}

lx::gfx::client_tier surface::tier() const {
    return ctx_ ? ctx_->gpu().tier() : lx::gfx::client_tier::none;
}

const lx::gfx::client_gpu& surface::gpu() const {
    if (ctx_)
        return ctx_->gpu();
    // Not created: an empty gpu (tier none), made on first need and never destroyed (a
    // static object's initialised state would sit in every binary's .data).
    static const lx::gfx::client_gpu* const none = new lx::gfx::client_gpu{};
    return *none;
}

lx::gfx::atlas& surface::atlas() { return ctx_->atlas(); }

void surface::set_opaque_region(std::span<const lx::rect2i> rects) {
    opaque_region_set_ = true;
    surface_.set_opaque_region(rects);
}

void surface::set_input_region(std::span<const lx::rect2i> rects) { surface_.set_input_region(rects); }

void surface::reset_input_region() { surface_.reset_input_region(); }

// ── the pixel path (ADR-010 §4.10) ──

lx::result<void> surface::render(lx::gfx::swapchain_image& image,
                                 const lx::gfx::damage_region& repaint) {
    const lx::gfx::ui_stream stream = stream_of(*draws_);
    // Each repainted rect starts from the window background (premultiplied, encoded); a
    // translucent surface's from nothing (its UI draws its own backgrounds).
    lx::color bg = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    if (options_.opaque) {
        bg = lx::ui::active_theme().color(lx::ui::tok::background);
        bg = lx::color::rgb(bg.r * bg.a, bg.g * bg.a, bg.b * bg.a, bg.a);
    }
    lx::gfx::atlas* atlas = &ctx_->atlas();
    switch (ctx_->gpu().tier()) {
    case lx::gfx::client_tier::gl: {
        auto drawn = ctx_->gl_renderer()->draw(image.gl_framebuffer, image.width, image.height,
                                               stream, atlas, repaint, bg);
        if (!drawn)
            return drawn.get_error();
        stats_.slot_wait_ns += drawn.value().slot_wait_ns;
        return {};
    }
    case lx::gfx::client_tier::vulkan: {
        auto drawn = ctx_->vk_renderer()->draw(image.vk, stream, atlas, repaint, bg);
        if (!drawn)
            return drawn.get_error();
        return {};
    }
    default: {
        auto drawn = ctx_->cpu_renderer().draw(image.pixels, stream, atlas, repaint, bg);
        if (!drawn)
            return drawn.get_error();
        return {};
    }
    }
}

lx::result<void> surface::apply_geometry() {
    lx::wayland::display& d = ctx_->display();
    // The size: the configure's, else what the surface has (the options' at first).
    lx::size2i size = logical_.width > 0 ? logical_ : options_.size;
    if (configure_pending_) {
        if (pending_cfg_.size.width > 0)
            size.width = pending_cfg_.size.width;
        if (pending_cfg_.size.height > 0)
            size.height = pending_cfg_.size.height;
    }
    // The minimum holds only where the compositor left the size to the client. A bounded
    // size (a maximized or tiled toplevel's, a popup's) is the size (xdg-shell: anything else
    // is invalid_surface_state), a fullscreen or resizing size the most the surface may take
    // — and the compositor may configure below set_min_size. The UI lays out inside it
    // (clipped) rather than grow.
    const bool bounded = pending_cfg_.bounded;
    if (!bounded || pending_cfg_.size.width <= 0)
        size.width = std::max(size.width, options_.min_size.width);
    if (!bounded || pending_cfg_.size.height <= 0)
        size.height = std::max(size.height, options_.min_size.height);
    size.width = std::max(size.width, 1);
    size.height = std::max(size.height, 1);

    // The scale: fractional (1/120) with a viewport, else the integer buffer scale.
    const std::uint32_t s120 = surface_.preferred_scale120();
    float scale = 1.f;
    int bscale = 1;
    if (fractional_ && viewport_) {
        scale = static_cast<float>(s120) / 120.f;
    } else {
        bscale = static_cast<int>((s120 + 119) / 120);
        if (d.version(lx::wayland::global_kind::compositor) < 6) {
            // No preferred_buffer_scale before wl_compositor v6: the largest scale of the
            // outputs the surface is on, as GTK does.
            const std::uint32_t mask = surface_.outputs();
            for (unsigned slot = 0; slot < lx::wayland::k_max_outputs; ++slot)
                if (mask & (1u << slot))
                    if (const lx::wayland::output* o = d.output_at(slot))
                        bscale = std::max(bscale, static_cast<int>(o->info().scale));
        }
        bscale = std::max(bscale, 1);
        scale = static_cast<float>(bscale);
    }
    const lx::size2i buffer{ceil_scaled(size.width, scale), ceil_scaled(size.height, scale)};

    if (size.width != logical_.width || size.height != logical_.height) {
        root_->set_bounds({0, 0, size.width, size.height});
        if (options_.opaque && !opaque_region_set_) {
            const lx::rect2i opaque[1] = {{0, 0, size.width, size.height}};
            surface_.set_opaque_region(opaque);
        }
        logical_ = size;
    }
    if (scale != scale_ || !swapchain_.valid()) {
        if (scale != scale_)
            ++stats_.scale_changes;
        root_->set_scale(scale);
        scale_ = scale;
    }
    if (viewport_) {
        if (viewport_dst_.width != size.width || viewport_dst_.height != size.height) {
            surface_.set_viewport_destination(size);
            viewport_dst_ = size;
        }
    } else if (bscale != buffer_scale_) {
        surface_.set_buffer_scale(bscale);
        buffer_scale_ = bscale;
    }

    if (!swapchain_.valid()) {
        lx::gfx::swapchain_options so{};
        so.width = static_cast<unsigned>(buffer.width);
        so.height = static_cast<unsigned>(buffer.height);
        so.format = format_;
        so.opaque = options_.opaque;
        so.explicit_sync = d.has(lx::wayland::global_kind::syncobj);
        if (auto failed = swapchain_.create(ctx_->gpu(), so); !failed)
            return failed.get_error();
        ++stats_.reallocations;
        // The context's renderer, built with its first swapchain (GL: the context current).
        if (auto failed = ctx_->ensure_renderer(); !failed)
            return failed.get_error();
        if (const int fd = swapchain_.release_fd(); fd >= 0 && !release_source_) {
            // Explicit sync: release points signal an eventfd, not wl_buffer.release.
            release_source_ = std::make_unique<lx::runtime::fd_source>(
                fd, lx::runtime::task{[this] { collect_releases(); }});
            ctx_->loop().add_source(release_source_.get());
        }
    } else if (buffer.width != buffer_.width || buffer.height != buffer_.height) {
        last_image_ = nullptr;
        auto resized = swapchain_.resize(static_cast<unsigned>(buffer.width),
                                         static_cast<unsigned>(buffer.height));
        if (!resized)
            return resized.get_error();
        if (resized.value())
            ++stats_.reallocations;
    }
    buffer_ = buffer;
    scale_pending_ = false;
    return {};
}

void surface::collect_releases() {
    if (swapchain_.collect_releases() > 0)
        waiting_release_ = false;
}

surface::buffer_slot* surface::slot_of(std::uint32_t id) {
    for (buffer_slot& b : buffers_)
        if (b.id == id && id != 0)
            return &b;
    return nullptr;
}

void surface::on_image_created(const lx::gfx::swapchain_image& image) {
    buffer_slot* slot = nullptr;
    for (buffer_slot& b : buffers_)
        if (b.id == 0) {
            slot = &b;
            break;
        }
    if (!slot) {
        log_surface.error("swapchain image {}: no buffer slot left", image.id);
        return;
    }
    lx::wayland::display& d = ctx_->display();
    const lx::size2i size{static_cast<int>(image.width), static_cast<int>(image.height)};
    lx::result<void> made{};
    if (image.pixels.pixels) {
        // wl_shm over the swapchain's memfd: one pool per block (the compositor maps a pool
        // once, for as long as its buffers live), kept while the block has images.
        shm_pool_slot* pool = nullptr;
        for (shm_pool_slot& p : pools_)
            if (p.id == image.pool_id) {
                pool = &p;
                break;
            }
        if (!pool)
            for (shm_pool_slot& p : pools_)
                if (p.id == 0) {
                    pool = &p;
                    break;
                }
        if (!pool) {
            made = lx::make_error(lx::wayland_err::object_failed,
                                  "lx::ui::host::surface: no wl_shm_pool slot left");
        } else {
            if (pool->id == 0) {
                made = pool->pool.create(d, image.planes[0].fd, image.pool_bytes);
                if (made)
                    pool->id = image.pool_id;
            }
            if (made)
                made = slot->buffer.create_shm(pool->pool, image.planes[0].offset, size,
                                               static_cast<std::int32_t>(image.planes[0].stride),
                                               image.fourcc == lx::gfx::k_fourcc_xrgb8888 ? 1u : 0u);
            if (made) {
                ++pool->refs;
                slot->pool_id = pool->id;
            } else if (pool->refs == 0) {
                pool->pool.destroy();
                pool->id = 0;
            }
        }
    } else {
        lx::wayland::dmabuf_plane planes[4]{};
        for (unsigned p = 0; p < image.plane_count && p < 4; ++p)
            planes[p] = {image.planes[p].fd, image.planes[p].offset, image.planes[p].stride};
        made = slot->buffer.create_dmabuf(d, std::span{planes, image.plane_count}, size,
                                          image.fourcc, image.modifier);
    }
    if (!made) {
        log_surface.error(made.get_error());
        return;
    }
    if (image.timeline_fd >= 0)
        if (auto t = slot->timeline.create(d, image.timeline_fd); !t)
            log_surface.error(t.get_error());
    slot->buffer.set_handler(this);
    slot->id = image.id;
}

void surface::on_image_destroyed(std::uint32_t id) {
    if (buffer_slot* slot = slot_of(id)) {
        slot->buffer.destroy();
        slot->timeline.destroy();
        slot->id = 0;
        release_pool(slot->pool_id);
        slot->pool_id = 0;
    }
}

void surface::release_pool(std::uint32_t pool_id) {
    if (pool_id == 0)
        return;
    for (shm_pool_slot& p : pools_)
        if (p.id == pool_id) {
            if (p.refs > 0)
                --p.refs;
            if (p.refs == 0) {
                p.pool.destroy();
                p.id = 0;
            }
            return;
        }
}

void surface::on_release(lx::wayland::buffer& b) {
    for (buffer_slot& s : buffers_)
        if (&s.buffer == &b && s.id != 0) {
            swapchain_.release(s.id);
            waiting_release_ = false;
            return;
        }
}

void surface::on_failed(lx::wayland::buffer& b) {
    (void)b;
    // The compositor could not import a dma-buf: fail loudly once; the surface stops drawing
    // rather than committing buffers it cannot show.
    if (!broken_)
        log_surface.error("the compositor refused a {} buffer (zwp_linux_buffer_params_v1.failed)",
                         lx::gfx::client_tier_name(tier()));
    broken_ = true;
}

// ── configure, scale, callbacks ──

void surface::on_frame(lx::wayland::surface& s, std::uint32_t time_ms) {
    (void)s;
    (void)time_ms;
    ++stats_.frame_callbacks;
}

void surface::on_preferred_scale(lx::wayland::surface& s, std::uint32_t scale120) {
    (void)s;
    (void)scale120;
    // Answered with a frame at the new scale (at once: the content is wrong until then).
    scale_pending_ = true;
}

void surface::on_presentation(lx::wayland::surface& s, const lx::wayland::presentation_event& e) {
    (void)s;
    const std::uint64_t n = ring_ ? std::min<std::uint64_t>(ring_next_, k_frame_ring) : 0;
    for (std::uint64_t i = 1; i <= n; ++i) {
        frame_record& r = ring_[(ring_next_ - i) % k_frame_ring];
        if (r.commit_id != e.commit_id)
            continue;
        r.presented = e.presented;
        r.discarded = !e.presented;
        r.presented_ns = e.time_ns;
        r.refresh_ns = e.refresh_ns;
        r.sequence = e.sequence;
        r.flags = e.flags;
        break;
    }
    if (e.presented) {
        ++stats_.presented;
        if (timeline_.first_presented_ns == 0)
            timeline_.first_presented_ns =
                ctx_ && ctx_->display().presentation_clock() == 1 ? e.time_ns : now_ns();
    } else {
        ++stats_.discarded;
    }
}

const frame_record& surface::frame_at(unsigned i) const {
    static const frame_record none{};
    if (!ring_)
        return none;
    const std::uint64_t n = std::min<std::uint64_t>(ring_next_, k_frame_ring);
    const std::uint64_t first = ring_next_ - n;
    return ring_[(first + i) % k_frame_ring];
}

unsigned surface::frame_count() const {
    if (!ring_)
        return 0;
    return static_cast<unsigned>(std::min<std::uint64_t>(ring_next_, k_frame_ring));
}

const lx::gfx::pixel_surface* surface::cpu_pixels() const {
    if (tier() != lx::gfx::client_tier::cpu || !last_image_ || !last_image_->pixels.pixels)
        return nullptr;
    return &last_image_->pixels;
}

std::uint64_t surface::next_deadline_ns(std::uint64_t now) const {
    if (!created_)
        return 0;
    std::uint64_t out = input_.repeat_deadline_ns();
    // A UI timer already due is the frame loop's (now, or at the frame callback it waits
    // for): arming the timer for the past would only spin.
    if (const std::uint64_t ui = root_->next_deadline_ns(); ui > now && (out == 0 || ui < out))
        out = ui;
    // A configure held for the outstanding frame callback: answered by then at the latest.
    if (configure_pending_ && configured_ && surface_.frame_pending()) {
        const std::uint64_t held = frame_requested_ns_ + k_configure_wait_ns;
        if (held > now && (out == 0 || held < out))
            out = held;
    }
    return out;
}

void surface::on_deadline(std::uint64_t now) { input_.on_repeat_due(now); }

// ── the frame loop (ADR-010 §4.9) ──

bool surface::frame_due() const {
    if (!created_ || !pixel_path_ || broken_ || waiting_release_)
        return false;
    if (configure_pending_) {
        // The first configure, or one that finds no frame callback outstanding, is answered
        // at once. One that arrives while a callback is outstanding (a compositor configuring
        // per pointer motion) waits for it, the newest replacing the older (only its serial
        // is acked: xdg-shell allows it) — one resize per refresh, as GTK 4's frozen frame
        // clock gives. A callback withheld past `k_configure_wait_ns` (an occluded surface)
        // no longer holds it.
        if (!configured_ || !surface_.frame_pending())
            return true;
        return now_ns() >= frame_requested_ns_ + k_configure_wait_ns;
    }
    if (configured_ && scale_pending_)
        return true;
    if (!configured_)
        return false;
    // Throttled: the outstanding callback is the next frame.
    if (surface_.frame_pending())
        return false;
    return input_.motion_pending() || continuous_ || root_->wants_frame();
}

void surface::run_frame() {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    frame_start_ns_ = now_ns();
    frame_start_cpu_ns_ = thread_cpu_ns();
    // The frame's one hit test: the latest pointer position.
    input_.flush_motion();
    frame_input_ns_ = now_ns() - frame_start_ns_;
    if (!configure_pending_ && !scale_pending_ && !continuous_ && !root_->wants_frame())
        return;
    (void)draw();
}

void surface::ack_pending() {
    if (!configure_pending_)
        return;
    if (role_)
        role_->role_ack(pending_cfg_.serial);
    configure_pending_ = false;
    configured_ = true;
    ++stats_.acks;
}

void surface::restore_damage() {
    // §4.9: the ledger is cleared only by a successful commit.
    lx::ui::damage_ledger& ledger = root_->damage();
    const lx::ui::damage_ledger& lost = root_->frame_damage();
    if (lost.count() == 0)
        ledger.add(root_->bounds());
    for (unsigned i = 0; i < lost.count(); ++i)
        ledger.add(lost.rects()[i]);
    ledger.request(lx::ui::invalidation::paint);
}

bool surface::draw() {
    // The first frame's stages, logged once at debug (§8.2 "Start-up": where G7's time goes).
    const bool first = stats_.frames == 0;
    const std::uint64_t t_begin = now_ns();
    if (auto g = apply_geometry(); !g) {
        log_surface.error(g.get_error());
        broken_ = true;
        return false;
    }
    const std::uint64_t t_geometry = now_ns();
    auto acquired = swapchain_.acquire();
    if (!acquired) {
        log_surface.error(acquired.get_error());
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    if (!acquired.value()) {
        // Every image is with the compositor: draw once one comes back.
        waiting_release_ = true;
        ++stats_.no_image;
        return false;
    }
    lx::gfx::swapchain_image& image = *acquired.value();
    if (swapchain_.explicit_sync() && !syncobj_) {
        // Explicit sync settles with the first image (lx.gfx:swapchain: it never downgrades
        // after); the syncobj surface exists from then on, and every commit with a buffer
        // carries its two points (linux-drm-syncobj: no_acquire_point / no_release_point).
        if (auto e = surface_.enable_syncobj(); e) {
            syncobj_ = true;
        } else {
            // Releases come from the timeline, which the compositor would never signal.
            log_surface.error(e.get_error());
            swapchain_.cancel(image);
            ++stats_.failures;
            broken_ = true;
            return false;
        }
    }
    buffer_slot* slot = slot_of(image.id);
    if (!slot || (syncobj_ && !slot->timeline.valid())) {
        // No wl_buffer, or a buffer with no timeline on a syncobj surface: a commit would be
        // a protocol error. Nothing was ticked yet: the ledger is untouched.
        log_surface.error("swapchain image {} has no {}", image.id,
                         slot ? "syncobj timeline" : "wl_buffer");
        swapchain_.cancel(image);
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    const std::uint64_t t_acquired = now_ns();
    ctx_->atlas().begin_frame(ctx_->next_atlas_frame());

    // What the image lacks: the damage of the frames since it was last drawn.
    lx::gfx::damage_region age{};
    swapchain_.add_age_damage(image, age);
    if (age.full || image.age == 0) {
        root_->add_repaint(root_->bounds());
    } else {
        for (unsigned i = 0; i < age.count; ++i)
            root_->add_repaint(lx::ui::unproject_rect(age.rects[i], scale_));
    }
    if (continuous_)
        root_->damage().add(root_->bounds());

    const std::uint64_t t_ticking = now_ns();
    const lx::ui::layout_counters layout_before = lx::ui::ui_layout_counters();
    if (!root_->tick(*draws_)) {
        swapchain_.cancel(image);
        ++stats_.empty_ticks;
        if (configure_pending_) {
            // A configure that changed nothing the UI draws (a state bit): acked, committed.
            ack_pending();
            (void)surface_.commit();
            ++stats_.commits;
        }
        return false;
    }
    const std::uint64_t t_ticked = now_ns();
    draws_->batch_for_render();
    const std::uint64_t t_ordered = now_ns();

    // Device damage: what changed (the compositor is told) and what is repainted.
    const lx::rect2i whole{0, 0, buffer_.width, buffer_.height};
    lx::gfx::damage_region change{};
    lx::gfx::damage_region repaint{};
    const lx::ui::damage_ledger& changed = root_->frame_damage();
    if (changed.count() == 0)
        change.set_full();
    for (unsigned i = 0; i < changed.count(); ++i)
        change.add(lx::ui::project_rect(changed.rects()[i], scale_));
    const lx::ui::damage_ledger& painted = root_->frame_repaint();
    for (unsigned i = 0; i < painted.count(); ++i)
        repaint.add(lx::ui::project_rect(painted.rects()[i], scale_));
    if (change.full || image.age == 0)
        repaint.set_full();
    change.clip(whole);
    repaint.clip(whole);

    const std::uint64_t t_render = now_ns();
    if (auto r = render(image, repaint); !r) {
        log_surface.error(r.get_error());
        restore_damage();
        swapchain_.cancel(image);
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    const std::uint64_t t_rendered = now_ns();
    if (auto p = swapchain_.present(image, change); !p) {
        log_surface.error(p.get_error());
        restore_damage();
        swapchain_.cancel(image);
        ++stats_.failures;
        broken_ = true;
        return false;
    }
    const std::uint64_t t_presented = now_ns();
    if (first && log_surface.enabled(lx::trace::level::debug)) {
        const std::uint64_t t_end = now_ns();
        const lx::ui::ui_root::stage_times st = root_->last_stage_times();
        log_surface.debug("first frame {}×{} ({}): geometry+swapchain+acquire {} µs, tick {} µs "
                         "(describe {} reconcile {} layout {} emit {}), render+present {} µs",
                         buffer_.width, buffer_.height, lx::gfx::client_tier_name(tier()),
                         (t_ticking - t_begin) / 1000, (t_render - t_ticking) / 1000,
                         st.describe / 1000, st.reconcile / 1000, st.layout / 1000, st.emit / 1000,
                         (t_end - t_render) / 1000);
    }
    // The configure this frame answers: acked right before the commit that shows it.
    ack_pending();
    surface_.attach(&slot->buffer);
    for (unsigned i = 0; i < change.count; ++i)
        surface_.damage_buffer(change.rects[i]);
    if (syncobj_) {
        surface_.set_acquire_point(slot->timeline, image.acquire_point);
        surface_.set_release_point(slot->timeline, image.release_point);
    }
    // The next frame's throttle: it fires once; with nothing left to draw it asks for no more.
    if (!surface_.frame_pending()) {
        surface_.request_frame();
        frame_requested_ns_ = now_ns();
        ++stats_.frame_requests;
    }
    // Feedback only where something reads it: every frame when asked (the frame ring, the
    // bench), else until the first frame is presented (the timeline). Each request is a
    // proxy, its events and a delete_id.
    if (presentation_feedback_ || timeline_.first_presented_ns == 0)
        (void)surface_.request_presentation_feedback();
    const std::uint64_t commit_id = surface_.commit();
    const std::uint64_t committed = now_ns();
    ++stats_.commits;
    ++stats_.frames;
    if (timeline_.first_commit_ns == 0)
        timeline_.first_commit_ns = committed;

    frame_record scratch{};
    frame_record& rec = ring_ ? ring_[ring_next_++ % k_frame_ring] : scratch;
    rec = {};
    rec.commit_id = commit_id;
    rec.commit_ns = committed;
    rec.start_ns = frame_start_ns_;
    rec.cpu_ns = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(thread_cpu_ns() - frame_start_cpu_ns_, 0xFFFF'FFFFu));
    rec.damage_rects = static_cast<std::uint16_t>(change.count);
    rec.width = static_cast<std::uint16_t>(buffer_.width);
    rec.height = static_cast<std::uint16_t>(buffer_.height);
    {
        const lx::ui::ui_root::stage_times st = root_->last_stage_times();
        const auto ns32 = [](std::uint64_t ns) {
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(ns, 0xFFFF'FFFFu));
        };
        const lx::ui::layout_counters& lc = lx::ui::ui_layout_counters();
        rec.input_ns = ns32(frame_input_ns_);
        rec.damage_px = static_cast<std::uint32_t>(std::min<std::uint64_t>(change.area(), 0xFFFF'FFFFu));
        rec.geometry_ns = ns32(t_geometry - t_begin);
        rec.acquire_ns = ns32(t_acquired - t_geometry);
        rec.tick_ns = ns32(t_ticked - t_ticking);
        rec.describe_ns = ns32(st.describe);
        rec.reconcile_ns = ns32(st.reconcile);
        rec.layout_ns = ns32(st.layout);
        rec.emit_ns = ns32(st.emit);
        rec.order_ns = ns32(t_ordered - t_ticked);
        rec.render_ns = ns32(t_rendered - t_render);
        rec.present_ns = ns32(t_presented - t_rendered);
        rec.measures = static_cast<std::uint32_t>(lc.measures - layout_before.measures);
        rec.arranges = static_cast<std::uint32_t>(lc.arranges - layout_before.arranges);
    }
    last_damage_ = change;
    stats_.last_damage_rects = change.count;
    stats_.last_repaint_rects = repaint.count;
    stats_.last_damage_px = change.area();
    stats_.last_damage_bounds = change.bounds();
    last_image_ = &image;
    // The field's caret moved or its text changed with this frame: the input method hears.
    input_.after_frame();
    return true;
}

} // namespace lx::ui::host
