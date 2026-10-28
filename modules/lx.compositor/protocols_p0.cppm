module;

#include <sys/mman.h>
#include <signal.h>
#include <stddef.h>

import lx.foundation;
import lx.sync;
import lx.wayland.server;
import lx.gfx;
import lx.input;
import lx.scene;

export module lx.compositor:protocols_p0;

import std;

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
import :keybindings;
import :presentation_feedback;
import :idle;
import :session_lock;
import :capture_engine;
import :output_management;
import :text_input;
import :toplevel_icon;
import :foreign_registry;

export namespace lx::compositor {

/// One line of a configuration report (zlm_shell_v1 v7 config_diagnostic).
struct shell_config_line {
    unsigned severity = 0; ///< zlm_shell_v1.config_severity
    const char* file = "";
    unsigned line = 0;
    const char* text = "";
};

/// What a shell is told of the configuration (zlm_shell_v1 v7).
struct shell_config_report {
    /// A sealed memfd of `size` bytes (the effective bytes), or -1: no `config` event (the
    /// bytes did not change, or the generation was rejected).
    int fd = -1;
    unsigned size = 0;
    unsigned generation = 0;
    bool applied = true;
    unsigned errors = 0;
    unsigned warnings = 0;
    const shell_config_line* lines = nullptr;
    unsigned line_count = 0;
};

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
    /// `shm_pixel_store` slot backing `pixels`, released once the composite has read it.
    /// Set only when `pixels` came from `shm_pixel_store`.
    unsigned long long shm_token = 0;
    /// `shm_upload_pool` block backing `pixels` (GL path), released once uploaded.
    unsigned long long shm_pool_token = 0;
    /// `shm_direct_uploads` handle: the pixels are read from the client's buffer, through
    /// that table, instead of from `pixels`. Set only on the GL client-upload path.
    unsigned shm_direct = 0;
    /// The part of the `width` x `height` image that `pixels` carries, packed (row stride
    /// `region.width * 4`). Empty means the whole image. A partial region updates texture
    /// storage that already holds the rest — see `shm_upload_region`.
    lx::rect2i region{};
    /// The region as separate rects (inside `region`, its bounds) when there are several:
    /// a backend that uploads rect by rect copies only these. Empty: `region` as one.
    client_damage rects{};
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
    /// Bytes staged and not yet uploaded, beyond which `acquire` declines (one update is
    /// always allowed). Seven 1080p frames: a backend that falls behind (software Vulkan
    /// takes ~165 ms a frame) makes the client wait instead of the pool mapping a block for
    /// every frame the client gets ahead.
    static constexpr unsigned long long k_max_in_flight_bytes = 64ull << 20;

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
        unsigned long long staged = 0; // bytes of the update in it; UI-owned
        std::atomic<bool> in_flight{false}; // set by UI on acquire, cleared by render
    };
    void unmap(block& b);
    block blocks_[k_max_blocks]{};
    unsigned long long mapped_bytes_ = 0;
    /// Sum of `staged` over blocks in flight: added by UI, taken back by render.
    std::atomic<unsigned long long> in_flight_bytes_{0};
};

/// GL path: client shm buffers uploaded by the render thread straight from the client's
/// memory. Staging copied every damaged pixel once on the UI thread and the driver copied
/// it again; now the driver's read is the only one, and the UI thread copies nothing.
///
/// The price is holding the client's buffer until the upload has happened. The
/// wl_buffer.release that went out at commit goes out from `retire` once the render thread
/// is done, and an external wl_shm_pool reference keeps the mapping where it is (libwayland
/// defers a pool resize while one is held). A client may still destroy the wl_buffer in
/// between: `on_buffer_gone` runs while the buffer is still readable and copies the region
/// out under the lock, so the render thread never reads a buffer that no longer exists and
/// the UI thread never waits on a frame.
///
/// Handles are index + 1; 0 means none. UI affinity throughout, except `upload` (render).
class shm_direct_uploads {
public:
    static constexpr unsigned k_max = 64;

    /// The libwayland calls, kept out of this class so it can be tested without a client.
    struct hooks {
        void (*begin_access)(void* shm_buffer) = nullptr;
        void (*end_access)(void* shm_buffer) = nullptr;
        void (*release)(void* wl_buffer) = nullptr;
        void (*unref_pool)(void* shm_pool) = nullptr;
    };

    void bind(hooks h, shm_upload_pool* staging) {
        hooks_ = h;
        staging_ = staging;
    }

    /// `pixels` is `region`'s first pixel in the client's memory, rows `stride` apart.
    /// Returns 0 when every entry is in use (the caller stages instead).
    [[nodiscard]] unsigned add(void* wl_buffer, void* shm_buffer, void* shm_pool,
                               const unsigned char* pixels, unsigned stride, lx::rect2i region) {
        for (unsigned i = 0; i < k_max; ++i) {
            entry& e = entries_[i];
            if (e.used)
                continue;
            e.used = true;
            e.buffer = wl_buffer;
            e.shm = shm_buffer;
            e.pool = shm_pool;
            e.pixels = pixels;
            e.stride = stride;
            e.region = region;
            e.fallback_token = 0;
            e.done.store(false, std::memory_order_relaxed);
            ++used_;
            return i + 1;
        }
        return 0;
    }

    /// The update naming `handle` was never queued: nothing will upload it.
    void drop(unsigned handle) {
        if (handle == 0 || handle > k_max)
            return;
        entries_[handle - 1].done.store(true, std::memory_order_release);
        retire();
    }

    /// Render affinity. Calls `fn(pixels, stride)` — `pixels` null when the client
    /// destroyed the buffer and its region could not be kept — with the client's memory
    /// guarded against a truncated pool, then marks the entry finished.
    template <class Fn>
    void upload(unsigned handle, Fn&& fn) {
        if (handle == 0 || handle > k_max)
            return;
        entry& e = entries_[handle - 1];
        unsigned long long staged = 0;
        {
            std::lock_guard lock{mutex_};
            if (e.shm && hooks_.begin_access)
                hooks_.begin_access(e.shm);
            fn(e.pixels, e.stride);
            if (e.shm && hooks_.end_access)
                hooks_.end_access(e.shm);
            staged = e.fallback_token;
            e.fallback_token = 0;
        }
        if (staged && staging_)
            staging_->release(staged);
        e.done.store(true, std::memory_order_release);
    }

    /// The wl_buffer is being destroyed (its destroy listener; still readable). Uploads not
    /// yet done keep a copy of their region; finished ones just forget the object.
    void on_buffer_gone(void* wl_buffer) {
        if (used_ == 0 || !wl_buffer)
            return;
        std::lock_guard lock{mutex_};
        for (auto& e : entries_) {
            if (!e.used || e.buffer != wl_buffer)
                continue;
            if (!e.done.load(std::memory_order_acquire) && e.shm && e.pixels) {
                const unsigned row = static_cast<unsigned>(e.region.width) * 4u;
                const unsigned rows = static_cast<unsigned>(e.region.height);
                unsigned long long token = 0;
                unsigned char* copy = staging_ ? staging_->acquire(row * rows, token) : nullptr;
                if (copy) {
                    if (hooks_.begin_access)
                        hooks_.begin_access(e.shm);
                    for (unsigned y = 0; y < rows; ++y)
                        std::memcpy(copy + static_cast<size_t>(y) * row,
                                    e.pixels + static_cast<size_t>(y) * e.stride, row);
                    if (hooks_.end_access)
                        hooks_.end_access(e.shm);
                }
                e.pixels = copy;
                e.stride = row;
                e.fallback_token = token;
            }
            e.buffer = nullptr;
            e.shm = nullptr;
        }
    }

    /// Sends the release of every buffer whose upload is done — unless a later commit of
    /// the same buffer is still waiting, which then releases it — and drops pool refs.
    void retire() {
        if (used_ == 0)
            return;
        for (auto& e : entries_) {
            if (!e.used || !e.done.load(std::memory_order_acquire))
                continue;
            bool reused = false;
            for (const auto& other : entries_)
                if (&other != &e && other.used && other.buffer == e.buffer && e.buffer &&
                    !other.done.load(std::memory_order_acquire))
                    reused = true;
            if (e.buffer && !reused && hooks_.release)
                hooks_.release(e.buffer);
            if (e.pool && hooks_.unref_pool)
                hooks_.unref_pool(e.pool);
            e.used = false;
            e.buffer = e.shm = e.pool = nullptr;
            e.pixels = nullptr;
            --used_;
        }
    }

    /// A client buffer is held for an upload, so a later frame has work to do.
    [[nodiscard]] bool idle() const { return used_ == 0; }
    [[nodiscard]] unsigned in_use() const { return used_; }

private:
    struct entry {
        void* buffer = nullptr; // wl_resource*; null once destroyed
        void* shm = nullptr;    // wl_shm_buffer*; null once destroyed
        void* pool = nullptr;   // wl_shm_pool*, externally referenced
        const unsigned char* pixels = nullptr;
        unsigned stride = 0;
        lx::rect2i region{};
        unsigned long long fallback_token = 0;
        bool used = false;
        std::atomic<bool> done{false};
    };
    entry entries_[k_max]{};
    unsigned used_ = 0;
    std::mutex mutex_{};
    hooks hooks_{};
    shm_upload_pool* staging_ = nullptr;
};

/// Damage history of one surface, for buffer-age uploads: commit `seq` (1-based) has its
/// damage (buffer pixels) in `rects[seq % k_depth]`.
struct surface_damage_history {
    static constexpr unsigned k_depth = 8;
    unsigned long long seq = 0;
    client_damage rects[k_depth]{};

    void record_rects(const client_damage& damage) {
        ++seq;
        rects[seq % k_depth] = damage;
    }
    void record(lx::rect2i damage) {
        client_damage d{};
        d.add(damage);
        record_rects(d);
    }
    /// The damage of commits (`after`, `seq`]. False when that range is older than the
    /// history keeps, or empty.
    [[nodiscard]] bool since_rects(unsigned long long after, client_damage& out) const;
    /// Its bounding box.
    [[nodiscard]] bool since(unsigned long long after, lx::rect2i& out) const;
};

/// What has been staged for one buffer's texture, for buffer-age uploads.
struct buffer_upload_state {
    lx::surface_id surface{};
    unsigned long long seq = 0; // surface commit last uploaded
    unsigned uploads = 0;
    client_damage last{};       // damage that upload carried
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
/// The same as rects (`shm_upload_region` is their bounds): what the upload copies.
[[nodiscard]] client_damage shm_upload_damage(const surface_damage_history& history,
                                              buffer_upload_state& state, lx::surface_id surface,
                                              unsigned width, unsigned height);

/// What a `shm_pixel_store` slot holds: the content of `surface` as of its commit `seq`
/// (the `surface_damage_history` numbering). `seq == 0`: nothing usable for this texture.
struct shm_pixel_store_stamp {
    lx::surface_id surface{};
    unsigned long long seq = 0;
};

/// The region of a `width` x `height` image to rewrite into a `shm_pixel_store` slot that
/// holds `held` (see `shm_pixel_store::acquire`) to bring it to the content `history`
/// ends with: the damage since the slot's commit, or the whole image when the slot holds
/// nothing of this surface or the history no longer reaches back that far. Exact buffer
/// age per slot, so a slot written twice in a row (its sibling was still being read) never
/// misses a commit. `history` must already hold this commit.
[[nodiscard]] lx::rect2i shm_store_region(const surface_damage_history& history,
                                          shm_pixel_store_stamp held, lx::surface_id surface,
                                          unsigned width, unsigned height);
/// The same as rects (`shm_store_region` is their bounds): what the staging rewrites.
[[nodiscard]] client_damage shm_store_damage(const surface_damage_history& history,
                                             shm_pixel_store_stamp held, lx::surface_id surface,
                                             unsigned width, unsigned height);

/// Per-texture staging for shm pixels, used by the CPU composite path.
///
/// `shm_upload_pool` hands out a block per update, which is right when the consumer copies
/// the pixels out during the drain (the GL and Vulkan uploads do). The CPU compositor
/// instead *samples the staged rows in place* during the composite, so its staging has to
/// stay live well past the drain, as the texture itself.
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

    using stamp = shm_pixel_store_stamp;

    /// UI affinity. Writable storage of `bytes` for `texture_id`, or nullptr when both
    /// slots are still being sampled. `token_out` is nonzero on success. The slot is
    /// stamped `next` (the caller writes that content now); `previous` receives what it
    /// held, so the caller can rewrite only the damage since (`shm_store_region`) instead
    /// of the whole image.
    [[nodiscard]] unsigned char* acquire(unsigned texture_id, unsigned bytes,
                                         unsigned long long& token_out, stamp next = {},
                                         stamp* previous = nullptr);

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
        stamp stamps[k_slots]{};
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
    /// CPU path: per-texture staging the composite samples in place.
    shm_pixel_store* shm_store = nullptr;
    /// GL and Vulkan paths: transient per-update staging. Exactly one of shm_store /
    /// shm_pool is set.
    shm_upload_pool* shm_pool = nullptr;
    /// The backend uploads part of an image (GL): staging carries the buffer-age region.
    /// Otherwise (Vulkan) every staged update is the whole image.
    bool shm_regions = false;
    /// GL path with client-memory uploads (`gl_compositor::supports_client_upload`): shm
    /// commits skip staging and are uploaded from the client's buffer. Requires shm_pool.
    shm_direct_uploads* shm_direct = nullptr;
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
    /// UI affinity. Per output view (slot): the newest published frame, and the views that
    /// ever published. A surface is on an output now when the visibility pass of that
    /// view's newest frame found it (`surface_node::visible_in(frame, slot)`).
    unsigned published_frame[16]{};
    std::uint32_t published_mask = 0;
    /// UI affinity. Outputs whose display runs (not powered off, session not paused): what
    /// an idle inhibitor's surface must be shown on.
    std::uint32_t lit_mask = 0;
    /// Ends the session cleanly (zlm_shell_v1.request_logout). `source` names who asked,
    /// for the log. Null until the compositor supplies it.
    void (*request_logout)(void* user, const char* source) = nullptr;
    void* logout_user = nullptr;
    /// Tells bound shells the session is ending (zlm_shell_v1.session_ending). Set by
    /// `install_p0_protocols`.
    void (*notify_session_ending)() = nullptr;
    /// The shell's key grabs (zlm_shell_v1 v4 grab_key) are recorded here; owned by the
    /// compositor, which consults it for every key.
    keybindings* keys = nullptr;
    /// wl_keyboard.repeat_info (config.toml input.keyboard.repeat-rate/-delay), sent to every
    /// keyboard and input-method grab at creation. UI affinity.
    int repeat_rate = 25;
    int repeat_delay = 600;
    /// Set by `install_p0_protocols`: the seat's keymap was replaced; the cached memfd every
    /// wl_keyboard is sent is serialized again from it.
    void (*invalidate_seat_keymap)() = nullptr;
    /// zlm_shell_v1 v7: a shell bound; the compositor sends it the configuration in use
    /// (`send_shell_config` with that resource). Null: nothing is sent.
    void (*shell_config_on_bind)(void* user, void* shell_resource) = nullptr;
    void* shell_config_user = nullptr;
    /// Set by `install_p0_protocols`: config (when `fd` ≥ 0), every diagnostic and
    /// config_done to `shell_resource`, or to every bound v7 shell when it is null.
    void (*send_shell_config)(void* shell_resource, const shell_config_report& report) = nullptr;
    /// Set by `install_p0_protocols`: zlm_shell_v1.action to every bound v7 shell, with a
    /// fresh input serial noted as a key press delivered to it when `input` (else serial 0).
    /// False when no v7 shell is bound.
    bool (*send_shell_action)(const char* action, const char* argument, unsigned time_ms,
                              bool input) = nullptr;
    /// Sends zlm_key_grab_v1.activated to the grab `grab_resource` with a fresh input serial
    /// noted as a key press delivered to the shell. Set by `install_p0_protocols`.
    void (*fire_key_grab)(void* grab_resource, unsigned time_ms) = nullptr;
    /// zlm_shell_v1.held_action (v7, design-session-and-wm K.1) to the first bound v7 shell:
    /// returns the new zlm_held_action_v1 resource (the hold's owner in `keybindings`), null
    /// when no v7 shell is bound. Its serial is noted as a key press delivered to the shell.
    /// The resource's destruction ends the hold (`keybindings::end_hold`).
    void* (*send_held_action)(const char* action, const char* argument, unsigned time_ms) = nullptr;
    /// zlm_held_action_v1.key / released / cancelled to the hold `hold` (a resource from
    /// `send_held_action`). Set by `install_p0_protocols`.
    void (*send_hold_key)(void* hold, unsigned keysym, unsigned mods, const char* binding,
                          unsigned time_ms) = nullptr;
    void (*send_hold_released)(void* hold, unsigned time_ms) = nullptr;
    void (*send_hold_cancelled)(void* hold) = nullptr;
    /// wp_presentation feedback in flight (owned by the compositor). Null: the global is not
    /// advertised.
    presentation_feedback_table* presentation = nullptr;
    /// ext_idle_notifier_v1 notifications and zwp_idle_inhibit_manager_v1 inhibitors
    /// (wire_idle.cpp), owned by the compositor. Null: neither global is advertised.
    idle_tracker* idle = nullptr;
    inhibitor_set* inhibitors = nullptr;
    /// A notification was added or removed, or an inhibitor came or went: the compositor
    /// re-arms its idle timer (and re-evaluates inhibition at the next tick).
    void (*idle_changed)(void* user) = nullptr;
    /// zwlr_output_power_manager_v1 (wire_output_power.cpp): output `slot`'s power mode is
    /// asked for (UI). Null without a display to power (headless, nested): every
    /// zwlr_output_power_v1 is sent `failed`.
    void (*set_output_power)(void* user, unsigned slot, bool on) = nullptr;
    /// What zwlr_output_power_v1.mode reports for output `slot` right now.
    bool (*output_power_on)(void* user, unsigned slot) = nullptr;
    void* idle_user = nullptr;
    /// ext-session-lock-v1 (wire_session_lock.cpp): the lock state machine, owned by the
    /// compositor, and the scene layer lock surfaces map into (above everything a client
    /// draws, under the cursor). Null `lock`: the global is not advertised.
    session_lock* lock = nullptr;
    scene::container_node* lock_layer = nullptr;
    /// Executes a lock decision (the compositor's half: input, scene, logind, the shell, the
    /// timer; it calls back `session_lock_send` for the wire half). UI affinity.
    void (*lock_apply)(void* user, const lock_actions& a) = nullptr;
    void* lock_user = nullptr;
    /// zlm_shell_v1.lock_state / lock_requested (v5) to every bound shell. Set by
    /// `install_p0_protocols`.
    void (*notify_lock_state)(unsigned state) = nullptr;
    /// True when a shell that speaks v5 was told (it may start a locker).
    bool (*notify_lock_requested)(unsigned reason) = nullptr;
    /// text-input-v3 / input-method-v2 / virtual-keyboard-v1 (wire_text_input.cpp,
    /// wire_input_method.cpp): the relay, owned by the compositor, and the scene layer input
    /// method popups map into (above the lock layer, under the cursor). Null `text_input`:
    /// none of the three globals is advertised.
    text_input_relay* text_input = nullptr;
    scene::container_node* ime_popup_layer = nullptr;
    /// zwlr_output_manager_v1 (wire_output_management.cpp): an apply or test passed its
    /// protocol checks and was queued (`output_mgmt_jobs`); the compositor runs it when it is
    /// free (one at a time, ADR 008 §C.3). Null: the global is not advertised.
    void (*output_mgmt_kick)(void* user) = nullptr;
    void* output_mgmt_user = nullptr;

    // ── content-update queue (wire_surface.cpp; design-app-compat-capture §3) ──
    /// When content applied now would first be presented on output `slot` (its frame
    /// clock's prediction; `cu_in_tick` while a tick builds that output's frame). Null:
    /// unknown, commit-timing targets count as reached.
    unsigned long long (*cu_predict)(void* user, unsigned slot) = nullptr;
    /// When an update committed now must be ready to join a frame of output `slot`: the
    /// start of the next repaint that latches content (CLOCK_MONOTONIC ns) — the dma-fence
    /// deadline hint of its buffer fences. 0 or null: unknown, no hint.
    unsigned long long (*cu_latch)(void* user, unsigned slot) = nullptr;
    /// Arms output `slot`'s clock for the tick that presents at or after `target_ns`.
    void (*cu_request_at)(void* user, unsigned slot, unsigned long long target_ns) = nullptr;
    void* cu_user = nullptr;
    /// UI: a tick is between its dispatch and its publish (what is applied now joins the
    /// frame being built).
    bool cu_in_tick = false;
    /// linux-drm-syncobj-v1: the DRM fd timelines are imported on (the KMS card; -1: none)
    /// and whether the global is offered (`gfx::drm_timeline_supported`).
    int syncobj_drm_fd = -1;
    bool syncobj_supported = false;
    /// The newest composite fence render published (explicit-sync release points).
    lx::gfx::release_fence_mailbox* release_fences = nullptr;
    /// tearing-control-v1: whether async page flips can happen on some output at all (the
    /// hint is stored either way).
    bool async_flip_capable = false;

    // ── window and session protocols (design-app-compat-capture Part B) ──
    /// xdg-toplevel-icon-v1's icon store, owned by the compositor (null: the global is not
    /// advertised). The shell bridge sends its memfds.
    toplevel_icon_table* icons = nullptr;
    /// xdg-foreign's handle registry (v1 and v2), owned by the compositor (null: neither
    /// exporter nor importer is advertised).
    foreign_registry* foreign = nullptr;

    // ── screen capture (design-app-compat-capture Part D) ──
    /// What the compositor tells capture (outputs, backend, the render channel); null: no
    /// capture global is advertised.
    capture_host* capture = nullptr;

    // ── pointer protocols (design-app-compat-capture Part C) ──
    /// Moves the pointer to the global point (`x`, `y`) as the compositor moves it after
    /// motion — clamped onto the outputs, the cursor and pointer focus follow, the focus gets
    /// `wl_pointer.motion` — but with no relative motion: a pointer lock's cursor hint on
    /// unlock, a confined pointer brought back inside a new region. UI affinity. Null: no
    /// warps (constraints still work; the pointer just stays where it is).
    void (*warp_pointer)(void* user, double x, double y) = nullptr;
    void* warp_user = nullptr;
    /// UI. The session is paused (VT switched away), from the start of the pause's input
    /// teardown to the end of the resume's: no pointer constraint holds meanwhile.
    bool session_paused = false;

    /// Stage shm pixels in the client's own channel order instead of converting them to
    /// RGBA. The Vulkan composite path samples RGBA textures, so it needs the conversion;
    /// the CPU path writes a scanout buffer that already uses the client's order, and a
    /// per-pixel swizzle there is a full extra pass over the frame for nothing.
    bool shm_native_format = false;
};

/// Install wl_compositor, wl_subcompositor, xdg_wm_base, zwp_linux_dmabuf_v1,
/// wl_seat, wl_output, wl_data_device_manager with real bind handlers.
[[nodiscard]] lx::result<void> install_p0_protocols(p0_protocol_context& ctx);

/// Answers every pending wl_surface.frame callback (the 1 Hz paused tick: the display is
/// stopped, and a client blocked on its callback keeps running).
void fire_frame_callbacks(p0_protocol_context& ctx, unsigned frame_time_ms);
/// Answers the pending wl_surface.frame callbacks of surfaces on screen on any output (the
/// visibility pass of each view's newest frame; cursor surfaces always count) and, for the
/// others — occluded, off every output, unmapped, hidden — only once 995 ms have passed
/// since their surface's last callback (niri's throttle). What is held back is throttled:
/// it does not count for `frame_callbacks_pending`, so the frame clocks may stop with it
/// queued. `now_ns`: CLOCK_MONOTONIC. (The housekeeping tick.)
void fire_visible_frame_callbacks(p0_protocol_context& ctx, unsigned frame_time_ms,
                                  unsigned long long now_ns);
/// The same for one output's flip (ADR 008 M8): the callbacks of surfaces whose pace output
/// is `slot` — the output showing most of them among those that showed them in their newest
/// frame — are answered; surfaces another output paces wait for that output's flip; those
/// shown nowhere follow the 995 ms throttle.
void fire_output_frame_callbacks(p0_protocol_context& ctx, unsigned slot, unsigned frame_time_ms,
                                 unsigned long long now_ns);
/// A client is waiting on a wl_surface.frame callback that the next present answers
/// (throttled ones excluded).
[[nodiscard]] bool frame_callbacks_pending(const p0_protocol_context& ctx);
/// The outputs the pending callbacks wait on (bit per pace output; ~0 when one waits on any).
[[nodiscard]] std::uint32_t frame_callbacks_pending_outputs(const p0_protocol_context& ctx);
/// Throttled frame callbacks are queued (answered by `fire_visible_frame_callbacks` at
/// the next site after their 995 ms, or the housekeeping tick while the clock is stopped).
[[nodiscard]] bool frame_callbacks_throttled(const p0_protocol_context& ctx);

/// wp_presentation: frame `frame` was published to output `slot`; queued feedback of the
/// surfaces it shows and paces is sampled into it. UI affinity, once per published view
/// (not while the display is stopped: nothing is shown then).
void presentation_published(p0_protocol_context& ctx, unsigned frame, unsigned slot);
/// A content flip of the composite of frame `frame` on output `slot` completed with `info`:
/// feedback that output paces is presented or discarded
/// (`presentation_feedback_table::on_flip`). UI affinity.
void presentation_flipped(p0_protocol_context& ctx, unsigned frame, unsigned slot,
                          const present_info& info);
/// The display stopped (`queued_only` false: everything in flight is discarded), or the
/// stopped display's 1 Hz tick (`queued_only` true).
void presentation_discard(p0_protocol_context& ctx, bool queued_only);
/// Output `slot` went dark (powered off): the feedback it paces is discarded.
void presentation_discard_output(p0_protocol_context& ctx, unsigned slot);

/// ext_idle_notification_v1.idled / resumed, as the compositor's idle tracker emits them
/// (wire_idle.cpp). A sink with no functions without the protocol's glue.
[[nodiscard]] idle_sink idle_events(p0_protocol_context& ctx);
/// Whether an idle inhibitor is honoured: its surface is on screen on a lit output (the
/// visibility pass of that view's newest frame — not covered, on the output). The caller
/// adds what the frames cannot know: the session must be running. UI affinity.
[[nodiscard]] bool idle_inhibitor_visible(p0_protocol_context& ctx);
/// zwlr_output_power_v1.mode to every object of output `slot` (the power state machine
/// reports a change). UI affinity.
void notify_output_power(p0_protocol_context& ctx, unsigned slot, bool on);

/// ext_session_lock_v1: the wire half of a lock decision — `finished` and `locked` to the
/// objects named, then (`drop_surfaces`) every lock surface leaves the lock layer and goes
/// inert. UI affinity.
void session_lock_send(p0_protocol_context& ctx, const lock_actions& a);
/// Topmost lock surface under the global point (the window manager's lock hit-test). Fills
/// `surface_res`, the surface, its root and the surface-local position.
[[nodiscard]] bool session_lock_hit(p0_protocol_context& ctx, double x, double y,
                                    void*& surface_res, lx::surface_id& surface,
                                    lx::surface_id& root, double& sx, double& sy);

/// Topmost input-method popup shown under the global point (the window manager's hit-test:
/// candidate windows take clicks, never the keyboard). Fills the surface, its resource and
/// the surface-local position.
[[nodiscard]] bool input_popup_hit(p0_protocol_context& ctx, double x, double y,
                                   void*& surface_res, lx::surface_id& surface, double& sx,
                                   double& sy);

/// Screen capture (wire_capture.cpp; design-app-compat-capture Part D), UI affinity. Once per
/// tick after the outputs published (frame `frame`): constraints follow their sources, frames
/// are decided and their jobs posted, window sessions publish their capture views, the
/// privacy indicator follows. `publish` false (outside a tick: outputs changed): window
/// sessions wait for the next tick to publish.
void capture_tick(p0_protocol_context& ctx, unsigned frame, bool publish);
/// The render thread's completions: frame events, fence waits, read-back copy-outs.
void capture_complete(p0_protocol_context& ctx);
/// The session lock may have changed state (a lock request curtains every capture at once).
void capture_lock_check(p0_protocol_context& ctx);
/// Outputs whose clock must run for a window capture waiting on its window (bit per slot).
[[nodiscard]] std::uint32_t capture_wanted_outputs(const p0_protocol_context& ctx);
/// The oldest capture-view frame a posted window job still has to be composited from (-1:
/// none). UI releases no client buffer tagged with it or later before then: render may not
/// have taken the job yet, so its low-water mark cannot know of it.
[[nodiscard]] long long capture_view_floor(const p0_protocol_context& ctx);

/// Sends wl_buffer.release for a dmabuf buffer once the render path is done with it (and
/// signals its explicit-sync release point after the newest composite fence).
void release_wl_buffer_resource(void* wl_buffer_resource, void* user_data);
/// The buffer lifecycle settles a release point it took over from a destroyed wl_buffer:
/// signalled after the newest composite fence, or only dropped (its client is gone).
void release_sync_point(unsigned timeline, unsigned long long point, bool signal,
                        void* user_data);

// ── content-update queue (wire_surface.cpp) ──
/// The readiness poller's epoll fd (-1 before install): the compositor watches it as one
/// event-loop source and calls `content_updates_poll` when it is readable.
[[nodiscard]] int content_updates_poll_fd(p0_protocol_context& ctx);
void content_updates_poll(p0_protocol_context& ctx);
/// Tries every queued update (a tick reached the outputs' deadlines). UI affinity.
void content_updates_try(p0_protocol_context& ctx);
/// Output `slot` published (latched) frame `frame`: fifo barriers it paces clear and queued
/// updates waiting on them apply (they join the next frame).
void content_updates_latched(p0_protocol_context& ctx, unsigned slot, unsigned frame);
/// Outputs whose clock must keep ticking for a queued fifo update of a shown surface.
[[nodiscard]] std::uint32_t content_updates_waiting_outputs(p0_protocol_context& ctx);
/// The earliest commit-timing target queued for output `slot` (0: none).
[[nodiscard]] unsigned long long content_updates_next_target(p0_protocol_context& ctx,
                                                             unsigned slot);
/// Updates queued anywhere (0 normally).
[[nodiscard]] unsigned content_updates_queued(const p0_protocol_context& ctx);
/// A surface with a tearing hint of `async` is the topmost content of output `slot`'s
/// fullscreen window (tearing-control-v1, the policy's input).
[[nodiscard]] bool surface_wants_async(p0_protocol_context& ctx, lx::surface_id surface);
/// The wp_content_type_v1 type applied to `surface` (0 none, 1 photo, 2 video, 3 game).
[[nodiscard]] unsigned surface_content_type(p0_protocol_context& ctx, lx::surface_id surface);

// ── keyboard-shortcuts-inhibit-unstable-v1 (wire_shortcuts_inhibit.cpp) ──
/// The surface with the keyboard has an inhibitor in effect (the user has not toggled it
/// off; not while locked — the lock surface has the keyboard then). The shell's key grabs
/// and taps without `not_inhibitable` are skipped while it holds. UI affinity.
[[nodiscard]] bool shortcuts_inhibited(p0_protocol_context& ctx);
/// Super+Escape: the focused surface's inhibitor is toggled (`inactive` / `active`). False
/// when that surface has none — the key is then not the compositor's.
bool shortcuts_inhibit_toggle(p0_protocol_context& ctx);
// ── pointer-constraints, relative-pointer, pointer-gestures (wire_pointer_constraints.cpp,
//    wire_relative_pointer.cpp, wire_gestures.cpp; design-app-compat-capture Part C) ──
/// What an active pointer constraint did to a motion.
enum class pointer_constraint_kind : unsigned char { none, locked, confined };
/// A motion from the global point (`from_x`, `from_y`) to (`to_x`, `to_y`): with an active
/// lock the pointer stays (`locked`; the caller restores the position and sends no
/// `wl_pointer.motion`); with an active confinement `to` is clipped into the region
/// (`confined`). UI affinity, O(rects) per motion, nothing without an active constraint.
[[nodiscard]] pointer_constraint_kind pointer_constraint_motion(p0_protocol_context& ctx,
                                                                double from_x, double from_y,
                                                                double& to_x, double& to_y);
/// Re-evaluates which constraint holds (after a motion, a button, a pause or resume, a lock
/// change, once per tick): activations and deactivations are sent. Pointer and keyboard focus
/// changes and region commits re-evaluate by themselves. UI affinity; nothing to do — one
/// pointer test — while no constraint exists.
void pointer_constraints_update(p0_protocol_context& ctx);
/// Super+Escape: the active constraint is released (`unlocked` / `unconfined`; a oneshot is
/// spent, a persistent one re-arms only after the pointer leaves its surface or its window
/// loses the keyboard). False when none is active — the key is then not the compositor's.
bool pointer_constraints_release(p0_protocol_context& ctx);
/// The active constraint's kind (`none` without one).
[[nodiscard]] pointer_constraint_kind pointer_constraint_active(const p0_protocol_context& ctx);
/// zwp_relative_pointer_v1.relative_motion to the pointer focus's client (every relative
/// pointer it made), `time_us` CLOCK_MONOTONIC: accelerated and unaccelerated deltas. True
/// when something was sent (the caller then owes the pointers a `frame` if no motion
/// follows). UI affinity, no allocation.
bool relative_pointer_motion(p0_protocol_context& ctx, unsigned long long time_us, double dx,
                             double dy, double dx_unaccel, double dy_unaccel);
/// A touchpad gesture to the gesture objects of the client that had pointer focus when it
/// began (latched until it ends). UI affinity.
void pointer_gesture(p0_protocol_context& ctx, const lx::input::gesture_event& event);

/// wlr-output-management (wire_output_management.cpp, ADR 008 Part C). The heads as the
/// compositor sees them now: every bound zwlr_output_manager_v1 is sent what changed since
/// the last call (heads introduced or finished, modes, state), then `done` with a new serial.
/// Nothing at all when nothing changed. UI affinity; called when outputs or connectors
/// changed, coalesced to once per loop iteration.
void output_heads_changed(p0_protocol_context& ctx, const mgmt::head_set& heads);
/// The heads managers were last told about, and the serial of that `done`.
[[nodiscard]] const mgmt::head_set& output_mgmt_heads(const p0_protocol_context& ctx);
[[nodiscard]] unsigned output_mgmt_serial(const p0_protocol_context& ctx);
/// Applies and tests waiting for the compositor.
[[nodiscard]] mgmt::job_queue& output_mgmt_jobs(p0_protocol_context& ctx);
enum class mgmt_result : unsigned char { succeeded, failed, cancelled };
/// A job's answer to its configuration object, unless the client destroyed it meanwhile.
void output_mgmt_finish(p0_protocol_context& ctx, unsigned long long token, mgmt_result result);

/// `output_manager::generation` moved: every bound wl_output and zxdg_output_v1 is sent
/// the new state (then `done`), and every surface's output membership is recomputed from
/// its node rect. UI affinity; rare (a mode change, an output added or removed).
void notify_outputs_changed(p0_protocol_context& ctx);
/// The window manager's workspaces or memberships changed (its observer): every
/// ext_workspace_manager_v1 is brought in line, one `done` each. UI affinity.
void workspaces_changed(p0_protocol_context& ctx);

} // namespace lx::compositor


unsigned char* lx::compositor::shm_pixel_store::acquire(unsigned texture_id, unsigned bytes,
                                                        unsigned long long& token_out,
                                                        stamp next, stamp* previous) {
    token_out = 0;
    if (previous)
        *previous = {};
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
            for (stamp& s : entries_[i].stamps)
                s = {};
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
            for (stamp& s : entries_[i].stamps)
                s = {};
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
        slot_entry->stamps[chosen] = {};
    }
    if (previous)
        *previous = slot_entry->stamps[chosen];
    slot_entry->stamps[chosen] = next;

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
    for (stamp& s : e.stamps)
        s = {};
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
    ::munmap(b.data, static_cast<std::size_t>(b.size));
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
    const unsigned long long staged = in_flight_bytes_.load(std::memory_order_acquire);
    if (staged != 0 && staged + bytes > k_max_in_flight_bytes)
        return nullptr; // the backend is behind: this update is dropped, the buffer still released
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
        void* mem = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED)
            return nullptr;
        empty->data = static_cast<unsigned char*>(mem);
        empty->size = size;
        mapped_bytes_ += size;
        best = empty;
    }
    best->last_used_ns = now_ns;
    best->staged = bytes;
    in_flight_bytes_.fetch_add(bytes, std::memory_order_acq_rel);
    best->in_flight.store(true, std::memory_order_release);
    token_out = static_cast<unsigned long long>(best - blocks_) + 1;
    return best->data;
}

void lx::compositor::shm_upload_pool::release(unsigned long long token) {
    if (token == 0 || token > k_max_blocks)
        return;
    block& b = blocks_[token - 1];
    // `staged` was written before the acquire's release store this load pairs with.
    if (b.in_flight.load(std::memory_order_acquire)) {
        in_flight_bytes_.fetch_sub(b.staged, std::memory_order_acq_rel);
        b.in_flight.store(false, std::memory_order_release);
    }
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

bool lx::compositor::surface_damage_history::since_rects(unsigned long long after,
                                                         client_damage& out) const {
    if (after >= seq || seq - after > k_depth)
        return false;
    client_damage u{};
    for (unsigned long long s = after + 1; s <= seq; ++s)
        u.add_region(rects[s % k_depth]);
    out = u;
    return true;
}

bool lx::compositor::surface_damage_history::since(unsigned long long after,
                                                   lx::rect2i& out) const {
    client_damage u{};
    if (!since_rects(after, u))
        return false;
    out = u.bounds();
    return true;
}

lx::compositor::client_damage lx::compositor::shm_store_damage(
    const surface_damage_history& history, shm_pixel_store_stamp held, lx::surface_id surface,
    unsigned width, unsigned height) {
    const lx::rect2i full{0, 0, static_cast<int>(width), static_cast<int>(height)};
    client_damage out{};
    client_damage damage{};
    if (held.seq == 0 || held.surface != surface || !history.since_rects(held.seq, damage)) {
        out.add(full);
        return out;
    }
    damage.clip(full);
    // Empty damage since: the content did not change, but the slot is written anyway;
    // an empty region would read as "whole" downstream, so keep one pixel.
    if (damage.empty())
        damage.add({0, 0, full.width > 0 ? 1 : 0, full.height > 0 ? 1 : 0});
    return damage;
}

lx::rect2i lx::compositor::shm_store_region(const surface_damage_history& history,
                                            shm_pixel_store_stamp held, lx::surface_id surface,
                                            unsigned width, unsigned height) {
    return shm_store_damage(history, held, surface, width, height).bounds();
}

lx::compositor::client_damage lx::compositor::shm_upload_damage(
    const surface_damage_history& history, buffer_upload_state& state, lx::surface_id surface,
    unsigned width, unsigned height) {
    const lx::rect2i full{0, 0, static_cast<int>(width), static_cast<int>(height)};
    client_damage damage{};
    bool known = false;
    if (state.uploads > 0 && state.surface == surface)
        known = history.since_rects(state.seq, damage);
    if (!known) {
        damage.clear();
        damage.add(full);
    }
    // The name being written missed the previous upload of this texture; before two
    // uploads, one name has never been written at all.
    client_damage region{};
    if (state.uploads >= 2) {
        region = damage;
        region.add_region(state.last);
    } else {
        region.add(full);
    }
    // Clip to the buffer: damage is clipped where it is recorded, but stay safe.
    region.clip(full);
    if (region.empty())
        region.add(full);

    state.surface = surface;
    state.seq = history.seq;
    ++state.uploads;
    state.last = damage;
    return region;
}

lx::rect2i lx::compositor::shm_upload_region(const surface_damage_history& history,
                                             buffer_upload_state& state, lx::surface_id surface,
                                             unsigned width, unsigned height) {
    return shm_upload_damage(history, state, surface, width, height).bounds();
}
