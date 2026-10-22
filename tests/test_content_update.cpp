// The content-update queue's Wayland-free core (`:content_update`): pool, per-surface order,
// sealing, the readiness policy (fences, fifo barrier, commit-timing target, hidden
// surfaces, acquire points without a poller entry, the pool reserve), the readiness poller on
// real eventfds and its reserve for other units; the frame clocks' `request_at`; the tearing
// policy; the DRM timeline table on fake ops (this host has no syncobjs); and when the buffer
// lifecycle lets a dma-buf (and its explicit-sync release point) go.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <signal.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scheduler;
import lx.compositor;

using lx::compositor::content_queue;
using lx::compositor::cu_conditions;
using lx::compositor::cu_ready;
using lx::compositor::cu_record;
using lx::compositor::k_cu_per_surface;
using lx::compositor::k_cu_pool;

LUMEN_TEST(queue_keeps_commit_order_per_surface) {
    content_queue q;
    const unsigned a = q.alloc(3, 1);
    const unsigned b = q.alloc(3, 1);
    const unsigned c = q.alloc(7, 1);
    LUMEN_CHECK(a && b && c && a != b);
    q.push(a);
    q.push(b);
    q.push(c);
    LUMEN_CHECK(q.depth(3) == 2 && q.depth(7) == 1);
    LUMEN_CHECK(q.head(3) == a && q.tail(3) == b);
    unsigned slots[8];
    LUMEN_CHECK(q.queued_surfaces(slots, 8) == 2);
    LUMEN_CHECK(slots[0] == 3 && slots[1] == 7);
    LUMEN_CHECK(q.pop(3) == a);
    LUMEN_CHECK(q.head(3) == b);
    q.release(a);
    LUMEN_CHECK(q.pop(3) == b);
    LUMEN_CHECK(q.depth(3) == 0 && q.head(3) == 0);
    q.release(b);
    LUMEN_CHECK(q.queued_surfaces(slots, 8) == 1);
    LUMEN_CHECK(q.client_count(1) == 1);
    q.release(q.pop(7));
    LUMEN_CHECK(q.used_count() == 0);
}

LUMEN_TEST(queue_generation_changes_on_reuse) {
    content_queue q;
    const unsigned a = q.alloc(1, 1);
    const unsigned gen = q.at(a).generation;
    q.release(a);
    const unsigned again = q.alloc(1, 1);
    LUMEN_CHECK(again == a); // the free list hands the warm record back
    LUMEN_CHECK(q.at(again).generation != gen && q.at(again).generation != 0);
}

LUMEN_TEST(queue_pool_exhaustion_returns_zero) {
    content_queue q;
    unsigned got = 0;
    for (unsigned i = 0; i < k_cu_pool; ++i)
        got += q.alloc(1 + (i % 200), 1) ? 1u : 0u;
    LUMEN_CHECK(got == k_cu_pool);
    LUMEN_CHECK(q.alloc(1, 1) == 0);
    LUMEN_CHECK(q.free_count() == 0);
}

LUMEN_TEST(sealed_children_block_the_graph_until_their_fences_clear) {
    content_queue q;
    const unsigned parent = q.alloc(1, 1);
    const unsigned child = q.alloc(2, 1);
    const unsigned grandchild = q.alloc(3, 1);
    q.seal(parent, child);
    q.seal(parent, grandchild);
    LUMEN_CHECK(q.at(parent).first_child == child);
    LUMEN_CHECK(q.at(child).next_sibling == grandchild);
    LUMEN_CHECK(q.fences_clear(parent));
    q.at(grandchild).waits = 1;
    LUMEN_CHECK(!q.fences_clear(parent));
    q.at(grandchild).waits = 0;
    q.at(parent).waits = 2;
    LUMEN_CHECK(!q.fences_clear(parent));
    q.at(parent).waits = 0;
    q.unseal(child);
    LUMEN_CHECK(q.at(parent).first_child == grandchild);
    LUMEN_CHECK(q.at(child).parent == 0);
}

LUMEN_TEST(fifo_wait_holds_only_shown_surfaces_with_a_barrier) {
    cu_record r{};
    r.fifo_wait = true;
    cu_conditions c{};
    c.shown = true;
    c.barrier = true;
    LUMEN_CHECK(!cu_ready(r, true, c));
    c.barrier = false;
    LUMEN_CHECK(cu_ready(r, true, c));
    // Hidden (occluded, off every output, locked, paused, dark): the wait is ignored.
    c.barrier = true;
    c.shown = false;
    LUMEN_CHECK(cu_ready(r, true, c));
    // Fences hold even a hidden surface.
    LUMEN_CHECK(!cu_ready(r, false, c));
}

LUMEN_TEST(commit_timing_target_ready_within_half_a_millisecond) {
    using lx::compositor::cu_time_reached;
    constexpr unsigned long long p = 1'000'000'000ull;
    LUMEN_CHECK(cu_time_reached(p, p));
    LUMEN_CHECK(cu_time_reached(p + 500'000ull, p));       // jitter slack
    LUMEN_CHECK(!cu_time_reached(p + 500'001ull, p));      // a vblank too early
    LUMEN_CHECK(cu_time_reached(p - 16'000'000ull, p));    // late: apply now
    LUMEN_CHECK(cu_time_reached(0, p));                    // no target
    LUMEN_CHECK(cu_time_reached(p + 50'000'000ull, 0));    // no prediction: never stuck
    cu_record r{};
    r.target_ns = p + 20'000'000ull;
    cu_conditions c{};
    c.shown = true;
    c.predicted_ns = p;
    LUMEN_CHECK(!cu_ready(r, true, c));
    c.shown = false; // hidden surfaces ignore T
    LUMEN_CHECK(cu_ready(r, true, c));
}

LUMEN_TEST(readiness_poller_wakes_on_signalled_fds_only) {
    lx::compositor::readiness_poller poller;
    LUMEN_CHECK(static_cast<bool>(poller.open()));
    const int a = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    const int b = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    const int a_signal = ::dup(a);
    LUMEN_CHECK(poller.add(a, 5, 11));
    LUMEN_CHECK(poller.add(b, 6, 12));
    LUMEN_CHECK(poller.in_use() == 2);
    lx::compositor::readiness_poller::wake w[4];
    LUMEN_CHECK(poller.drain(w, 4) == 0);
    const std::uint64_t one = 1;
    LUMEN_CHECK(::write(a_signal, &one, sizeof(one)) == sizeof(one));
    const unsigned n = poller.drain(w, 4);
    LUMEN_CHECK(n == 1);
    LUMEN_CHECK(w[0].cu == 5 && w[0].generation == 11);
    LUMEN_CHECK(poller.in_use() == 1); // consumed and closed
    LUMEN_CHECK(poller.drop(6) == 1);
    LUMEN_CHECK(poller.in_use() == 0);
    ::close(a_signal);
    LUMEN_CHECK(!poller.add(-1, 1, 1));
}

LUMEN_TEST(per_surface_cap_is_the_designed_bound) {
    LUMEN_CHECK(k_cu_per_surface == 128);
    LUMEN_CHECK(lx::compositor::k_cu_per_client == 128);
    LUMEN_CHECK(k_cu_per_surface <= k_cu_pool);
}

// ── frame clocks: request_at (commit-timing) ────────────────────────────────

LUMEN_TEST(request_at_arms_the_tick_for_the_target_vblank_only) {
    lx::scheduler::frame_clocks clocks;
    constexpr long long period = 16'000'000;
    clocks.set_live(0, true, period);
    clocks.on_present(0, 1'000'000'000, 1'000'000'000); // phase: vblanks at 1 s + k·16 ms
    const long long now = 1'001'000'000;
    const long long target = 1'100'000'000; // between vblanks 1.096 and 1.112
    LUMEN_CHECK(clocks.request_at(0, target, 500'000, now));
    LUMEN_CHECK((clocks.running_mask() & 1u) != 0);
    const long long start = clocks.start_of(0);
    const long long deadline = clocks.clock(0).next_deadline(start);
    LUMEN_CHECK(deadline == 1'112'000'000); // first vblank at or after the target
    LUMEN_CHECK(start < deadline && start > 1'096'000'000 - period);
    // A running clock keeps its schedule.
    LUMEN_CHECK(!clocks.request_at(0, target + period, 500'000, now));
    // A dark output is never armed.
    clocks.stop(0);
    clocks.set_dark(0, true);
    LUMEN_CHECK(!clocks.request_at(0, target, 500'000, now));
}

// ── tearing policy ───────────────────────────────────────────────────────────

LUMEN_TEST(allow_async_needs_every_condition) {
    using lx::compositor::allow_async;
    lx::compositor::async_flip_state s{};
    s.card_capable = s.pipeline_tested = s.fullscreen_top = s.wants_async = true;
    LUMEN_CHECK(allow_async(s));
    auto off = [&](bool lx::compositor::async_flip_state::*field) {
        auto t = s;
        t.*field = false;
        return allow_async(t);
    };
    LUMEN_CHECK(!off(&lx::compositor::async_flip_state::card_capable));
    LUMEN_CHECK(!off(&lx::compositor::async_flip_state::pipeline_tested));
    LUMEN_CHECK(!off(&lx::compositor::async_flip_state::fullscreen_top));
    LUMEN_CHECK(!off(&lx::compositor::async_flip_state::wants_async));
    auto locked = s;
    locked.locked_or_paused = true;
    LUMEN_CHECK(!allow_async(locked));
}

// ── DRM timelines on fake ops ────────────────────────────────────────────────

namespace {
struct fake_kernel {
    unsigned next_handle = 100;
    unsigned destroyed = 0;
    unsigned long long signalled_upto = 0; // one timeline is enough here
    int last_eventfd = -1;
    unsigned long long last_signal = 0;
    int last_sync_file = -2;
};

lx::gfx::drm_timeline_ops fake_ops(fake_kernel& k) {
    lx::gfx::drm_timeline_ops ops{};
    ops.user = &k;
    ops.fd_to_handle = [](void* u, int fd, unsigned* h) {
        if (fd == 666)
            return -22; // not a syncobj
        *h = static_cast<fake_kernel*>(u)->next_handle++;
        return 0;
    };
    ops.query = [](void* u, unsigned, unsigned long long point, bool* done) {
        *done = static_cast<fake_kernel*>(u)->signalled_upto >= point;
        return 0;
    };
    ops.eventfd = [](void* u, unsigned, unsigned long long, int efd) {
        static_cast<fake_kernel*>(u)->last_eventfd = efd;
        return 0;
    };
    ops.signal = [](void* u, unsigned, unsigned long long point) {
        static_cast<fake_kernel*>(u)->last_signal = point;
        return 0;
    };
    ops.import_sync_file = [](void* u, unsigned, unsigned long long point, int sync_file) {
        static_cast<fake_kernel*>(u)->last_signal = point;
        static_cast<fake_kernel*>(u)->last_sync_file = sync_file;
        return 0;
    };
    ops.destroy = [](void* u, unsigned) { ++static_cast<fake_kernel*>(u)->destroyed; };
    return ops;
}
} // namespace

LUMEN_TEST(timeline_import_failure_is_invalid_timeline_and_closes_the_fd) {
    fake_kernel k;
    lx::gfx::drm_timeline_table t;
    t.bind(fake_ops(k));
    auto bad = t.import(666);
    LUMEN_CHECK(!bad);
    LUMEN_CHECK(bad.get_error().code == std::to_underlying(lx::drm_err::timeline_failed));
    const int fd = ::eventfd(0, EFD_CLOEXEC); // any fd: the table must close it
    auto ok = t.import(fd);
    LUMEN_CHECK(static_cast<bool>(ok));
    LUMEN_CHECK(::fcntl(fd, F_GETFD) == -1); // closed
    LUMEN_CHECK(t.live() == 1);
}

LUMEN_TEST(timeline_points_outlive_the_object_by_reference) {
    fake_kernel k;
    lx::gfx::drm_timeline_table t;
    t.bind(fake_ops(k));
    const unsigned id = t.import(::eventfd(0, EFD_CLOEXEC)).value();
    t.ref(id);   // a point set from it
    t.unref(id); // the wp_linux_drm_syncobj_timeline_v1 destroyed
    LUMEN_CHECK(t.live() == 1 && k.destroyed == 0);
    LUMEN_CHECK(static_cast<bool>(t.signal(id, 7)));
    LUMEN_CHECK(k.last_signal == 7);
    t.unref(id); // the point used
    LUMEN_CHECK(t.live() == 0 && k.destroyed == 1);
    LUMEN_CHECK(!t.signal(id, 8));
}

LUMEN_TEST(timeline_acquire_query_watch_and_release_with_fence) {
    fake_kernel k;
    lx::gfx::drm_timeline_table t;
    t.bind(fake_ops(k));
    const unsigned id = t.import(::eventfd(0, EFD_CLOEXEC)).value();
    k.signalled_upto = 4;
    LUMEN_CHECK(t.signalled(id, 4).value());
    LUMEN_CHECK(!t.signalled(id, 5).value());
    LUMEN_CHECK(static_cast<bool>(t.notify(id, 5, 42)));
    LUMEN_CHECK(k.last_eventfd == 42);
    // Release: with a composite fence it is transferred at the point; without one (the
    // composite already completed on the CPU) the point is signalled now.
    LUMEN_CHECK(static_cast<bool>(t.signal_after(id, 9, 77)));
    LUMEN_CHECK(k.last_sync_file == 77 && k.last_signal == 9);
    k.last_sync_file = -2;
    LUMEN_CHECK(static_cast<bool>(t.signal_after(id, 10, -1)));
    LUMEN_CHECK(k.last_sync_file == -2 && k.last_signal == 10);
}

LUMEN_TEST(release_fence_mailbox_keeps_the_newest_and_dups) {
    lx::gfx::release_fence_mailbox box;
    LUMEN_CHECK(box.dup() == -1);
    box.publish(::eventfd(0, EFD_CLOEXEC), 3);
    box.publish(::eventfd(0, EFD_CLOEXEC), 4); // the older one is closed
    unsigned frame = 0;
    const int d = box.dup(&frame);
    LUMEN_CHECK(d >= 0 && frame == 4);
    ::close(d);
}

LUMEN_TEST(drm_timeline_unsupported_without_a_card) {
    LUMEN_CHECK(!lx::gfx::drm_timeline_supported(-1));
}

LUMEN_TEST(an_unwatched_acquire_point_blocks_like_a_wait) {
    content_queue q;
    const unsigned root = q.alloc(2, 1);
    const unsigned kid = q.alloc(3, 1);
    q.seal(root, kid);
    q.push(root);
    LUMEN_CHECK(q.fences_clear(root));
    q.at(kid).recheck = true;
    LUMEN_CHECK(!q.fences_clear(root));
    LUMEN_CHECK(!cu_ready(q.at(root), q.fences_clear(root), cu_conditions{}));
    q.at(kid).recheck = false;
    q.at(root).recheck = true;
    LUMEN_CHECK(!q.fences_clear(root));
    q.at(root).recheck = false;
    LUMEN_CHECK(q.fences_clear(root));
    // A reused record starts without it.
    q.release(kid);
    const unsigned again = q.alloc(3, 1);
    LUMEN_CHECK(!q.at(again).recheck);
}

LUMEN_TEST(pool_reserve_keeps_room_for_clients_that_queue_little) {
    using lx::compositor::cu_room_for;
    using lx::compositor::k_cu_light;
    using lx::compositor::k_cu_reserve;
    // A client past its light share stops where the reserve begins...
    LUMEN_CHECK(cu_room_for(k_cu_reserve + 1, k_cu_light, 1));
    LUMEN_CHECK(!cu_room_for(k_cu_reserve, k_cu_light, 1));
    // ...one with little queued still gets a record from it, down to the last one.
    LUMEN_CHECK(cu_room_for(k_cu_reserve, 0, 1));
    LUMEN_CHECK(cu_room_for(1, k_cu_light - 1, 1));
    LUMEN_CHECK(!cu_room_for(0, 0, 1));
    // The per-client cap holds regardless.
    LUMEN_CHECK(!cu_room_for(k_cu_pool, lx::compositor::k_cu_per_client, 1));
    // A commit sealing many children counts them all.
    LUMEN_CHECK(!cu_room_for(k_cu_reserve + 4, 0, k_cu_light + 5));
}

LUMEN_TEST(readiness_poller_keeps_a_reserve_for_other_units) {
    using poller_t = lx::compositor::readiness_poller;
    poller_t poller;
    LUMEN_CHECK(static_cast<bool>(poller.open()));
    unsigned taken = 0;
    while (poller.add(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), 1, 1))
        ++taken;
    LUMEN_CHECK(taken == poller_t::k_max - poller_t::k_reserved);
    LUMEN_CHECK(poller.available() == 0 && poller.available(true) == poller_t::k_reserved);
    // A capture's copy fence still finds an entry.
    for (unsigned i = 0; i < poller_t::k_reserved; ++i)
        LUMEN_CHECK(poller.add(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), 0x10000u + i, 1, true));
    LUMEN_CHECK(!poller.add(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), 0x20000u, 1, true));
    LUMEN_CHECK(poller.drop(1) == taken);
    // Content updates get their whole share back, whatever captures hold.
    LUMEN_CHECK(poller.available() == poller_t::k_max - poller_t::k_reserved);
    LUMEN_CHECK(poller.available(true) == poller_t::k_max - poller_t::k_reserved);
    // Every capture session can wait on one fence at a time.
    LUMEN_CHECK(lx::compositor::k_capture_sessions <= poller_t::k_reserved);
}

namespace {

struct lifecycle_rig {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};
    lx::compositor::buffer_lifecycle_tracker tracker{cache};
    std::vector<void*> released;
    struct settled {
        unsigned timeline;
        unsigned long long point;
        bool signal;
    };
    std::vector<settled> sync;
    lifecycle_rig() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        tracker.set_release_callback(
            [](void* b, void* u) { static_cast<lifecycle_rig*>(u)->released.push_back(b); },
            this);
        tracker.set_sync_release_callback(
            [](unsigned t, unsigned long long p, bool s, void* u) {
                static_cast<lifecycle_rig*>(u)->sync.push_back({t, p, s});
            },
            this);
    }
    void commit(unsigned surface, unsigned buffer, void* resource) {
        lx::gfx::import_cache_key key{};
        key.client = lx::client_id{1};
        key.buffer = lx::buffer_id{buffer};
        LUMEN_CHECK(static_cast<bool>(tracker.on_import(lx::client_id{1}, lx::surface_id{surface},
                                                        lx::buffer_id{buffer}, key, resource)));
        tracker.on_attach(lx::surface_id{surface});
    }
};

int g_buf_a = 0;
int g_buf_b = 0;

} // namespace

LUMEN_TEST(lifecycle_keeps_a_current_dmabuf_until_a_newer_commit_supersedes_it) {
    lifecycle_rig r;
    r.commit(1, 10, &g_buf_a);
    r.tracker.on_frame_submitted(5);
    LUMEN_CHECK(r.tracker.has_unreleased_frames() == false); // waits for a commit, not a frame
    // Render completed frame 5 and many more: still the surface's content, still sampled.
    (void)r.tracker.retire_completed_frames(100);
    LUMEN_CHECK(r.released.empty());
    r.tracker.on_frame_submitted(101); // a later frame samples it again
    // A newer buffer supersedes it; the frames that sampled it must complete first.
    r.tracker.on_detach(lx::surface_id{1});
    r.commit(1, 11, &g_buf_b);
    LUMEN_CHECK(r.tracker.has_unreleased_frames());
    (void)r.tracker.retire_completed_frames(100);
    LUMEN_CHECK(r.released.empty());
    (void)r.tracker.retire_completed_frames(101);
    LUMEN_CHECK(r.released.size() == 1 && r.released[0] == &g_buf_a);
    // The new one is current now.
    r.tracker.on_frame_submitted(102);
    (void)r.tracker.retire_completed_frames(500);
    LUMEN_CHECK(r.released.size() == 1);
    // The surface's end lets it go after its frames.
    r.tracker.on_detach(lx::surface_id{1});
    (void)r.tracker.retire_completed_frames(500);
    LUMEN_CHECK(r.released.size() == 2 && r.released[1] == &g_buf_b);
    LUMEN_CHECK(!r.tracker.has_unreleased_frames());
}

LUMEN_TEST(lifecycle_recommit_of_the_current_buffer_keeps_it) {
    lifecycle_rig r;
    r.commit(1, 10, &g_buf_a);
    r.tracker.on_frame_submitted(1);
    r.commit(1, 10, &g_buf_a); // the same buffer again (new content, same memory)
    r.tracker.on_frame_submitted(2);
    (void)r.tracker.retire_completed_frames(50);
    LUMEN_CHECK(r.released.empty());
}

LUMEN_TEST(lifecycle_destroyed_buffer_on_screen_keeps_its_release_point_until_released) {
    lifecycle_rig r;
    r.commit(1, 10, &g_buf_a);
    r.tracker.on_frame_submitted(7);
    // The client destroys the wl_buffer while it is on screen: the record keeps the
    // release point, no wl_buffer.release goes to the dead object.
    LUMEN_CHECK(r.tracker.on_buffer_destroyed(&g_buf_a, 3, 42));
    (void)r.tracker.retire_completed_frames(100);
    LUMEN_CHECK(r.sync.empty() && r.released.empty());
    r.tracker.on_frame_submitted(8);
    r.tracker.on_detach(lx::surface_id{1});
    (void)r.tracker.retire_completed_frames(7);
    LUMEN_CHECK(r.sync.empty()); // frame 8 still reads it
    (void)r.tracker.retire_completed_frames(8);
    LUMEN_CHECK(r.sync.size() == 1 && r.sync[0].timeline == 3 && r.sync[0].point == 42 &&
                r.sync[0].signal);
    LUMEN_CHECK(r.released.empty());
    // A buffer no record holds any more: the caller signals its point itself.
    LUMEN_CHECK(!r.tracker.on_buffer_destroyed(&g_buf_a, 3, 43));
}

LUMEN_TEST(lifecycle_client_gone_drops_a_held_release_point) {
    lifecycle_rig r;
    r.commit(1, 10, &g_buf_a);
    r.tracker.on_frame_submitted(1);
    LUMEN_CHECK(r.tracker.on_buffer_destroyed(&g_buf_a, 5, 9));
    r.tracker.on_client_disconnect(lx::client_id{1});
    LUMEN_CHECK(r.sync.size() == 1 && !r.sync[0].signal);
    (void)r.tracker.retire_completed_frames(100);
    LUMEN_CHECK(r.sync.size() == 1 && r.released.empty());
}

LUMEN_TEST(lifecycle_memory_pressure_never_releases_what_is_on_screen) {
    lifecycle_rig r;
    r.commit(1, 10, &g_buf_a);
    r.tracker.on_frame_submitted(1);
    r.tracker.on_memory_pressure(lx::runtime::memory_pressure::critical);
    LUMEN_CHECK(r.released.empty());
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
