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
    auto& draws = buf.write_draw_list();
    draws.clear();
    draws.push(lx::scene::draw_command{});
    LUMEN_CHECK(buf.try_publish({}, 1));

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
        auto& draws = buf.write_draw_list();
        draws.clear();
        for (unsigned i = 0; i < frame; ++i)
            draws.push(lx::scene::draw_command{});

        if (!buf.try_publish({}, frame))
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
    buf.write_draw_list().clear();
    LUMEN_CHECK(buf.try_publish({}, 1));

    lx::runtime::set_current_affinity(lx::runtime::affinity::render);
    const auto& snap = buf.acquire();
    const unsigned held = snap.frame_index();
    LUMEN_CHECK(held == 1);

    // UI publishes many more frames while render holds the lease.
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    for (unsigned i = 2; i < 64; ++i) {
        auto& draws = buf.write_draw_list();
        draws.clear();
        // Must not overwrite the leased slot — write_draw_list skips it.
        LUMEN_CHECK(&draws != &snap.draws());
        (void)buf.try_publish({}, i);
    }

    // Held snapshot must still be intact.
    LUMEN_CHECK(snap.frame_index() == held);
    buf.release(snap);
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
            auto& draws = buf.write_draw_list();
            draws.clear();
            for (unsigned i = 0; i <= frame % k_period; ++i)
                draws.push(lx::scene::draw_command{});
            (void)buf.try_publish({}, frame++);
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
