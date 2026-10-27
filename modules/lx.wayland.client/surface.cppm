module;

// lx.wayland.client:surface — wl_surface and what a frame loop attaches to it: buffers
// (wl_shm pools, linux-dmabuf, single-pixel), damage_buffer, scale and transform, regions,
// the frame callback (requested only on demand, one outstanding), wp_viewport,
// wp_fractional_scale_v1, wp_presentation feedback per commit, and the
// wp_linux_drm_syncobj_v1 acquire / release points. Bodies in surface.cpp.

struct wl_surface;
struct wl_buffer;
struct wl_shm_pool;
struct wl_callback;
struct wp_viewport;
struct wp_fractional_scale_v1;
struct wp_presentation_feedback;
struct wp_linux_drm_syncobj_timeline_v1;
struct wp_linux_drm_syncobj_surface_v1;
struct zwp_linux_buffer_params_v1;

export module lx.wayland.client:surface;

import std;

import lx.foundation;
import :types;
import :display;

export namespace lx::wayland {

/// A memfd-backed wl_shm_pool, mapped shared. Created and grown on structural changes
/// (swapchain creation, resize) only.
class shm_pool {
public:
    shm_pool() = default;
    ~shm_pool();
    shm_pool(const shm_pool&) = delete;
    shm_pool& operator=(const shm_pool&) = delete;

    [[nodiscard]] lx::result<void> create(display& d, std::size_t bytes);
    /// Grows the file, the mapping and the pool (wl_shm_pool.resize: never shrinks).
    [[nodiscard]] lx::result<void> grow(std::size_t bytes);
    void destroy();
    [[nodiscard]] bool valid() const { return pool_ != nullptr; }
    [[nodiscard]] std::byte* data() const { return data_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] void* native() const { return pool_; }

private:
    wl_shm_pool* pool_ = nullptr;
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

/// A wl_buffer with its release tracked: `busy()` from attach+commit until wl_buffer.release.
class buffer {
public:
    buffer() = default;
    ~buffer();
    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;

    /// wl_shm_pool.create_buffer; `format` is a wl_shm format (0 argb8888, 1 xrgb8888, else
    /// the fourcc).
    [[nodiscard]] lx::result<void> create_shm(shm_pool& pool, std::size_t offset, size2i size,
                                              std::int32_t stride, std::uint32_t format);
    /// zwp_linux_buffer_params_v1 → create_immed. The plane fds are dup'd by libwayland on
    /// send; the caller keeps its own. A `failed` reaches `buffer_handler::on_failed`.
    [[nodiscard]] lx::result<void> create_dmabuf(display& d, std::span<const dmabuf_plane> planes,
                                                 size2i size, std::uint32_t fourcc,
                                                 std::uint64_t modifier, std::uint32_t flags = 0);
    /// wp_single_pixel_buffer_v1 (premultiplied, 32-bit channels): a solid background
    /// without a pool, scaled by a viewport.
    [[nodiscard]] lx::result<void> create_single_pixel(display& d, std::uint32_t r,
                                                       std::uint32_t g, std::uint32_t b,
                                                       std::uint32_t a);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool failed() const { return failed_; }
    [[nodiscard]] size2i size() const { return size_; }
    void set_handler(buffer_handler* h) { handler_ = h; }
    [[nodiscard]] void* native() const { return proxy_; }

    // surface.cpp
    void mark_busy() { busy_ = true; }
    void on_release();
    void on_failed();

private:
    wl_buffer* proxy_ = nullptr;
    zwp_linux_buffer_params_v1* params_ = nullptr;
    buffer_handler* handler_ = nullptr;
    size2i size_{};
    bool busy_ = false;
    bool failed_ = false;
};

/// An imported DRM syncobj timeline (wp_linux_drm_syncobj_timeline_v1).
class syncobj_timeline {
public:
    syncobj_timeline() = default;
    ~syncobj_timeline();
    syncobj_timeline(const syncobj_timeline&) = delete;
    syncobj_timeline& operator=(const syncobj_timeline&) = delete;

    /// import_timeline from a syncobj fd (DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD); the fd is sent
    /// (dup'd by libwayland), the caller keeps its own.
    [[nodiscard]] lx::result<void> create(display& d, int syncobj_fd);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    [[nodiscard]] void* native() const { return proxy_; }

private:
    wp_linux_drm_syncobj_timeline_v1* proxy_ = nullptr;
};

/// A wl_surface. Non-movable (listeners hold its address); `from_native` maps a wl_surface
/// that one of these created back to it (proxies are tagged).
class surface {
public:
    /// Presentation feedbacks outstanding at once: one per commit until it is presented or
    /// discarded. Three swapchain images plus slack; more are dropped and counted.
    static constexpr unsigned k_max_feedback = 8;

    surface() = default;
    ~surface();
    surface(const surface&) = delete;
    surface& operator=(const surface&) = delete;

    [[nodiscard]] lx::result<void> create(display& d);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }
    void set_handler(surface_handler* h) { handler_ = h; }
    [[nodiscard]] display* owner() const { return display_; }
    [[nodiscard]] void* native() const { return proxy_; }
    [[nodiscard]] static surface* from_native(const void* wl_surface);

    // ── pending state, applied by commit ──
    /// Attaches `b` (null detaches: unmaps). The offset is wl_surface.offset (v5) when
    /// nonzero.
    void attach(buffer* b, point2i offset = {});
    /// Buffer coordinates (wl_surface v4).
    void damage_buffer(rect2i r);
    void set_buffer_scale(std::int32_t scale);
    void set_buffer_transform(std::int32_t transform);
    /// Logical rects; empty clears (opaque: none; input: everywhere). A wl_region per call
    /// — a structural change, not per frame.
    void set_opaque_region(std::span<const rect2i> rects);
    void set_input_region(std::span<const rect2i> rects);
    void reset_input_region();
    /// One wl_surface.frame for the next commit, unless one is outstanding (idempotent): the
    /// idle rule — nothing is requested unless something will be drawn.
    void request_frame();
    [[nodiscard]] bool frame_pending() const { return frame_ != nullptr; }
    /// wp_presentation.feedback for the next commit (dropped and counted past
    /// `k_max_feedback` outstanding). False without wp_presentation.
    bool request_presentation_feedback();
    /// wl_surface.commit. The id names this commit in `presentation_event::commit_id`.
    std::uint64_t commit();

    // ── extensions, each created on first use ──
    [[nodiscard]] lx::result<void> enable_viewport();
    /// wp_viewport.set_source in buffer pixels (negative width: unset).
    void set_viewport_source(double x, double y, double w, double h);
    /// wp_viewport.set_destination in logical pixels (0 × 0 unsets).
    void set_viewport_destination(size2i size);
    [[nodiscard]] lx::result<void> enable_fractional_scale();
    [[nodiscard]] lx::result<void> enable_syncobj();
    /// wp_linux_drm_syncobj_surface_v1 points for the next commit (both are required with
    /// a buffer once a syncobj surface exists).
    void set_acquire_point(syncobj_timeline& t, std::uint64_t point);
    void set_release_point(syncobj_timeline& t, std::uint64_t point);

    /// The scale to render at, in 1/120: the fractional scale when enabled and received,
    /// else wl_surface.preferred_buffer_scale × 120 (v6), else 120.
    [[nodiscard]] std::uint32_t preferred_scale120() const;
    [[nodiscard]] std::int32_t preferred_buffer_scale() const { return buffer_scale_pref_; }
    [[nodiscard]] std::uint32_t preferred_transform() const { return transform_pref_; }
    /// Output slots this surface is on (bit = `display::output_at` slot).
    [[nodiscard]] std::uint32_t outputs() const { return outputs_; }
    [[nodiscard]] std::uint64_t commits() const { return commit_id_; }
    [[nodiscard]] unsigned frames_done() const { return frames_done_; }
    [[nodiscard]] unsigned feedback_dropped() const { return feedback_dropped_; }
    [[nodiscard]] unsigned feedback_outstanding() const;

    // Event entry points (surface.cpp).
    void on_enter(void* wl_output);
    void on_leave(void* wl_output);
    void on_preferred_buffer_scale(std::int32_t scale);
    void on_preferred_transform(std::uint32_t transform);
    void on_fractional_scale(std::uint32_t scale120);
    void on_frame_done(std::uint32_t time_ms);

    struct feedback_slot {
        surface* owner = nullptr;
        wp_presentation_feedback* proxy = nullptr;
        std::uint64_t commit_id = 0;
        /// Requested, waiting for the commit that names it.
        bool armed = false;
    };
    void on_feedback(feedback_slot& slot, const presentation_event* presented);

private:
    void set_region(bool opaque, std::span<const rect2i> rects);

    display* display_ = nullptr;
    wl_surface* proxy_ = nullptr;
    wl_callback* frame_ = nullptr;
    wp_viewport* viewport_ = nullptr;
    wp_fractional_scale_v1* fractional_ = nullptr;
    wp_linux_drm_syncobj_surface_v1* syncobj_ = nullptr;
    surface_handler* handler_ = nullptr;
    std::uint64_t commit_id_ = 0;
    unsigned frames_done_ = 0;
    unsigned feedback_dropped_ = 0;
    std::uint32_t version_ = 0;
    std::uint32_t outputs_ = 0;
    std::uint32_t fractional120_ = 0;
    std::int32_t buffer_scale_pref_ = 1;
    std::uint32_t transform_pref_ = 0;
    feedback_slot feedback_[k_max_feedback]{};
};

} // namespace lx::wayland
