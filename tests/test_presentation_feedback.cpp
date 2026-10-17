// wp_presentation bookkeeping (`lx.compositor:presentation_feedback`): the sampled →
// flipped → presented chain keyed by frame index, superseding and discarding, the stopped
// display, a surface going away, overflow, client disconnects — and the flip event's tag,
// sequence and hardware flags carried from `drm::page_flip_event` through the scheduler's
// presentation tracker. Pure: no Wayland, no KMS.
#include "lumen_test.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

import lx.foundation;
import lx.drm;
import lx.scheduler;
import lx.compositor;

using lx::compositor::feedback_sink;
using lx::compositor::present_info;
using lx::compositor::presentation_feedback_table;
using table_state = lx::compositor::presentation_feedback_table::state;

namespace {

struct event {
    std::string kind; // "presented" | "discarded"
    int id = 0;
    unsigned long long seq = 0;
};

struct recorder {
    std::vector<event> events;

    feedback_sink sink() {
        feedback_sink k{};
        k.user = this;
        k.presented = [](void* u, void* resource, lx::surface_id, const present_info& info) {
            static_cast<recorder*>(u)->events.push_back(
                {"presented", static_cast<int>(reinterpret_cast<std::uintptr_t>(resource)),
                 info.seq});
        };
        k.discarded = [](void* u, void* resource) {
            static_cast<recorder*>(u)->events.push_back(
                {"discarded", static_cast<int>(reinterpret_cast<std::uintptr_t>(resource)), 0});
        };
        return k;
    }
    [[nodiscard]] bool has(const char* kind, int id) const {
        for (const auto& e : events)
            if (e.kind == kind && e.id == id)
                return true;
        return false;
    }
    void clear() { events.clear(); }
};

void* res(int id) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)); }

/// The visibility pass, faked: a set of (surface, frame) pairs that were on screen.
struct screen {
    std::set<std::pair<unsigned long long, unsigned>> shown;
    void show(lx::surface_id s, unsigned frame) { shown.insert({s.id(), frame}); }
    static bool visible(void* u, lx::surface_id s, unsigned frame) {
        return static_cast<screen*>(u)->shown.count({s.id(), frame}) != 0;
    }
};

/// One surface's pending and cached lists, as the wire keeps them.
struct surface {
    lx::surface_id id;
    presentation_feedback_table::list pending = 0;
    presentation_feedback_table::list cached = 0;
};

/// A root (desynchronized) commit: merge then apply, as wl_surface.commit does.
void commit(presentation_feedback_table& t, surface& s, unsigned building_frame,
            const feedback_sink& k) {
    t.on_merge(s.pending, s.cached, k);
    t.on_apply(s.id, s.cached, building_frame, k);
}

present_info flip_info(unsigned long long seq) {
    present_info info{};
    info.timestamp_ns = 1'000'000'000ull * seq;
    info.refresh_ns = 16'666'667;
    info.seq = seq;
    info.flags = lx::compositor::k_present_vsync | lx::compositor::k_present_hw_clock |
                 lx::compositor::k_present_hw_completion;
    return info;
}

} // namespace

LUMEN_TEST(presentation_superseded_before_sampling_is_discarded_at_once) {
    presentation_feedback_table t{};
    recorder r{};
    surface s{lx::surface_id{1}};
    LUMEN_CHECK(t.create(res(1), s.id, s.pending) != 0);
    commit(t, s, 10, r.sink());
    LUMEN_CHECK(t.count(table_state::queued) == 1);
    // Never on screen (no publish showed it); the next update replaces it.
    LUMEN_CHECK(t.create(res(2), s.id, s.pending) != 0);
    commit(t, s, 10, r.sink());
    LUMEN_CHECK(r.events.size() == 1 && r.has("discarded", 1));
    LUMEN_CHECK(t.in_use() == 1);
}

LUMEN_TEST(presentation_sampled_then_flipped_is_presented) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{1}};
    (void)t.create(res(1), s.id, s.pending);
    (void)t.create(res(2), s.id, s.pending); // two objects for one update: same feedback
    commit(t, s, 5, r.sink());
    scr.show(s.id, 5);
    t.on_publish(5, &screen::visible, &scr);
    LUMEN_CHECK(t.count(table_state::sampled) == 2);
    // A flip of an older composite does not show it yet.
    t.on_flip(4, flip_info(100), r.sink());
    LUMEN_CHECK(r.events.empty());
    t.on_flip(5, flip_info(101), r.sink());
    LUMEN_CHECK(r.events.size() == 2 && r.has("presented", 1) && r.has("presented", 2));
    LUMEN_CHECK(r.events[0].seq == 101);
    LUMEN_CHECK(t.in_use() == 0);
}

LUMEN_TEST(presentation_not_visible_stays_queued) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{1}};
    (void)t.create(res(1), s.id, s.pending);
    commit(t, s, 3, r.sink());
    // Occluded in 3 and 4: occlusion is not a reason to discard (the spec lists
    // "superseded or its surface destroyed").
    t.on_publish(3, &screen::visible, &scr);
    t.on_publish(4, &screen::visible, &scr);
    t.on_flip(4, flip_info(1), r.sink());
    LUMEN_CHECK(r.events.empty() && t.count(table_state::queued) == 1);
    // Uncovered in 6: sampled, and presented by its flip.
    scr.show(s.id, 6);
    t.on_publish(6, &screen::visible, &scr);
    t.on_flip(6, flip_info(2), r.sink());
    LUMEN_CHECK(r.events.size() == 1 && r.has("presented", 1));
}

LUMEN_TEST(presentation_superseded_update_resolves_by_flip_frame) {
    // Update A sampled at 10; update B applied at 12 and sampled at 12. A flip of 10 or 11
    // shows A; the first flip at or after 12 shows B — and A, if not presented by then, was
    // never seen.
    for (int variant = 0; variant < 2; ++variant) {
        presentation_feedback_table t{};
        recorder r{};
        screen scr{};
        surface s{lx::surface_id{7}};
        (void)t.create(res(1), s.id, s.pending);
        commit(t, s, 10, r.sink());
        scr.show(s.id, 10);
        t.on_publish(10, &screen::visible, &scr);
        (void)t.create(res(2), s.id, s.pending);
        commit(t, s, 12, r.sink());
        LUMEN_CHECK(t.count(table_state::superseded) == 1);
        scr.show(s.id, 12);
        t.on_publish(12, &screen::visible, &scr);
        if (variant == 0) {
            t.on_flip(11, flip_info(50), r.sink());
            LUMEN_CHECK(r.events.size() == 1 && r.has("presented", 1));
            t.on_flip(12, flip_info(51), r.sink());
            LUMEN_CHECK(r.events.size() == 2 && r.has("presented", 2));
        } else {
            // Frames 10 and 11 were composited but never committed (newest wins).
            t.on_flip(13, flip_info(50), r.sink());
            LUMEN_CHECK(r.events.size() == 2 && r.has("discarded", 1) && r.has("presented", 2));
        }
        LUMEN_CHECK(t.in_use() == 0);
    }
}

LUMEN_TEST(presentation_composited_not_committed_frames_present_later) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{2}};
    (void)t.create(res(1), s.id, s.pending);
    commit(t, s, 20, r.sink());
    scr.show(s.id, 20);
    scr.show(s.id, 21);
    t.on_publish(20, &screen::visible, &scr);
    t.on_publish(21, &screen::visible, &scr);
    // 20 and 21 were composited, only 22 reached the CRTC: it still shows the update.
    t.on_flip(22, flip_info(9), r.sink());
    LUMEN_CHECK(r.events.size() == 1 && r.has("presented", 1));
}

// Sampled in N, but the render thread took only N+1, in which an opaque window covers the
// surface: the flip of N+1 did not show the update, so it is not presented by it. It waits
// for a flip that shows it — or is discarded once a newer update supersedes it.
LUMEN_TEST(presentation_hidden_in_the_flipped_frame_is_not_presented) {
    for (int variant = 0; variant < 2; ++variant) {
        presentation_feedback_table t{};
        recorder r{};
        screen scr{};
        surface s{lx::surface_id{3}};
        (void)t.create(res(1), s.id, s.pending);
        commit(t, s, 30, r.sink());
        scr.show(s.id, 30);
        t.on_publish(30, &screen::visible, &scr);
        LUMEN_CHECK(t.count(table_state::sampled) == 1);
        t.on_publish(31, &screen::visible, &scr); // covered in 31
        t.on_flip(31, flip_info(7), r.sink());    // 30 was never composited
        LUMEN_CHECK(r.events.empty() && t.count(table_state::sampled) == 1);
        if (variant == 0) {
            // Uncovered in 33 with the update still current: presented by that flip.
            t.on_publish(32, &screen::visible, &scr);
            scr.show(s.id, 33);
            t.on_publish(33, &screen::visible, &scr);
            t.on_flip(32, flip_info(8), r.sink());
            LUMEN_CHECK(r.events.empty());
            t.on_flip(33, flip_info(9), r.sink());
            LUMEN_CHECK(r.events.size() == 1 && r.has("presented", 1) && r.events[0].seq == 9);
        } else {
            // The client's next update replaces it before it was ever seen.
            (void)t.create(res(2), s.id, s.pending);
            commit(t, s, 32, r.sink());
            scr.show(s.id, 32);
            t.on_publish(32, &screen::visible, &scr);
            t.on_flip(32, flip_info(8), r.sink());
            LUMEN_CHECK(r.events.size() == 2 && r.has("discarded", 1) && r.has("presented", 2));
        }
        LUMEN_CHECK(t.in_use() == 0);
    }
}

// The visibility window slides: a surface covered for longer than 64 frames and then
// uncovered is presented by the flip that shows it, not by an older recorded frame.
LUMEN_TEST(presentation_visibility_window_slides) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{4}};
    (void)t.create(res(1), s.id, s.pending);
    commit(t, s, 100, r.sink());
    scr.show(s.id, 100);
    t.on_publish(100, &screen::visible, &scr);
    for (unsigned f = 101; f < 300; ++f) {
        t.on_publish(f, &screen::visible, &scr);
        t.on_flip(f, flip_info(f), r.sink());
    }
    LUMEN_CHECK(r.events.empty());
    scr.show(s.id, 300);
    t.on_publish(300, &screen::visible, &scr);
    t.on_flip(300, flip_info(300), r.sink());
    LUMEN_CHECK(r.events.size() == 1 && r.has("presented", 1) && r.events[0].seq == 300);
}

LUMEN_TEST(presentation_frame_counter_wraps) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{2}};
    (void)t.create(res(1), s.id, s.pending);
    commit(t, s, 0xFFFFFFFEu, r.sink());
    scr.show(s.id, 0xFFFFFFFEu);
    t.on_publish(0xFFFFFFFEu, &screen::visible, &scr);
    t.on_flip(1, flip_info(3), r.sink()); // three frames later, past the wrap
    LUMEN_CHECK(r.has("presented", 1));
}

LUMEN_TEST(presentation_stopped_display_discards) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface a{lx::surface_id{1}};
    surface b{lx::surface_id{2}};
    (void)t.create(res(1), a.id, a.pending);
    commit(t, a, 1, r.sink());
    scr.show(a.id, 1);
    t.on_publish(1, &screen::visible, &scr); // sampled, its flip not yet drained
    (void)t.create(res(2), b.id, b.pending);
    commit(t, b, 2, r.sink()); // queued
    (void)t.create(res(3), b.id, b.pending); // pending: not part of anything in flight
    // The pause edge: everything in flight goes.
    t.discard_unpresented(r.sink());
    LUMEN_CHECK(r.events.size() == 2 && r.has("discarded", 1) && r.has("discarded", 2));
    LUMEN_CHECK(t.in_use() == 1);
    // While stopped, commits keep coming; the 1 Hz tick discards what was queued.
    r.clear();
    commit(t, b, 3, r.sink());
    LUMEN_CHECK(r.events.empty());
    t.discard_queued(r.sink());
    LUMEN_CHECK(r.events.size() == 1 && r.has("discarded", 3));
    LUMEN_CHECK(t.in_use() == 0);
}

LUMEN_TEST(presentation_surface_gone) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{4}};
    surface other{lx::surface_id{5}};
    (void)t.create(res(1), s.id, s.pending);
    commit(t, s, 1, r.sink());
    scr.show(s.id, 1);
    t.on_publish(1, &screen::visible, &scr);
    (void)t.create(res(2), s.id, s.pending);
    commit(t, s, 2, r.sink()); // 1 superseded, 2 queued
    (void)t.create(res(3), other.id, other.pending);
    commit(t, other, 2, r.sink());
    (void)t.create(res(4), s.id, s.pending); // pending
    // Destroyed: in flight (sampled, superseded, queued) and its own lists.
    t.on_surface_gone(s.id, r.sink());
    t.discard_list(s.pending, r.sink());
    t.discard_list(s.cached, r.sink());
    LUMEN_CHECK(r.events.size() == 3 && r.has("discarded", 1) && r.has("discarded", 2) &&
                r.has("discarded", 4));
    LUMEN_CHECK(s.pending == 0 && t.in_use() == 1 && t.count(table_state::queued) == 1);
}

LUMEN_TEST(presentation_synchronized_cache_replaced_is_discarded) {
    presentation_feedback_table t{};
    recorder r{};
    surface s{lx::surface_id{9}};
    // Two commits of a synchronized subsurface before its parent's: the first update is
    // replaced in the cache and never applied.
    (void)t.create(res(1), s.id, s.pending);
    t.on_merge(s.pending, s.cached, r.sink());
    (void)t.create(res(2), s.id, s.pending);
    t.on_merge(s.pending, s.cached, r.sink());
    LUMEN_CHECK(r.events.size() == 1 && r.has("discarded", 1));
    t.on_apply(s.id, s.cached, 4, r.sink()); // the parent's commit
    LUMEN_CHECK(t.count(table_state::queued) == 1 && s.cached == 0);
}

LUMEN_TEST(presentation_client_disconnect_forgets_resources) {
    presentation_feedback_table t{};
    recorder r{};
    screen scr{};
    surface s{lx::surface_id{3}};
    const unsigned pending_h = t.create(res(1), s.id, s.pending);
    // The client went away while its feedback was pending: dropped silently at apply.
    t.forget_resource(pending_h, res(1));
    const unsigned queued_h = t.create(res(2), s.id, s.pending);
    commit(t, s, 1, r.sink());
    LUMEN_CHECK(r.events.empty() && t.in_use() == 1);
    t.forget_resource(queued_h, res(2));
    LUMEN_CHECK(t.in_use() == 0);
    // A handle that no longer names the resource (already presented): ignored.
    const unsigned h3 = t.create(res(3), s.id, s.pending);
    commit(t, s, 2, r.sink());
    scr.show(s.id, 2);
    t.on_publish(2, &screen::visible, &scr);
    t.on_flip(2, flip_info(1), r.sink());
    LUMEN_CHECK(r.has("presented", 3));
    const unsigned h4 = t.create(res(4), s.id, s.pending); // likely reuses h3's slot
    t.forget_resource(h3, res(3));
    LUMEN_CHECK(t.in_use() == 1);
    (void)h4;
}

LUMEN_TEST(presentation_overflow_refuses_without_losing_track) {
    presentation_feedback_table t{};
    recorder r{};
    // One update may carry k_max_per_update objects.
    surface s{lx::surface_id{1}};
    for (unsigned i = 0; i < presentation_feedback_table::k_max_per_update; ++i)
        LUMEN_CHECK(t.create(res(static_cast<int>(i + 1)), s.id, s.pending) != 0);
    LUMEN_CHECK(t.create(res(999), s.id, s.pending) == 0);
    LUMEN_CHECK(t.overflows() == 1);
    // The table holds k_max across surfaces.
    unsigned made = presentation_feedback_table::k_max_per_update;
    for (unsigned n = 2; made < presentation_feedback_table::k_max; ++n) {
        surface o{lx::surface_id{n}};
        for (unsigned i = 0; i < presentation_feedback_table::k_max_per_update &&
                             made < presentation_feedback_table::k_max;
             ++i, ++made)
            LUMEN_CHECK(t.create(res(static_cast<int>(10000 + made)), o.id, o.pending) != 0);
    }
    surface last{lx::surface_id{5000}};
    LUMEN_CHECK(t.create(res(77), last.id, last.pending) == 0);
    LUMEN_CHECK(t.in_use() == presentation_feedback_table::k_max && t.overflows() == 2);
    // Freed slots are reused.
    t.discard_list(s.pending, r.sink());
    LUMEN_CHECK(r.events.size() == presentation_feedback_table::k_max_per_update);
    LUMEN_CHECK(t.create(res(78), last.id, last.pending) != 0);
}

LUMEN_TEST(presentation_flip_event_carries_tag_sequence_and_hw) {
    // drm::page_flip_event → scheduler::presentation_feedback, as the compositor's flip
    // handler receives it.
    lx::scheduler::presentation_tracker tracker{};
    lx::scheduler::presentation_feedback got{};
    tracker.set_handler(
        [](lx::scheduler::presentation_feedback fb, void* u) {
            *static_cast<lx::scheduler::presentation_feedback*>(u) = fb;
        },
        &got);
    lx::drm::page_flip_event ev{};
    ev.sequence = 123456;
    ev.timestamp_ns = 42'000'000'123ull;
    ev.presented = true;
    ev.hw = true;
    ev.vblank_timestamp = true;
    ev.content = true;
    ev.tag = 77;
    ev.crtc_id = 41;
    tracker.on_page_flip(ev);
    LUMEN_CHECK(got.seq == 123456 && got.actual_present_ns == 42'000'000'123ull);
    LUMEN_CHECK(got.hw_clock && got.vblank_clock && got.vsync && got.content && got.tag == 77 &&
                got.crtc_id == 41);
    // A driver without vblank support (vmwgfx): its event, but no vblank clock.
    ev.vblank_timestamp = false;
    ev.sequence = 0;
    tracker.on_page_flip(ev);
    LUMEN_CHECK(got.hw_clock && !got.vblank_clock && got.seq == 0);
    // A blocking modeset's synthetic completion: software-timed, no sequence.
    ev = {};
    ev.presented = true;
    ev.content = true;
    ev.tag = 78;
    tracker.on_page_flip(ev);
    LUMEN_CHECK(!got.hw_clock && !got.vblank_clock && got.vsync && got.seq == 0 &&
                got.tag == 78);
    // A cursor-only commit's flip shows no frame.
    ev = {};
    ev.presented = true;
    ev.hw = true;
    tracker.on_page_flip(ev);
    LUMEN_CHECK(!got.content && got.tag == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
