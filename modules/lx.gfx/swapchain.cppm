module;

#include <stdint.h>

// lx.gfx:swapchain — a client window's pixel path (ADR-010 §4.10): the device a client renders
// on, the format it allocates, and a swapchain of images in one of three tiers —
//
//   vulkan  VkImage with VK_EXT_image_drm_format_modifier, memory exported as a dma-buf,
//           a timeline semaphore per image exported as a DRM syncobj (acquire / release
//           points), drawn by :ui_vk. Hardware devices only; lavapipe is refused.
//   gl      gbm_bo (modifiers from the feedback) → EGLImage → renderbuffer → FBO on GLES 3.1,
//           drawn by :ui_gl; EGL_ANDROID_native_fence_sync → sync_file → the image's DRM
//           syncobj at the acquire point. A software GL_RENDERER is refused.
//   cpu     one memfd per swapchain size (sealed against shrink), two images, drawn by
//           :ui_cpu straight into the mapping, released by wl_buffer.release.
//
// Wayland-free and API-type-free: what crosses this interface is fds, fourcc, modifiers,
// dev_t, plane offsets and strides and void* handles. lx.app maps the images onto wl_buffers
// (one per image, made when `swapchain_listener::on_image_created` says so) through
// lx.wayland.client; lx.gfx never imports it.
//
// Frame (UI affinity in a client, never blocking):
//   acquire()            a free image, or null when every image is still with the compositor
//                        (wait for `release_fd()` / wl_buffer.release, then call again)
//   add_age_damage()     what the image's content misses (buffer age; 0 = undefined → full)
//   draw into the image  `pixels` (cpu) / `gl_framebuffer` (gl) / `vk` (vulkan)
//   present(image, dmg)  ends the GPU work: exports the acquire point (explicit) or flushes
//                        (implicit); the image is busy until its release
//   release(id)          wl_buffer.release (implicit sync, shm) — or collect_releases() for
//                        the release points (explicit sync)
//
// Allocation happens in `create`, `resize`, and the first acquire of each slot after them
// (images are made on demand, up to the image count — a compositor that releases promptly
// keeps two images alive, not three). acquire / present / release / collect_releases
// allocate nothing. Bodies: swapchain.cpp (common, cpu, device choice), swapchain_gl.cpp,
// swapchain_vk.cpp.

export module lx.gfx:swapchain;

import std;

import lx.foundation;
import lx.runtime;
import :damage;
import :cpu_renderer;
import :drm_timeline;
import :vk_renderer;
import :gl_renderer;
import :ui_vk;

export namespace lx::gfx {

// ── formats and devices ─────────────────────────────────────────────────────────

inline constexpr std::uint32_t k_fourcc_argb8888 = 0x34325241u; ///< 'AR24'
inline constexpr std::uint32_t k_fourcc_xrgb8888 = 0x34325258u; ///< 'XR24'
inline constexpr std::uint32_t k_fourcc_argb2101010 = 0x30335241u; ///< 'AR30'
inline constexpr std::uint32_t k_fourcc_xrgb2101010 = 0x30335258u; ///< 'XR30'
inline constexpr std::uint64_t k_modifier_linear = 0;
inline constexpr std::uint64_t k_modifier_invalid = 0x00ffffffffffffffull;

/// The tier a client window draws with, in `automatic`'s order.
enum class client_tier : std::uint8_t { none, vulkan, gl, cpu };

[[nodiscard]] constexpr const char* client_tier_name(client_tier t) {
    switch (t) {
    case client_tier::none: return "none";
    case client_tier::vulkan: return "vulkan";
    case client_tier::gl: return "gl";
    case client_tier::cpu: return "cpu";
    }
    return "?";
}

/// `LUMEN_UI_BACKEND` / a settings string: vulkan, gl, cpu; anything else (or null) is none.
[[nodiscard]] client_tier parse_client_tier(const char* name);

/// One (fourcc, modifier) of a dma-buf feedback tranche.
struct client_format {
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
};

/// A zwp_linux_dmabuf_feedback_v1 tranche in plain values (lx.app copies them out of
/// lx.wayland.client's snapshot when it (re)creates a swapchain).
struct client_tranche {
    std::uint64_t target_device = 0;
    bool scanout = false;
    std::span<const client_format> formats{};
};

/// A feedback (`done`): the main device and the tranches in the compositor's order.
struct client_feedback {
    std::uint64_t main_device = 0;
    std::span<const client_tranche> tranches{};
};

inline constexpr unsigned k_client_max_modifiers = 32;

struct format_request {
    /// XRGB first (ARGB accepted); false: ARGB only.
    bool opaque = true;
    /// 2:10:10:10 first, 8-bit as the fallback. Off unless a window asks for it.
    bool ten_bit = false;
    /// Take the scanout-flagged tranche's set when the feedback has one for the device — a
    /// fullscreen surface's feedback, so the compositor can put the buffer on a plane.
    bool prefer_scanout = true;
};

struct format_choice {
    std::uint32_t fourcc = 0;
    /// The tranche's target device.
    std::uint64_t device = 0;
    /// From a scanout-flagged tranche (allocate with scanout usage).
    bool scanout = false;
    unsigned modifier_count = 0;
    /// Entries past `k_client_max_modifiers` (dropped, warned about once by the caller).
    unsigned modifiers_dropped = 0;
    std::uint64_t modifiers[k_client_max_modifiers]{};

    [[nodiscard]] std::span<const std::uint64_t> list() const {
        return {modifiers, modifier_count};
    }
    /// Only DRM_FORMAT_MOD_INVALID: the driver picks the layout (implicit modifier).
    [[nodiscard]] bool implicit_only() const {
        return modifier_count == 1 && modifiers[0] == k_modifier_invalid;
    }
    [[nodiscard]] bool has(std::uint64_t modifier) const;
};

/// Pure. The first tranche (in the compositor's order, scanout-flagged ones first when
/// `prefer_scanout`) whose target is `device` — the node the client renders on; 0 means the
/// feedback's main device — and that offers one of the requested fourccs, with every modifier
/// it lists for that fourcc. `gl_err::unsupported` when no tranche fits.
[[nodiscard]] lx::result<format_choice> choose_client_format(const client_feedback& feedback,
                                                             const format_request& request,
                                                             std::uint64_t device = 0);

/// An opened DRM render node.
struct render_node {
    lx::unique_fd fd{};
    /// st_rdev of the node opened.
    std::uint64_t dev = 0;
    /// It is the feedback's main device (or that device's render node); false: the main
    /// device was 0 or not a DRM device here, and the first render node was opened instead.
    bool matched = false;
    char path[64]{};
};

/// The render node of `main_device` (a primary or render node's dev_t, as linux-dmabuf
/// feedback sends it; drmGetDeviceFromDevId). 0 or an unknown device: the first
/// /dev/dri/renderD* (matched = false). `drm_err::open_failed` when there is none.
[[nodiscard]] lx::result<render_node> open_render_node(std::uint64_t main_device);

/// Whether two dev_t name the same DRM device (a card and its render node do).
[[nodiscard]] bool same_drm_device(std::uint64_t a, std::uint64_t b);

// ── the client's device ─────────────────────────────────────────────────────────

struct client_gpu_options {
    /// An explicit tier (the app's settings); none: `LUMEN_UI_BACKEND`, else automatic.
    /// A forced tier that cannot start is an error, not a fallback.
    client_tier force = client_tier::none;
    /// Read `LUMEN_UI_BACKEND` when `force` is none.
    bool read_environment = true;
    /// Accept a software rasterizer (lavapipe, llvmpipe) for a forced tier: tests that check
    /// the tier's correctness. Never in production, and reported (`software()`).
    bool allow_software = false;
    /// The node to render on instead of the feedback's main device (tests on a headless
    /// compositor, which advertises none). 0: the main device.
    std::uint64_t device_override = 0;
};

/// The device a client window renders on, chosen once per window in `automatic`'s order and
/// refusal rules — the compositor's `present_backend::automatic` (AGENTS.md §1.1): Vulkan on
/// the render node for a hardware device whose DRM node is the feedback's device, else
/// GLES 3.1 on GBM for a hardware GL_RENDERER, else wl_shm + the CPU renderer. Logged once
/// (`gfx.swapchain`). UI affinity.
class client_gpu {
public:
    client_gpu() = default;
    ~client_gpu();
    client_gpu(const client_gpu&) = delete;
    client_gpu& operator=(const client_gpu&) = delete;

    [[nodiscard]] lx::result<void> create(const client_feedback& feedback,
                                          const client_gpu_options& options = {});
    void destroy();

    [[nodiscard]] client_tier tier() const { return tier_; }
    /// The tier runs on a software rasterizer (only when `allow_software` let it).
    [[nodiscard]] bool software() const { return software_; }
    /// The tier was forced (settings or environment).
    [[nodiscard]] bool forced() const { return forced_; }
    /// The render node (-1 for cpu).
    [[nodiscard]] int drm_fd() const { return node_.fd.get(); }
    [[nodiscard]] std::uint64_t device() const { return node_.dev; }
    [[nodiscard]] const render_node& node() const { return node_; }
    /// The device / renderer name ("SVGA3D; build: RELEASE;", "AMD Radeon …", "cpu").
    [[nodiscard]] const char* name() const { return name_; }
    /// Why a tier was not taken ("" when it was, or was not tried).
    [[nodiscard]] const char* refusal(client_tier t) const;
    /// The render node has timeline syncobjs with eventfd waits (linux-drm-syncobj-v1 can be
    /// used); the window also needs the compositor's wp_linux_drm_syncobj_manager_v1.
    [[nodiscard]] bool timeline_syncobj() const { return timeline_; }

    /// gl: the EGL display / context on the render node (current on the creating thread).
    [[nodiscard]] egl_device* egl() { return tier_ == client_tier::gl ? &egl_ : nullptr; }
    /// vulkan: the device's handles for :ui_vk and the swapchain (invalid on other tiers).
    [[nodiscard]] vk_context vulkan_context() const;

private:
    void refuse(client_tier t, const char* why);
    [[nodiscard]] lx::result<void> try_vulkan(bool forced, bool allow_software);
    [[nodiscard]] lx::result<void> try_gl(bool forced, bool allow_software);

    render_node node_{};
    /// vulkan: a `gfx::device` (owned; the type lives in the facade, so it is held here
    /// type-erased and made / destroyed in swapchain_vk.cpp).
    void* vk_ = nullptr;
    egl_device egl_{};
    client_tier tier_ = client_tier::none;
    bool software_ = false;
    bool forced_ = false;
    bool timeline_ = false;
    char name_[128]{};
    char refused_[4][96]{};
};

// ── the swapchain ───────────────────────────────────────────────────────────────

/// Images per GPU swapchain: the compositor's three scanout slots, for the same reason
/// (rendering-performance.md §6.1): one on screen, one queued, one being drawn.
inline constexpr unsigned k_swapchain_images = 3;
/// Images per shm swapchain (ADR-010 §4.10): the compositor copies shm at commit, so the
/// one it shows is not the one it holds; `swapchain_options::images` can ask for 3.
inline constexpr unsigned k_shm_swapchain_images = 2;
/// Old images a resize leaves waiting for their release; past this the oldest is destroyed
/// at once (`overflow_action::warn_and_drop`: the compositor holds its own reference to the
/// memory, only the wl_buffer goes early).
inline constexpr unsigned k_swapchain_retired = 6;
/// Frames of damage kept for buffer age; an image older than this repaints in full.
inline constexpr unsigned k_swapchain_damage_history = 4;

struct swapchain_options {
    unsigned width = 0;
    unsigned height = 0;
    /// dma-buf tiers: the fourcc and modifiers (`choose_client_format`). The cpu tier takes
    /// ARGB8888 / XRGB8888 from `opaque`.
    format_choice format{};
    bool opaque = true;
    /// 0: `k_swapchain_images` for dma-buf tiers, `k_shm_swapchain_images` for cpu (max 3).
    unsigned images = 0;
    /// Use DRM syncobj timelines (linux-drm-syncobj-v1) when the device has them; the
    /// window turns this off when the compositor has no wp_linux_drm_syncobj_manager_v1.
    bool explicit_sync = true;
    /// The kernel timeline operations (tests: a fake kernel timeline). Empty: the real ones
    /// on the render node.
    drm_timeline_ops timeline{};
    /// GL tier: skip the native fence even when EGL has it (forces the implicit path for
    /// the test that proves it).
    bool disable_native_fence = false;
};

struct swapchain_plane {
    /// Borrowed: owned by the swapchain until `on_image_destroyed`.
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

/// One image as the frame sees it. Valid from `acquire` (or `on_image_created`) until the
/// image is destroyed.
struct swapchain_image {
    /// Unique over the swapchain's life (≥ 1); the key lx.app keeps its wl_buffer under.
    std::uint32_t id = 0;
    unsigned slot = 0;
    unsigned width = 0;
    unsigned height = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    unsigned plane_count = 0;
    swapchain_plane planes[4]{};
    /// cpu: the memfd's size (planes[0].fd is the pool; one pool per swapchain size).
    std::size_t pool_bytes = 0;
    std::uint32_t pool_id = 0;
    /// Frames since this image's content was current (EGL_EXT_buffer_age): 0 undefined (a
    /// new image) — repaint all; 1 the previous frame's content; n misses n − 1 frames.
    unsigned age = 0;
    /// Explicit sync: the image's DRM syncobj timeline fd (wp_linux_drm_syncobj_manager_v1.
    /// import_timeline; borrowed) and this frame's points on it. -1 / 0 on the implicit path.
    int timeline_fd = -1;
    std::uint64_t acquire_point = 0;
    std::uint64_t release_point = 0;
    /// The draw targets: one is set, per tier.
    pixel_surface pixels{};
    unsigned gl_framebuffer = 0;
    ui_vk_attachment vk{};
};

/// Structural events (UI affinity): lx.app makes and destroys the image's wl_buffer here.
class swapchain_listener {
public:
    virtual ~swapchain_listener() = default;
    virtual void on_image_created(const swapchain_image& image) = 0;
    virtual void on_image_destroyed(std::uint32_t id) = 0;
};

struct swapchain_stats {
    /// Image sets allocated: one for `create`, one per `resize` to a new size (G11).
    unsigned allocations = 0;
    unsigned images_created = 0;
    unsigned images_destroyed = 0;
    unsigned presents = 0;
    unsigned explicit_presents = 0;
    /// Acquires that found every image busy (the frame waits for a release).
    unsigned acquire_none = 0;
    /// Old images destroyed before their release (the retired list was full).
    unsigned retired_overflow = 0;
    /// gbm: 2 gbm_bo_create_with_modifiers2, 1 gbm_bo_create_with_modifiers, 0 gbm_bo_create
    /// with the implicit modifier (the last image made).
    unsigned gbm_path = 0;
    /// Native fences exported into acquire points.
    unsigned native_fences = 0;
};

class client_swapchain {
public:
    client_swapchain() = default;
    /// `destroy`.
    ~client_swapchain();
    client_swapchain(const client_swapchain&) = delete;
    client_swapchain& operator=(const client_swapchain&) = delete;

    /// For `gpu`'s tier (borrowed; it outlives the swapchain). No image is made yet.
    [[nodiscard]] lx::result<void> create(client_gpu& gpu, const swapchain_options& options);
    /// Waits for the device once (structural), destroys every image (`on_image_destroyed`).
    void destroy();
    [[nodiscard]] bool valid() const { return tier_ != client_tier::none; }
    [[nodiscard]] client_tier tier() const { return tier_; }
    /// Acquire and release points are used (the device has syncobjs and the options asked).
    [[nodiscard]] bool explicit_sync() const { return explicit_; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }
    [[nodiscard]] unsigned image_count() const { return count_; }
    [[nodiscard]] std::uint32_t fourcc() const { return fourcc_; }

    void set_listener(swapchain_listener* l) { listener_ = l; }
    /// The affinity acquire / present / release must run on (`ui` in a client); `any` (the
    /// default) checks nothing.
    void set_owner_affinity(lx::runtime::affinity a) { owner_ = a; }

    /// A free image — the one with the newest content (smallest age) — or, when none is free
    /// and a slot is still empty, a new one. Null: every image is with the compositor; never
    /// blocks. GL: makes the context current. Vulkan: submits the image's acquire barrier.
    [[nodiscard]] lx::result<swapchain_image*> acquire();
    /// Adds to `out` what `image`'s content misses (the damage of the frames since it was
    /// current); full for age 0 or an age past the history. Call before the frame's draw.
    void add_age_damage(const swapchain_image& image, damage_region& out) const;
    /// Ends the frame's GPU work into `image`: GL exports a native fence into the acquire
    /// point (explicit) or flushes (implicit); Vulkan releases the image to the foreign queue
    /// and signals the acquire point (explicit) or attaches the frame's sync_file to the
    /// dma-buf (implicit); cpu does nothing. `damage` (≤ 16 rects; more merge) is recorded
    /// for the images' age. Then attach, damage_buffer and commit are the caller's.
    [[nodiscard]] lx::result<void> present(swapchain_image& image, const damage_region& damage);
    /// An acquired image the frame did not present: free again, content unchanged.
    void cancel(swapchain_image& image);
    /// wl_buffer.release for image `id` (implicit sync and shm; ignored with explicit sync,
    /// where linux-drm-syncobj-v1 makes wl_buffer.release undefined). Frees a retired image.
    void release(std::uint32_t id);
    /// Explicit sync: reads `release_fd` and frees every busy image whose release point has
    /// signalled (non-blocking). Returns how many were freed.
    unsigned collect_releases();
    /// Explicit sync: an eventfd readable once a busy image's release point signals; watch it
    /// in the loop while `acquire` returns null. -1 on the implicit path.
    [[nodiscard]] int release_fd() const { return release_fd_.get(); }
    /// A new size: the free images are destroyed now, busy ones once released; the new set
    /// is made on demand. Returns whether this was a new size (one allocation counted).
    [[nodiscard]] lx::result<bool> resize(unsigned width, unsigned height);

    [[nodiscard]] unsigned busy() const;
    [[nodiscard]] unsigned retired() const;
    [[nodiscard]] const swapchain_stats& stats() const { return stats_; }

    /// Bench hook: waits until the last presented frame's GPU work has finished and returns
    /// the wall time waited (CLOCK_MONOTONIC ns). GL: glFinish; Vulkan: the frame's fence;
    /// cpu: 0. Not for the frame path.
    [[nodiscard]] lx::result<std::uint64_t> wait_presented();

private:
    enum class slot_state : std::uint8_t { empty, free, acquired, busy };
    struct slot {
        slot_state state = slot_state::empty;
        swapchain_image image{};
        std::uint64_t last_frame = 0; ///< frame its content is from (0: none)
        std::uint64_t uses = 0;       ///< presents (timeline point pairs used)
        lx::unique_fd plane_fds[4]{};
        lx::unique_fd timeline{};      ///< syncobj fd handed to the compositor
        unsigned timeline_handle = 0;  ///< the same syncobj on our render node
        // gl
        void* bo = nullptr;
        void* egl_image = nullptr;
        unsigned renderbuffer = 0;
        unsigned framebuffer = 0;
        // vulkan
        void* vk_image = nullptr;
        void* vk_memory = nullptr;
        void* vk_view = nullptr;
        void* vk_timeline = nullptr;   ///< VkSemaphore (timeline, exported)
        void* vk_binary = nullptr;     ///< VkSemaphore (binary, sync_fd export: implicit path)
        void* vk_wait = nullptr;       ///< VkSemaphore (binary, sync_fd import: implicit acquire)
        void* vk_fence = nullptr;      ///< the release submit's fence
        void* cb_first = nullptr;      ///< UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
        void* cb_acquire = nullptr;    ///< foreign / GENERAL → COLOR_ATTACHMENT_OPTIMAL
        void* cb_release = nullptr;    ///< COLOR_ATTACHMENT_OPTIMAL → foreign / GENERAL
        bool vk_owned = false;         ///< acquired from the foreign queue, not yet released
        bool vk_submitted = false;     ///< the fence has a submit behind it
        unsigned pool = 0;             ///< cpu: index into pools_
    };
    struct shm_block {
        lx::unique_fd fd{};
        std::byte* map = nullptr;
        std::size_t bytes = 0;
        std::uint32_t id = 0;
        unsigned refs = 0;
    };
    static constexpr unsigned k_shm_blocks = 1 + k_swapchain_retired;

    [[nodiscard]] lx::result<void> make_image(slot& s, unsigned index);
    void destroy_image(slot& s);
    /// The tier's objects and the timeline handle (no listener call, no stats).
    void destroy_native(slot& s);
    void retire(slot& s);
    void free_retired(std::uint32_t id);
    [[nodiscard]] slot* find(std::uint32_t id);
    [[nodiscard]] bool release_signalled(const slot& s) const;

    // per tier (swapchain_gl.cpp, swapchain_vk.cpp, swapchain.cpp)
    [[nodiscard]] lx::result<void> make_gl(slot& s);
    void destroy_gl(slot& s);
    [[nodiscard]] lx::result<void> acquire_gl(slot& s);
    [[nodiscard]] lx::result<void> present_gl(slot& s);
    [[nodiscard]] lx::result<std::uint64_t> wait_gl();
    [[nodiscard]] lx::result<void> make_vk(slot& s);
    void destroy_vk(slot& s);
    [[nodiscard]] lx::result<void> acquire_vk(slot& s);
    [[nodiscard]] lx::result<void> present_vk(slot& s);
    [[nodiscard]] lx::result<std::uint64_t> wait_vk();
    [[nodiscard]] bool vk_idle(const slot& s) const;
    void vk_wait_device();
    [[nodiscard]] lx::result<void> make_vk_pool();
    void destroy_vk_pool();
    [[nodiscard]] lx::result<void> make_shm(slot& s);
    [[nodiscard]] lx::result<unsigned> ensure_shm_block();
    void unref_shm_block(unsigned index);
    [[nodiscard]] lx::result<void> make_timeline(slot& s);

    client_gpu* gpu_ = nullptr;
    client_tier tier_ = client_tier::none;
    swapchain_listener* listener_ = nullptr;
    lx::runtime::affinity owner_ = lx::runtime::affinity::any;
    swapchain_options options_{};
    drm_timeline_ops ops_{};
    unsigned width_ = 0;
    unsigned height_ = 0;
    unsigned count_ = 0;
    std::uint32_t fourcc_ = 0;
    bool explicit_ = false;
    bool native_fence_ = false;
    bool warned_fence_ = false;
    std::uint32_t next_id_ = 1;
    std::uint32_t next_pool_id_ = 1;
    std::uint64_t frame_ = 0;
    slot slots_[k_swapchain_images]{};
    slot retired_[k_swapchain_retired]{};
    unsigned retired_count_ = 0;
    shm_block blocks_[k_shm_blocks]{};
    int current_block_ = -1;
    lx::unique_fd release_fd_{};
    std::uint64_t history_frame_[k_swapchain_damage_history]{};
    damage_region history_[k_swapchain_damage_history]{};
    swapchain_image* last_presented_ = nullptr;
    swapchain_stats stats_{};
    // GL: the context the images were made in, and the native-fence entry points.
    void* gl_context_ = nullptr;
    void* gl_fns_[4]{};
    // Vulkan entry points and the command pool (resolved once at create).
    void* vk_pool_ = nullptr;
    void* vk_fns_[12]{};
    bool vk_foreign_ = false;
    bool vk_sync_file_ = false; ///< binary semaphores export / import SYNC_FD
};

} // namespace lx::gfx
