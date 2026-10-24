#include "lumen_test.hpp"

#include <sys/mman.h>
#include <unistd.h>

import std;
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

namespace {
lx::runtime::memory_pressure g_last = lx::runtime::memory_pressure::normal;
int g_calls = 0;
void on_pressure(lx::runtime::memory_pressure level, void*) {
    g_last = level;
    ++g_calls;
}
} // namespace

LUMEN_TEST(staging_over_budget_raises_pressure_by_itself) {
    lx::runtime::memory_budget_config cfg{};
    cfg.staging_budget_bytes = 100u * 1024u * 1024u;
    lx::runtime::memory_budget_coordinator coord{cfg};
    coord.set_pressure_handler(&on_pressure);
    g_calls = 0;
    // 80 MiB of 100: moderate, even though the frame arenas are empty. Summing every pool
    // used to dilute this; the fullest pool decides now. (80 MiB * 100 also overflows 32
    // bits, which the old arithmetic did.)
    coord.report_usage(lx::runtime::memory_pool::staging, 80u * 1024u * 1024u, 0);
    LUMEN_CHECK(coord.current_pressure() == lx::runtime::memory_pressure::moderate);
    LUMEN_CHECK(g_last == lx::runtime::memory_pressure::moderate && g_calls == 1);
    coord.report_usage(lx::runtime::memory_pool::staging, 95u * 1024u * 1024u, 0);
    LUMEN_CHECK(coord.current_pressure() == lx::runtime::memory_pressure::critical);
    coord.report_usage(lx::runtime::memory_pool::staging, 1024u, 0);
    LUMEN_CHECK(coord.current_pressure() == lx::runtime::memory_pressure::normal);
    // An arena tick must not erase the staging figure.
    coord.report_usage(lx::runtime::memory_pool::staging, 92u * 1024u * 1024u, 0);
    coord.tick(1);
    LUMEN_CHECK(coord.current_pressure() == lx::runtime::memory_pressure::critical);
}

LUMEN_TEST(staging_is_not_an_arena) {
    lx::runtime::memory_budget_coordinator coord{};
    auto& ui = coord.arena(lx::runtime::memory_pool::ui);
    (void)ui.allocate(64);
    const unsigned used = ui.used_bytes();
    coord.reset_frame_arenas(lx::runtime::memory_pool::staging);
    LUMEN_CHECK(ui.used_bytes() == used); // resetting "staging" must not reset the UI arena
}

namespace {

/// Pages of [p, p + bytes) resident in this process, by mincore.
[[nodiscard]] std::size_t resident_pages(const void* p, std::size_t bytes) {
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    std::vector<unsigned char> v((bytes + page - 1) / page);
    if (::mincore(const_cast<void*>(p), bytes, v.data()) != 0)
        return ~std::size_t{0};
    std::size_t n = 0;
    for (unsigned char c : v)
        n += c & 1u;
    return n;
}

} // namespace

// Atlas pages and cache arenas rely on this: nothing resident until written, and discard
// hands whole pages back (reading zero after), keeping partial pages at the ends.
LUMEN_TEST(page_mapping_is_lazy_and_discard_returns_pages) {
    const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    lx::runtime::page_mapping m;
    LUMEN_CHECK(!m.valid() && m.size() == 0);
    LUMEN_CHECK(!m.map(0));
    LUMEN_CHECK(m.map(4 * 1024 * 1024 + 1));
    LUMEN_CHECK(m.valid() && m.size() == 4 * 1024 * 1024 + page);
    LUMEN_CHECK(resident_pages(m.data(), m.size()) == 0);

    // Residency is checked before each read: mincore counts a page that was only read,
    // because reading maps the shared zero page (which costs no RSS).
    m.data()[0] = std::byte{1};
    m.data()[3 * page + 5] = std::byte{2};
    m.data()[7 * page] = std::byte{3};
    LUMEN_CHECK(resident_pages(m.data(), m.size()) == 3);

    // [1 byte into page 0, end of page 7): only pages 1–6 are whole, so page 0 stays.
    m.discard(1, 7 * page - 1);
    LUMEN_CHECK(resident_pages(m.data(), m.size()) == 2);
    LUMEN_CHECK(m.data()[0] == std::byte{1});
    LUMEN_CHECK(m.data()[3 * page + 5] == std::byte{0});
    LUMEN_CHECK(m.data()[7 * page] == std::byte{3});
    m.discard(0, m.size() * 2); // clamped to the mapping
    LUMEN_CHECK(resident_pages(m.data(), m.size()) == 0);
    bool zero = true;
    for (std::size_t i = 0; i < m.size(); ++i)
        zero = zero && m.data()[i] == std::byte{0};
    LUMEN_CHECK(zero);

    lx::runtime::page_mapping moved{std::move(m)};
    LUMEN_CHECK(!m.valid() && moved.valid() && moved.size() == 4 * 1024 * 1024 + page);
    moved.release();
    LUMEN_CHECK(!moved.valid() && moved.size() == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
