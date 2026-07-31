#include "lumen_test.hpp"

import lx.foundation;
import lx.gfx;

namespace {

lx::gfx::dmabuf_desc make_desc() {
    lx::gfx::dmabuf_desc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.plane_count = 1;
    desc.planes[0].stride = 64 * 4;
    return desc;
}

lx::gfx::import_cache_key make_key(unsigned client, unsigned buffer) {
    lx::gfx::import_cache_key key{};
    key.client = lx::client_id{client};
    key.buffer = lx::buffer_id{buffer};
    return key;
}

/// Insert `n` buffers for `client`, all released so they are evictable.
void fill(lx::gfx::dmabuf_import_cache& cache, unsigned client, unsigned n) {
    const auto desc = make_desc();
    for (unsigned i = 0; i < n; ++i) {
        const auto key = make_key(client, i);
        LUMEN_CHECK(static_cast<bool>(cache.acquire(key, desc)));
        cache.release(key);
    }
}

} // namespace

LUMEN_TEST(import_cache_empty_tick_does_not_underflow) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};

    LUMEN_CHECK(cache.size() == 0);
    cache.tick(1);
    LUMEN_CHECK(cache.size() == 0);
    LUMEN_CHECK(cache.evictions() == 0);
}

LUMEN_TEST(import_cache_release_unknown_key_is_safe) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};
    cache.release(make_key(1, 2));
    LUMEN_CHECK(cache.size() == 0);
}

LUMEN_TEST(import_cache_acquire_hits_on_same_key) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};
    const auto desc = make_desc();
    const auto key = make_key(1, 1);

    auto first = cache.acquire(key, desc);
    LUMEN_CHECK(static_cast<bool>(first));
    auto second = cache.acquire(key, desc);
    LUMEN_CHECK(static_cast<bool>(second));

    LUMEN_CHECK(cache.size() == 1);
    LUMEN_CHECK(cache.hits() == 1);
    LUMEN_CHECK(cache.misses() == 1);
}

// Disconnect must drop every entry for the client, including entries that
// compaction moves into a slot the loop has already passed.
LUMEN_TEST(import_cache_evict_client_drops_all_entries) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};

    fill(cache, 1, 4);
    fill(cache, 2, 4);
    LUMEN_CHECK(cache.size() == 8);
    LUMEN_CHECK(cache.entries_for_client(lx::client_id{1}) == 4);

    cache.evict_client(lx::client_id{1});

    LUMEN_CHECK(cache.entries_for_client(lx::client_id{1}) == 0);
    LUMEN_CHECK(cache.entries_for_client(lx::client_id{2}) == 4);
    LUMEN_CHECK(cache.size() == 4);
    LUMEN_CHECK(cache.evictions() == 4);
}

// Contiguous-client layout is the case where compaction skipping is easiest to hit.
LUMEN_TEST(import_cache_evict_client_handles_trailing_entries) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::dmabuf_import_cache cache{importer, {}};

    fill(cache, 1, 3);
    cache.evict_client(lx::client_id{1});
    LUMEN_CHECK(cache.size() == 0);
    LUMEN_CHECK(cache.entries_for_client(lx::client_id{1}) == 0);
}

LUMEN_TEST(import_cache_tick_evicts_every_stale_entry) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::import_cache_config cfg{};
    cfg.stale_frame_threshold = 10;
    lx::gfx::dmabuf_import_cache cache{importer, cfg};

    fill(cache, 1, 5);
    LUMEN_CHECK(cache.size() == 5);

    cache.tick(100);
    LUMEN_CHECK(cache.size() == 0);
    LUMEN_CHECK(cache.evictions() == 5);
}

LUMEN_TEST(import_cache_never_evicts_in_use_entries) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::import_cache_config cfg{};
    cfg.stale_frame_threshold = 10;
    lx::gfx::dmabuf_import_cache cache{importer, cfg};

    const auto desc = make_desc();
    const auto held = make_key(1, 99);
    LUMEN_CHECK(static_cast<bool>(cache.acquire(held, desc))); // still referenced
    fill(cache, 1, 3);

    cache.tick(100);
    LUMEN_CHECK(cache.size() == 1);
    LUMEN_CHECK(cache.entries_for_client(lx::client_id{1}) == 1);
}

LUMEN_TEST(import_cache_respects_max_entries) {
    lx::gfx::dmabuf_importer importer{nullptr, nullptr};
    lx::gfx::import_cache_config cfg{};
    cfg.max_entries = 4;
    cfg.max_entries_per_client = 64;
    cfg.stale_frame_threshold = 1000;
    lx::gfx::dmabuf_import_cache cache{importer, cfg};

    fill(cache, 1, 10);
    cache.tick(1);
    LUMEN_CHECK(cache.size() <= cfg.max_entries);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
