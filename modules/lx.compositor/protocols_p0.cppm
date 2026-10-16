module;

#include <atomic>
#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#endif

import lx.foundation;
import lx.sync;
import lx.wayland.server;
import lx.gfx;
import lx.input;

export module lx.compositor:protocols_p0;

import :input_router;
import :shell_bridge;
import :surface;
import :surface_tree;
import :toplevel;
import :output;
import :buffer_lifecycle;
import :wm;
import :popup;
import :cursor;
import :decoration;
import :layer_shell;
import :dnd;
import :liveness;
import :activation;

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
    /// `shm_upload_pool` block backing `pixels` (GL path), released once uploaded.
    unsigned long long shm_pool_token = 0;
    /// The part of the `width` x `height` image that `pixels` carries, packed (row stride
    /// `region.width * 4`). Empty means the whole image. A partial region updates texture
    /// storage that already holds the rest — see `shm_upload_region`.
    lx::rect2i region{};
    /// `forget` only: the first frame index whose snapshot is guaranteed not to reference
    /// this texture. Applying the forget before that frame has been composited drops a
    /// texture an in-flight snapshot still draws. See `drain_texture_updates`.
    unsigned retire_after_frame = 0;
    /// Index of the first snapshot allowed to see this update: the one being built when it
    /// was queued (`p0_protocol_context::building_frame`). The render thread holds back
    /// every update, and every later one (FIFO), until it composites a snapshot at least
    /// this new — otherwise a re-committed buffer would show its new pixels under the
    /// previous snapshot's geometry, a parent/child mismatch inside one frame.
    unsigned visible_from_frame = 0;
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

/// Transient staging for the GL upload path.
///
/// GL copies staged pixels into a texture during the drain and never reads them again, so
/// staging only has to live from the commit (UI thread) to the upload (render thread). A
/// block per in-flight update, sized by the region it carries — usually the damage, not the
/// window — and handed back the moment the upload is issued. Steady state recycles cached
/// blocks with no syscalls; blocks idle for a while are unmapped, so an idle desktop holds
/// no staging at all. Per-texture staging that persisted for the texture's lifetime is
/// what grew the compositor by two window-sized copies per buffer it had ever seen.
///
/// Blocks are mmap'd directly rather than malloc'd: large, frequently freed malloc blocks
/// raise glibc's dynamic mmap threshold and then fragment the brk heap, which is how
/// staging memory used to stay resident after it was freed.
class shm_upload_pool {
public:
    /// Size classes are powers of two from 64 KiB.
    static constexpr unsigned k_min_shift = 16;
    static constexpr unsigned k_max_blocks = 64;

    shm_upload_pool() = default;
    shm_upload_pool(const shm_upload_pool&) = delete;
    shm_upload_pool& operator=(const shm_upload_pool&) = delete;
    ~shm_upload_pool();

    /// UI affinity. Storage for `bytes` and a nonzero token, or nullptr when every block is
    /// in flight or a mapping fails (the caller skips the update, as for a full queue).
    [[nodiscard]] unsigned char* acquire(unsigned bytes, unsigned long long& token_out,
                                         unsigned long long now_ns = 0);
    /// Render affinity. Hands the block back; ignores a zero token.
    void release(unsigned long long token);
    /// UI affinity. Unmaps blocks that are not in flight and have not been used for
    /// `idle_ns`.
    void collect(unsigned long long now_ns, unsigned long long idle_ns);
    /// UI affinity. Unmaps every block that is not in flight (memory pressure).
    void trim();
    /// UI affinity. Bytes currently mapped, in flight or cached.
    [[nodiscard]] unsigned long long bytes_mapped() const { return mapped_bytes_; }
    [[nodiscard]] unsigned blocks_mapped() const;

private:
    struct block {
        unsigned char* data = nullptr; // UI-owned
        unsigned long long size = 0;   // UI-owned
        unsigned long long last_used_ns = 0;
        std::atomic<bool> in_flight{false}; // set by UI on acquire, cleared by render
    };
    void unmap(block& b);
    block blocks_[k_max_blocks]{};
    unsigned long long mapped_bytes_ = 0;
};

/// Damage history of one surface, for buffer-age uploads: commit `seq` (1-based) has its
/// damage in `rects[seq % k_depth]`.
struct surface_damage_history {
    static constexpr unsigned k_depth = 8;
    unsigned long long seq = 0;
    lx::rect2i rects[k_depth]{};

    void record(lx::rect2i damage) {
        ++seq;
        rects[seq % k_depth] = damage;
    }
    /// Union of the damage of commits (`after`, `seq`]. False when that range is older than
    /// the history keeps, or empty.
    [[nodiscard]] bool since(unsigned long long after, lx::rect2i& out) const;
};

/// What has been staged for one buffer's texture, for buffer-age uploads.
struct buffer_upload_state {
    lx::surface_id surface{};
    unsigned long long seq = 0; // surface commit last uploaded
    unsigned uploads = 0;
    lx::rect2i last{};          // damage that upload carried
};

/// The region of a `width` x `height` shm buffer that must be staged for this upload.
///
/// Wayland damage describes what changed since the surface's previous content, whichever
/// buffer held it; a texture belongs to one buffer, so its staleness is the union of all
/// surface damage since that buffer was last uploaded (buffer age). The GL compositor also
/// alternates two texture names per texture, so the name about to be written missed the
/// previous upload too: the region adds that upload's damage. The first two uploads of a
/// texture, a buffer moved to another surface, or history that ran out stage everything.
/// `history` must already hold this commit. Updates `state`.
[[nodiscard]] lx::rect2i shm_upload_region(const surface_damage_history& history,
                                           buffer_upload_state& state, lx::surface_id surface,
                                           unsigned width, unsigned height);

/// Per-texture staging for shm pixels, used by the CPU composite path.
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
///
/// Storage belongs to a texture for its lifetime, because the CPU compositor samples it in
/// place. A retired texture's storage is freed by `collect` once neither slot is in flight;
/// before, nothing retired entries and `vector::clear` kept their capacity, so every buffer
/// the compositor had ever seen kept two full copies resident. (GL no longer uses this:
/// it stages through `shm_upload_pool`.)
class shm_pixel_store {
public:
    /// Matches the GL and CPU compositors' texture tables.
    static constexpr unsigned k_max_textures = 256;
    static constexpr unsigned k_slots = 2;

    /// UI affinity. Writable storage of `bytes` for `texture_id`, or nullptr when both
    /// slots are still being sampled. `token_out` is nonzero on success.
    [[nodiscard]] unsigned char* acquire(unsigned texture_id, unsigned bytes,
                                         unsigned long long& token_out);

    /// Render affinity. Marks the slot behind `token` reusable. Ignores a zero token.
    void release(unsigned long long token);

    /// UI affinity. The texture is being retired: free its storage at the next `collect`
    /// after the render thread has let go of both slots.
    void retire(unsigned texture_id);
    /// UI affinity. Frees the storage of retired textures nothing samples any more.
    void collect();
    /// UI affinity. Bytes of storage held.
    [[nodiscard]] unsigned long long bytes_held() const { return held_bytes_; }

private:
    struct entry {
        unsigned texture_id = 0;
        std::vector<unsigned char> slots[k_slots]{};
        std::atomic<bool> in_flight[k_slots]{};
        unsigned next = 0;
        bool used = false;
        bool retiring = false;
    };
    void free_entry(entry& e);

    entry entries_[k_max_textures]{};
    unsigned retiring_ = 0;
    unsigned long long held_bytes_ = 0;
};

/// Context shared by all P0 protocol handlers (owned by compositor_impl).
struct p0_protocol_context {
    wayland::server* server = nullptr;
    /// Delivers seat events to focused clients. Null until the compositor supplies one, in
    /// which case seat objects are still created but nothing is routed.
    input_router* input = nullptr;
    /// The seat's serial counter and keyboard state, owned by the compositor.
    lx::input::seat* seat = nullptr;
    /// Compositor → shell state sync over zlm_policy_bridge_v1.
    shell_bridge* shell = nullptr;
    surface_manager* surfaces = nullptr;
    /// Subsurface parentage, cached child state and each root's scene container.
    surface_tree* tree = nullptr;
    toplevel_manager* toplevels = nullptr;
    /// xdg_popup records, placement, grab and dismissal. Receives its wire ops from
    /// `install_p0_protocols`.
    popup_manager* popups = nullptr;
    /// wlr-layer-shell records, arrangement and exclusive zones. Receives its wire ops from
    /// `install_p0_protocols`.
    layer_shell* layers = nullptr;
    /// Window placement, configure flow and focus. Receives its wire ops from
    /// `install_p0_protocols`.
    window_manager* wm = nullptr;
    /// Pointer image: wl_pointer.set_cursor, wp_cursor_shape_v1 and cursor-role commits
    /// land here. Receives its texture-staging hook from `install_p0_protocols`.
    cursor_manager* cursor = nullptr;
    /// Server-side frames: title changes and the decoration mode reach it through the window
    /// manager; it receives its texture-staging hook from `install_p0_protocols`.
    decoration_manager* decorations = nullptr;
    /// The drag-and-drop grab: wl_data_device.start_drag begins it (after the window
    /// manager validated the serial), the icon role's commits and surface destruction reach
    /// it. Receives its wire ops from `install_p0_protocols`.
    dnd_grab* dnd = nullptr;
    /// xdg_wm_base.ping/pong liveness: pongs land here, and it receives its wire ops (send
    /// a ping, the not-responding change) from `install_p0_protocols`.
    liveness_tracker* liveness = nullptr;
    /// xdg-activation tokens issued to clients (wire_activation.cpp); the window manager
    /// decides what presenting one does.
    activation_tokens* activation = nullptr;
    /// Advertise zxdg_decoration_manager_v1 (`config::server_side_decorations`).
    bool server_side_decorations = true;
    output_manager* outputs = nullptr;
    gfx::headless_backend* headless = nullptr;
    gfx::dmabuf_importer* dmabuf = nullptr;
    texture_update_queue* textures = nullptr;
    buffer_lifecycle_tracker* lifecycle = nullptr;
    shm_staging_ring* shm_staging = nullptr;
    /// Set instead of `shm_staging` when the CPU or GL composite path is active.
    shm_pixel_store* shm_store = nullptr;
    /// GL path: transient per-update staging. Exactly one of shm_store / shm_pool /
    /// shm_staging is set.
    shm_upload_pool* shm_pool = nullptr;
    /// dev_t sent as zwp_linux_dmabuf_feedback_v1.main_device and tranche target: the
    /// render node of the KMS device. 0 when there is none (headless).
    unsigned long long dmabuf_main_device = 0;

    /// UI affinity. The frame index currently being built, refreshed by `tick_ui`. Used to
    /// date a texture retirement so the render thread can tell which in-flight snapshots
    /// might still draw it.
    unsigned frame_index = 0;
    /// UI affinity. Index of the next snapshot to be published — the first one that may
    /// show what is committed now. `tick_ui` sets it to the tick's frame before dispatch
    /// and to the one after once that frame is published (commits dispatched between ticks
    /// belong to the next snapshot); a dropped publish leaves it, since its damage — and so
    /// its visibility — carries forward. Every queued texture update is stamped with it.
    unsigned building_frame = 0;
    /// Monotonic time of the current UI tick, for staging-pool idle tracking.
    unsigned long long now_ns = 0;
    /// Ends the session cleanly (zlm_shell_v1.request_logout). `source` names who asked,
    /// for the log. Null until the compositor supplies it.
    void (*request_logout)(void* user, const char* source) = nullptr;
    void* logout_user = nullptr;
    /// Tells bound shells the session is ending (zlm_shell_v1.session_ending). Set by
    /// `install_p0_protocols`.
    void (*notify_session_ending)() = nullptr;

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
/// `output_manager::generation` moved: every bound wl_output and zxdg_output_v1 is sent
/// the new state (then `done`), and every surface's output membership is recomputed from
/// its node rect. UI affinity; rare (a mode change, an output added or removed).
void notify_outputs_changed(p0_protocol_context& ctx);

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
            if (entries_[i].retiring) {
                entries_[i].retiring = false;
                --retiring_;
            }
            entries_[i].used = true;
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
    if (storage.size() < bytes) {
        held_bytes_ += bytes - storage.size();
        storage.resize(bytes);
    }

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

void lx::compositor::shm_pixel_store::free_entry(entry& e) {
    for (auto& s : e.slots) {
        held_bytes_ -= s.size();
        // `clear()` keeps the capacity — the leak this replaced. Swapping with an empty
        // vector is what actually returns the storage.
        std::vector<unsigned char>{}.swap(s);
    }
    e.used = false;
    e.retiring = false;
    e.texture_id = 0;
    e.next = 0;
}

void lx::compositor::shm_pixel_store::retire(unsigned texture_id) {
    if (texture_id == 0)
        return;
    for (auto& e : entries_) {
        if (!e.used || e.texture_id != texture_id || e.retiring)
            continue;
        e.retiring = true;
        ++retiring_;
        return;
    }
}

void lx::compositor::shm_pixel_store::collect() {
    if (retiring_ == 0)
        return;
    for (auto& e : entries_) {
        if (!e.used || !e.retiring)
            continue;
        bool busy = false;
        for (unsigned i = 0; i < k_slots; ++i)
            busy = busy || e.in_flight[i].load(std::memory_order_acquire);
        if (busy)
            continue; // the composite still samples it; the forget releases it later
        free_entry(e);
        --retiring_;
    }
}

lx::compositor::shm_upload_pool::~shm_upload_pool() {
    for (auto& b : blocks_)
        unmap(b);
}

void lx::compositor::shm_upload_pool::unmap(block& b) {
    if (!b.data)
        return;
#if !defined(_WIN32)
    ::munmap(b.data, static_cast<std::size_t>(b.size));
#endif
    mapped_bytes_ -= b.size;
    b.data = nullptr;
    b.size = 0;
    b.last_used_ns = 0;
}

unsigned char* lx::compositor::shm_upload_pool::acquire(unsigned bytes,
                                                       unsigned long long& token_out,
                                                       unsigned long long now_ns) {
    token_out = 0;
    if (bytes == 0)
        return nullptr;
    unsigned long long size = 1ull << k_min_shift;
    while (size < bytes)
        size <<= 1;

    // Smallest cached block that fits and is not in flight: a damaged strip should not take
    // a block sized for a whole window when a strip-sized one is free.
    block* best = nullptr;
    block* empty = nullptr;
    for (auto& b : blocks_) {
        if (!b.data) {
            if (!empty)
                empty = &b;
            continue;
        }
        if (b.size < size || b.in_flight.load(std::memory_order_acquire))
            continue;
        if (!best || b.size < best->size)
            best = &b;
    }
    if (!best) {
        if (!empty)
            return nullptr; // every block in flight: the client is far ahead of the display
#if !defined(_WIN32)
        void* mem = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            return nullptr;
        empty->data = static_cast<unsigned char*>(mem);
#else
        return nullptr;
#endif
        empty->size = size;
        mapped_bytes_ += size;
        best = empty;
    }
    best->last_used_ns = now_ns;
    best->in_flight.store(true, std::memory_order_release);
    token_out = static_cast<unsigned long long>(best - blocks_) + 1;
    return best->data;
}

void lx::compositor::shm_upload_pool::release(unsigned long long token) {
    if (token == 0 || token > k_max_blocks)
        return;
    blocks_[token - 1].in_flight.store(false, std::memory_order_release);
}

void lx::compositor::shm_upload_pool::collect(unsigned long long now_ns,
                                              unsigned long long idle_ns) {
    if (mapped_bytes_ == 0)
        return;
    for (auto& b : blocks_) {
        if (!b.data || b.in_flight.load(std::memory_order_acquire))
            continue;
        if (now_ns >= b.last_used_ns && now_ns - b.last_used_ns >= idle_ns)
            unmap(b);
    }
}

void lx::compositor::shm_upload_pool::trim() {
    for (auto& b : blocks_)
        if (b.data && !b.in_flight.load(std::memory_order_acquire))
            unmap(b);
}

unsigned lx::compositor::shm_upload_pool::blocks_mapped() const {
    unsigned n = 0;
    for (const auto& b : blocks_)
        n += b.data ? 1u : 0u;
    return n;
}

namespace {
lx::rect2i union_rect(lx::rect2i a, lx::rect2i b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = (a.x + a.width) > (b.x + b.width) ? a.x + a.width : b.x + b.width;
    const int y1 = (a.y + a.height) > (b.y + b.height) ? a.y + a.height : b.y + b.height;
    return {x0, y0, x1 - x0, y1 - y0};
}
} // namespace

bool lx::compositor::surface_damage_history::since(unsigned long long after,
                                                   lx::rect2i& out) const {
    if (after >= seq || seq - after > k_depth)
        return false;
    lx::rect2i u{};
    for (unsigned long long s = after + 1; s <= seq; ++s)
        u = union_rect(u, rects[s % k_depth]);
    out = u;
    return true;
}

lx::rect2i lx::compositor::shm_upload_region(const surface_damage_history& history,
                                             buffer_upload_state& state, lx::surface_id surface,
                                             unsigned width, unsigned height) {
    const lx::rect2i full{0, 0, static_cast<int>(width), static_cast<int>(height)};
    lx::rect2i damage = full;
    if (state.uploads > 0 && state.surface == surface) {
        lx::rect2i since{};
        if (history.since(state.seq, since))
            damage = since;
    }
    // The name being written missed the previous upload of this texture; before two
    // uploads, one name has never been written at all.
    lx::rect2i region = state.uploads >= 2 ? union_rect(damage, state.last) : full;
    // Clip to the buffer: damage is clipped where it is recorded, but stay safe.
    const int x0 = region.x < 0 ? 0 : region.x;
    const int y0 = region.y < 0 ? 0 : region.y;
    int x1 = region.x + region.width;
    int y1 = region.y + region.height;
    if (x1 > full.width) x1 = full.width;
    if (y1 > full.height) y1 = full.height;
    region = (x1 > x0 && y1 > y0) ? lx::rect2i{x0, y0, x1 - x0, y1 - y0} : full;

    state.surface = surface;
    state.seq = history.seq;
    ++state.uploads;
    state.last = damage;
    return region;
}
