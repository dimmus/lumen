// GPU pools (AGENTS.md §5: "GPU pools: use `pipeline_cache` and `lx.gfx.semaphore_pool`
// (`timeline_semaphore_pool`) — no per-frame create/destroy"; docs/invariants.md, the "GPU
// pools" row). Two pools, each asked the one question a pool exists to answer: does a frame
// get back what an earlier frame used?
//
// `pipeline_cache` (renderer.cppm:445-460) needs no device: simulated frames acquire the
// same (blend, format) keys and must be answered with the same ids, with no id burned on the
// way — a new key takes the next one.
//
// `timeline_semaphore_pool` (semaphore_pool.cppm:45-109) needs a real Vulkan device, the one
// `device_selector::select_best()` picks. Its rows bring one up and say in their output
// which device ran them; without a Vulkan device they print SKIP, and the row is not
// evidenced on that host.
//
// Not pinned: what `pipeline_cache::acquire` answers for a 65th key. It hands out a fresh,
// uncached id (renderer.cppm:456) — a fresh id per call is the "no pool" behaviour, and
// pinning it would make the better outcome look like a regression.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.gfx;

namespace {

/// `LUMEN_CHECK` repeats its expression; these checks say which frame, key and slot.
void expect(bool ok, const std::string& what) {
    if (!ok)
        throw lumen_test::assertion_error(what);
}

// How many simulated frames a pool is asked about: a pool that created one object a frame
// would have created this many by the end.
constexpr unsigned k_frames = 64;

// ── The pipeline cache ─────────────────────────────────────────────────────────────────

/// The `k`-th of `count` (blend, format) keys: `formats` formats under each blend.
constexpr std::pair<unsigned, unsigned> key_of(unsigned k, unsigned formats) {
    return {k / formats, k % formats + 1u};
}

// ── The device the semaphore rows run on ────────────────────────────────────────────────

/// Why the Vulkan rows cannot run, or null when they can.
const char* g_no_vulkan = nullptr;
/// The device the Vulkan rows run on.
std::string g_vulkan_device;

/// The device `device_selector::select_best()` picks, brought up once for the process.
lx::gfx::device& the_device() {
    static lx::gfx::device device = [] {
        auto selected = lx::gfx::device_selector::select_best();
        if (!selected) {
            g_no_vulkan = "no graphics device";
            return lx::gfx::device{};
        }
        lx::gfx::device got = std::move(selected).value();
        if (!got.vk_device()) {
            g_no_vulkan = "the selected device has no Vulkan device (the headless fallback)";
            return lx::gfx::device{};
        }
        g_vulkan_device = got.info().device_name;
        return got;
    }();
    return device;
}

/// True when the Vulkan rows can run; names the device once, and says why when they cannot.
bool have_vulkan(const char* row) {
    (void)the_device();
    if (!g_no_vulkan) {
        static const bool announced = [] {
            std::println("    Vulkan rows EXECUTE on \"{}\"{}", g_vulkan_device,
                         the_device().info().is_software_rasterizer ? " (a software rasterizer)"
                                                                    : "");
            return true;
        }();
        (void)announced;
        return true;
    }
    std::println("    SKIP: {} — {}", row, g_no_vulkan);
    return false;
}

/// A semaphore pool that gives its semaphores back to the device when it goes: the class has
/// no destructor, so `reset()` is the only way.
struct scoped_pool {
    lx::gfx::timeline_semaphore_pool pool;
    explicit scoped_pool(void* vk_device) : pool(vk_device) {}
    ~scoped_pool() { pool.reset(); }
    scoped_pool(const scoped_pool&) = delete;
    scoped_pool& operator=(const scoped_pool&) = delete;
};

using handle = lx::gfx::timeline_semaphore_handle;

/// An acquired handle: a real semaphore, in a slot of the pool.
handle acquired(lx::gfx::timeline_semaphore_pool& pool, const char* what) {
    const auto got = pool.acquire();
    expect(got.has_value(), std::format("{}: acquire failed: {}", what,
                                        got.has_value() ? "" : got.get_error().message));
    expect(got->vk_semaphore != nullptr, std::format("{}: a null semaphore", what));
    return *got;
}

/// The error of an acquire that must have been refused: the pool's exhaustion error.
void exhausted(const lx::result<handle>& got, const char* what) {
    expect(!got.has_value(), std::format("{}: the full pool handed out another semaphore", what));
    expect(got.get_error().domain == lx::error_domain::vulkan &&
               got.get_error().code == std::to_underlying(lx::vulkan_err::device_lost),
           std::format("{}: the full pool must answer with vulkan_err::device_lost", what));
}

} // namespace

// ── pipeline_cache ─────────────────────────────────────────────────────────────────────

// `pipeline_cache::acquire` (renderer.cppm:445-457): a frame's keys answered from the
// table, each with the id an earlier frame gave it.
LUMEN_TEST(pipeline_cache_answers_each_key_with_its_own_id_every_frame) {
    constexpr unsigned k_keys = 8;
    lx::gfx::pipeline_cache cache;
    std::array<unsigned, k_keys> id{};
    for (unsigned frame = 0; frame < k_frames; ++frame)
        for (unsigned call = 0; call < k_keys; ++call) {
            // Another order every frame: the id belongs to the key, not to the call.
            const unsigned k = (call + frame) % k_keys;
            const auto [blend, format] = key_of(k, 4);
            const unsigned got = cache.acquire(blend, format);
            if (frame == 0) {
                id[k] = got;
                continue;
            }
            expect(got == id[k],
                   std::format("frame {}: key ({}, {}) was answered {} after {} on frame 0", frame,
                               blend, format, got, id[k]));
        }
    std::array<bool, k_keys> seen{};
    for (const unsigned value : id) {
        expect(value >= 1u && value <= k_keys,
               std::format("{} keys must take the ids 1..{}; one took {}", k_keys, k_keys, value));
        seen[value - 1u] = true;
    }
    for (const bool own : seen)
        expect(own, "every key must have an id of its own");

    // A new key afterwards takes the next id: none was burned on the way.
    const unsigned fresh = cache.acquire(7u, 7u);
    expect(fresh == k_keys + 1u,
           std::format("a new key after {} frames must take id {}; it took {}", k_frames,
                       k_keys + 1u, fresh));
}

// A full table (`pipeline_cache::k_capacity` = 64 keys): the same ids every frame, and again
// after `clear()`, which forgets the keys but must leave a table that works. What a 65th key
// gets is not pinned (the file comment says why).
LUMEN_TEST(pipeline_cache_keeps_a_full_table_stable_across_frames) {
    constexpr unsigned k_keys = 64;
    lx::gfx::pipeline_cache cache;
    const auto run_frames = [&](const char* table) {
        std::array<unsigned, k_keys> id{};
        for (unsigned frame = 0; frame < k_frames; ++frame)
            for (unsigned call = 0; call < k_keys; ++call) {
                const unsigned k = (call + frame) % k_keys;
                const auto [blend, format] = key_of(k, 8);
                const unsigned got = cache.acquire(blend, format);
                if (frame == 0) {
                    id[k] = got;
                    continue;
                }
                expect(got == id[k],
                       std::format("{}: frame {}: key ({}, {}) was answered {} after {} on frame 0",
                                   table, frame, blend, format, got, id[k]));
            }
        const std::set<unsigned> unique(id.begin(), id.end());
        expect(unique.size() == k_keys,
               std::format("{}: {} keys must have {} ids of their own; they had {}", table, k_keys,
                           k_keys, unique.size()));
    };
    run_frames("a full table");
    cache.clear();
    run_frames("a cleared table");
}

// ── timeline_semaphore_pool ─────────────────────────────────────────────────────────────

// A pool with no device answers `not_implemented` (AGENTS.md §3.4), whatever the host has.
LUMEN_TEST(semaphore_pool_without_a_device_is_not_implemented) {
    lx::gfx::timeline_semaphore_pool pool;
    const auto got = pool.acquire();
    expect(!got.has_value(), "a pool with no device must not hand out a semaphore");
    expect(got.get_error().domain == lx::error_domain::not_implemented,
           "a pool with no device must answer not_implemented");
}

// `timeline_semaphore_pool::acquire` / `release` (semaphore_pool.cppm:45-96): frames that
// take `k_held` semaphores and give them all back get the same ones — the same Vulkan
// semaphore in the same slot — every frame. Nothing is created after the first.
LUMEN_TEST(semaphore_pool_hands_back_the_released_semaphores_every_frame) {
    if (!have_vulkan("semaphore_pool_hands_back_the_released_semaphores_every_frame"))
        return;
    constexpr unsigned k_held = 8;
    scoped_pool pools{the_device().vk_device()};
    std::array<handle, k_held> first{};
    for (unsigned frame = 0; frame < k_frames; ++frame) {
        std::array<handle, k_held> held{};
        for (unsigned h = 0; h < k_held; ++h) {
            held[h] = acquired(pools.pool, std::format("frame {}, acquire {}", frame, h).c_str());
            if (frame == 0) {
                first[h] = held[h];
                continue;
            }
            bool known = false;
            for (const handle& old : first)
                known = known || (held[h].pool_index == old.pool_index &&
                                  held[h].vk_semaphore == old.vk_semaphore);
            expect(known,
                   std::format("frame {}: acquire {} got a semaphore the first frame never had "
                               "(slot {})",
                               frame, h, held[h].pool_index));
        }
        // Given back in the other order than they were taken.
        for (unsigned h = k_held; h-- > 0;)
            pools.pool.release(held[h]);
    }
}

// The cap (semaphore_pool.cppm:57-58): 32 held, and the next acquire is the pool's
// exhaustion error — for as long as the pool stays full. One released, the next acquire gets
// that semaphore back in its slot.
LUMEN_TEST(semaphore_pool_exhausts_at_32_and_reuses_a_released_slot) {
    if (!have_vulkan("semaphore_pool_exhausts_at_32_and_reuses_a_released_slot"))
        return;
    constexpr unsigned k_capacity = 32; // timeline_semaphore_pool::k_capacity
    scoped_pool pools{the_device().vk_device()};
    std::array<handle, k_capacity> held{};
    std::array<bool, k_capacity> slot_taken{};
    for (unsigned h = 0; h < k_capacity; ++h) {
        held[h] = acquired(pools.pool, std::format("acquire {}", h).c_str());
        expect(held[h].pool_index < k_capacity,
               std::format("acquire {} took slot {}, past the capacity", h, held[h].pool_index));
        expect(!slot_taken[held[h].pool_index],
               std::format("acquire {} was handed a slot an earlier acquire still holds", h));
        slot_taken[held[h].pool_index] = true;
    }
    exhausted(pools.pool.acquire(), "the 33rd acquire");
    // However many more come: still an error, never growth.
    for (unsigned more = 0; more < k_frames; ++more)
        exhausted(pools.pool.acquire(), std::format("a further acquire ({})", more).c_str());

    constexpr unsigned k_released = 17;
    pools.pool.release(held[k_released]);
    const handle again = acquired(pools.pool, "the acquire after one release");
    expect(again.pool_index == held[k_released].pool_index &&
               again.vk_semaphore == held[k_released].vk_semaphore,
           std::format("the acquire after releasing slot {} must get that semaphore back; it "
                       "got slot {}",
                       held[k_released].pool_index, again.pool_index));
    exhausted(pools.pool.acquire(), "the acquire after the pool filled again");
}

// `reset()` (semaphore_pool.cppm:98-109) gives every semaphore back and empties the pool:
// the next acquire starts at the first slot again, with a semaphore of its own.
LUMEN_TEST(semaphore_pool_reset_empties_the_pool) {
    if (!have_vulkan("semaphore_pool_reset_empties_the_pool"))
        return;
    scoped_pool pools{the_device().vk_device()};
    for (unsigned h = 0; h < 4; ++h)
        (void)acquired(pools.pool, std::format("acquire {}", h).c_str());
    pools.pool.reset();
    const handle got = acquired(pools.pool, "the acquire after reset");
    expect(got.pool_index == 0,
           std::format("an empty pool must hand out its first slot; it handed out slot {}",
                       got.pool_index));
    const handle next = acquired(pools.pool, "the second acquire after reset");
    expect(next.pool_index != got.pool_index && next.vk_semaphore != got.vk_semaphore,
           "two semaphores held at once must be two semaphores");
}

// ── The report ──────────────────────────────────────────────────────────────────────────

// What ran on this host, said in the output: the semaphore rows are the row's evidence only
// where they execute.
LUMEN_TEST(gpu_pools_report_says_what_ran) {
    (void)the_device();
    if (g_no_vulkan)
        std::println("  Vulkan rows NOT EXECUTED ({}): on this host the semaphore half of the "
                     "\"GPU pools\" row is not evidenced",
                     g_no_vulkan);
    else
        std::println("  Vulkan rows executed on \"{}\"", g_vulkan_device);
    std::println("  pipeline_cache rows executed (they need no device)");
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
