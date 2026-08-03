#include "lumen_test.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
