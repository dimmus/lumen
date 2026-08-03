#include "lumen_test.hpp"

#include <cstring>

import lx.runtime;

// The arena used to set capacity_ to the requested size while storage_ stayed on a 64 KiB
// inline array, so allocate() bounds-checked against memory it did not own. Capacity must
// never exceed what is actually mapped.
LUMEN_TEST(arena_capacity_is_backed_by_real_storage) {
    lx::runtime::memory_arena arena{4 * 1024 * 1024};
    LUMEN_CHECK(arena.valid());
    LUMEN_CHECK(arena.capacity_bytes() >= 4 * 1024 * 1024);

    // Fill the whole reported capacity in chunks and touch every byte. If capacity_ were
    // larger than the mapping, this writes past the end.
    constexpr unsigned k_chunk = 64 * 1024;
    unsigned written = 0;
    while (arena.capacity_bytes() - written >= k_chunk) {
        auto* p = static_cast<unsigned char*>(arena.allocate(k_chunk, 16));
        LUMEN_CHECK(p != nullptr);
        std::memset(p, 0xAB, k_chunk);
        written += k_chunk;
    }
    LUMEN_CHECK(written > 64 * 1024); // more than the old inline array
    LUMEN_CHECK(arena.used_bytes() == written);
}

LUMEN_TEST(arena_refuses_past_capacity) {
    lx::runtime::memory_arena arena{4096};
    const unsigned cap = arena.capacity_bytes();
    LUMEN_CHECK(cap >= 4096);

    LUMEN_CHECK(arena.allocate(cap + 1) == nullptr);
    LUMEN_CHECK(arena.used_bytes() == 0);

    LUMEN_CHECK(arena.allocate(cap, 1) != nullptr);
    LUMEN_CHECK(arena.would_overflow(1));
    LUMEN_CHECK(arena.allocate(1) == nullptr);

    arena.reset();
    LUMEN_CHECK(arena.used_bytes() == 0);
    LUMEN_CHECK(arena.allocate(1) != nullptr);
}

LUMEN_TEST(arena_honors_alignment_and_rejects_bad_alignment) {
    lx::runtime::memory_arena arena{64 * 1024};

    LUMEN_CHECK(arena.allocate(1, 1) != nullptr); // knock the offset off alignment
    for (unsigned align : {2u, 4u, 8u, 16u, 64u}) {
        auto* p = arena.allocate(8, align);
        LUMEN_CHECK(p != nullptr);
        LUMEN_CHECK(reinterpret_cast<unsigned long long>(p) % align == 0);
    }

    // Non-power-of-two alignment used to corrupt the mask arithmetic silently.
    LUMEN_CHECK(arena.allocate(8, 3) == nullptr);
    LUMEN_CHECK(arena.allocate(8, 0) == nullptr);
    LUMEN_CHECK(arena.allocate(0, 8) == nullptr);
}

LUMEN_TEST(arena_move_transfers_storage) {
    lx::runtime::memory_arena source{128 * 1024};
    auto* p = static_cast<unsigned char*>(source.allocate(1024, 16));
    LUMEN_CHECK(p != nullptr);
    std::memset(p, 0x5A, 1024);
    const unsigned used = source.used_bytes();

    lx::runtime::memory_arena moved{std::move(source)};
    LUMEN_CHECK(moved.valid());
    LUMEN_CHECK(moved.used_bytes() == used);
    LUMEN_CHECK(p[0] == 0x5A && p[1023] == 0x5A);

    // Moved-from arena owns nothing and must hand out nothing.
    LUMEN_CHECK(!source.valid());               // NOLINT(bugprone-use-after-move)
    LUMEN_CHECK(source.capacity_bytes() == 0);  // NOLINT(bugprone-use-after-move)
    LUMEN_CHECK(source.allocate(8) == nullptr); // NOLINT(bugprone-use-after-move)
}

// Pressure must be evaluated against storage that exists, not against the requested config.
LUMEN_TEST(budget_coordinator_reports_mapped_capacity) {
    lx::runtime::memory_budget_coordinator coord{
        {.ui_arena_bytes = 1024 * 1024, .render_arena_bytes = 2 * 1024 * 1024}};

    auto& ui = coord.arena(lx::runtime::memory_pool::ui);
    LUMEN_CHECK(ui.valid());
    LUMEN_CHECK(ui.capacity_bytes() >= 1024 * 1024);

    LUMEN_CHECK(ui.allocate(900 * 1024, 16) != nullptr);
    coord.tick(1);
    coord.reset_frame_arenas(lx::runtime::memory_pool::ui);
    LUMEN_CHECK(ui.used_bytes() == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
