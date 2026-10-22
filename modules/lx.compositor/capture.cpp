module;

// Bodies of `:capture`. See capture.cppm for the model.

module lx.compositor;

import std;

import lx.foundation;
import lx.gfx;
import :capture;

namespace lx::compositor {

namespace {

/// DRM_FORMAT_MOD_LINEAR and DRM_FORMAT_MOD_INVALID (the implicit modifier a client that
/// passes none gets from linux-dmabuf).
constexpr unsigned long long k_mod_linear = 0;
constexpr unsigned long long k_mod_invalid = 0x00ffffffffffffffull;

constexpr lx::fourcc fourcc_of(char a, char b, char c, char d) {
    return static_cast<lx::fourcc>(static_cast<unsigned>(a) | (static_cast<unsigned>(b) << 8) |
                                   (static_cast<unsigned>(c) << 16) |
                                   (static_cast<unsigned>(d) << 24));
}
constexpr lx::fourcc k_xr24 = fourcc_of('X', 'R', '2', '4');
constexpr lx::fourcc k_ar24 = fourcc_of('A', 'R', '2', '4');

} // namespace

bool capture_constraints::operator==(const capture_constraints& o) const {
    if (size.width != o.size.width || size.height != o.size.height ||
        shm_count != o.shm_count || dmabuf_count != o.dmabuf_count ||
        dmabuf_device != o.dmabuf_device)
        return false;
    for (unsigned i = 0; i < shm_count; ++i)
        if (shm_formats[i] != o.shm_formats[i])
            return false;
    for (unsigned i = 0; i < dmabuf_count; ++i)
        if (dmabuf_formats[i] != o.dmabuf_formats[i])
            return false;
    return true;
}

capture_constraints make_capture_constraints(lx::size2i size, bool dmabuf,
                                             unsigned long long dmabuf_device) {
    capture_constraints c{};
    c.size = size;
    // XRGB first: the scanout has no alpha, and a client that takes the first format offered
    // (grim, xdpw) then gets exactly what the copy produces. The order never changes (niri:
    // clients depend on it).
    c.shm_formats[0] = k_wl_shm_xrgb8888;
    c.shm_formats[1] = k_wl_shm_argb8888;
    c.shm_count = 2;
    if (dmabuf && dmabuf_device != 0) {
        c.dmabuf_formats[0] = k_xr24;
        c.dmabuf_formats[1] = k_ar24;
        c.dmabuf_count = 2;
        c.dmabuf_device = dmabuf_device;
    }
    return c;
}

bool capture_buffer_fits(const capture_constraints& c, const capture_buffer_desc& b) {
    if (!c.valid() || b.width != static_cast<unsigned>(c.size.width) ||
        b.height != static_cast<unsigned>(c.size.height))
        return false;
    switch (b.kind) {
    case capture_buffer_kind::shm:
        if (b.stride < b.width * 4u)
            return false;
        for (unsigned i = 0; i < c.shm_count; ++i)
            if (c.shm_formats[i] == b.format)
                return true;
        return false;
    case capture_buffer_kind::dmabuf:
        if (b.planes != 1 || (b.modifier != k_mod_linear && b.modifier != k_mod_invalid))
            return false;
        for (unsigned i = 0; i < c.dmabuf_count; ++i)
            if (c.dmabuf_formats[i] == static_cast<lx::fourcc>(b.format))
                return true;
        return false;
    case capture_buffer_kind::none:
    default:
        return false;
    }
}

capture_frame_error capture_frame_attach(capture_frame_requests& f) {
    if (f.captured)
        return capture_frame_error::already_captured;
    f.buffer = true;
    return capture_frame_error::none;
}

capture_frame_error capture_frame_damage(capture_frame_requests& f, int x, int y, int w, int h) {
    // The XML lists the conditions in this order; already_captured is about the request
    // itself, so it wins over a malformed rect sent after capture.
    if (f.captured)
        return capture_frame_error::already_captured;
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
        return capture_frame_error::invalid_buffer_damage;
    f.damage.add({x, y, w, h});
    return capture_frame_error::none;
}

capture_frame_error capture_frame_capture(capture_frame_requests& f) {
    if (f.captured)
        return capture_frame_error::already_captured;
    if (!f.buffer)
        return capture_frame_error::no_buffer;
    f.captured = true;
    return capture_frame_error::none;
}

// ── sessions ─────────────────────────────────────────────────────────────────

unsigned capture_session_table::open(unsigned client, capture_source_kind kind) {
    if (count_ >= k_capture_sessions || count_of(client) >= k_capture_sessions_per_client)
        return 0;
    for (unsigned i = 0; i < k_capture_sessions; ++i) {
        capture_session& s = sessions_[i];
        if (s.used)
            continue;
        const unsigned gen = s.generation + 1u;
        s = {};
        s.used = true;
        s.generation = gen == 0 ? 1u : gen;
        s.client = client;
        s.kind = kind;
        ++count_;
        return i + 1;
    }
    return 0;
}

void capture_session_table::close(unsigned handle) {
    capture_session* s = get(handle);
    if (!s)
        return;
    const unsigned gen = s->generation;
    *s = {};
    s->generation = gen;
    --count_;
}

capture_session* capture_session_table::get(unsigned handle, unsigned generation) {
    capture_session* s = get(handle);
    return s && s->generation == generation ? s : nullptr;
}

unsigned capture_session_table::count_of(unsigned client) const {
    unsigned n = 0;
    for (const auto& s : sessions_)
        if (s.used && s.client == client)
            ++n;
    return n;
}

// ── policy ───────────────────────────────────────────────────────────────────

capture_action capture_decide(const capture_session& s, const capture_env& env) {
    if (s.stopped || env.source_gone)
        return capture_action::stop;
    if (env.locked) {
        // The lock holder's own output sessions see what the outputs show — only the lock
        // surfaces — once a frame built locked exists; before that, and for everyone else,
        // the curtain, then nothing until unlock. A window's content is never captured
        // while locked, not even by the locker (its window is behind the lock).
        const bool locker = s.kind == capture_source_kind::output && env.locker_client != 0 &&
                            s.client == env.locker_client && env.lock_scene_shown;
        if (!locker)
            return s.curtained ? capture_action::wait : capture_action::curtain;
    }
    if (env.held)
        return capture_action::wait;
    return capture_action::copy;
}

// ── damage ring ──────────────────────────────────────────────────────────────

unsigned capture_damage_ring::record(const lx::gfx::damage_region& damage) {
    ++seq_;
    if (seq_ == 0) {
        // 2^32 composites later: everything older than now is "everything".
        seq_ = 1;
        floor_ = 0;
    }
    ring_[seq_ % k_depth] = damage;
    return seq_;
}

bool capture_damage_ring::since(unsigned after, lx::gfx::damage_region& out) const {
    out.clear();
    // Before a reset (the content changed in a way no damage describes), from the future
    // (another ring), or beyond what the ring keeps: everything.
    if (after < floor_ || after > seq_ || seq_ - after > k_depth)
        return false;
    if (after == seq_)
        return true;
    for (unsigned s = after + 1; s <= seq_; ++s)
        out.add_region(ring_[s % k_depth]);
    return true;
}

void capture_damage_ring::reset() { floor_ = seq_ + 1; }

// ── cursor ───────────────────────────────────────────────────────────────────

lx::rect2i capture_cursor_rect(double x, double y, lx::point2i origin, double scale,
                               lx::buffer_transform transform, lx::size2i mode, unsigned img_w,
                               unsigned img_h, int hot_x, int hot_y) {
    // The cursor plane's placement (compositor_impl::prepare_cursor_plane): the hotspot's
    // pixel, turned with the output, minus the (turned) hotspot.
    int px = static_cast<int>(std::floor((x - origin.x) * scale));
    int py = static_cast<int>(std::floor((y - origin.y) * scale));
    if (transform != lx::buffer_transform::normal) {
        const bool swap = (static_cast<unsigned>(transform) & 1u) != 0;
        const int cw = swap ? mode.height : mode.width;
        const int ch = swap ? mode.width : mode.height;
        const lx::rect2i d = lx::gfx::transform_rect({px, py, 1, 1}, transform, cw, ch);
        px = d.x;
        py = d.y;
    }
    return {px - hot_x, py - hot_y, static_cast<int>(img_w), static_cast<int>(img_h)};
}

void capture_blend_argb(unsigned char* dst, unsigned dst_w, unsigned dst_h, unsigned dst_stride,
                        const std::uint32_t* src, unsigned src_w, unsigned src_h,
                        unsigned src_stride_px, int x, int y) {
    if (!dst || !src)
        return;
    const int x0 = x < 0 ? 0 : x;
    const int y0 = y < 0 ? 0 : y;
    const long long x1l = static_cast<long long>(x) + src_w;
    const long long y1l = static_cast<long long>(y) + src_h;
    const int x1 = x1l > dst_w ? static_cast<int>(dst_w) : static_cast<int>(x1l);
    const int y1 = y1l > dst_h ? static_cast<int>(dst_h) : static_cast<int>(y1l);
    if (x1 <= x0 || y1 <= y0)
        return;
    for (int row = y0; row < y1; ++row) {
        const std::uint32_t* s =
            src + static_cast<std::size_t>(row - y) * src_stride_px + static_cast<unsigned>(x0 - x);
        unsigned char* d = dst + static_cast<std::size_t>(row) * dst_stride +
                           static_cast<std::size_t>(x0) * 4u;
        for (int col = x0; col < x1; ++col, ++s, d += 4) {
            const std::uint32_t p = *s;
            const unsigned a = p >> 24;
            if (a == 0)
                continue;
            const unsigned sr = (p >> 16) & 0xFFu;
            const unsigned sg = (p >> 8) & 0xFFu;
            const unsigned sb = p & 0xFFu;
            if (a == 255) {
                d[0] = static_cast<unsigned char>(sb);
                d[1] = static_cast<unsigned char>(sg);
                d[2] = static_cast<unsigned char>(sr);
                d[3] = 0xFF;
                continue;
            }
            // Premultiplied over: dst = src + dst × (1 − a), rounded (x/255 ≈ (x+128)·257>>16).
            const unsigned ia = 255u - a;
            const auto mul = [](unsigned c, unsigned f) {
                const unsigned t = c * f + 128u;
                return (t + (t >> 8)) >> 8;
            };
            d[0] = static_cast<unsigned char>(sb + mul(d[0], ia));
            d[1] = static_cast<unsigned char>(sg + mul(d[1], ia));
            d[2] = static_cast<unsigned char>(sr + mul(d[2], ia));
            d[3] = static_cast<unsigned char>(a + mul(d[3], ia));
        }
    }
}

// ── wlr-screencopy ───────────────────────────────────────────────────────────

bool screencopy_seq_table::get(unsigned client, lx::output_id output, unsigned& seq,
                               capture_cursor_memo& cursor) const {
    for (const auto& e : entries_)
        if (e.live && e.client == client && e.output == output) {
            seq = e.seq;
            cursor = e.cursor;
            return true;
        }
    return false;
}

void screencopy_seq_table::set(unsigned client, lx::output_id output, unsigned seq,
                               const capture_cursor_memo& cursor) {
    entry* slot = nullptr;
    for (auto& e : entries_)
        if (e.live && e.client == client && e.output == output)
            slot = &e;
    for (unsigned i = 0; !slot && i < k_entries; ++i)
        if (!entries_[i].live)
            slot = &entries_[i];
    if (!slot) {
        slot = &entries_[0];
        for (auto& e : entries_)
            if (e.used < slot->used)
                slot = &e;
    }
    slot->live = true;
    slot->client = client;
    slot->output = output;
    slot->seq = seq;
    slot->cursor = cursor;
    slot->used = ++clock_;
}

void screencopy_seq_table::forget_client(unsigned client) {
    for (auto& e : entries_)
        if (e.live && e.client == client)
            e = {};
}

} // namespace lx::compositor
