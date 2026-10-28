#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;
import lx.scene;
import lx.runtime;

LUMEN_TEST(snapshot_publish_acquire_basic) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    lx::scene::snapshot_buffer buf{};
    auto lease = buf.begin_frame();
    LUMEN_CHECK(lease.valid());
    lease.draws().clear();
    lease.draws().push(lx::scene::draw_command{});
    LUMEN_CHECK(buf.publish(lease, {}, 1));
    // Publishing consumes the lease, so a second publish cannot double-count the frame.
    LUMEN_CHECK(!lease.valid());
    LUMEN_CHECK(!buf.publish(lease, {}, 1));
    LUMEN_CHECK(buf.publish_count() == 1);

    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = buf.acquire();
    LUMEN_CHECK(snap.frame_index() == 1);
    LUMEN_CHECK(snap.draws().size() == 1);
    buf.release(snap);
    LUMEN_CHECK(buf.in_flight_count() == 0);
}

LUMEN_TEST(snapshot_sequential_publish_matches_content) {
    lx::scene::snapshot_buffer buf{};

    for (unsigned frame = 1; frame <= 16; ++frame) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        auto lease = buf.begin_frame();
        if (!lease.valid())
            continue;
        lease.draws().clear();
        for (unsigned i = 0; i < frame; ++i)
            lease.draws().push(lx::scene::draw_command{});

        if (!buf.publish(lease, {}, frame))
            continue;

        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        const auto& snap = buf.acquire();
        LUMEN_CHECK(snap.frame_index() == frame);
        LUMEN_CHECK(snap.draws().size() == frame);
        buf.release(snap);
    }
}

LUMEN_TEST(snapshot_reader_lease_survives_publish_storm) {
    lx::scene::snapshot_buffer buf{};

    // Publish first frame so there is something to acquire.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto first = buf.begin_frame();
    LUMEN_CHECK(first.valid());
    first.draws().clear();
    LUMEN_CHECK(buf.publish(first, {}, 1));

    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = buf.acquire();
    const unsigned held = snap.frame_index();
    LUMEN_CHECK(held == 1);

    // UI publishes many more frames while render holds the lease.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    for (unsigned i = 2; i < 64; ++i) {
        auto lease = buf.begin_frame();
        if (!lease.valid())
            continue;
        // Must never hand back the slot the reader is holding.
        LUMEN_CHECK(&lease.draws() != &snap.draws());
        lease.draws().clear();
        (void)buf.publish(lease, {}, i);
    }

    // Held snapshot must still be intact.
    LUMEN_CHECK(snap.frame_index() == held);
    buf.release(snap);
}

// Regression for the fallback that used to hand a reader-held slot back to the writer.
// With every non-published slot leased out, begin_frame must refuse rather than return
// a live slot.
LUMEN_TEST(snapshot_refuses_when_every_slot_is_leased) {
    lx::scene::snapshot_buffer buf{
        {.backpressure = lx::scene::backpressure_policy::triple_buffer, .max_in_flight = 8}};

    // A reader can only lease whichever slot is currently published, so walk publish →
    // acquire once per slot until every slot is held.
    const lx::scene::immutable_frame_snapshot* held[lx::scene::k_snapshot_slot_count]{};
    for (unsigned i = 0; i < lx::scene::k_snapshot_slot_count; ++i) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        auto lease = buf.begin_frame();
        LUMEN_CHECK(lease.valid());
        lease.draws().clear();
        LUMEN_CHECK(buf.publish(lease, {}, i + 1));

        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        held[i] = &buf.acquire();
        for (unsigned j = 0; j < i; ++j)
            LUMEN_CHECK(held[j] != held[i]); // each iteration leases a distinct slot
    }

    // Every slot is now reader-held. The old fallback returned a live slot here.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto starved = buf.begin_frame();
    LUMEN_CHECK(!starved.valid());
    LUMEN_CHECK(buf.dropped_count() >= 1);

    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    for (auto* h : held)
        buf.release(*h);

    // Once the readers let go, the producer makes progress again.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto recovered = buf.begin_frame();
    LUMEN_CHECK(recovered.valid());
    recovered.draws().clear();
    LUMEN_CHECK(buf.publish(recovered, {}, 3));
}

// drop_latest and triple_buffer must not be two names for the same behavior: with
// max_in_flight exhausted, drop_latest refuses and triple_buffer keeps going.
LUMEN_TEST(snapshot_backpressure_policies_differ) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    lx::scene::snapshot_buffer dropping{
        {.backpressure = lx::scene::backpressure_policy::drop_latest, .max_in_flight = 1}};
    auto d1 = dropping.begin_frame();
    LUMEN_CHECK(d1.valid());
    d1.draws().clear();
    LUMEN_CHECK(dropping.publish(d1, {}, 1));
    LUMEN_CHECK(dropping.in_flight_count() == 1);

    // In-flight budget is spent and nothing released it — drop_latest refuses up front.
    LUMEN_CHECK(dropping.would_stall());
    auto d2 = dropping.begin_frame();
    LUMEN_CHECK(!d2.valid());
    LUMEN_CHECK(dropping.dropped_count() == 1);
    LUMEN_CHECK(dropping.publish_count() == 1);

    lx::scene::snapshot_buffer buffering{
        {.backpressure = lx::scene::backpressure_policy::triple_buffer, .max_in_flight = 1}};
    auto t1 = buffering.begin_frame();
    LUMEN_CHECK(t1.valid());
    t1.draws().clear();
    LUMEN_CHECK(buffering.publish(t1, {}, 1));

    // Same in-flight count, but triple_buffer admits on slot availability instead.
    LUMEN_CHECK(!buffering.would_stall());
    auto t2 = buffering.begin_frame();
    LUMEN_CHECK(t2.valid());
    t2.draws().clear();
    LUMEN_CHECK(buffering.publish(t2, {}, 2));
    LUMEN_CHECK(buffering.publish_count() == 2);
    LUMEN_CHECK(buffering.dropped_count() == 0);
}

// batch_for_render applies its sort as an in-place cycle permutation over a scratch index
// array. Getting a cycle walk subtly wrong duplicates or drops commands rather than just
// misordering them, so check identity preservation as well as order.
LUMEN_TEST(batch_for_render_permutes_without_losing_commands) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    lx::scene::snapshot_buffer buf{};
    auto lease = buf.begin_frame();
    LUMEN_CHECK(lease.valid());
    auto& draws = lease.draws();
    draws.clear();

    // Interleave opaque and translucent, with z-order deliberately out of sequence so the
    // sort has real work to do and the permutation contains multi-element cycles.
    constexpr unsigned k_count = 64;
    for (unsigned i = 0; i < k_count; ++i) {
        lx::scene::draw_command cmd{};
        cmd.texture = lx::texture_id{i + 1}; // identity tag, survives the permutation
        cmd.blend = (i % 3 == 0) ? lx::blend_mode::opaque : lx::blend_mode::premultiplied;
        cmd.sort_key = lx::draw_sort_key::make(0, (k_count * 7u - i * 5u) % k_count);
        draws.push(cmd);
    }
    LUMEN_CHECK(draws.size() == k_count);

    draws.batch_for_render();
    LUMEN_CHECK(draws.size() == k_count);

    // Every input command appears exactly once — no duplicates, no drops.
    bool seen[k_count + 1]{};
    for (unsigned i = 0; i < draws.size(); ++i) {
        const auto id = draws.data()[i].texture.id();
        LUMEN_CHECK(id >= 1 && id <= k_count);
        LUMEN_CHECK(!seen[id]);
        seen[id] = true;
    }
    for (unsigned i = 1; i <= k_count; ++i)
        LUMEN_CHECK(seen[i]);

    // Painter's order throughout, regardless of blend mode: back-to-front, no partition.
    for (unsigned i = 1; i < draws.size(); ++i) {
        LUMEN_CHECK(draws.data()[i - 1].sort_key.value <= draws.data()[i].sort_key.value);
    }
}

// Regression: batch_for_render used to hoist every opaque draw ahead of every translucent
// one. Without a depth buffer that inverts z — a translucent surface behind an opaque one
// ends up painted last, blending over the window that should cover it. `surface_node::emit`
// marks anything at full opacity as opaque, so this was an ordinary two-window stack.
LUMEN_TEST(batch_for_render_keeps_translucent_behind_opaque_behind) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    lx::scene::snapshot_buffer buf{};
    auto lease = buf.begin_frame();
    LUMEN_CHECK(lease.valid());
    auto& draws = lease.draws();
    draws.clear();

    // Emission order is paint order: translucent window behind, opaque window in front.
    lx::scene::draw_command behind{};
    behind.texture = lx::texture_id{1};
    behind.blend = lx::blend_mode::premultiplied;
    behind.opacity = 0.5f;
    behind.sort_key = lx::draw_sort_key::make(0, 0);
    draws.push(behind);

    lx::scene::draw_command front{};
    front.texture = lx::texture_id{2};
    front.blend = lx::blend_mode::opaque;
    front.sort_key = lx::draw_sort_key::make(0, 1);
    draws.push(front);

    draws.batch_for_render();

    LUMEN_CHECK(draws.size() == 2);
    // The opaque window must still be painted last, so it covers the one behind it.
    LUMEN_CHECK(draws.data()[0].texture.id() == 1);
    LUMEN_CHECK(draws.data()[1].texture.id() == 2);
}

// A sort that is already in order must be a no-op, and a full reversal is the worst case
// for the cycle walk — one long cycle rather than many short ones.
LUMEN_TEST(batch_for_render_handles_identity_and_reversal) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    for (bool reversed : {false, true}) {
        lx::scene::snapshot_buffer buf{};
        auto lease = buf.begin_frame();
        LUMEN_CHECK(lease.valid());
        auto& draws = lease.draws();
        draws.clear();

        constexpr unsigned k_count = 32;
        for (unsigned i = 0; i < k_count; ++i) {
            lx::scene::draw_command cmd{};
            cmd.texture = lx::texture_id{i + 1};
            cmd.blend = lx::blend_mode::premultiplied;
            // Translucent sorts ascending by z-order, so descending input reverses.
            cmd.sort_key = lx::draw_sort_key::make(0, reversed ? (k_count - i) : i);
            draws.push(cmd);
        }

        draws.batch_for_render();
        LUMEN_CHECK(draws.size() == k_count);

        for (unsigned i = 0; i < k_count; ++i) {
            const auto expected = reversed ? (k_count - i) : (i + 1);
            LUMEN_CHECK(draws.data()[i].texture.id() == expected);
        }
    }
}

// The publisher encodes the frame number in the draw count, so the reader can
// assert the published slot really holds the frame it claims — that catches
// publishing a slot the UI thread never wrote, not just torn reads.
LUMEN_TEST(snapshot_tsan_stress) {
    constexpr unsigned k_period = 200;
    lx::scene::snapshot_buffer buf{};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> reader_tears{0};
    std::atomic<unsigned> content_mismatches{0};

    std::thread publisher([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        unsigned frame = 1;
        while (!stop.load(std::memory_order_acquire)) {
            auto lease = buf.begin_frame();
            if (lease.valid()) {
                lease.draws().clear();
                for (unsigned i = 0; i <= frame % k_period; ++i)
                    lease.draws().push(lx::scene::draw_command{});
                (void)buf.publish(lease, {}, frame);
            }
            ++frame;
            std::this_thread::yield();
        }
    });

    std::thread reader([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        for (int i = 0; i < 5000; ++i) {
            const auto& snap = buf.acquire();
            const unsigned idx = snap.frame_index();
            const unsigned size_a = snap.draws().size();
            // frame_index 0 is the initial slot, before the publisher's first frame.
            if (idx != 0 && size_a != idx % k_period + 1)
                content_mismatches.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            const unsigned size_b = snap.draws().size();
            if (size_a != size_b || snap.frame_index() != idx)
                reader_tears.fetch_add(1, std::memory_order_relaxed);
            buf.release(snap);
        }
        stop.store(true, std::memory_order_release);
    });

    reader.join();
    publisher.join();
    LUMEN_CHECK(reader_tears.load() == 0);
    LUMEN_CHECK(content_mismatches.load() == 0);
}

// ── The visibility pass (`mark_occlusion`, run by `scene_graph::commit_frame`) ──

namespace {

lx::scene::draw_command opaque_draw(lx::rect2i dst) {
    lx::scene::draw_command d{};
    d.dst = dst;
    d.transform = lx::transform2d::translate(dst.x, dst.y);
    d.blend = lx::blend_mode::opaque;
    return d;
}

lx::scene::draw_command translucent_draw(lx::rect2i dst) {
    lx::scene::draw_command d = opaque_draw(dst);
    d.blend = lx::blend_mode::premultiplied;
    return d;
}

} // namespace

LUMEN_TEST(occlusion_opaque_cover_hides_what_is_below) {
    // Paint order: bottom first.
    lx::scene::draw_command d[3] = {opaque_draw({10, 10, 50, 50}),      // fully covered
                                    opaque_draw({100, 100, 50, 50}),    // partly covered
                                    opaque_draw({0, 0, 120, 120})};     // on top
    bool shown[3]{};
    const unsigned marked = lx::scene::mark_occlusion(d, {0, 0, 640, 480}, shown);
    LUMEN_CHECK(marked == 1);
    LUMEN_CHECK(d[0].occluded && !shown[0]);
    LUMEN_CHECK(!d[1].occluded && shown[1]);
    LUMEN_CHECK(!d[2].occluded && shown[2]);
}

LUMEN_TEST(occlusion_translucent_never_occludes) {
    lx::scene::draw_command d[4] = {opaque_draw({10, 10, 50, 50}), translucent_draw({0, 0, 200, 200}),
                                    opaque_draw({300, 0, 50, 50}), opaque_draw({0, 0, 400, 400})};
    d[3].opacity = 0.5f; // a fading window
    lx::scene::draw_command tinted = opaque_draw({300, 0, 100, 100});
    tinted.tint = lx::color::rgb(1.f, 1.f, 1.f, 0.9f);
    lx::scene::draw_command scaled = opaque_draw({300, 0, 100, 100});
    scaled.transform = lx::transform2d::scale(2.f, 2.f);
    LUMEN_CHECK(lx::scene::mark_occlusion(d, {0, 0, 640, 480}, nullptr) == 0);
    lx::scene::draw_command e[2] = {opaque_draw({310, 10, 10, 10}), tinted};
    LUMEN_CHECK(lx::scene::mark_occlusion(e, {0, 0, 640, 480}, nullptr) == 0);
    lx::scene::draw_command f[2] = {opaque_draw({310, 10, 10, 10}), scaled};
    LUMEN_CHECK(lx::scene::mark_occlusion(f, {0, 0, 640, 480}, nullptr) == 0);
}

LUMEN_TEST(occlusion_off_output_and_clip) {
    lx::scene::draw_command d[3] = {opaque_draw({700, 10, 50, 50}),   // off the output
                                    opaque_draw({10, 10, 50, 50}),    // under the clipped top
                                    opaque_draw({0, 0, 200, 200})};
    // The top draw is clipped to its left half: it covers x < 30 only.
    d[2].clip = {0, 0, 30, 200};
    bool shown[3]{};
    LUMEN_CHECK(lx::scene::mark_occlusion(d, {0, 0, 640, 480}, shown) == 1);
    LUMEN_CHECK(d[0].occluded && !shown[0]);
    LUMEN_CHECK(!d[1].occluded && shown[1]);
    // The part of a draw past the output does not count against covering it.
    lx::scene::draw_command e[2] = {opaque_draw({600, 400, 80, 80}), opaque_draw({590, 390, 50, 90})};
    LUMEN_CHECK(lx::scene::mark_occlusion(e, {0, 0, 640, 480}, nullptr) == 1);
    LUMEN_CHECK(e[0].occluded);
    // Unbounded (no output size): nothing is off-screen.
    lx::scene::draw_command g[1] = {opaque_draw({5000, 5000, 10, 10})};
    LUMEN_CHECK(lx::scene::mark_occlusion(g, {}, nullptr) == 0);
}

LUMEN_TEST(occlusion_keeps_the_eight_largest_occluders) {
    // Paint order: the target, a small cover of it, then eight large disjoint opaque draws.
    // Walking top-down, the eight large ones fill the set first; the small cover is smaller
    // than all of them and is not kept, so the target counts as visible (conservative).
    lx::scene::draw_command d[10]{};
    d[0] = opaque_draw({1000, 1000, 10, 10});
    d[1] = opaque_draw({995, 995, 20, 20});
    for (int i = 0; i < 8; ++i)
        d[2 + i] = opaque_draw({i * 110, 0, 100, 100});
    LUMEN_CHECK(lx::scene::mark_occlusion(d, {}, nullptr) == 0);
    LUMEN_CHECK(!d[0].occluded);
    // A cover larger than the smallest kept one replaces it, and hides the target.
    d[1] = opaque_draw({900, 900, 200, 200});
    LUMEN_CHECK(lx::scene::mark_occlusion(d, {}, nullptr) == 1);
    LUMEN_CHECK(d[0].occluded);
}

LUMEN_TEST(occlusion_commit_frame_marks_surface_visibility) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_projection({{0, 0}, 1.0, {640, 480}});
    lx::gfx::imported_image img{};
    img.width = 100;
    img.height = 100;
    lx::scene::surface_node below{lx::surface_id{1}};
    img.image_id = 11;
    below.attach(img);
    below.set_bounds({0, 0, 100, 100});
    below.set_transform({20, 20});
    below.set_opaque(true);
    lx::scene::surface_node above{lx::surface_id{2}};
    img.image_id = 12;
    above.attach(img);
    above.set_bounds({0, 0, 100, 100});
    above.set_transform({20, 20});
    above.set_opaque(true);
    lx::scene::surface_node away{lx::surface_id{3}};
    img.image_id = 13;
    away.attach(img);
    away.set_bounds({0, 0, 100, 100});
    away.set_transform({900, 20}); // off the 640-wide output
    LUMEN_CHECK(scene.root().add(&below) && scene.root().add(&above) && scene.root().add(&away));
    LUMEN_CHECK(scene.commit_frame(5));
    LUMEN_CHECK(above.visible_in(5) && !below.visible_in(5) && !away.visible_in(5));
    // The covered draw is marked; the one off the output is not even emitted (a view's
    // draw list holds only what can reach its pixels).
    LUMEN_CHECK(scene.last_culled() == 1);
    // The top one moves aside: the one below is on screen in the next frame.
    above.set_transform({300, 20});
    LUMEN_CHECK(scene.commit_frame(6));
    LUMEN_CHECK(above.visible_in(6) && below.visible_in(6) && !above.visible_in(5));
    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = scene.acquire_render_snapshot();
    unsigned occluded = 0;
    for (unsigned i = 0; i < snap.draws().size(); ++i)
        occluded += snap.draws().data()[i].occluded ? 1u : 0u;
    LUMEN_CHECK(snap.frame_index() == 6 && occluded == 0 && snap.draws().size() == 2);
    scene.release_render_frame(snap);
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    (void)scene.root().remove(&below);
    (void)scene.root().remove(&above);
    (void)scene.root().remove(&away);
}

// ── Visible rects (a draw partly covered by opaque draws above it) ──

namespace {

[[nodiscard]] bool contains(lx::rect2i r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

/// Checks every pixel of `bounds` against draw `i` of `d` after the pass: inside at most one
/// of its visible rects (disjoint), never outside its clipped rect, and inside one whenever
/// no opaque draw above covers it — what shows is always drawn. Returns the pixels drawn.
[[nodiscard]] long long check_visible(const lx::scene::draw_command* d, unsigned count,
                                      unsigned i, lx::rect2i bounds, bool& ok) {
    const lx::scene::draw_command& c = d[i];
    long long drawn = 0;
    for (int y = bounds.y; y < bounds.y + bounds.height; ++y)
        for (int x = bounds.x; x < bounds.x + bounds.width; ++x) {
            const bool in_draw = contains(c.dst, x, y);
            bool covered = false;
            for (unsigned k = i + 1; k < count; ++k)
                covered = covered || (d[k].blend == lx::blend_mode::opaque && contains(d[k].dst, x, y));
            unsigned hits = 0;
            for (unsigned v = 0; v < c.visible_count; ++v)
                hits += contains(c.visible[v], x, y) ? 1u : 0u;
            const bool drawn_here =
                !c.occluded && in_draw && (c.visible_count == 0 || hits == 1);
            if (hits > 1 || (hits > 0 && !in_draw) || (in_draw && !covered && !drawn_here))
                ok = false;
            drawn += drawn_here ? 1 : 0;
        }
    return drawn;
}

} // namespace

LUMEN_TEST(visible_rects_confine_a_partly_covered_draw_to_what_shows) {
    // A wallpaper under two overlapping opaque windows, and a translucent window between
    // them: the wallpaper keeps the four bands around the larger window (the smaller one
    // would split it into more than four rects, so it is not subtracted).
    lx::scene::draw_command d[4] = {opaque_draw({0, 0, 64, 48}), translucent_draw({4, 4, 30, 20}),
                                    opaque_draw({10, 10, 30, 24}), opaque_draw({44, 30, 12, 10})};
    bool shown[4]{};
    LUMEN_CHECK(lx::scene::mark_occlusion(d, {0, 0, 64, 48}, shown) == 0);
    LUMEN_CHECK(d[0].visible_count == 4 && d[1].visible_count == 2);
    LUMEN_CHECK(d[2].visible_count == 0 && d[3].visible_count == 0);
    bool ok = true;
    long long drawn = 0;
    for (unsigned i = 0; i < 4; ++i)
        drawn += check_visible(d, 4, i, {0, 0, 64, 48}, ok);
    LUMEN_CHECK(ok);
    // Wallpaper 64x48 - 30x24 = 2352, the translucent one 600 - 24x14 = 264, the windows
    // 720 and 120 — against 4512 drawn whole.
    LUMEN_CHECK(drawn == 2352 + 264 + 720 + 120);

    // Two tiled halves cover the wallpaper between them: occluded, though neither does alone.
    lx::scene::draw_command t[3] = {opaque_draw({0, 0, 64, 48}), opaque_draw({0, 0, 32, 48}),
                                    opaque_draw({32, 0, 32, 48})};
    LUMEN_CHECK(lx::scene::mark_occlusion(t, {0, 0, 64, 48}, shown) == 1);
    LUMEN_CHECK(t[0].occluded && !shown[0] && t[0].visible_count == 0);

    // A cut that narrows rows by less than a quarter of their width is not made (it costs
    // the CPU path more than it saves); one that spans the rows, or hides a quarter, is.
    lx::scene::draw_command m[2] = {opaque_draw({0, 0, 64, 48}), opaque_draw({20, 10, 15, 20})};
    (void)lx::scene::mark_occlusion(m, {0, 0, 64, 48}, nullptr);
    LUMEN_CHECK(!m[0].occluded && m[0].visible_count == 0);
    m[1].dst = {20, 10, 16, 20};
    (void)lx::scene::mark_occlusion(m, {0, 0, 64, 48}, nullptr);
    LUMEN_CHECK(m[0].visible_count == 4);
    m[1].dst = {0, 10, 64, 2};
    (void)lx::scene::mark_occlusion(m, {0, 0, 64, 48}, nullptr);
    LUMEN_CHECK(m[0].visible_count == 2);

    // A pass run again on the same draws starts from scratch: nothing above, nothing cut.
    d[2].dst = {100, 100, 4, 4};
    d[3].dst = {100, 100, 4, 4};
    (void)lx::scene::mark_occlusion(d, {0, 0, 64, 48}, nullptr);
    LUMEN_CHECK(d[0].visible_count == 0 && d[1].visible_count == 0);
}

LUMEN_TEST(visible_rects_hold_for_random_stacks) {
    // Random stacks of opaque and translucent draws, some reaching past the output: every
    // pixel that shows is drawn exactly once, nothing outside a draw is drawn, and the pass
    // never draws more than drawing each draw whole would.
    std::uint32_t seed = 12345u;
    const auto next = [&](int n) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<int>((seed >> 8) % static_cast<std::uint32_t>(n));
    };
    const lx::rect2i output{0, 0, 48, 40};
    for (int round = 0; round < 200; ++round) {
        lx::scene::draw_command d[7]{};
        const unsigned count = 2 + static_cast<unsigned>(next(6));
        for (unsigned i = 0; i < count; ++i) {
            const lx::rect2i r{next(56) - 4, next(48) - 4, 1 + next(40), 1 + next(32)};
            d[i] = next(3) == 0 ? translucent_draw(r) : opaque_draw(r);
        }
        (void)lx::scene::mark_occlusion({d, count}, output, nullptr);
        bool ok = true;
        for (unsigned i = 0; i < count; ++i) {
            // Off the output nothing is checked against cover (no draw can be seen there).
            lx::rect2i inside = d[i].dst;
            const int x0 = std::max(inside.x, 0);
            const int y0 = std::max(inside.y, 0);
            const int x1 = std::min(inside.x + inside.width, output.width);
            const int y1 = std::min(inside.y + inside.height, output.height);
            if (x1 <= x0 || y1 <= y0)
                continue;
            (void)check_visible(d, count, i, {x0, y0, x1 - x0, y1 - y0}, ok);
            for (unsigned v = 0; v < d[i].visible_count; ++v) {
                const lx::rect2i& r = d[i].visible[v];
                ok = ok && r.width > 0 && r.height > 0 && r.x >= x0 && r.y >= y0 &&
                     r.x + r.width <= x1 && r.y + r.height <= y1;
            }
        }
        LUMEN_CHECK(ok);
    }
}

// ── Partial opaque regions (a CSD window's body inside its shadow and rounded corners) ──

LUMEN_TEST(occlusion_partial_opaque_rects_hide_only_what_lies_under_them) {
    // On top, a blended 400x300 window: shadow 24 px, corners 12 px, its body opaque.
    lx::scene::draw_command top = translucent_draw({0, 0, 400, 300});
    top.opaque_rects[0] = {24, 36, 352, 228}; // the body band
    top.opaque_rects[1] = {36, 24, 328, 12};  // the top strip between the corners
    top.opaque_rect_count = 2;
    lx::scene::draw_command d[4] = {opaque_draw({100, 100, 100, 100}), // under the body
                                    opaque_draw({10, 10, 100, 100}),   // into the shadow
                                    opaque_draw({30, 30, 40, 40}),     // into a corner
                                    top};
    bool shown[4]{};
    LUMEN_CHECK(lx::scene::mark_occlusion(d, {0, 0, 640, 480}, shown) == 1);
    LUMEN_CHECK(d[0].occluded && !shown[0]);
    LUMEN_CHECK(!d[1].occluded && shown[1]);
    LUMEN_CHECK(!d[2].occluded && shown[2]);
    LUMEN_CHECK(!d[3].occluded && shown[3]); // the window itself still draws, blended
    // Not at full alpha, or not placed by translation: the rects hide nothing.
    lx::scene::draw_command e[2] = {opaque_draw({100, 100, 100, 100}), top};
    e[1].opacity = 0.5f;
    LUMEN_CHECK(lx::scene::mark_occlusion(e, {0, 0, 640, 480}, nullptr) == 0);
    e[1] = top;
    e[1].tint = lx::color::rgb(1.f, 1.f, 1.f, 0.9f);
    LUMEN_CHECK(lx::scene::mark_occlusion(e, {0, 0, 640, 480}, nullptr) == 0);
    e[1] = top;
    e[1].transform = lx::transform2d::scale(2.f, 2.f);
    LUMEN_CHECK(lx::scene::mark_occlusion(e, {0, 0, 640, 480}, nullptr) == 0);
    // Clipped like the draw: a clip that cuts the body cuts the rect it seeds.
    e[1] = top;
    e[1].clip = {0, 0, 150, 300};
    LUMEN_CHECK(lx::scene::mark_occlusion(e, {0, 0, 640, 480}, nullptr) == 0);
}

namespace {

/// A 400x300 ARGB surface node at `at`, image 1:1 (`image_px` otherwise), its opaque
/// region the body band and the top strip of a window with a 24 px shadow, 12 px corners.
void csd_node(lx::scene::surface_node& n, unsigned id, lx::point2i at, lx::size2i image_px) {
    lx::gfx::imported_image img{};
    img.image_id = id;
    img.width = static_cast<unsigned>(image_px.width);
    img.height = static_cast<unsigned>(image_px.height);
    n.attach(img);
    n.set_bounds({0, 0, 400, 300});
    n.set_transform(at);
    const lx::rect2i rects[2] = {{24, 36, 352, 228}, {36, 24, 328, 12}};
    n.set_opaque_rects(rects);
}

void plain_node(lx::scene::surface_node& n, unsigned id, lx::rect2i r) {
    lx::gfx::imported_image img{};
    img.image_id = id;
    img.width = static_cast<unsigned>(r.width);
    img.height = static_cast<unsigned>(r.height);
    n.attach(img);
    n.set_bounds({0, 0, r.width, r.height});
    n.set_transform({r.x, r.y});
    n.set_opaque(true);
}

} // namespace

LUMEN_TEST(occlusion_partial_opaque_region_through_commit_frame) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::scene_graph scene{};
    scene.set_projection({{0, 0}, 1.0, {640, 480}});
    // The window's body spans output x 54..406, y 66..294.
    lx::scene::surface_node inside{lx::surface_id{1}};
    plain_node(inside, 11, {150, 150, 100, 100});
    lx::scene::surface_node peeking{lx::surface_id{2}};
    plain_node(peeking, 12, {40, 100, 100, 100}); // reaches into the left shadow
    lx::scene::surface_node top{lx::surface_id{3}};
    csd_node(top, 13, {30, 30}, {400, 300});
    LUMEN_CHECK(scene.root().add(&inside) && scene.root().add(&peeking) && scene.root().add(&top));
    LUMEN_CHECK(scene.commit_frame(5));
    LUMEN_CHECK(scene.last_culled() == 1);
    LUMEN_CHECK(!inside.visible_in(5) && peeking.visible_in(5) && top.visible_in(5));

    // Whole-surface opaque behaves as before: its dst is the occluder, no rects emitted.
    top.set_opaque(true);
    LUMEN_CHECK(scene.commit_frame(6));
    LUMEN_CHECK(scene.last_culled() == 2 && !peeking.visible_in(6));
    top.set_opaque(false);

    // A resampled draw (a 200x150 buffer stretched over 400x300) seeds nothing.
    csd_node(top, 13, {30, 30}, {200, 150});
    LUMEN_CHECK(scene.commit_frame(7));
    LUMEN_CHECK(scene.last_culled() == 0 && inside.visible_in(7));
    // Neither does a translucent node.
    csd_node(top, 13, {30, 30}, {400, 300});
    top.set_transform({30, 30}, 1.f, 0.5f);
    LUMEN_CHECK(scene.commit_frame(8));
    LUMEN_CHECK(scene.last_culled() == 0);
    top.set_transform({30, 30});
    // No rects: what an ARGB window without an opaque region always did.
    top.set_opaque_rects({});
    LUMEN_CHECK(scene.commit_frame(9));
    LUMEN_CHECK(scene.last_culled() == 0);
    (void)scene.root().remove(&inside);
    (void)scene.root().remove(&peeking);
    (void)scene.root().remove(&top);
}

LUMEN_TEST(partial_opaque_rects_round_inwards_and_turn_with_the_output) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    // Scale 1.25: a 10x10 surface is 13x13 pixels (round(12.5)) and its 13x13 buffer is 1:1.
    // Its opaque rect 1..9 maps to 1.3..11.7: only pixels 2..11 lie wholly inside.
    const auto emitted = [](lx::buffer_transform t, lx::rect2i& out) {
        lx::scene::scene_graph scene{};
        lx::scene::output_projection p{{0, 0}, 1.25, {100, 80}};
        p.transform = t;
        scene.set_projection(p);
        lx::scene::surface_node n{lx::surface_id{1}};
        lx::gfx::imported_image img{};
        img.image_id = 7;
        img.width = 13;
        img.height = 13;
        n.attach(img);
        n.set_bounds({0, 0, 10, 10});
        n.set_transform({4, 0});
        const lx::rect2i rect{1, 1, 8, 8};
        n.set_opaque_rects({&rect, 1});
        LUMEN_CHECK(scene.root().add(&n));
        lx::scene::draw_list list{};
        scene.build_draw_list_into(list, 0);
        const bool one = list.size() == 1 && list.data()[0].opaque_rect_count == 1;
        if (one)
            out = list.data()[0].opaque_rects[0];
        (void)scene.root().remove(&n);
        return one;
    };
    lx::rect2i r{};
    LUMEN_CHECK(emitted(lx::buffer_transform::normal, r));
    // dst.x = round(4 * 1.25) = 5.
    LUMEN_CHECK(r.x == 5 + 2 && r.y == 2 && r.width == 9 && r.height == 9);
    // A turned output turns the rect with the draw (content space 80x100).
    lx::rect2i turned{};
    LUMEN_CHECK(emitted(lx::buffer_transform::rotate_90, turned));
    const lx::rect2i want = lx::gfx::transform_rect(r, lx::buffer_transform::rotate_90, 80, 100);
    LUMEN_CHECK(turned.x == want.x && turned.y == want.y && turned.width == want.width &&
                turned.height == want.height);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
