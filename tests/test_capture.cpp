// Screen capture's Wayland-free parts (design-app-compat-capture Part D): the policy
// (`:capture` — constraints and buffers, the frame requests and their errors, the session
// table and its limits, `capture_decide` across lock / pause / power / gone sources, the
// composite damage ring, pointer placement and blending, wlr-screencopy's per-client table)
// and the render side on the CPU backend (`:capture_engine` — the channel, the shm guards,
// copies on a composite and without one, damage, crops, paint_cursors, the lock epoch,
// curtains, window views).
#include "lumen_test.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

import lx.foundation;
import lx.gfx;
import lx.compositor;

using namespace lx::compositor;

namespace {

constexpr lx::fourcc k_xr24 = 0x34325258u;
constexpr lx::fourcc k_ar24 = 0x34325241u;
constexpr lx::fourcc k_ab24 = 0x34324241u;

capture_buffer_desc shm(unsigned w, unsigned h, std::uint32_t format = k_wl_shm_xrgb8888,
                        unsigned stride = 0) {
    capture_buffer_desc d{};
    d.kind = capture_buffer_kind::shm;
    d.width = w;
    d.height = h;
    d.format = format;
    d.stride = stride ? stride : w * 4u;
    return d;
}

capture_buffer_desc dmabuf(unsigned w, unsigned h, lx::fourcc f, unsigned long long mod,
                           unsigned planes = 1) {
    capture_buffer_desc d{};
    d.kind = capture_buffer_kind::dmabuf;
    d.width = w;
    d.height = h;
    d.format = f;
    d.modifier = mod;
    d.planes = planes;
    d.stride = w * 4u;
    return d;
}

} // namespace

// ── policy ───────────────────────────────────────────────────────────────────

LUMEN_TEST(constraints_offer_xrgb_first_and_dmabuf_only_with_a_device) {
    const capture_constraints c = make_capture_constraints({640, 480}, true, 0xE280ull);
    LUMEN_CHECK(c.valid() && c.size.width == 640 && c.size.height == 480);
    LUMEN_CHECK(c.shm_count == 2 && c.shm_formats[0] == k_wl_shm_xrgb8888 &&
                c.shm_formats[1] == k_wl_shm_argb8888);
    LUMEN_CHECK(c.dmabuf_count == 2 && c.dmabuf_formats[0] == k_xr24 &&
                c.dmabuf_formats[1] == k_ar24 && c.dmabuf_device == 0xE280ull);
    const capture_constraints cpu = make_capture_constraints({640, 480}, false, 0xE280ull);
    LUMEN_CHECK(cpu.dmabuf_count == 0 && cpu.dmabuf_device == 0);
    const capture_constraints nodev = make_capture_constraints({640, 480}, true, 0);
    LUMEN_CHECK(nodev.dmabuf_count == 0);
    LUMEN_CHECK(c == make_capture_constraints({640, 480}, true, 0xE280ull));
    LUMEN_CHECK(!(c == make_capture_constraints({641, 480}, true, 0xE280ull)));
    LUMEN_CHECK(!(c == cpu));
}

LUMEN_TEST(buffers_fit_size_format_stride_planes_and_linear_modifier) {
    const capture_constraints c = make_capture_constraints({64, 32}, true, 1);
    LUMEN_CHECK(capture_buffer_fits(c, shm(64, 32)));
    LUMEN_CHECK(capture_buffer_fits(c, shm(64, 32, k_wl_shm_argb8888)));
    LUMEN_CHECK(capture_buffer_fits(c, shm(64, 32, k_wl_shm_xrgb8888, 512)));
    LUMEN_CHECK(!capture_buffer_fits(c, shm(64, 33)));
    LUMEN_CHECK(!capture_buffer_fits(c, shm(64, 32, 0x34324241u /* abgr */)));
    LUMEN_CHECK(!capture_buffer_fits(c, shm(64, 32, k_wl_shm_xrgb8888, 200)));
    LUMEN_CHECK(capture_buffer_fits(c, dmabuf(64, 32, k_xr24, 0)));
    LUMEN_CHECK(capture_buffer_fits(c, dmabuf(64, 32, k_ar24, 0x00ffffffffffffffull)));
    LUMEN_CHECK(!capture_buffer_fits(c, dmabuf(64, 32, k_ab24, 0)));
    LUMEN_CHECK(!capture_buffer_fits(c, dmabuf(64, 32, k_xr24, 0x0100000000000001ull)));
    LUMEN_CHECK(!capture_buffer_fits(c, dmabuf(64, 32, k_xr24, 0, 2)));
    const capture_constraints cpu = make_capture_constraints({64, 32}, false, 0);
    LUMEN_CHECK(!capture_buffer_fits(cpu, dmabuf(64, 32, k_xr24, 0)));
    LUMEN_CHECK(!capture_buffer_fits({}, shm(0, 0)));
}

LUMEN_TEST(frame_requests_post_every_error_in_order) {
    capture_frame_requests f{};
    LUMEN_CHECK(capture_frame_capture(f) == capture_frame_error::no_buffer);
    LUMEN_CHECK(capture_frame_damage(f, -1, 0, 4, 4) == capture_frame_error::invalid_buffer_damage);
    LUMEN_CHECK(capture_frame_damage(f, 0, -1, 4, 4) == capture_frame_error::invalid_buffer_damage);
    LUMEN_CHECK(capture_frame_damage(f, 0, 0, 0, 4) == capture_frame_error::invalid_buffer_damage);
    LUMEN_CHECK(capture_frame_damage(f, 0, 0, 4, -2) == capture_frame_error::invalid_buffer_damage);
    LUMEN_CHECK(capture_frame_damage(f, 2, 3, 4, 5) == capture_frame_error::none);
    LUMEN_CHECK(f.damage.count == 1 && f.damage.rects[0].x == 2 && f.damage.rects[0].height == 5);
    LUMEN_CHECK(capture_frame_attach(f) == capture_frame_error::none);
    LUMEN_CHECK(capture_frame_attach(f) == capture_frame_error::none); // replaces
    LUMEN_CHECK(capture_frame_capture(f) == capture_frame_error::none);
    LUMEN_CHECK(capture_frame_capture(f) == capture_frame_error::already_captured);
    LUMEN_CHECK(capture_frame_attach(f) == capture_frame_error::already_captured);
    LUMEN_CHECK(capture_frame_damage(f, 0, 0, 1, 1) == capture_frame_error::already_captured);
    // After capture, already_captured wins over a malformed rect.
    LUMEN_CHECK(capture_frame_damage(f, -5, 0, 1, 1) == capture_frame_error::already_captured);
}

LUMEN_TEST(session_table_limits_total_and_per_client_and_bumps_generations) {
    capture_session_table t{};
    unsigned first = t.open(7, capture_source_kind::output);
    LUMEN_CHECK(first != 0 && t.get(first) && t.get(first)->client == 7);
    const unsigned gen = t.get(first)->generation;
    t.close(first);
    LUMEN_CHECK(!t.get(first) && t.count() == 0);
    const unsigned again = t.open(7, capture_source_kind::toplevel);
    LUMEN_CHECK(again == first && t.get(again)->generation == gen + 1);
    LUMEN_CHECK(t.get(again, gen) == nullptr && t.get(again, gen + 1) != nullptr);
    t.close(again);
    // 16 per client.
    std::vector<unsigned> mine;
    for (unsigned i = 0; i < k_capture_sessions_per_client; ++i)
        mine.push_back(t.open(1, capture_source_kind::output));
    for (unsigned h : mine)
        LUMEN_CHECK(h != 0);
    LUMEN_CHECK(t.open(1, capture_source_kind::output) == 0);
    LUMEN_CHECK(t.count_of(1) == k_capture_sessions_per_client);
    // 64 in all.
    unsigned client = 2;
    while (t.count() < k_capture_sessions) {
        if (!t.open(client, capture_source_kind::output))
            ++client;
    }
    LUMEN_CHECK(t.open(99, capture_source_kind::output) == 0);
    unsigned visited = 0;
    t.for_each([&](unsigned, capture_session& s) { visited += s.used ? 1 : 0; });
    LUMEN_CHECK(visited == k_capture_sessions);
}

LUMEN_TEST(decide_copies_holds_curtains_and_stops) {
    capture_session s{};
    s.used = true;
    s.kind = capture_source_kind::output;
    s.client = 5;
    capture_env env{};
    LUMEN_CHECK(capture_decide(s, env) == capture_action::copy);
    env.held = true; // VT away or powered off
    LUMEN_CHECK(capture_decide(s, env) == capture_action::wait);
    env.held = false;
    env.source_gone = true;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::stop);
    env.source_gone = false;
    s.stopped = true;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::stop);
    s.stopped = false;
    // Locked: the curtain once, then nothing.
    env.locked = true;
    env.locker_client = 9;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::curtain);
    s.curtained = true;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::wait);
    // The lock holder's own output session sees the lock surfaces once the scene is locked.
    s.curtained = false;
    s.client = 9;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::curtain); // not shown yet
    env.lock_scene_shown = true;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::copy);
    env.held = true;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::wait);
    env.held = false;
    // …but never a window: it is behind the lock, even for the locker.
    s.kind = capture_source_kind::toplevel;
    LUMEN_CHECK(capture_decide(s, env) == capture_action::curtain);
}

LUMEN_TEST(damage_ring_unions_since_a_point_and_forgets_past_its_depth_or_a_reset) {
    capture_damage_ring r{};
    lx::gfx::damage_region out{};
    LUMEN_CHECK(r.seq() == 0 && r.since(0, out) && out.empty());
    lx::gfx::damage_region a{};
    a.add({0, 0, 10, 10});
    lx::gfx::damage_region b{};
    b.add({100, 100, 5, 5});
    LUMEN_CHECK(r.record(a) == 1 && r.record(b) == 2);
    LUMEN_CHECK(r.since(2, out) && out.empty());
    LUMEN_CHECK(r.since(1, out) && out.count == 1 && out.rects[0].x == 100);
    LUMEN_CHECK(r.since(0, out) && out.count == 2);
    LUMEN_CHECK(!r.since(5, out)); // the future: another ring
    for (unsigned i = 0; i < capture_damage_ring::k_depth; ++i)
        (void)r.record(a);
    LUMEN_CHECK(!r.since(1, out));                         // older than the ring
    LUMEN_CHECK(r.since(r.seq() - capture_damage_ring::k_depth, out));
    const unsigned at = r.seq();
    r.reset();
    LUMEN_CHECK(!r.since(at, out)); // the content changed in a way no damage describes
    (void)r.record(b);
    LUMEN_CHECK(r.since(r.seq(), out) && out.empty());
    LUMEN_CHECK(!r.since(at, out));
}

LUMEN_TEST(cursor_rect_follows_scale_origin_and_transform) {
    // Scale 1.5, origin (100, 0): the hotspot pixel is floor((x − 100) × 1.5).
    const lx::rect2i r = capture_cursor_rect(110.0, 20.0, {100, 0}, 1.5,
                                             lx::buffer_transform::normal, {1920, 1080}, 32, 32, 4,
                                             6);
    LUMEN_CHECK(r.x == 15 - 4 && r.y == 30 - 6 && r.width == 32 && r.height == 32);
    // rotate_90 of a 1080x1920 portrait mode: the content space is 1920x1080.
    const lx::rect2i t = capture_cursor_rect(10.0, 20.0, {0, 0}, 1.0, lx::buffer_transform::rotate_90,
                                             {1080, 1920}, 8, 8, 0, 0);
    const lx::rect2i expect = lx::gfx::transform_rect({10, 20, 1, 1}, lx::buffer_transform::rotate_90,
                                                      1920, 1080);
    LUMEN_CHECK(t.x == expect.x && t.y == expect.y);
}

LUMEN_TEST(blend_is_premultiplied_over_and_clipped) {
    std::vector<unsigned char> dst(4u * 4u * 4u, 0);
    for (unsigned i = 0; i < 16; ++i) {
        dst[i * 4] = 200; // B
        dst[i * 4 + 1] = 100;
        dst[i * 4 + 2] = 0;
        dst[i * 4 + 3] = 255;
    }
    // A 2x2 image at (3, 3): only its top-left pixel lands. Half-transparent premultiplied
    // red (a = 128, r = 128).
    const std::uint32_t src[4] = {0x80800000u, 0xFF00FF00u, 0xFF0000FFu, 0x00000000u};
    capture_blend_argb(dst.data(), 4, 4, 16, src, 2, 2, 2, 3, 3);
    const unsigned char* p = &dst[(3 * 4 + 3) * 4];
    // r = 128 + 0 × 127/255, g = 0 + 100 × 127/255 ≈ 50, b = 0 + 200 × 127/255 ≈ 100.
    LUMEN_CHECK(p[2] == 128 && p[1] >= 49 && p[1] <= 50 && p[0] >= 99 && p[0] <= 100);
    LUMEN_CHECK(dst[0] == 200 && dst[2] == 0); // untouched elsewhere
    // Opaque pixels replace; fully transparent ones keep the destination.
    capture_blend_argb(dst.data(), 4, 4, 16, src, 2, 2, 2, -1, 0);
    const unsigned char* q = &dst[0];
    LUMEN_CHECK(q[1] == 255 && q[2] == 0 && q[0] == 0); // src(1,0) green at dst(0,0)
    const unsigned char* t = &dst[(1 * 4 + 0) * 4];
    LUMEN_CHECK(t[0] == 200 && t[1] == 100); // src(1,1) transparent
}

LUMEN_TEST(screencopy_table_keeps_per_client_output_points_lru) {
    screencopy_seq_table t{};
    unsigned seq = 0;
    capture_cursor_memo memo{};
    LUMEN_CHECK(!t.get(1, lx::output_id{1}, seq, memo));
    capture_cursor_memo m{};
    m.known = true;
    m.painted = true;
    m.rect = {1, 2, 3, 4};
    t.set(1, lx::output_id{1}, 0, m);
    LUMEN_CHECK(t.get(1, lx::output_id{1}, seq, memo) && seq == 0 && memo.painted &&
                memo.rect.width == 3);
    t.set(1, lx::output_id{1}, 7, {});
    LUMEN_CHECK(t.get(1, lx::output_id{1}, seq, memo) && seq == 7 && !memo.known);
    LUMEN_CHECK(!t.get(1, lx::output_id{2}, seq, memo) && !t.get(2, lx::output_id{1}, seq, memo));
    for (unsigned c = 10; c < 10 + screencopy_seq_table::k_entries; ++c)
        t.set(c, lx::output_id{1}, c, {});
    LUMEN_CHECK(!t.get(1, lx::output_id{1}, seq, memo)); // the oldest was taken over
    LUMEN_CHECK(t.get(10 + screencopy_seq_table::k_entries - 1, lx::output_id{1}, seq, memo));
    t.forget_client(11);
    LUMEN_CHECK(!t.get(11, lx::output_id{1}, seq, memo));
}

// ── render side, CPU backend ─────────────────────────────────────────────────

namespace {

/// A client's shm buffer as the guard sees it.
struct client_buffer {
    std::vector<unsigned char> px;
    unsigned w = 0;
    unsigned h = 0;
    unsigned handle = 0;
    explicit client_buffer(unsigned width, unsigned height, unsigned char fill = 0x5A)
        : px(static_cast<std::size_t>(width) * height * 4u, fill), w(width), h(height) {}
    [[nodiscard]] std::uint32_t at(unsigned x, unsigned y) const {
        const unsigned char* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16);
    }
};

unsigned g_access = 0;
unsigned g_unref = 0;

struct cpu_rig {
    lx::gfx::cpu_compositor cpu{};
    capture_shm_guards guards{};
    capture_channel channel{};
    capture_renderer renderer{};
    std::vector<std::uint32_t> texture;
    lx::gfx::blit_command blit{};
    cpu_rig() {
        capture_shm_guards::hooks h{};
        h.begin_access = [](void*) { ++g_access; };
        h.end_access = [](void*) {};
        h.unref_pool = [](void*) { ++g_unref; };
        guards.bind(h);
        renderer.bind_cpu(&cpu);
        renderer.bind_guards(&guards);
        // A 16x16 opaque blue texture drawn at (8, 8) of a 64x48 output.
        texture.assign(16u * 16u, 0xFF0000FFu);
        lx::gfx::pixel_surface s{};
        s.pixels = reinterpret_cast<unsigned char*>(texture.data());
        s.width = 16;
        s.height = 16;
        s.stride = 64;
        s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
        (void)cpu.register_texture(1, s);
        blit.texture_id = 1;
        blit.dst = {8, 8, 16, 16};
        blit.blend = lx::blend_mode::opaque;
        channel.sessions.store(1);
    }
    [[nodiscard]] capture_frame_source source(unsigned frame) const {
        capture_frame_source src{};
        src.frame = frame;
        src.time_ns = 1000ull * frame;
        src.size = {64, 48};
        src.blits = &blit;
        src.blit_count = 1;
        src.clear = lx::color::rgb(0.f, 0.f, 0.f);
        return src;
    }
    capture_job job(client_buffer& b, unsigned serial, bool full, unsigned since = 0) {
        if (!b.handle)
            b.handle = guards.add(&b, &b, b.px.data(), b.w * 4u, b.w, b.h, true);
        capture_job j{};
        j.kind = capture_job_kind::output;
        j.session = 1;
        j.generation = 1;
        j.serial = serial;
        j.output_slot = 0;
        j.full = full;
        j.since_seq = since;
        j.target.width = b.w;
        j.target.height = b.h;
        j.target.shm = b.handle;
        return j;
    }
    std::vector<capture_completion> drain() {
        renderer.flush(channel);
        capture_completion c[16];
        const unsigned n = channel.drain(c, 16);
        return {c, c + n};
    }
};

lx::gfx::damage_region rect(lx::rect2i r) {
    lx::gfx::damage_region d{};
    d.add(r);
    return d;
}

} // namespace

LUMEN_TEST(channel_and_guards_bound_their_queues_and_never_write_after_cancel) {
    capture_channel ch{};
    capture_job j{};
    unsigned posted = 0;
    while (ch.post(j))
        ++posted;
    LUMEN_CHECK(posted == capture_channel::k_jobs);
    capture_job out[8];
    LUMEN_CHECK(ch.take(out, 8) == 8);
    LUMEN_CHECK(ch.post(j));
    LUMEN_CHECK(!ch.has_completions());
    LUMEN_CHECK(ch.complete({}));
    LUMEN_CHECK(ch.has_completions());
    capture_completion c[4];
    LUMEN_CHECK(ch.drain(c, 4) == 1 && !ch.has_completions());

    capture_shm_guards g{};
    capture_shm_guards::hooks h{};
    h.unref_pool = [](void*) { ++g_unref; };
    g.bind(h);
    unsigned char mem[16]{};
    const unsigned handle = g.add(mem, mem, mem, 16, 4, 1, true);
    bool wrote = g.write(handle, [](unsigned char* p, unsigned, unsigned, unsigned, bool) { p[0] = 7; });
    LUMEN_CHECK(handle && wrote && mem[0] == 7);
    g.cancel(handle);
    wrote = g.write(handle, [](unsigned char* p, unsigned, unsigned, unsigned, bool) { p[0] = 9; });
    LUMEN_CHECK(!wrote && mem[0] == 7);
    const unsigned before = g_unref;
    g.release(handle);
    LUMEN_CHECK(g_unref == before + 1 && g.in_use() == 0);
}

LUMEN_TEST(cpu_first_frame_is_full_and_has_the_frames_pixels) {
    cpu_rig rig{};
    client_buffer b{64, 48};
    LUMEN_CHECK(rig.channel.post(rig.job(b, 1, true)));
    rig.renderer.take(rig.channel);
    // A job with `full` runs without a composite (the newest frame).
    LUMEN_CHECK(rig.renderer.wants_front(0, {}));
    rig.renderer.output_front(0, rig.source(3), rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].result == capture_result::ready && done[0].serial == 1);
    LUMEN_CHECK(done[0].damage.count == 1 && done[0].damage.rects[0].width == 64 &&
                done[0].damage.rects[0].height == 48 && done[0].frame == 3);
    LUMEN_CHECK(done[0].shm == b.handle);
    LUMEN_CHECK(b.at(10, 10) == 0x0000FFu && b.at(0, 0) == 0 && b.at(40, 40) == 0);
}

LUMEN_TEST(cpu_later_frames_wait_for_a_composite_and_copy_its_damage) {
    cpu_rig rig{};
    client_buffer b{64, 48};
    (void)rig.channel.post(rig.job(b, 1, true));
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, rig.source(1), rig.channel);
    auto first = rig.drain();
    const unsigned seq = first[0].seq;
    // The client scribbles over its buffer; the next frame copies only what changed.
    std::memset(b.px.data(), 0x11, b.px.size());
    (void)rig.channel.post(rig.job(b, 2, false, seq));
    rig.renderer.take(rig.channel);
    LUMEN_CHECK(!rig.renderer.wants_front(0, {})); // nothing new: wait
    rig.renderer.output_front(0, rig.source(1), rig.channel);
    LUMEN_CHECK(rig.drain().empty());
    rig.renderer.output_composited(0, rect({8, 8, 4, 4}), rig.source(2), rig.channel);
    auto second = rig.drain();
    LUMEN_CHECK(second.size() == 1 && second[0].result == capture_result::ready);
    LUMEN_CHECK(second[0].damage.count == 1 && second[0].damage.rects[0].x == 8 &&
                second[0].damage.rects[0].width == 4 && second[0].seq == seq + 1);
    LUMEN_CHECK(b.at(9, 9) == 0x0000FFu);   // copied
    LUMEN_CHECK(b.at(20, 20) == 0x111111u); // not touched
}

LUMEN_TEST(cpu_client_damage_widens_the_copy_but_not_the_report) {
    cpu_rig rig{};
    client_buffer b{64, 48};
    (void)rig.channel.post(rig.job(b, 1, true));
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, rig.source(1), rig.channel);
    const unsigned seq = rig.drain()[0].seq;
    std::memset(b.px.data(), 0x11, b.px.size());
    capture_job j = rig.job(b, 2, false, seq);
    j.client_damage = rect({16, 16, 8, 8});
    (void)rig.channel.post(j);
    rig.renderer.take(rig.channel);
    rig.renderer.output_composited(0, rect({0, 0, 2, 2}), rig.source(2), rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].damage.count == 1 &&
                done[0].damage.rects[0].width == 2);
    LUMEN_CHECK(b.at(20, 20) == 0x0000FFu); // inside the client's damage: copied
    LUMEN_CHECK(b.at(40, 40) == 0x111111u);
}

LUMEN_TEST(cpu_crop_copies_a_region_to_the_buffer_origin_and_waits_for_change_inside) {
    cpu_rig rig{};
    client_buffer b{10, 10};
    capture_job j = rig.job(b, 1, true);
    j.crop = {6, 6, 10, 10};
    (void)rig.channel.post(j);
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, rig.source(1), rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].result == capture_result::ready);
    LUMEN_CHECK(b.at(0, 0) == 0 && b.at(2, 2) == 0x0000FFu && b.at(9, 9) == 0x0000FFu);
    LUMEN_CHECK(done[0].damage.count == 1 && done[0].damage.rects[0].x == 0 &&
                done[0].damage.rects[0].width == 10);
    // A change outside the crop does not make a frame; one inside does.
    capture_job k = rig.job(b, 2, false, done[0].seq);
    k.crop = {6, 6, 10, 10};
    (void)rig.channel.post(k);
    rig.renderer.take(rig.channel);
    rig.renderer.output_composited(0, rect({40, 40, 4, 4}), rig.source(2), rig.channel);
    LUMEN_CHECK(rig.drain().empty());
    rig.renderer.output_composited(0, rect({7, 7, 2, 2}), rig.source(3), rig.channel);
    auto later = rig.drain();
    LUMEN_CHECK(later.size() == 1 && later[0].damage.count == 1 &&
                later[0].damage.rects[0].x == 1 && later[0].damage.rects[0].width == 2);
}

LUMEN_TEST(cpu_a_buffer_that_no_longer_fits_fails_with_buffer_constraints) {
    cpu_rig rig{};
    client_buffer b{32, 32};
    (void)rig.channel.post(rig.job(b, 1, true));
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, rig.source(1), rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].result == capture_result::failed &&
                done[0].reason == capture_failure::buffer_constraints);
}

LUMEN_TEST(cpu_paint_cursors_blends_the_pointer_and_damages_its_moves) {
    cpu_rig rig{};
    client_buffer b{64, 48};
    std::vector<std::uint32_t> arrow(4u * 4u, 0xFFFFFFFFu);
    capture_cursor cur{};
    cur.pixels = arrow.data();
    cur.width = 4;
    cur.height = 4;
    cur.stride_px = 4;
    cur.visible = true;
    cur.generation = 1;
    cur.rect = {40, 30, 4, 4};
    capture_job j = rig.job(b, 1, true);
    j.paint_cursors = true;
    (void)rig.channel.post(j);
    rig.renderer.take(rig.channel);
    capture_frame_source src = rig.source(1);
    src.cursor = cur;
    rig.renderer.output_front(0, src, rig.channel);
    auto first = rig.drain();
    LUMEN_CHECK(first.size() == 1 && b.at(41, 31) == 0xFFFFFFu && first[0].cursor.painted);
    // The pointer moves: a frame without new content, damage = old ∪ new place.
    capture_job k = rig.job(b, 2, false, first[0].seq);
    k.paint_cursors = true;
    k.cursor = first[0].cursor;
    (void)rig.channel.post(k);
    rig.renderer.take(rig.channel);
    LUMEN_CHECK(!rig.renderer.wants_front(0, cur)); // still: nothing to do
    cur.rect = {50, 30, 4, 4};
    LUMEN_CHECK(rig.renderer.wants_front(0, cur));
    src.cursor = cur;
    rig.renderer.output_front(0, src, rig.channel);
    auto second = rig.drain();
    LUMEN_CHECK(second.size() == 1 && second[0].damage.count >= 1);
    long area = 0;
    for (unsigned i = 0; i < second[0].damage.count; ++i)
        area += static_cast<long>(second[0].damage.rects[i].width) * second[0].damage.rects[i].height;
    LUMEN_CHECK(area >= 32 && area <= 64);
    LUMEN_CHECK(b.at(41, 31) == 0 && b.at(51, 31) == 0xFFFFFFu);
    // Without paint_cursors the pointer is never painted.
    client_buffer c{64, 48};
    capture_job m = rig.job(c, 3, true);
    m.session = 2;
    (void)rig.channel.post(m);
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, src, rig.channel);
    (void)rig.drain();
    LUMEN_CHECK(c.at(51, 31) == 0);
}

LUMEN_TEST(cpu_software_pointer_is_left_out_without_paint_cursors) {
    cpu_rig rig{};
    // The scene's pointer draw (the hidden texture) over the frame.
    std::vector<std::uint32_t> white(4u * 4u, 0xFFFFFFFFu);
    lx::gfx::pixel_surface s{};
    s.pixels = reinterpret_cast<unsigned char*>(white.data());
    s.width = 4;
    s.height = 4;
    s.stride = 16;
    s.format = static_cast<lx::fourcc>(lx::pixel_format::argb8888);
    (void)rig.cpu.register_texture(k_capture_hidden_texture, s);
    lx::gfx::blit_command draws[2] = {rig.blit, {}};
    draws[1].texture_id = k_capture_hidden_texture;
    draws[1].dst = {30, 30, 4, 4};
    capture_frame_source src = rig.source(1);
    src.blits = draws;
    src.blit_count = 2;
    src.cursor.visible = true;
    src.cursor.in_frame = true;
    client_buffer without{64, 48};
    (void)rig.channel.post(rig.job(without, 1, true));
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, src, rig.channel);
    (void)rig.drain();
    LUMEN_CHECK(without.at(31, 31) == 0 && without.at(10, 10) == 0x0000FFu);
    client_buffer with{64, 48};
    capture_job j = rig.job(with, 2, true);
    j.session = 2;
    j.paint_cursors = true;
    (void)rig.channel.post(j);
    rig.renderer.take(rig.channel);
    rig.renderer.output_front(0, src, rig.channel);
    (void)rig.drain();
    LUMEN_CHECK(with.at(31, 31) == 0xFFFFFFu);
}

LUMEN_TEST(lock_epoch_cancels_content_jobs_and_curtains_run_at_once) {
    cpu_rig rig{};
    client_buffer b{64, 48};
    capture_job j = rig.job(b, 1, false, 0);
    j.lock_epoch = 0;
    (void)rig.channel.post(j);
    rig.renderer.take(rig.channel);
    rig.channel.lock_epoch.store(1); // a lock request
    rig.renderer.output_composited(0, rect({0, 0, 4, 4}), rig.source(2), rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].result == capture_result::cancelled &&
                done[0].shm == b.handle);
    capture_job c = rig.job(b, 2, true);
    c.kind = capture_job_kind::curtain;
    (void)rig.channel.post(c);
    rig.renderer.take(rig.channel);
    rig.renderer.run_curtains(rig.channel);
    auto curtain = rig.drain();
    LUMEN_CHECK(curtain.size() == 1 && curtain[0].result == capture_result::ready &&
                curtain[0].damage.full);
    bool black = true;
    for (unsigned y = 0; y < 48; y += 7)
        for (unsigned x = 0; x < 64; x += 5)
            black = black && b.at(x, y) == 0;
    LUMEN_CHECK(black);
}

LUMEN_TEST(cpu_window_view_composites_its_draws_and_reports_its_damage) {
    cpu_rig rig{};
    client_buffer b{32, 32};
    capture_job j = rig.job(b, 1, true);
    j.kind = capture_job_kind::toplevel;
    j.view = 16;
    j.view_frame = 5;
    (void)rig.channel.post(j);
    rig.renderer.take(rig.channel);
    capture_toplevel_job jobs[4];
    LUMEN_CHECK(rig.renderer.toplevel_jobs(jobs, 4) == 1 && jobs[0].view == 16 &&
                jobs[0].frame == 5 && jobs[0].size.width == 32);
    lx::gfx::blit_command at_origin = rig.blit;
    at_origin.dst = {0, 0, 16, 16};
    capture_frame_source src{};
    src.frame = 4; // an older publish of the view: not this job's yet
    src.blits = &at_origin;
    src.blit_count = 1;
    lx::gfx::damage_region all{};
    all.set_full();
    rig.renderer.toplevel_composited(16, all, src, rig.channel);
    LUMEN_CHECK(rig.drain().empty());
    src.frame = 5;
    rig.renderer.toplevel_composited(16, all, src, rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(done.size() == 1 && done[0].result == capture_result::ready);
    LUMEN_CHECK(b.at(4, 4) == 0x0000FFu && b.at(20, 20) == 0);
}

LUMEN_TEST(forgetting_a_session_drops_its_job_and_lets_go_of_its_guard) {
    cpu_rig rig{};
    client_buffer b{64, 48};
    (void)rig.channel.post(rig.job(b, 1, false, 0));
    rig.renderer.take(rig.channel);
    LUMEN_CHECK(rig.renderer.has_pending());
    capture_job f{};
    f.kind = capture_job_kind::forget_session;
    f.session = 1;
    f.generation = 1;
    (void)rig.channel.post(f);
    rig.renderer.take(rig.channel);
    auto done = rig.drain();
    LUMEN_CHECK(!rig.renderer.has_pending() && done.size() == 1 &&
                done[0].result == capture_result::cancelled && done[0].shm == b.handle);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
