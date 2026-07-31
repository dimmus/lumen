module;

#include <atomic>
#include <vector>

import lx.foundation;
import lx.sync;
import lx.wayland.server;
import lx.gfx;

export module lx.compositor:protocols_p0;

import :surface;
import :toplevel;
import :output;
import :buffer_lifecycle;

export namespace lx::compositor {

/// One client buffer's worth of work for the render thread. Vulkan objects may only be
/// recorded on the render affinity, but commits arrive on the UI thread, so the UI side
/// only ever describes the work — it never touches Vulkan itself.
struct texture_update {
    enum class op : unsigned char {
        none,
        /// Sample a zero-copy dmabuf import; carries the stable VkImage by value.
        dmabuf,
        /// Upload CPU pixels; `pixels` stays valid until the matching `forget`.
        shm,
        forget,
    };

    op kind = op::none;
    unsigned texture_id = 0;
    unsigned width = 0;
    unsigned height = 0;
    lx::fourcc format = 0;
    /// Bytes per row of `pixels`. Staged shm rows are packed, so this is `width * 4`.
    unsigned stride = 0;
    void* vk_image = nullptr;
    void* vk_memory = nullptr;
    /// Plane 0 of the client dma-buf, duplicated for the render thread, which owns it and
    /// must close it. -1 when the update carries no dma-buf.
    ///
    /// The Vulkan path never needs this — it hands over an already-imported `VkImage`. GL
    /// imports on the render thread instead, because that is where its context lives, so
    /// the descriptor has to survive the trip across the queue.
    int dmabuf_fd = -1;
    unsigned dmabuf_offset = 0;
    unsigned dmabuf_stride = 0;
    unsigned long long dmabuf_modifier = 0;
    const unsigned char* pixels = nullptr;
    /// Staging-ring sequence backing `pixels`; the render thread publishes it as consumed.
    /// Set only when `pixels` came from `shm_staging_ring`.
    unsigned long long shm_seq = 0;
    /// `shm_pixel_store` slot backing `pixels`, released once the composite has read it.
    /// Set only when `pixels` came from `shm_pixel_store`.
    unsigned long long shm_token = 0;
    /// `forget` only: the first frame index whose snapshot is guaranteed not to reference
    /// this texture. Applying the forget before that frame has been composited drops a
    /// texture an in-flight snapshot still draws. See `drain_texture_updates`.
    unsigned retire_after_frame = 0;
};

using texture_update_queue = lx::sync::spsc_queue<texture_update, 256>;

/// Staging for converted SHM pixels, owned by the compositor rather than by the client's
/// wl_buffer.
///
/// Tying this storage to the buffer would be a use-after-free: the client can destroy a
/// buffer (or disconnect) while a `texture_update` referencing it is still queued, and
/// FIFO ordering does not keep freed memory alive. Slots are recycled only once the render
/// thread reports them consumed, so a queued pointer always addresses live memory.
class shm_staging_ring {
public:
    static constexpr unsigned k_slots = 8;

    /// UI affinity. Returns writable storage for `bytes`, or nullptr when every slot is
    /// still in flight (the caller should skip the update rather than overwrite one).
    [[nodiscard]] unsigned char* acquire(unsigned bytes, unsigned long long& seq_out);

    /// Render affinity. Marks everything up to and including `seq` reusable.
    void publish_consumed(unsigned long long seq);

private:
    std::vector<unsigned char> slots_[k_slots]{};
    unsigned long long produced_ = 0;
    std::atomic<unsigned long long> consumed_{0};
};

/// Per-surface staging for shm pixels, used by the CPU composite path.
///
/// `shm_staging_ring` is a single FIFO shared by every surface, which is right when the
/// consumer copies the pixels out during the drain (the Vulkan upload does). The CPU
/// compositor instead *samples the staged rows in place* during the composite, so a slot
/// stays live well past the drain and a shared FIFO would let another surface's commits
/// recycle it underneath — the texture would silently start showing someone else's pixels.
///
/// Two slots per texture: the UI thread stages the next commit into one while the render
/// thread still reads the other. A client that runs more than one commit ahead of the
/// present is told to wait (`acquire` returns nullptr) rather than served a slot in flight.
class shm_pixel_store {
public:
    static constexpr unsigned k_max_textures = 64;
    static constexpr unsigned k_slots = 2;

    /// UI affinity. Writable storage of `bytes` for `texture_id`, or nullptr when both
    /// slots are still being sampled. `token_out` is nonzero on success.
    [[nodiscard]] unsigned char* acquire(unsigned texture_id, unsigned bytes,
                                         unsigned long long& token_out);

    /// Render affinity. Marks the slot behind `token` reusable. Ignores a zero token.
    void release(unsigned long long token);

    /// UI affinity. Drops a texture's slots; safe only once nothing references them.
    void forget(unsigned texture_id);

private:
    struct entry {
        unsigned texture_id = 0;
        std::vector<unsigned char> slots[k_slots]{};
        std::atomic<bool> in_flight[k_slots]{};
        unsigned next = 0;
        bool used = false;
    };

    entry entries_[k_max_textures]{};
};

/// Context shared by all P0 protocol handlers (owned by compositor_impl).
struct p0_protocol_context {
    wayland::server* server = nullptr;
    surface_manager* surfaces = nullptr;
    toplevel_manager* toplevels = nullptr;
    output_manager* outputs = nullptr;
    gfx::headless_backend* headless = nullptr;
    gfx::dmabuf_importer* dmabuf = nullptr;
    texture_update_queue* textures = nullptr;
    buffer_lifecycle_tracker* lifecycle = nullptr;
    shm_staging_ring* shm_staging = nullptr;
    /// Set instead of `shm_staging` when the CPU composite path is active.
    shm_pixel_store* shm_store = nullptr;

    /// UI affinity. The frame index currently being built, refreshed by `tick_ui`. Used to
    /// date a texture retirement so the render thread can tell which in-flight snapshots
    /// might still draw it.
    unsigned frame_index = 0;

    /// Stage shm pixels in the client's own channel order instead of converting them to
    /// RGBA. The Vulkan composite path samples RGBA textures, so it needs the conversion;
    /// the CPU path writes a scanout buffer that already uses the client's order, and a
    /// per-pixel swizzle there is a full extra pass over the frame for nothing.
    bool shm_native_format = false;
};

/// Install wl_compositor, wl_subcompositor, xdg_wm_base, zwp_linux_dmabuf_v1,
/// wl_seat, wl_output, wl_data_device_manager with real bind handlers.
[[nodiscard]] lx::result<void> install_p0_protocols(p0_protocol_context& ctx);

/// Fire pending wl_surface.frame callbacks after a present (presentation feedback).
void fire_frame_callbacks(p0_protocol_context& ctx, unsigned frame_time_ms);

/// Sends wl_buffer.release for a dmabuf buffer once the render path is done with it.
void release_wl_buffer_resource(void* wl_buffer_resource, void* user_data);

} // namespace lx::compositor


unsigned char* lx::compositor::shm_staging_ring::acquire(unsigned bytes,
                                                        unsigned long long& seq_out) {
    if (bytes == 0)
        return nullptr;
    if (produced_ - consumed_.load(std::memory_order_acquire) >= k_slots)
        return nullptr;

    // The slot is provably not in flight, so growing it cannot invalidate a pointer the
    // render thread still holds. Never shrink: steady state stops allocating.
    auto& slot = slots_[produced_ % k_slots];
    if (slot.size() < bytes)
        slot.resize(bytes);
    seq_out = produced_++;
    return slot.data();
}

void lx::compositor::shm_staging_ring::publish_consumed(unsigned long long seq) {
    consumed_.store(seq + 1, std::memory_order_release);
}

unsigned char* lx::compositor::shm_pixel_store::acquire(unsigned texture_id, unsigned bytes,
                                                        unsigned long long& token_out) {
    token_out = 0;
    if (texture_id == 0 || bytes == 0)
        return nullptr;

    entry* slot_entry = nullptr;
    unsigned index = 0;
    for (unsigned i = 0; i < k_max_textures; ++i) {
        if (entries_[i].used && entries_[i].texture_id == texture_id) {
            slot_entry = &entries_[i];
            index = i;
            break;
        }
    }
    if (!slot_entry) {
        for (unsigned i = 0; i < k_max_textures; ++i) {
            if (entries_[i].used)
                continue;
            entries_[i].used = true;
            entries_[i].texture_id = texture_id;
            entries_[i].next = 0;
            slot_entry = &entries_[i];
            index = i;
            break;
        }
    }
    // Texture ids never repeat, so without reclaiming entries a long session would fill the
    // table and stop staging anything. An entry with neither slot in flight is provably
    // unreferenced: a texture the compositor still samples always holds exactly one slot,
    // and dropping a texture releases it. Reuse its storage rather than allocate.
    if (!slot_entry) {
        for (unsigned i = 0; i < k_max_textures; ++i) {
            bool free_entry = true;
            for (unsigned s = 0; s < k_slots; ++s)
                free_entry = free_entry && !entries_[i].in_flight[s].load(std::memory_order_acquire);
            if (!free_entry)
                continue;
            entries_[i].texture_id = texture_id;
            entries_[i].next = 0;
            slot_entry = &entries_[i];
            index = i;
            break;
        }
    }
    if (!slot_entry)
        return nullptr;

    // Prefer the slot that is not in flight; both in flight means the client is more than
    // one frame ahead of the display, and the right answer is to make it wait.
    unsigned chosen = k_slots;
    for (unsigned n = 0; n < k_slots; ++n) {
        const unsigned candidate = (slot_entry->next + n) % k_slots;
        if (!slot_entry->in_flight[candidate].load(std::memory_order_acquire)) {
            chosen = candidate;
            break;
        }
    }
    if (chosen == k_slots)
        return nullptr;

    auto& storage = slot_entry->slots[chosen];
    // The slot is provably not in flight, so growing it cannot invalidate a pointer the
    // render thread still holds. Never shrink: steady state stops allocating.
    if (storage.size() < bytes)
        storage.resize(bytes);

    slot_entry->next = (chosen + 1) % k_slots;
    slot_entry->in_flight[chosen].store(true, std::memory_order_release);
    token_out = (static_cast<unsigned long long>(index + 1) << 8) | chosen;
    return storage.data();
}

void lx::compositor::shm_pixel_store::release(unsigned long long token) {
    if (token == 0)
        return;
    const unsigned index = static_cast<unsigned>((token >> 8) - 1);
    const unsigned slot = static_cast<unsigned>(token & 0xFFu);
    if (index >= k_max_textures || slot >= k_slots)
        return;
    entries_[index].in_flight[slot].store(false, std::memory_order_release);
}

void lx::compositor::shm_pixel_store::forget(unsigned texture_id) {
    for (auto& e : entries_) {
        if (!e.used || e.texture_id != texture_id)
            continue;
        for (unsigned i = 0; i < k_slots; ++i) {
            if (e.in_flight[i].load(std::memory_order_acquire))
                return; // still sampled — keep the storage rather than free it underneath
        }
        e.used = false;
        e.texture_id = 0;
        e.next = 0;
        for (auto& s : e.slots)
            s.clear();
        return;
    }
}
