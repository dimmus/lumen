// Pixel staging between a commit (UI thread) and an upload (render thread): the GL path's
// transient upload pool, the CPU path's per-texture store, and the buffer-age region that
// decides how much of a buffer is copied at all. These are what kept the compositor's heap
// growing by two window-sized copies per buffer it had ever seen.
#include "lumen_test.hpp"

#include <cstring>

import lx.foundation;
import lx.compositor;

using lx::compositor::buffer_upload_state;
using lx::compositor::shm_direct_uploads;
using lx::compositor::shm_pixel_store;
using lx::compositor::shm_upload_pool;
using lx::compositor::shm_upload_region;
using lx::compositor::surface_damage_history;

namespace {
constexpr unsigned long long k_ms = 1'000'000ull;
constexpr unsigned long long k_idle = 1000 * k_ms;

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
} // namespace

LUMEN_TEST(pool_recycles_a_released_block_without_mapping_more) {
    shm_upload_pool pool;
    unsigned long long t1 = 0;
    unsigned char* a = pool.acquire(300 * 1024, t1, 1);
    LUMEN_CHECK(a != nullptr && t1 != 0);
    std::memset(a, 0x5a, 300 * 1024); // the storage is real and writable
    const auto mapped = pool.bytes_mapped();
    LUMEN_CHECK(mapped >= 300u * 1024u);
    pool.release(t1);

    unsigned long long t2 = 0;
    unsigned char* b = pool.acquire(200 * 1024, t2, 2);
    LUMEN_CHECK(b == a); // the cached block, not a new mapping
    LUMEN_CHECK(pool.bytes_mapped() == mapped);
    pool.release(t2);
}

LUMEN_TEST(pool_never_hands_out_a_block_in_flight) {
    shm_upload_pool pool;
    unsigned long long t1 = 0, t2 = 0;
    unsigned char* a = pool.acquire(64 * 1024, t1, 1);
    unsigned char* b = pool.acquire(64 * 1024, t2, 1);
    LUMEN_CHECK(a && b && a != b);
    LUMEN_CHECK(pool.blocks_mapped() == 2);
    pool.release(t1);
    pool.release(t2);
}

LUMEN_TEST(pool_prefers_the_smallest_block_that_fits) {
    shm_upload_pool pool;
    unsigned long long big = 0, small = 0;
    unsigned char* b = pool.acquire(4 * 1024 * 1024, big, 1);
    unsigned char* s = pool.acquire(64 * 1024, small, 1);
    pool.release(big);
    pool.release(small);
    unsigned long long t = 0;
    // A damaged strip must not take the window-sized block while a strip-sized one is free.
    LUMEN_CHECK(pool.acquire(10 * 1024, t, 2) == s);
    pool.release(t);
    LUMEN_CHECK(b != nullptr);
}

LUMEN_TEST(pool_unmaps_idle_blocks_and_keeps_busy_ones) {
    shm_upload_pool pool;
    unsigned long long idle = 0, busy = 0;
    (void)pool.acquire(1024 * 1024, idle, 0);
    (void)pool.acquire(1024 * 1024, busy, 0);
    pool.release(idle);
    pool.collect(k_idle / 2, k_idle);
    LUMEN_CHECK(pool.blocks_mapped() == 2); // not idle long enough yet
    pool.collect(2 * k_idle, k_idle);
    LUMEN_CHECK(pool.blocks_mapped() == 1); // the busy one stays: the render thread holds it
    pool.release(busy);
    pool.collect(4 * k_idle, k_idle);
    LUMEN_CHECK(pool.blocks_mapped() == 0);
    LUMEN_CHECK(pool.bytes_mapped() == 0);
}

LUMEN_TEST(pool_trim_drops_every_spare_block) {
    shm_upload_pool pool;
    unsigned long long a = 0, b = 0;
    (void)pool.acquire(512 * 1024, a, 0);
    (void)pool.acquire(512 * 1024, b, 0);
    pool.release(a);
    pool.trim();
    LUMEN_CHECK(pool.blocks_mapped() == 1);
    pool.release(b);
    pool.trim();
    LUMEN_CHECK(pool.bytes_mapped() == 0);
}

LUMEN_TEST(pool_refuses_when_every_block_is_in_flight) {
    shm_upload_pool pool;
    unsigned long long tokens[shm_upload_pool::k_max_blocks]{};
    for (auto& t : tokens)
        LUMEN_CHECK(pool.acquire(64 * 1024, t, 0) != nullptr);
    unsigned long long extra = 0;
    LUMEN_CHECK(pool.acquire(64 * 1024, extra, 0) == nullptr);
    LUMEN_CHECK(extra == 0);
    for (auto t : tokens)
        pool.release(t);
    pool.trim();
}

LUMEN_TEST(store_frees_a_retired_texture_once_nothing_samples_it) {
    shm_pixel_store store;
    unsigned long long token = 0;
    LUMEN_CHECK(store.acquire(7, 1024 * 1024, token) != nullptr);
    LUMEN_CHECK(store.bytes_held() == 1024u * 1024u);

    store.retire(7);
    store.collect();
    LUMEN_CHECK(store.bytes_held() == 1024u * 1024u); // the composite still samples it

    store.release(token); // the render thread applied the forget
    store.collect();
    LUMEN_CHECK(store.bytes_held() == 0); // returned, not merely cleared
}

LUMEN_TEST(store_without_retirement_keeps_live_textures) {
    shm_pixel_store store;
    unsigned long long token = 0;
    (void)store.acquire(9, 4096, token);
    store.release(token);
    store.collect();
    LUMEN_CHECK(store.bytes_held() == 4096u);
}

LUMEN_TEST(region_is_whole_for_the_first_two_uploads_of_a_texture) {
    surface_damage_history h;
    buffer_upload_state st;
    const lx::surface_id s{1};
    h.record({10, 10, 5, 5});
    LUMEN_CHECK(same(shm_upload_region(h, st, s, 100, 50), {0, 0, 100, 50}));
    h.record({10, 10, 5, 5});
    // The second GL name has never been written.
    LUMEN_CHECK(same(shm_upload_region(h, st, s, 100, 50), {0, 0, 100, 50}));
}

LUMEN_TEST(region_is_buffer_age_damage_plus_the_previous_upload) {
    surface_damage_history h;
    buffer_upload_state st;
    const lx::surface_id s{1};
    h.record({0, 0, 100, 50});
    (void)shm_upload_region(h, st, s, 100, 50);
    h.record({20, 0, 10, 10});
    (void)shm_upload_region(h, st, s, 100, 50); // damage {20,0,10,10}, staged whole
    h.record({60, 30, 5, 5});
    // This name was written two uploads ago: it needs {20,0,10,10} and {60,30,5,5}.
    LUMEN_CHECK(same(shm_upload_region(h, st, s, 100, 50), {20, 0, 45, 35}));
}

LUMEN_TEST(region_accumulates_damage_from_commits_of_other_buffers) {
    // Two buffers on one surface: damage is relative to the surface's previous content,
    // so buffer A's staleness includes what changed while B was shown.
    surface_damage_history h;
    buffer_upload_state a, b;
    const lx::surface_id s{1};
    h.record({0, 0, 100, 50});
    (void)shm_upload_region(h, a, s, 100, 50);
    h.record({0, 0, 100, 50});
    (void)shm_upload_region(h, b, s, 100, 50);
    h.record({0, 0, 1, 1});
    (void)shm_upload_region(h, a, s, 100, 50);
    h.record({0, 0, 1, 1});
    (void)shm_upload_region(h, b, s, 100, 50);
    // A again. Its previous upload carried B's whole first frame as damage, so the name
    // being written is stale everywhere: whole.
    h.record({50, 40, 2, 2});
    LUMEN_CHECK(same(shm_upload_region(h, a, s, 100, 50), {0, 0, 100, 50}));
    h.record({0, 0, 1, 1});
    (void)shm_upload_region(h, b, s, 100, 50);
    // A once more: changed since its last upload are {0,0,1,1} (shown in B) and {90,0,1,1};
    // its other name also missed the previous upload's damage, {0,0,52,42}.
    h.record({90, 0, 1, 1});
    LUMEN_CHECK(same(shm_upload_region(h, a, s, 100, 50), {0, 0, 91, 42}));
}

LUMEN_TEST(region_is_whole_when_the_history_ran_out_or_the_surface_changed) {
    surface_damage_history h;
    buffer_upload_state st;
    const lx::surface_id s{1};
    for (int i = 0; i < 2; ++i) {
        h.record({0, 0, 100, 50});
        (void)shm_upload_region(h, st, s, 100, 50);
    }
    for (unsigned i = 0; i < surface_damage_history::k_depth + 1; ++i)
        h.record({1, 1, 1, 1}); // other buffers shown for longer than the history keeps
    LUMEN_CHECK(same(shm_upload_region(h, st, s, 100, 50), {0, 0, 100, 50}));

    surface_damage_history other;
    other.record({1, 1, 1, 1});
    LUMEN_CHECK(same(shm_upload_region(other, st, lx::surface_id{2}, 100, 50), {0, 0, 100, 50}));
}

// ── direct uploads: the client's buffer is held until the render thread has read it ──

namespace {
int g_released = 0;
void* g_last_released = nullptr;
int g_unrefs = 0;
int g_access_depth = 0;
int g_accesses = 0;

shm_direct_uploads::hooks counting_hooks() {
    g_released = g_unrefs = g_access_depth = g_accesses = 0;
    g_last_released = nullptr;
    shm_direct_uploads::hooks h{};
    h.begin_access = [](void*) { ++g_access_depth; ++g_accesses; };
    h.end_access = [](void*) { --g_access_depth; };
    h.release = [](void* b) { ++g_released; g_last_released = b; };
    h.unref_pool = [](void*) { ++g_unrefs; };
    return h;
}

// Stand-ins for the wl_buffer, wl_shm_buffer and wl_shm_pool: only identity matters.
int g_buffer_a, g_buffer_b, g_shm_a, g_shm_b, g_pool;
} // namespace

LUMEN_TEST(direct_upload_releases_only_after_the_render_thread_read_it) {
    shm_direct_uploads t;
    shm_upload_pool staging;
    t.bind(counting_hooks(), &staging);
    unsigned char pixels[16 * 4]{};
    const unsigned h = t.add(&g_buffer_a, &g_shm_a, &g_pool, pixels, 16 * 4, {0, 0, 16, 1});
    LUMEN_CHECK(h != 0);
    t.retire();
    LUMEN_CHECK(g_released == 0); // the client must not draw into it yet
    LUMEN_CHECK(!t.idle());

    const unsigned char* seen = nullptr;
    t.upload(h, [&](const unsigned char* p, unsigned) {
        seen = p;
        LUMEN_CHECK(g_access_depth == 1); // read inside the SIGBUS guard
    });
    LUMEN_CHECK(seen == pixels);
    LUMEN_CHECK(g_access_depth == 0);
    t.retire();
    LUMEN_CHECK(g_released == 1 && g_last_released == &g_buffer_a);
    LUMEN_CHECK(g_unrefs == 1);
    LUMEN_CHECK(t.idle());
}

LUMEN_TEST(direct_upload_of_a_recommitted_buffer_releases_it_once_after_the_last_read) {
    shm_direct_uploads t;
    t.bind(counting_hooks(), nullptr);
    unsigned char pixels[4]{};
    const unsigned first = t.add(&g_buffer_a, &g_shm_a, &g_pool, pixels, 4, {0, 0, 1, 1});
    const unsigned second = t.add(&g_buffer_a, &g_shm_a, &g_pool, pixels, 4, {0, 0, 1, 1});
    t.upload(first, [](const unsigned char*, unsigned) {});
    t.retire();
    // The second commit still has to read it: releasing now would hand the client a
    // buffer the compositor is about to read.
    LUMEN_CHECK(g_released == 0);
    t.upload(second, [](const unsigned char*, unsigned) {});
    t.retire();
    LUMEN_CHECK(g_released == 1);
    LUMEN_CHECK(g_unrefs == 2); // each commit held its own pool reference
}

LUMEN_TEST(direct_upload_keeps_a_copy_when_the_client_destroys_the_buffer_first) {
    shm_direct_uploads t;
    shm_upload_pool staging;
    t.bind(counting_hooks(), &staging);
    // A 4x2 region of a buffer with an 8-pixel stride, rows marked so the copy is checkable.
    unsigned char buffer[8 * 4 * 2]{};
    for (unsigned i = 0; i < sizeof(buffer); ++i)
        buffer[i] = static_cast<unsigned char>(i);
    const unsigned h = t.add(&g_buffer_b, &g_shm_b, &g_pool, buffer, 8 * 4, {0, 0, 4, 2});
    t.on_buffer_gone(&g_buffer_b);
    LUMEN_CHECK(g_accesses == 1); // copied under the guard, while still readable

    bool ok = true;
    unsigned stride_seen = 0;
    t.upload(h, [&](const unsigned char* p, unsigned stride) {
        stride_seen = stride;
        ok = p && p != buffer && std::memcmp(p, buffer, 16) == 0 &&
             std::memcmp(p + 16, buffer + 32, 16) == 0;
    });
    LUMEN_CHECK(ok);
    LUMEN_CHECK(stride_seen == 16); // packed
    LUMEN_CHECK(g_accesses == 1);   // the upload did not touch the destroyed buffer
    t.retire();
    LUMEN_CHECK(g_released == 0); // nothing to release: the object is gone
    LUMEN_CHECK(g_unrefs == 1);
    staging.trim();
    LUMEN_CHECK(staging.bytes_mapped() == 0); // the copy's block went back
}

LUMEN_TEST(direct_upload_dropped_before_queueing_releases_at_once) {
    shm_direct_uploads t;
    t.bind(counting_hooks(), nullptr);
    unsigned char pixels[4]{};
    const unsigned h = t.add(&g_buffer_a, &g_shm_a, &g_pool, pixels, 4, {0, 0, 1, 1});
    t.drop(h);
    LUMEN_CHECK(g_released == 1 && g_unrefs == 1 && t.idle());
}

LUMEN_TEST(direct_upload_table_full_falls_back) {
    shm_direct_uploads t;
    t.bind(counting_hooks(), nullptr);
    unsigned char pixels[4]{};
    for (unsigned i = 0; i < shm_direct_uploads::k_max; ++i)
        LUMEN_CHECK(t.add(&g_buffer_a, &g_shm_a, &g_pool, pixels, 4, {0, 0, 1, 1}) != 0);
    LUMEN_CHECK(t.add(&g_buffer_a, &g_shm_a, &g_pool, pixels, 4, {0, 0, 1, 1}) == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
