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
    const unsigned marked = lx::scene::mark_occlusion(d, 3, {0, 0, 640, 480}, shown);
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
    LUMEN_CHECK(lx::scene::mark_occlusion(d, 4, {0, 0, 640, 480}, nullptr) == 0);
    lx::scene::draw_command e[2] = {opaque_draw({310, 10, 10, 10}), tinted};
    LUMEN_CHECK(lx::scene::mark_occlusion(e, 2, {0, 0, 640, 480}, nullptr) == 0);
    lx::scene::draw_command f[2] = {opaque_draw({310, 10, 10, 10}), scaled};
    LUMEN_CHECK(lx::scene::mark_occlusion(f, 2, {0, 0, 640, 480}, nullptr) == 0);
}

LUMEN_TEST(occlusion_off_output_and_clip) {
    lx::scene::draw_command d[3] = {opaque_draw({700, 10, 50, 50}),   // off the output
                                    opaque_draw({10, 10, 50, 50}),    // under the clipped top
                                    opaque_draw({0, 0, 200, 200})};
    // The top draw is clipped to its left half: it covers x < 30 only.
    d[2].clip = {0, 0, 30, 200};
    bool shown[3]{};
    LUMEN_CHECK(lx::scene::mark_occlusion(d, 3, {0, 0, 640, 480}, shown) == 1);
    LUMEN_CHECK(d[0].occluded && !shown[0]);
    LUMEN_CHECK(!d[1].occluded && shown[1]);
    // The part of a draw past the output does not count against covering it.
    lx::scene::draw_command e[2] = {opaque_draw({600, 400, 80, 80}), opaque_draw({590, 390, 50, 90})};
    LUMEN_CHECK(lx::scene::mark_occlusion(e, 2, {0, 0, 640, 480}, nullptr) == 1);
    LUMEN_CHECK(e[0].occluded);
    // Unbounded (no output size): nothing is off-screen.
    lx::scene::draw_command g[1] = {opaque_draw({5000, 5000, 10, 10})};
    LUMEN_CHECK(lx::scene::mark_occlusion(g, 1, {}, nullptr) == 0);
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
    LUMEN_CHECK(lx::scene::mark_occlusion(d, 10, {}, nullptr) == 0);
    LUMEN_CHECK(!d[0].occluded);
    // A cover larger than the smallest kept one replaces it, and hides the target.
    d[1] = opaque_draw({900, 900, 200, 200});
    LUMEN_CHECK(lx::scene::mark_occlusion(d, 10, {}, nullptr) == 1);
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

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
