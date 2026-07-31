module;

import lx.foundation;
import lx.runtime;

export module lx.gfx:import_cache;

import :dmabuf;

export namespace lx::gfx {

struct import_cache_key {
    lx::client_id client{};
    lx::buffer_id buffer{};
    unsigned long long modifier = 0;

    [[nodiscard]] constexpr bool operator==(const import_cache_key&) const = default;
};

struct cached_import {
    imported_image image{};
    unsigned use_count = 0;
    unsigned frame_last_used = 0;
    unsigned estimated_bytes = 0;
    bool valid = false;
};

struct import_cache_config {
    unsigned max_entries = 512;
    unsigned max_entries_per_client = 64;
    unsigned stale_frame_threshold = 120;
};

/// Reuse VkImage imports keyed by (client, buffer_id, modifier) — avoids re-import per frame.
class dmabuf_import_cache {
public:
    explicit dmabuf_import_cache(dmabuf_importer& importer, import_cache_config config = {});

    void set_config(import_cache_config config);
    [[nodiscard]] import_cache_config config() const;

    [[nodiscard]] lx::result<imported_image> acquire(const import_cache_key& key,
                                                     const dmabuf_desc& desc);
    void release(const import_cache_key& key);
    void evict_client(lx::client_id client);

    /// Per-frame: LRU timestamps, stale eviction, pressure response.
    void tick(unsigned frame_index);
    void on_memory_pressure(lx::runtime::memory_pressure level);

    [[nodiscard]] unsigned size() const;
    [[nodiscard]] unsigned entries_for_client(lx::client_id client) const;
    [[nodiscard]] unsigned hits() const;
    [[nodiscard]] unsigned misses() const;
    [[nodiscard]] unsigned evictions() const;

private:
    [[nodiscard]] int find_slot(const import_cache_key& key) const;
    [[nodiscard]] int find_lru_victim() const;
    [[nodiscard]] int find_client_lru_victim(lx::client_id client) const;
    void evict_slot(unsigned index);
    void evict_lru(unsigned count);

    dmabuf_importer* importer_ = nullptr;
    import_cache_config config_{};
    static constexpr unsigned k_capacity = 512;
    import_cache_key keys_[512]{};
    cached_import entries_[512]{};
    unsigned count_ = 0;
    unsigned hits_ = 0;
    unsigned misses_ = 0;
    unsigned evictions_ = 0;
};

} // namespace lx::gfx


unsigned lx_gfx_estimate_dmabuf_bytes(const lx::gfx::dmabuf_desc& desc) {
    unsigned total = 0;
    for (unsigned i = 0; i < desc.plane_count && i < 4; ++i)
        total += desc.planes[i].stride * desc.height;
    return total > 0 ? total : desc.width * desc.height * 4;
}

int lx::gfx::dmabuf_import_cache::find_slot(const import_cache_key& key) const {
    for (unsigned i = 0; i < count_; ++i)
        if (keys_[i] == key && entries_[i].valid) return static_cast<int>(i);
    return -1;
}

int lx::gfx::dmabuf_import_cache::find_lru_victim() const {
    int best = -1;
    unsigned oldest = 0;
    for (unsigned i = 0; i < count_; ++i) {
        if (!entries_[i].valid) return static_cast<int>(i);
        // Never evict an image the compositor still holds.
        if (entries_[i].use_count > 0) continue;
        if (best < 0 || entries_[i].frame_last_used < oldest) {
            oldest = entries_[i].frame_last_used;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int lx::gfx::dmabuf_import_cache::find_client_lru_victim(lx::client_id client) const {
    int best = -1;
    unsigned oldest = 0;
    for (unsigned i = 0; i < count_; ++i) {
        if (!entries_[i].valid || keys_[i].client != client) continue;
        if (entries_[i].use_count > 0) continue;
        if (best < 0 || entries_[i].frame_last_used < oldest) {
            oldest = entries_[i].frame_last_used;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void lx::gfx::dmabuf_import_cache::evict_slot(unsigned index) {
    if (index >= count_)
        return;
    if (entries_[index].valid) {
        if (importer_)
            importer_->release(entries_[index].image);
        entries_[index] = {};
        keys_[index] = {};
        ++evictions_;
    }
    // Compact: swap with last valid entry so count_ stays accurate.
    if (index + 1 < count_) {
        entries_[index] = entries_[count_ - 1];
        keys_[index] = keys_[count_ - 1];
        entries_[count_ - 1] = {};
        keys_[count_ - 1] = {};
    }
    if (count_ > 0)
        --count_;
}

void lx::gfx::dmabuf_import_cache::evict_lru(unsigned evict_count) {
    for (unsigned n = 0; n < evict_count && count_ > 0; ++n) {
        const int victim = find_lru_victim();
        if (victim < 0) break;
        evict_slot(static_cast<unsigned>(victim));
    }
}

lx::gfx::dmabuf_import_cache::dmabuf_import_cache(dmabuf_importer& importer,
                                                    import_cache_config config)
    : importer_{&importer}, config_{config} {}

void lx::gfx::dmabuf_import_cache::set_config(import_cache_config config) { config_ = config; }
lx::gfx::import_cache_config lx::gfx::dmabuf_import_cache::config() const { return config_; }

unsigned lx::gfx::dmabuf_import_cache::entries_for_client(lx::client_id client) const {
    unsigned n = 0;
    for (unsigned i = 0; i < count_; ++i)
        if (entries_[i].valid && keys_[i].client == client) ++n;
    return n;
}

lx::result<lx::gfx::imported_image>
lx::gfx::dmabuf_import_cache::acquire(const import_cache_key& key, const dmabuf_desc& desc) {
    if (const int slot = find_slot(key); slot >= 0) {
        ++entries_[static_cast<unsigned>(slot)].use_count;
        ++hits_;
        return entries_[static_cast<unsigned>(slot)].image;
    }
    ++misses_;

    if (entries_for_client(key.client) >= config_.max_entries_per_client) {
        if (const int victim = find_client_lru_victim(key.client); victim >= 0)
            evict_slot(static_cast<unsigned>(victim));
        else
            return lx::make_error(lx::error_domain::vulkan, 0, "client import cache full");
    }

    if (count_ >= config_.max_entries)
        evict_lru(1);

    if (!importer_) return lx::not_implemented("lx::gfx::dmabuf_import_cache::acquire");
    auto imported = importer_->import(desc);
    if (!imported)
        return imported.get_error();
    imported_image image = static_cast<decltype(imported)&&>(imported).value();

    if (count_ < k_capacity) {
        keys_[count_] = key;
        entries_[count_] = {image, 1, 0, lx_gfx_estimate_dmabuf_bytes(desc), true};
        ++count_;
    }
    return image;
}

void lx::gfx::dmabuf_import_cache::release(const import_cache_key& key) {
    if (const int slot = find_slot(key); slot >= 0 && entries_[static_cast<unsigned>(slot)].use_count > 0)
        --entries_[static_cast<unsigned>(slot)].use_count;
}

void lx::gfx::dmabuf_import_cache::evict_client(lx::client_id client) {
    // evict_slot compacts the last entry into `i`, so only advance when the
    // slot survives — otherwise the swapped-in entry is never examined.
    for (unsigned i = 0; i < count_;) {
        if (keys_[i].client == client)
            evict_slot(i);
        else
            ++i;
    }
}

void lx::gfx::dmabuf_import_cache::tick(unsigned frame_index) {
    for (unsigned i = 0; i < count_;) {
        if (entries_[i].valid && entries_[i].use_count == 0 &&
            frame_index - entries_[i].frame_last_used >= config_.stale_frame_threshold) {
            evict_slot(i);
            continue;
        }
        if (entries_[i].valid && entries_[i].use_count > 0)
            entries_[i].frame_last_used = frame_index;
        ++i;
    }
    if (count_ > config_.max_entries)
        evict_lru(count_ - config_.max_entries);
}

void lx::gfx::dmabuf_import_cache::on_memory_pressure(lx::runtime::memory_pressure level) {
    if (level == lx::runtime::memory_pressure::normal) return;
    const unsigned evict = level == lx::runtime::memory_pressure::critical
                               ? count_ / 2 + 1
                               : count_ / 4 + 1;
    evict_lru(evict);
}

unsigned lx::gfx::dmabuf_import_cache::size() const { return count_; }
unsigned lx::gfx::dmabuf_import_cache::hits() const { return hits_; }
unsigned lx::gfx::dmabuf_import_cache::misses() const { return misses_; }
unsigned lx::gfx::dmabuf_import_cache::evictions() const { return evictions_; }
