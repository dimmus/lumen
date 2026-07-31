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
    void* vk_image = nullptr;
    void* vk_memory = nullptr;
    const unsigned char* pixels = nullptr;
    /// Staging-ring sequence backing `pixels`; the render thread publishes it as consumed.
    unsigned long long shm_seq = 0;
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
