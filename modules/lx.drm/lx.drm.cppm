module;

#include <atomic>
#include <chrono>
#include <memory>

#if defined(LUMEN_HAS_DRM)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
// Linux 6.8 uapi; libdrm's bundled drm.h predates it.
#ifndef DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6
#endif
#endif

import lx.foundation;
import lx.runtime;

export module lx.drm;

export import :atomic;
export import :cursor;
export import :crtc_alloc;
export import :edid;
export import :uevent;
export import :modes;

export namespace lx::drm {

enum class connector_status { connected, disconnected, unknown };

/// Connectors, CRTCs and planes one device describes at most (vmwgfx has 8 connectors;
/// real GPUs ≤ 6 CRTCs).
inline constexpr unsigned k_max_connectors = 16;
inline constexpr unsigned k_max_crtcs = 32;
inline constexpr unsigned k_max_planes = 64;
/// Modes kept per connector (vmwgfx offers its preferred mode plus 34; a 4K monitor ~60).
inline constexpr unsigned k_max_modes = 96;

struct mode {
    unsigned width = 0;
    unsigned height = 0;
    unsigned refresh_millihz = 60000;
    bool preferred = false;
};

/// A summary of one connector of the last probe (`kms_device::connector`).
struct connector_info {
    unsigned id = 0;
    /// The CRTC it is routed to now (0: none).
    unsigned crtc_id = 0;
    connector_status status = connector_status::unknown;
    /// The kernel's connector name, `<type>-<type id>` ("DP-1", "HDMI-A-2", "Virtual-1"):
    /// what wlroots, Mutter and KWin call the output, so a name is the same everywhere.
    const char* name = "";
    /// DRM_MODE_CONNECTOR_* and its per-type index.
    unsigned type = 0;
    unsigned type_id = 0;
    /// Physical size of the display, 0 when unknown (projectors, virtual outputs).
    unsigned phys_w_mm = 0;
    unsigned phys_h_mm = 0;
};

/// Everything one probe of one connector tells (`kms_device::probe_connectors`). Plain
/// data: a scan is produced on one thread and read on another.
struct connector_probe {
    unsigned connector_id = 0;
    unsigned type = 0;
    unsigned type_id = 0;
    /// "DP-2", "Virtual-1".
    char name[32]{};
    connector_status status = connector_status::unknown;
    /// From EDID (`parse_edid_identity`); empty when absent.
    char make[64]{};
    char model[64]{};
    char serial[64]{};
    bool edid = false;
    unsigned phys_w_mm = 0;
    unsigned phys_h_mm = 0;
    kms_mode modes[k_max_modes]{};
    unsigned mode_count = 0;
    /// Index into `modes` of the preferred mode, -1 when none is flagged.
    int preferred = -1;
    /// Bit per CRTC index: the OR of its encoders' `possible_crtcs`.
    unsigned possible_crtcs = 0;
    /// The CRTC it is routed to now (atomic CRTC_ID; a previous master's at start).
    unsigned current_crtc_id = 0;
    /// wl_output.subpixel (DRM_MODE_SUBPIXEL_* maps 1:1 onto it).
    unsigned subpixel = 0;
    /// `non-desktop` (VR headsets): never part of the desktop.
    bool non_desktop = false;
    bool vrr_capable = false;
    /// `hotplug_mode_update` (virtual GPUs: the host decides modes and layout).
    bool hotplug_mode_update = false;
    /// `link-status` is Bad: the link needs a modeset to retrain.
    bool link_bad = false;
    /// `suggested X` / `suggested Y` (virtual GPUs: the host's layout); -1 when absent.
    long suggested_x = -1;
    long suggested_y = -1;

    [[nodiscard]] const kms_mode* preferred_mode() const {
        if (preferred >= 0 && static_cast<unsigned>(preferred) < mode_count)
            return &modes[preferred];
        return mode_count > 0 ? &modes[0] : nullptr;
    }
};

/// One probe of every connector of a device (or of the hinted one, the others copied).
struct connector_scan {
    unsigned count = 0;
    connector_probe entries[k_max_connectors]{};
    /// Bumped by every probe.
    unsigned generation = 0;
    /// Every connector of the device fit (count ≤ k_max_connectors).
    bool complete = true;
};

struct kms_crtc_info {
    unsigned id = 0;
    unsigned index = 0;
    /// ACTIVE when the device was enumerated (a previous master's: at start, or another
    /// master's while we were away, at a resume).
    bool active = false;
};

struct kms_plane_info {
    unsigned id = 0;
    plane_kind kind = plane_kind::overlay;
    unsigned possible_crtcs = 0;
    /// The CRTC it is bound to now (0: none).
    unsigned crtc_id = 0;
    /// Takes XRGB8888 (a primary plane the compositor can scan out on) / ARGB8888 (cursor).
    bool xrgb8888 = false;
    bool argb8888 = false;
};

/// "DP", "HDMI-A", "Virtual", … for a DRM_MODE_CONNECTOR_* value — the kernel's own names,
/// as a static table so no libdrm version (drmModeGetConnectorTypeName is 2.4.112+) is
/// needed. "Unknown" past the table.
[[nodiscard]] const char* connector_type_name(unsigned type);

/// Refresh in mHz from a mode's timings (wlroots' `calculate_refresh_rate`): the pixel clock
/// over the frame size, interlace, double scan and vscan applied. 0 for a degenerate mode.
[[nodiscard]] unsigned mode_refresh_mhz(unsigned clock_khz, unsigned htotal, unsigned vtotal,
                                        unsigned vscan, unsigned flags);

/// The KMS device: resources (CRTCs, planes), connector probing, and the event stream every
/// pipeline's flips arrive on. UI affinity for `dispatch_events`; probing is a pure function
/// of the fd (a worker may run it).
class kms_device {
public:
    kms_device() = default;
    ~kms_device();

    kms_device(const kms_device&) = delete;
    kms_device& operator=(const kms_device&) = delete;
    kms_device(kms_device&& other) noexcept;
    kms_device& operator=(kms_device&& other) noexcept;

    /// Open a KMS card node. Uses direct open; callers with logind should prefer
    /// `open_fd` after `logind_session::take_device_path`.
    [[nodiscard]] static lx::result<kms_device> open(const char* device_path = nullptr);
    /// Sets the atomic, universal-planes and cursor-hotspot caps, enumerates the resources
    /// and probes every connector once (`last_scan`).
    [[nodiscard]] static lx::result<kms_device> open_fd(lx::unique_fd card_fd);

    /// CRTCs and planes, with what is bound to them now. At open; again at resume.
    [[nodiscard]] lx::result<void> enumerate_resources();
    [[nodiscard]] unsigned crtc_count() const { return crtc_count_; }
    [[nodiscard]] kms_crtc_info crtc(unsigned index) const {
        return index < crtc_count_ ? crtcs_[index] : kms_crtc_info{};
    }
    /// Resource index of CRTC `crtc_id`, -1 when unknown.
    [[nodiscard]] int crtc_index_of(unsigned crtc_id) const;
    [[nodiscard]] unsigned plane_count() const { return plane_count_; }
    [[nodiscard]] kms_plane_info plane(unsigned index) const {
        return index < plane_count_ ? planes_[index] : kms_plane_info{};
    }

    /// Probes every connector (or only `hint_connector_id` when nonzero, the others copied
    /// from `out` as it is) into `out`, and with `record` keeps it as `last_scan`. Only the
    /// DRM master gets a forced probe — the kernel answers anyone else from its cache
    /// ("demoting to read-only probe"). May read EDID over DDC for tens of ms on real
    /// monitors. With `record` false it touches nothing of this object but the fd, so a
    /// worker thread may run it (the hotplug probe, ADR 008 H2) while other threads commit.
    [[nodiscard]] lx::result<void> probe_connectors(unsigned hint_connector_id,
                                                    connector_scan& out, bool record = true);
    /// The newest recorded probe (at open, and every recording `probe_connectors`).
    [[nodiscard]] const connector_scan& last_scan() const;

    // Summary of `last_scan` (tests, tools).
    [[nodiscard]] unsigned connector_count() const;
    [[nodiscard]] connector_info connector(unsigned index) const;
    /// The preferred mode of connector `index` of `last_scan`.
    [[nodiscard]] lx::result<mode> preferred_mode(unsigned index) const;
    /// A pipeline for connector `index` of `last_scan` on its own: its routed CRTC, else
    /// the first possible one, with that CRTC's primary plane (tests, single-output tools).
    [[nodiscard]] lx::result<kms_pipeline_desc> pipeline_desc_for(unsigned index) const;

    /// Borrowed DRM master / card FD for KMS ioctls (never transfers ownership).
    [[nodiscard]] int card_fd() const;

    /// True when this FD holds DRM master. Modesetting and atomic commits require it, so
    /// a nested session (another compositor owns the card) can enumerate but not present.
    [[nodiscard]] bool is_master() const;

    /// DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP: an atomic commit may carry DRM_MODE_PAGE_FLIP_ASYNC
    /// (tearing-control-v1). Asked once, at startup.
    [[nodiscard]] bool supports_async_flip() const;

    /// Open an owning render-node FD suitable for Vulkan / GBM (`/dev/dri/renderD128`).
    [[nodiscard]] lx::result<lx::unique_fd> open_render_node() const;

    /// DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT was accepted. Virtualised drivers hide their
    /// cursor plane from atomic clients that have not set it.
    [[nodiscard]] bool cursor_hotspot_cap() const { return cursor_hotspot_cap_; }

    /// The thread that owns the flip handlers (UI): drains the card fd once (it is
    /// nonblocking, so this never waits), routes every flip event to the pipeline whose CRTC
    /// it names, and reports blocking modesets' completions. An event from before a
    /// pipeline's last `prepare_resume` is dropped unreported.
    void dispatch_events();

    /// CRTC indices another master left active (found by `enumerate_resources`: at open,
    /// and again at each resume) and no modeset of ours has turned off or taken since (a
    /// modeset turns off those no registered pipeline owns, and unroutes every connector the
    /// kernel has on them now).
    [[nodiscard]] unsigned stale_crtc_mask() const;
    /// The registered pipeline driving `crtc_id`, null when none.
    [[nodiscard]] kms_pipeline* pipeline_for_crtc(unsigned crtc_id) const;

private:
    friend class kms_pipeline;
    friend lx::result<void> commit_modeset(kms_device& device, const modeset_entry* entries,
                                           unsigned count, bool test_only);

    void release();
    [[nodiscard]] lx::result<void> init_from_fd();
    /// Pipeline registry (the routing table of `dispatch_events`). False when full.
    [[nodiscard]] bool register_pipeline(kms_pipeline* pipe, unsigned& slot);
    void unregister_pipeline(unsigned slot);
    /// CRTC `index` went dark through a modeset of ours.
    void note_crtc_inactive(unsigned index);
    [[nodiscard]] flip_cookie& next_combined_cookie();

    lx::unique_fd card_fd_{};
    kms_crtc_info crtcs_[k_max_crtcs]{};
    unsigned crtc_count_ = 0;
    kms_plane_info planes_[k_max_planes]{};
    unsigned plane_count_ = 0;
    std::unique_ptr<connector_scan> scan_{};
    std::atomic<kms_pipeline*> pipelines_[k_max_pipelines]{};
    flip_cookie combined_cookies_[2]{};
    unsigned combined_seq_ = 0;
    std::atomic<unsigned> stale_crtcs_{0};
    bool cursor_hotspot_cap_ = false;
};


/// A scanout framebuffer backed by an imported dma-buf — the object that lets the
/// compositor's own render target be handed to an atomic commit.
class kms_framebuffer {
public:
    kms_framebuffer() = default;
    ~kms_framebuffer();

    kms_framebuffer(const kms_framebuffer&) = delete;
    kms_framebuffer& operator=(const kms_framebuffer&) = delete;
    kms_framebuffer(kms_framebuffer&& other) noexcept;
    kms_framebuffer& operator=(kms_framebuffer&& other) noexcept;

    /// Imports `dmabuf_fd` (borrowed) via PRIME and registers it with
    /// `drmModeAddFB2WithModifiers`.
    [[nodiscard]] static lx::result<kms_framebuffer> import_dmabuf(
        const kms_device& device, int dmabuf_fd, unsigned width, unsigned height,
        unsigned stride, unsigned offset, lx::fourcc format, unsigned long long modifier);

    [[nodiscard]] unsigned id() const { return fb_id_; }
    [[nodiscard]] bool valid() const { return fb_id_ != 0; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }

private:
    void release();

    int card_fd_ = -1;
    unsigned fb_id_ = 0;
    unsigned gem_handle_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

/// A scanout framebuffer the CPU can write into directly (DRM dumb buffer).
///
/// The GPU-composite path exports a Vulkan image as a dma-buf and imports it as a
/// `kms_framebuffer`. That is the right shape when compositing happens on a real GPU. When
/// the only Vulkan device is a software rasterizer, routing pixels through it costs an
/// upload, a tiling pass and a fullscreen fragment shader to produce bytes the CPU already
/// had — so this gives the CPU compositor a scanout target it can write once and flip.
///
/// The mapping is write-combining: read-modify-write of the mapped pixels is very slow.
/// Writers must produce whole rows sequentially and never read back.
class kms_dumb_framebuffer {
public:
    kms_dumb_framebuffer() = default;
    ~kms_dumb_framebuffer();

    kms_dumb_framebuffer(const kms_dumb_framebuffer&) = delete;
    kms_dumb_framebuffer& operator=(const kms_dumb_framebuffer&) = delete;
    kms_dumb_framebuffer(kms_dumb_framebuffer&& other) noexcept;
    kms_dumb_framebuffer& operator=(kms_dumb_framebuffer&& other) noexcept;

    /// Allocates via `DRM_IOCTL_MODE_CREATE_DUMB`, maps it, and registers it with
    /// `drmModeAddFB2`. `format` must be a 32-bpp fourcc — dumb buffers are described to
    /// the kernel by bit depth, so packed 8:8:8:8 is the only layout this allocates.
    [[nodiscard]] static lx::result<kms_dumb_framebuffer> create(const kms_device& device,
                                                                 unsigned width, unsigned height,
                                                                 lx::fourcc format);

    [[nodiscard]] unsigned id() const { return fb_id_; }
    [[nodiscard]] bool valid() const { return fb_id_ != 0 && pixels_ != nullptr; }
    [[nodiscard]] unsigned width() const { return width_; }
    [[nodiscard]] unsigned height() const { return height_; }
    [[nodiscard]] unsigned stride() const { return stride_; }
    [[nodiscard]] lx::fourcc format() const { return format_; }
    [[nodiscard]] unsigned size_bytes() const { return size_; }

    /// Mapped, write-combining pixels. Null until `create` succeeds.
    [[nodiscard]] unsigned char* pixels() const { return pixels_; }

private:
    void release();

    int card_fd_ = -1;
    unsigned fb_id_ = 0;
    unsigned gem_handle_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
    unsigned stride_ = 0;
    unsigned size_ = 0;
    lx::fourcc format_ = 0;
    unsigned char* pixels_ = nullptr;
};

} // namespace lx::drm

module :private;

namespace {

[[nodiscard]] lx::error drm_error(lx::drm_err code, const char* message) {
    return lx::make_error(lx::error_domain::drm, static_cast<int>(code), message);
}

[[nodiscard]] lx::runtime::clock_time steady_now_ns() {
    return static_cast<lx::runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

// ── modes ─────────────────────────────────────────────────────────────────────

unsigned lx::drm::mode_refresh_mhz(unsigned clock_khz, unsigned htotal, unsigned vtotal,
                                   unsigned vscan, unsigned flags) {
    // DRM_MODE_FLAG_INTERLACE (1 << 4), DRM_MODE_FLAG_DBLSCAN (1 << 5): uapi values.
    if (clock_khz == 0 || htotal == 0 || vtotal == 0)
        return 0;
    unsigned long long refresh =
        (static_cast<unsigned long long>(clock_khz) * 1'000'000ull / htotal + vtotal / 2) / vtotal;
    if (flags & (1u << 4))
        refresh *= 2;
    if (flags & (1u << 5))
        refresh /= 2;
    if (vscan > 1)
        refresh /= vscan;
    return static_cast<unsigned>(refresh);
}

bool lx::drm::kms_mode::same_timings(const kms_mode& o) const {
    // drmModeModeInfo: clock, h*, hskew, v*, vscan (24 bytes), vrefresh, flags (28..31),
    // type, name — the timings and the flags are what a modeset programs.
    return width == o.width && height == o.height &&
           std::memcmp(kms.bytes, o.kms.bytes, 24) == 0 &&
           std::memcmp(kms.bytes + 28, o.kms.bytes + 28, 4) == 0;
}

const char* lx::drm::connector_type_name(unsigned type) {
    // Indexed by DRM_MODE_CONNECTOR_* (drm_mode.h; the values are uapi and never change).
    static constexpr const char* k_names[] = {
        "Unknown",   "VGA",  "DVI-I",  "DVI-D",  "DVI-A", "Composite", "SVIDEO",
        "LVDS",      "Component", "DIN", "DP", "HDMI-A", "HDMI-B", "TV", "eDP",
        "Virtual",   "DSI",  "DPI",    "Writeback", "SPI", "USB",
    };
    return type < sizeof(k_names) / sizeof(k_names[0]) ? k_names[type] : "Unknown";
}

// ── kms_device ────────────────────────────────────────────────────────────────

lx::drm::kms_device::~kms_device() { release(); }

lx::drm::kms_device::kms_device(kms_device&& other) noexcept { *this = static_cast<kms_device&&>(other); }

lx::drm::kms_device& lx::drm::kms_device::operator=(kms_device&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    // Moved only before any pipeline registers (open returns it by value): the registry and
    // the cookies are empty on both sides.
    card_fd_ = static_cast<lx::unique_fd&&>(other.card_fd_);
    crtc_count_ = other.crtc_count_;
    for (unsigned i = 0; i < k_max_crtcs; ++i)
        crtcs_[i] = other.crtcs_[i];
    plane_count_ = other.plane_count_;
    for (unsigned i = 0; i < k_max_planes; ++i)
        planes_[i] = other.planes_[i];
    scan_ = static_cast<std::unique_ptr<connector_scan>&&>(other.scan_);
    stale_crtcs_.store(other.stale_crtcs_.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    cursor_hotspot_cap_ = other.cursor_hotspot_cap_;
    other.crtc_count_ = 0;
    other.plane_count_ = 0;
    other.stale_crtcs_.store(0, std::memory_order_relaxed);
    other.cursor_hotspot_cap_ = false;
    return *this;
}

void lx::drm::kms_device::release() {
    card_fd_.reset();
    crtc_count_ = 0;
    plane_count_ = 0;
    scan_.reset();
    stale_crtcs_.store(0, std::memory_order_relaxed);
    cursor_hotspot_cap_ = false;
    for (auto& p : pipelines_)
        p.store(nullptr, std::memory_order_relaxed);
}

lx::result<void> lx::drm::kms_device::init_from_fd() {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0)
        return drm_error(lx::drm_err::open_failed, "invalid card fd");
    // Non-blocking so drmHandleEvent / drm_read never stalls the UI thread when
    // there is no pending page-flip event.
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
        return drm_error(lx::drm_err::open_failed, "DRM_CLIENT_CAP_ATOMIC failed");
    (void)drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    // Declares that this client sets HOTSPOT_X/Y on cursor planes. Virtualised drivers
    // (vmwgfx, virtio-gpu, qxl, vboxvideo) only expose their cursor plane to atomic
    // clients that do, because the host draws the pointer and needs the hotspot to place
    // it. Other drivers reject the cap, which is expected and harmless.
    cursor_hotspot_cap_ = drmSetClientCap(fd, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1) == 0;
    if (auto enumerated = enumerate_resources(); !enumerated)
        return enumerated;
    scan_ = std::make_unique<connector_scan>();
    return probe_connectors(0, *scan_);
#else
    return lx::not_implemented("lx::drm::kms_device::init_from_fd");
#endif
}

lx::result<lx::drm::kms_device> lx::drm::kms_device::open(const char* device_path) {
#if defined(LUMEN_HAS_DRM)
    const char* path = device_path ? device_path : "/dev/dri/card0";
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return drm_error(lx::drm_err::open_failed, "failed to open DRM card");
    return open_fd(lx::unique_fd{fd});
#else
    (void)device_path;
    return lx::not_implemented("lx::drm::kms_device::open");
#endif
}

lx::result<lx::drm::kms_device> lx::drm::kms_device::open_fd(lx::unique_fd card_fd) {
#if defined(LUMEN_HAS_DRM)
    kms_device dev{};
    dev.card_fd_ = static_cast<lx::unique_fd&&>(card_fd);
    if (auto init = dev.init_from_fd(); !init)
        return init.get_error();
    return dev;
#else
    (void)card_fd;
    return lx::not_implemented("lx::drm::kms_device::open_fd");
#endif
}

#if defined(LUMEN_HAS_DRM)
namespace {

/// Value of property `name` of an object, false when it has none.
bool object_prop(int fd, unsigned id, unsigned type, const char* name, unsigned long long& out) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, id, type);
    if (!props)
        return false;
    bool found = false;
    for (uint32_t i = 0; i < props->count_props && !found; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (std::strcmp(p->name, name) == 0) {
            out = props->prop_values[i];
            found = true;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found;
}

void fill_mode(lx::drm::kms_mode& m, const drmModeModeInfo& info) {
    static_assert(sizeof(drmModeModeInfo) == sizeof(lx::drm::drm_mode_blob::bytes));
    m.width = info.hdisplay;
    m.height = info.vdisplay;
    m.refresh_mhz =
        lx::drm::mode_refresh_mhz(info.clock, info.htotal, info.vtotal, info.vscan, info.flags);
    if (m.refresh_mhz == 0)
        m.refresh_mhz = info.vrefresh ? info.vrefresh * 1000u : 60000u;
    m.preferred = (info.type & DRM_MODE_TYPE_PREFERRED) != 0;
    m.flags = info.flags;
    std::memcpy(m.kms.bytes, &info, sizeof(info));
}

void probe_one(int fd, uint32_t connector_id, lx::drm::connector_probe& e) {
    e = {};
    e.connector_id = connector_id;
    // drmModeGetConnector, not ...Current: a forced probe (the master's re-reads modes and
    // EDID; anyone else gets the kernel's cache).
    drmModeConnector* conn = drmModeGetConnector(fd, connector_id);
    if (!conn)
        return;
    e.type = conn->connector_type;
    e.type_id = conn->connector_type_id;
    std::snprintf(e.name, sizeof(e.name), "%s-%u", lx::drm::connector_type_name(conn->connector_type),
                  conn->connector_type_id);
    e.status = conn->connection == DRM_MODE_CONNECTED      ? lx::drm::connector_status::connected
               : conn->connection == DRM_MODE_DISCONNECTED ? lx::drm::connector_status::disconnected
                                                           : lx::drm::connector_status::unknown;
    e.phys_w_mm = conn->mmWidth;
    e.phys_h_mm = conn->mmHeight;
    e.subpixel = conn->subpixel == DRM_MODE_SUBPIXEL_UNKNOWN ? 0u
                 : conn->subpixel == DRM_MODE_SUBPIXEL_NONE  ? 1u
                 : conn->subpixel == DRM_MODE_SUBPIXEL_HORIZONTAL_RGB ? 2u
                 : conn->subpixel == DRM_MODE_SUBPIXEL_HORIZONTAL_BGR ? 3u
                 : conn->subpixel == DRM_MODE_SUBPIXEL_VERTICAL_RGB   ? 4u
                 : conn->subpixel == DRM_MODE_SUBPIXEL_VERTICAL_BGR   ? 5u
                                                                      : 0u;
    for (int m = 0; m < conn->count_modes && e.mode_count < lx::drm::k_max_modes; ++m) {
        lx::drm::kms_mode& out = e.modes[e.mode_count];
        fill_mode(out, conn->modes[m]);
        if (out.preferred && e.preferred < 0)
            e.preferred = static_cast<int>(e.mode_count);
        ++e.mode_count;
    }
    for (int i = 0; i < conn->count_encoders; ++i) {
        if (drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[i])) {
            e.possible_crtcs |= enc->possible_crtcs;
            drmModeFreeEncoder(enc);
        }
    }
    drmModeFreeConnector(conn);

    drmModeObjectProperties* props =
        drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (!props)
        return;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        const unsigned long long v = props->prop_values[i];
        if (std::strcmp(p->name, "CRTC_ID") == 0) {
            e.current_crtc_id = static_cast<unsigned>(v);
        } else if (std::strcmp(p->name, "non-desktop") == 0) {
            e.non_desktop = v != 0;
        } else if (std::strcmp(p->name, "vrr_capable") == 0) {
            e.vrr_capable = v != 0;
        } else if (std::strcmp(p->name, "hotplug_mode_update") == 0) {
            e.hotplug_mode_update = v != 0;
        } else if (std::strcmp(p->name, "suggested X") == 0) {
            e.suggested_x = static_cast<long>(v);
        } else if (std::strcmp(p->name, "suggested Y") == 0) {
            e.suggested_y = static_cast<long>(v);
        } else if (std::strcmp(p->name, "link-status") == 0) {
            for (int k = 0; k < p->count_enums; ++k)
                if (p->enums[k].value == v && std::strcmp(p->enums[k].name, "Bad") == 0)
                    e.link_bad = true;
        } else if (std::strcmp(p->name, "EDID") == 0 && v != 0) {
            if (drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, static_cast<uint32_t>(v))) {
                lx::drm::edid_identity id{};
                if (lx::drm::parse_edid_identity(static_cast<const unsigned char*>(blob->data),
                                                 blob->length, id)) {
                    e.edid = true;
                    std::memcpy(e.make, id.make, sizeof(e.make));
                    std::memcpy(e.model, id.model, sizeof(e.model));
                    std::memcpy(e.serial, id.serial, sizeof(e.serial));
                }
                drmModeFreePropertyBlob(blob);
            }
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    // A connector routed through an encoder only (legacy state): its encoder's CRTC.
    (void)fd;
}

} // namespace
#endif

lx::result<void> lx::drm::kms_device::enumerate_resources() {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0)
        return drm_error(lx::drm_err::open_failed, "no card fd");
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return drm_error(lx::drm_err::open_failed, "drmModeGetResources failed");
    crtc_count_ = 0;
    unsigned stale = 0;
    for (int i = 0; i < res->count_crtcs && crtc_count_ < k_max_crtcs; ++i) {
        kms_crtc_info& c = crtcs_[crtc_count_];
        c = {};
        c.id = res->crtcs[i];
        c.index = static_cast<unsigned>(i);
        unsigned long long active = 0;
        c.active = object_prop(fd, c.id, DRM_MODE_OBJECT_CRTC, "ACTIVE", active) && active != 0;
        if (c.active)
            stale |= 1u << c.index;
        ++crtc_count_;
    }
    drmModeFreeResources(res);

    plane_count_ = 0;
    if (drmModePlaneRes* planes = drmModeGetPlaneResources(fd)) {
        for (uint32_t i = 0; i < planes->count_planes && plane_count_ < k_max_planes; ++i) {
            drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
            if (!p)
                continue;
            kms_plane_info& info = planes_[plane_count_];
            info = {};
            info.id = p->plane_id;
            info.possible_crtcs = p->possible_crtcs;
            info.crtc_id = p->crtc_id;
            for (uint32_t f = 0; f < p->count_formats; ++f) {
                info.xrgb8888 = info.xrgb8888 || p->formats[f] == DRM_FORMAT_XRGB8888;
                info.argb8888 = info.argb8888 || p->formats[f] == DRM_FORMAT_ARGB8888;
            }
            drmModeFreePlane(p);
            unsigned long long type = DRM_PLANE_TYPE_OVERLAY;
            (void)object_prop(fd, info.id, DRM_MODE_OBJECT_PLANE, "type", type);
            info.kind = type == DRM_PLANE_TYPE_PRIMARY ? plane_kind::primary
                        : type == DRM_PLANE_TYPE_CURSOR ? plane_kind::cursor
                                                        : plane_kind::overlay;
            ++plane_count_;
        }
        drmModeFreePlaneResources(planes);
    }
    // Active CRTCs a registered pipeline drives are ours, not stale.
    for (const auto& slot : pipelines_)
        if (const kms_pipeline* p = slot.load(std::memory_order_acquire))
            stale &= ~(1u << p->desc().crtc_index);
    stale_crtcs_.store(stale, std::memory_order_release);
    return {};
#else
    return lx::not_implemented("lx::drm::kms_device::enumerate_resources");
#endif
}

int lx::drm::kms_device::crtc_index_of(unsigned crtc_id) const {
    for (unsigned i = 0; i < crtc_count_; ++i)
        if (crtcs_[i].id == crtc_id)
            return static_cast<int>(crtcs_[i].index);
    return -1;
}

lx::result<void> lx::drm::kms_device::probe_connectors(unsigned hint_connector_id,
                                                       connector_scan& out, bool record) {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0)
        return drm_error(lx::drm_err::open_failed, "no card fd");
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return drm_error(lx::drm_err::open_failed, "drmModeGetResources failed");
    if (hint_connector_id != 0) {
        bool found = false;
        for (unsigned i = 0; i < out.count; ++i) {
            if (out.entries[i].connector_id != hint_connector_id)
                continue;
            probe_one(fd, hint_connector_id, out.entries[i]);
            found = true;
        }
        if (!found)
            hint_connector_id = 0; // a new connector (MST): everything
    }
    if (hint_connector_id == 0) {
        out.count = 0;
        out.complete = static_cast<unsigned>(res->count_connectors) <= k_max_connectors;
        for (int i = 0; i < res->count_connectors && out.count < k_max_connectors; ++i)
            probe_one(fd, res->connectors[i], out.entries[out.count++]);
    }
    drmModeFreeResources(res);
    ++out.generation;
    if (!record)
        return {};
    if (!scan_)
        scan_ = std::make_unique<connector_scan>();
    if (scan_.get() != &out)
        *scan_ = out;
    return {};
#else
    (void)hint_connector_id;
    (void)out;
    (void)record;
    return lx::not_implemented("lx::drm::kms_device::probe_connectors");
#endif
}

const lx::drm::connector_scan& lx::drm::kms_device::last_scan() const {
    static const connector_scan k_empty{};
    return scan_ ? *scan_ : k_empty;
}

unsigned lx::drm::kms_device::connector_count() const { return last_scan().count; }

lx::drm::connector_info lx::drm::kms_device::connector(unsigned index) const {
    const connector_scan& s = last_scan();
    if (index >= s.count)
        return {};
    const connector_probe& e = s.entries[index];
    connector_info info{};
    info.id = e.connector_id;
    info.crtc_id = e.current_crtc_id;
    info.status = e.status;
    info.name = e.name;
    info.type = e.type;
    info.type_id = e.type_id;
    info.phys_w_mm = e.phys_w_mm;
    info.phys_h_mm = e.phys_h_mm;
    return info;
}

lx::result<lx::drm::mode> lx::drm::kms_device::preferred_mode(unsigned index) const {
    const connector_scan& s = last_scan();
    if (index >= s.count)
        return drm_error(lx::drm_err::mode_invalid, "connector index out of range");
    const connector_probe& e = s.entries[index];
    const kms_mode* m = e.status == connector_status::connected ? e.preferred_mode() : nullptr;
    if (!m || !m->valid())
        return drm_error(lx::drm_err::mode_invalid, "no mode");
    return mode{m->width, m->height, m->refresh_mhz, m->preferred};
}

lx::result<lx::drm::kms_pipeline_desc> lx::drm::kms_device::pipeline_desc_for(unsigned index) const {
    const connector_scan& s = last_scan();
    if (index >= s.count)
        return drm_error(lx::drm_err::mode_invalid, "connector index out of range");
    const connector_probe& e = s.entries[index];
    int crtc = e.current_crtc_id ? crtc_index_of(e.current_crtc_id) : -1;
    for (unsigned i = 0; crtc < 0 && i < crtc_count_; ++i)
        if (e.possible_crtcs & (1u << crtcs_[i].index))
            crtc = static_cast<int>(crtcs_[i].index);
    if (crtc < 0)
        return drm_error(lx::drm_err::mode_invalid, "no CRTC for connector");
    kms_pipeline_desc d{};
    d.crtc_index = static_cast<unsigned>(crtc);
    for (unsigned i = 0; i < crtc_count_; ++i)
        if (crtcs_[i].index == d.crtc_index)
            d.crtc_id = crtcs_[i].id;
    d.connector_id = e.connector_id;
    for (unsigned i = 0; i < plane_count_ && !d.primary_plane_id; ++i)
        if (planes_[i].kind == plane_kind::primary &&
            (planes_[i].possible_crtcs & (1u << d.crtc_index)))
            d.primary_plane_id = planes_[i].id;
    if (!d.primary_plane_id)
        return drm_error(lx::drm_err::mode_invalid, "no primary plane for CRTC");
    return d;
}

int lx::drm::kms_device::card_fd() const { return card_fd_.get(); }

bool lx::drm::kms_device::supports_async_flip() const {
#if defined(LUMEN_HAS_DRM) && defined(DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP)
    uint64_t cap = 0;
    return card_fd_.get() >= 0 &&
           drmGetCap(card_fd_.get(), DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap != 0;
#else
    return false;
#endif
}

bool lx::drm::kms_device::is_master() const {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0)
        return false;
    // drmIsMaster reports whether this FD may modeset; it does not steal master from a
    // compositor that already holds it.
    return drmIsMaster(fd) != 0;
#else
    return false;
#endif
}

lx::result<lx::unique_fd> lx::drm::kms_device::open_render_node() const {
#if defined(LUMEN_HAS_DRM)
    // Prefer renderD128; fall back to renderD129 for dual-GPU hosts.
    static const char* paths[] = {"/dev/dri/renderD128", "/dev/dri/renderD129"};
    for (const char* p : paths) {
        const int fd = ::open(p, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
            return lx::unique_fd{fd};
    }
    return drm_error(lx::drm_err::open_failed, "no render node");
#else
    return lx::not_implemented("lx::drm::kms_device::open_render_node");
#endif
}

bool lx::drm::kms_device::register_pipeline(kms_pipeline* pipe, unsigned& slot) {
    for (unsigned i = 0; i < k_max_pipelines; ++i) {
        kms_pipeline* expected = nullptr;
        if (pipelines_[i].compare_exchange_strong(expected, pipe, std::memory_order_acq_rel)) {
            slot = i;
            return true;
        }
    }
    return false;
}

void lx::drm::kms_device::unregister_pipeline(unsigned slot) {
    if (slot < k_max_pipelines)
        pipelines_[slot].store(nullptr, std::memory_order_release);
}

void lx::drm::kms_device::note_crtc_inactive(unsigned index) {
    stale_crtcs_.fetch_and(~(1u << index), std::memory_order_acq_rel);
    for (unsigned i = 0; i < crtc_count_; ++i)
        if (crtcs_[i].index == index)
            crtcs_[i].active = false;
}

unsigned lx::drm::kms_device::stale_crtc_mask() const {
    return stale_crtcs_.load(std::memory_order_acquire);
}

lx::drm::kms_pipeline* lx::drm::kms_device::pipeline_for_crtc(unsigned crtc_id) const {
    for (const auto& slot : pipelines_)
        if (kms_pipeline* p = slot.load(std::memory_order_acquire); p && p->crtc_id() == crtc_id)
            return p;
    return nullptr;
}

lx::drm::flip_cookie& lx::drm::kms_device::next_combined_cookie() {
    flip_cookie& c = combined_cookies_[combined_seq_++ & 1u];
    c.device = this;
    return c;
}

void lx::drm::kms_device::dispatch_events() {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0)
        return;
    // A blocking modeset has no event of its own: its completion is reported here, on this
    // thread, as the flip it stands for.
    for (const auto& slot : pipelines_)
        if (kms_pipeline* p = slot.load(std::memory_order_acquire))
            p->sweep_synthetic();
    drmEventContext ctx{};
    ctx.version = 3;
    ctx.page_flip_handler2 = [](int /*fd*/, unsigned int sequence, unsigned int tv_sec,
                                unsigned int tv_usec, unsigned int crtc_id, void* user_data) {
        auto* cookie = static_cast<flip_cookie*>(user_data);
        if (!cookie || !cookie->device)
            return;
        // Routed by the CRTC the event names: a combined modeset raises one per CRTC with
        // the same cookie, and a CRTC no pipeline owns (a stale one turned off) is ignored.
        kms_pipeline* p = cookie->device->pipeline_for_crtc(crtc_id);
        if (!p)
            return;
        p->on_kernel_flip(cookie, sequence,
                          static_cast<lx::runtime::clock_time>(tv_sec) * 1'000'000'000ull +
                              static_cast<lx::runtime::clock_time>(tv_usec) * 1'000ull);
    };
    // The card fd is nonblocking (`init_from_fd`): with nothing queued this returns at once.
    (void)drmHandleEvent(fd, &ctx);
#endif
}

// ── kms_framebuffer ──────────────────────────────────────────────────────────

lx::drm::kms_framebuffer::~kms_framebuffer() { release(); }

lx::drm::kms_framebuffer::kms_framebuffer(kms_framebuffer&& other) noexcept {
    *this = static_cast<kms_framebuffer&&>(other);
}

lx::drm::kms_framebuffer& lx::drm::kms_framebuffer::operator=(kms_framebuffer&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    card_fd_ = other.card_fd_;
    fb_id_ = other.fb_id_;
    gem_handle_ = other.gem_handle_;
    width_ = other.width_;
    height_ = other.height_;
    other.card_fd_ = -1;
    other.fb_id_ = 0;
    other.gem_handle_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    return *this;
}

void lx::drm::kms_framebuffer::release() {
#if defined(LUMEN_HAS_DRM)
    if (card_fd_ >= 0) {
        if (fb_id_ != 0)
            (void)drmModeRmFB(card_fd_, fb_id_);
        if (gem_handle_ != 0)
            (void)drmCloseBufferHandle(card_fd_, gem_handle_);
    }
#endif
    card_fd_ = -1;
    fb_id_ = 0;
    gem_handle_ = 0;
    width_ = 0;
    height_ = 0;
}

lx::result<lx::drm::kms_framebuffer> lx::drm::kms_framebuffer::import_dmabuf(
    const kms_device& device, int dmabuf_fd, unsigned width, unsigned height, unsigned stride,
    unsigned offset, lx::fourcc format, unsigned long long modifier) {
#if defined(LUMEN_HAS_DRM)
    const int fd = device.card_fd();
    if (fd < 0 || dmabuf_fd < 0 || width == 0 || height == 0 || stride == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "invalid dmabuf framebuffer parameters");
    }

    uint32_t handle = 0;
    if (drmPrimeFDToHandle(fd, dmabuf_fd, &handle) != 0 || handle == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "drmPrimeFDToHandle failed");
    }

    kms_framebuffer framebuffer;
    framebuffer.card_fd_ = fd;
    framebuffer.gem_handle_ = handle;
    framebuffer.width_ = width;
    framebuffer.height_ = height;

    uint32_t handles[4] = {handle, 0, 0, 0};
    uint32_t pitches[4] = {stride, 0, 0, 0};
    uint32_t offsets[4] = {offset, 0, 0, 0};
    uint64_t modifiers[4] = {modifier, 0, 0, 0};

    // DRM_MODE_FB_MODIFIERS must only be set when a modifier is actually supplied;
    // drivers reject the flag with an implicit layout.
    const uint32_t flags = modifier != 0 ? DRM_MODE_FB_MODIFIERS : 0u;

    uint32_t fb_id = 0;
    if (drmModeAddFB2WithModifiers(fd, width, height, static_cast<uint32_t>(format), handles,
                                   pitches, offsets, modifiers, &fb_id, flags) != 0 ||
        fb_id == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "drmModeAddFB2WithModifiers failed");
    }
    framebuffer.fb_id_ = fb_id;
    return framebuffer;
#else
    (void)device;
    (void)dmabuf_fd;
    (void)width;
    (void)height;
    (void)stride;
    (void)offset;
    (void)format;
    (void)modifier;
    return lx::not_implemented("lx::drm::kms_framebuffer::import_dmabuf");
#endif
}

// ── kms_dumb_framebuffer ─────────────────────────────────────────────────────

lx::drm::kms_dumb_framebuffer::~kms_dumb_framebuffer() { release(); }

lx::drm::kms_dumb_framebuffer::kms_dumb_framebuffer(kms_dumb_framebuffer&& other) noexcept {
    *this = static_cast<kms_dumb_framebuffer&&>(other);
}

lx::drm::kms_dumb_framebuffer& lx::drm::kms_dumb_framebuffer::operator=(
    kms_dumb_framebuffer&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    card_fd_ = other.card_fd_;
    fb_id_ = other.fb_id_;
    gem_handle_ = other.gem_handle_;
    width_ = other.width_;
    height_ = other.height_;
    stride_ = other.stride_;
    size_ = other.size_;
    format_ = other.format_;
    pixels_ = other.pixels_;
    other.card_fd_ = -1;
    other.fb_id_ = 0;
    other.gem_handle_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.stride_ = 0;
    other.size_ = 0;
    other.format_ = 0;
    other.pixels_ = nullptr;
    return *this;
}

void lx::drm::kms_dumb_framebuffer::release() {
#if defined(LUMEN_HAS_DRM)
    if (pixels_ && size_ > 0)
        munmap(pixels_, size_);
    if (card_fd_ >= 0 && fb_id_ != 0)
        drmModeRmFB(card_fd_, fb_id_);
    if (card_fd_ >= 0 && gem_handle_ != 0)
        drmModeDestroyDumbBuffer(card_fd_, gem_handle_);
#endif
    card_fd_ = -1;
    fb_id_ = 0;
    gem_handle_ = 0;
    width_ = 0;
    height_ = 0;
    stride_ = 0;
    size_ = 0;
    format_ = 0;
    pixels_ = nullptr;
}

lx::result<lx::drm::kms_dumb_framebuffer> lx::drm::kms_dumb_framebuffer::create(
    const kms_device& device, unsigned width, unsigned height, lx::fourcc format) {
#if defined(LUMEN_HAS_DRM)
    const int fd = device.card_fd();
    if (fd < 0 || width == 0 || height == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "invalid dumb framebuffer parameters");
    }

    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    if (drmModeCreateDumbBuffer(fd, width, height, 32, 0, &handle, &pitch, &size) != 0 ||
        handle == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "drmModeCreateDumbBuffer failed");
    }

    kms_dumb_framebuffer framebuffer;
    framebuffer.card_fd_ = fd;
    framebuffer.gem_handle_ = handle;
    framebuffer.width_ = width;
    framebuffer.height_ = height;
    framebuffer.stride_ = pitch;
    framebuffer.size_ = static_cast<unsigned>(size);
    framebuffer.format_ = format;

    uint64_t map_offset = 0;
    if (drmModeMapDumbBuffer(fd, handle, &map_offset) != 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "drmModeMapDumbBuffer failed");
    }

    void* mapped = mmap(nullptr, static_cast<size_t>(size), PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        static_cast<off_t>(map_offset));
    if (mapped == MAP_FAILED) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "mmap of dumb buffer failed");
    }
    framebuffer.pixels_ = static_cast<unsigned char*>(mapped);

    // No modifier: a dumb buffer is linear by construction, and passing
    // DRM_MODE_FB_MODIFIERS with an implicit layout is rejected by drivers.
    uint32_t handles[4] = {handle, 0, 0, 0};
    uint32_t pitches[4] = {pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

    uint32_t fb_id = 0;
    if (drmModeAddFB2(fd, width, height, static_cast<uint32_t>(format), handles, pitches, offsets,
                      &fb_id, 0) != 0 ||
        fb_id == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "drmModeAddFB2 failed for dumb buffer");
    }
    framebuffer.fb_id_ = fb_id;
    return framebuffer;
#else
    (void)device;
    (void)width;
    (void)height;
    (void)format;
    return lx::not_implemented("lx::drm::kms_dumb_framebuffer::create");
#endif
}


bool lx::drm::neutral_property_value(const kms_property_shape& p, neutral_rule rule,
                                     const char* enum_name, unsigned long long& out) {
    if (p.immutable)
        return false;
    using kind = kms_property_shape::kind;
    const bool ranged = p.type == kind::range || p.type == kind::signed_range;
    switch (rule) {
    case neutral_rule::zero:
        if (p.type == kind::blob || p.type == kind::object) {
            out = 0;
            return true;
        }
        if (p.type == kind::signed_range ? static_cast<long long>(p.min) <= 0 &&
                                               static_cast<long long>(p.max) >= 0
                                         : p.type == kind::range && p.min == 0) {
            out = 0;
            return true;
        }
        return false;
    case neutral_rule::range_min:
    case neutral_rule::range_max:
        if (!ranged)
            return false;
        out = rule == neutral_rule::range_min ? p.min : p.max;
        return true;
    case neutral_rule::enum_entry:
        if ((p.type != kind::enumeration && p.type != kind::bitmask) || !enum_name ||
            !p.enum_names || !p.enum_values)
            return false;
        for (unsigned i = 0; i < p.enum_count; ++i) {
            if (!p.enum_names[i] || std::strcmp(p.enum_names[i], enum_name) != 0)
                continue;
            if (p.type == kind::bitmask) {
                if (p.enum_values[i] >= 64)
                    return false;
                out = 1ull << p.enum_values[i];
            } else {
                out = p.enum_values[i];
            }
            return true;
        }
        return false;
    }
    return false;
}

#if defined(LUMEN_HAS_DRM)
namespace {
/// What a modeset resets (`commit_modeset`): state a previous master — the
/// console, KWin with HDR or night light, Mutter on a rotated monitor — can leave behind
/// and Lumen does not manage, back to the value the driver starts with. zpos: the primary
/// plane at the bottom of its range, the cursor plane at the top of its.
struct neutral_spec {
    unsigned object_type;
    const char* name;
    lx::drm::neutral_rule rule;
    const char* entry;
};
constexpr neutral_spec k_neutral_specs[] = {
    {DRM_MODE_OBJECT_CRTC, "GAMMA_LUT", lx::drm::neutral_rule::zero, nullptr},
    {DRM_MODE_OBJECT_CRTC, "DEGAMMA_LUT", lx::drm::neutral_rule::zero, nullptr},
    {DRM_MODE_OBJECT_CRTC, "CTM", lx::drm::neutral_rule::zero, nullptr},
    {DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", lx::drm::neutral_rule::zero, nullptr},
    {DRM_MODE_OBJECT_CONNECTOR, "link-status", lx::drm::neutral_rule::enum_entry, "Good"},
    {DRM_MODE_OBJECT_CONNECTOR, "Colorspace", lx::drm::neutral_rule::enum_entry, "Default"},
    {DRM_MODE_OBJECT_CONNECTOR, "HDR_OUTPUT_METADATA", lx::drm::neutral_rule::zero, nullptr},
    // The kernel starts every connector at the top of the range (the link may still use
    // less); another master may have capped it.
    {DRM_MODE_OBJECT_CONNECTOR, "max bpc", lx::drm::neutral_rule::range_max, nullptr},
    {DRM_MODE_OBJECT_CONNECTOR, "Broadcast RGB", lx::drm::neutral_rule::enum_entry,
     "Automatic"},
    {DRM_MODE_OBJECT_PLANE, "rotation", lx::drm::neutral_rule::enum_entry, "rotate-0"},
    {DRM_MODE_OBJECT_PLANE, "alpha", lx::drm::neutral_rule::range_max, nullptr},
    {DRM_MODE_OBJECT_PLANE, "pixel blend mode", lx::drm::neutral_rule::enum_entry,
     "Pre-multiplied"},
    {DRM_MODE_OBJECT_PLANE, "zpos", lx::drm::neutral_rule::range_min, nullptr},
};

/// Static literals only — `error::message` must not allocate on the frame path.
const char* atomic_commit_message(int err) {
    switch (err) {
    case EACCES: return "drmModeAtomicCommit rejected: DRM master lost (EACCES)";
    case EPERM: return "drmModeAtomicCommit rejected: not permitted (EPERM)";
    case EBUSY: return "drmModeAtomicCommit rejected: flip already pending (EBUSY)";
    case EINVAL: return "drmModeAtomicCommit rejected: invalid state (EINVAL)";
    case ENOSPC: return "drmModeAtomicCommit rejected: no scanout resources (ENOSPC)";
    case ENOMEM: return "drmModeAtomicCommit rejected: out of memory (ENOMEM)";
    case ERANGE: return "drmModeAtomicCommit rejected: mode/plane range (ERANGE)";
    default: return "drmModeAtomicCommit failed";
    }
}

unsigned find_object_prop(int fd, unsigned object_id, unsigned object_type, const char* name) {
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!props)
        return 0;
    unsigned found = 0;
    for (uint32_t i = 0; i < props->count_props && found == 0; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (std::strcmp(p->name, name) == 0)
            found = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return found;
}
} // namespace
#endif

void lx::drm::kms_pipeline::resolve_neutral(int fd, unsigned object, unsigned object_type,
                                                bool cursor_plane) {
#if defined(LUMEN_HAS_DRM)
    drmModeObjectProperties* props = drmModeObjectGetProperties(fd, object, object_type);
    if (!props)
        return;
    for (uint32_t i = 0; i < props->count_props && neutral_count_ < k_max_neutral; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        for (const neutral_spec& spec : k_neutral_specs) {
            if (spec.object_type != object_type || std::strcmp(spec.name, p->name) != 0)
                continue;
            constexpr unsigned k_entries = 32;
            const char* names[k_entries];
            unsigned long long values[k_entries];
            kms_property_shape shape{};
            shape.immutable = (p->flags & DRM_MODE_PROP_IMMUTABLE) != 0;
            using kind = kms_property_shape::kind;
            if (drm_property_type_is(p, DRM_MODE_PROP_SIGNED_RANGE))
                shape.type = kind::signed_range;
            else if (drm_property_type_is(p, DRM_MODE_PROP_RANGE))
                shape.type = kind::range;
            else if (drm_property_type_is(p, DRM_MODE_PROP_ENUM))
                shape.type = kind::enumeration;
            else if (drm_property_type_is(p, DRM_MODE_PROP_BITMASK))
                shape.type = kind::bitmask;
            else if (drm_property_type_is(p, DRM_MODE_PROP_BLOB))
                shape.type = kind::blob;
            else
                shape.type = kind::object;
            if ((shape.type == kind::range || shape.type == kind::signed_range) &&
                p->count_values >= 2) {
                shape.min = p->values[0];
                shape.max = p->values[1];
            }
            if (shape.type == kind::enumeration || shape.type == kind::bitmask) {
                const unsigned n = p->count_enums < 0 ? 0u
                                   : static_cast<unsigned>(p->count_enums) < k_entries
                                       ? static_cast<unsigned>(p->count_enums)
                                       : k_entries;
                for (unsigned e = 0; e < n; ++e) {
                    names[e] = p->enums[e].name;
                    values[e] = p->enums[e].value;
                }
                shape.enum_names = names;
                shape.enum_values = values;
                shape.enum_count = n;
            }
            const neutral_rule rule =
                cursor_plane && std::strcmp(spec.name, "zpos") == 0 ? neutral_rule::range_max
                                                                    : spec.rule;
            unsigned long long value = 0;
            if (neutral_property_value(shape, rule, spec.entry, value))
                neutral_[neutral_count_++] = {object, p->prop_id, value};
            break;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
#else
    (void)fd;
    (void)object;
    (void)object_type;
    (void)cursor_plane;
#endif
}

// ── kms_pipeline ──────────────────────────────────────────────────────────────

lx::drm::kms_pipeline::kms_pipeline(kms_device& device, const kms_pipeline_desc& desc,
                                    bool route_events)
    : device_{&device}, desc_{desc} {
    for (auto& c : cookies_)
        c.device = &device;
    unsigned slot = ~0u;
    if (route_events && device.register_pipeline(this, slot))
        reg_slot_ = slot;
}

void lx::drm::kms_pipeline::leave_registry() {
    if (device_ && reg_slot_ != ~0u)
        device_->unregister_pipeline(reg_slot_);
    reg_slot_ = ~0u;
}

lx::drm::kms_pipeline::~kms_pipeline() {
#if defined(LUMEN_HAS_DRM)
    const int fd = device_ ? device_->card_fd() : -1;
    if (fd >= 0) {
        // Without master (exiting while another VT is in front) the restore can only fail;
        // logind gives the VT back to the console when control is released.
        if (saved_crtc_ && drmIsMaster(fd)) {
            auto* saved = static_cast<drmModeCrtc*>(saved_crtc_);
            uint32_t connector = saved_connector_;
            // buffer_id 0 means the console had nothing attached; leave the CRTC off rather
            // than pointing it at a framebuffer that is about to be destroyed.
            if (saved->buffer_id && saved->mode_valid && connector) {
                (void)drmModeSetCrtc(fd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                                     &connector, 1, &saved->mode);
            }
        }
        if (mode_blob_id_)
            drmModeDestroyPropertyBlob(fd, mode_blob_id_);
        if (damage_blob_id_)
            drmModeDestroyPropertyBlob(fd, damage_blob_id_);
    }
    if (saved_crtc_)
        drmModeFreeCrtc(static_cast<drmModeCrtc*>(saved_crtc_));
    saved_crtc_ = nullptr;
#endif
    if (device_ && reg_slot_ != ~0u)
        device_->unregister_pipeline(reg_slot_);
}

void lx::drm::kms_pipeline::set_mode(const kms_mode& mode) {
    mode_ = mode;
    modeset_attempted_ = false;
}

void lx::drm::kms_pipeline::set_framebuffer(unsigned fb_id) { framebuffer_id_ = fb_id; }

void lx::drm::kms_pipeline::forget_saved_state() {
#if defined(LUMEN_HAS_DRM)
    if (saved_crtc_)
        drmModeFreeCrtc(static_cast<drmModeCrtc*>(saved_crtc_));
#endif
    saved_crtc_ = nullptr;
    saved_connector_ = 0;
}

void lx::drm::kms_pipeline::set_failure(int commit_errno) {
#if defined(LUMEN_HAS_DRM)
    last_rejected_state_ = commit_errno == EINVAL || commit_errno == ERANGE || commit_errno == ENOSPC;
    last_lost_master_ = commit_errno == EACCES || commit_errno == EPERM;
#else
    (void)commit_errno;
#endif
}

lx::drm::flip_cookie& lx::drm::kms_pipeline::next_cookie(bool with_event) {
    const unsigned gen = flip_generation_.load(std::memory_order_acquire);
    flip_cookie& c = cookies_[gen & 1u];
    c.device = device_;
    if (with_event)
        c.arm(reg_slot_, gen);
    return c;
}

void lx::drm::kms_pipeline::emit_flip(lx::runtime::clock_time timestamp_ns, unsigned sequence,
                                      bool hw) {
    if (!flip_handler_)
        return;
    page_flip_event ev{};
    // The kernel's own vblank counter (MSC), not a count of our events: a client pacing by
    // it (wp_presentation seq) sees skipped vblanks as gaps.
    ev.sequence = sequence;
    ev.timestamp_ns = timestamp_ns;
    ev.presented = true;
    ev.hw = hw;
    ev.vblank_timestamp = hw && vblank_timestamps_.load(std::memory_order_acquire);
    ev.crtc_id = desc_.crtc_id;
    ev.content = inflight_content_.load(std::memory_order_acquire);
    ev.tag = inflight_tag_.load(std::memory_order_relaxed);
    flip_handler_(ev, flip_user_);
}

void lx::drm::kms_pipeline::on_kernel_flip(flip_cookie* cookie, unsigned sequence,
                                           lx::runtime::clock_time timestamp_ns) {
    if (reg_slot_ >= k_max_pipelines)
        return;
    // Not owed to this pipeline (a CRTC it took over after the commit), or a commit from
    // before the last resume: its CRTC state is gone and its flag was already cleared.
    // Reporting it would clear the resume modeset's flag instead.
    if (!cookie->take(reg_slot_, flip_generation_.load(std::memory_order_acquire)))
        return;
    // Clear before the handler check: the CRTC is free again even with no handler bound.
    flip_pending_.store(false, std::memory_order_release);
    emit_flip(timestamp_ns, sequence, true);
}

void lx::drm::kms_pipeline::sweep_synthetic() {
    if (!synthetic_flip_.exchange(false, std::memory_order_acq_rel))
        return;
    flip_pending_.store(false, std::memory_order_release);
    // Sampled in software, and no vblank counter: hw = false, sequence 0.
    emit_flip(steady_now_ns(), 0, false);
}

void lx::drm::kms_pipeline::prepare_resume() {
    // Events of commits made before the pause belong to the old generation: drained later,
    // they must neither clear the resume modeset's flip_pending nor be reported.
    flip_generation_.fetch_add(1, std::memory_order_acq_rel);
    flip_pending_.store(false, std::memory_order_release);
    synthetic_flip_.store(false, std::memory_order_release);
    modeset_attempted_ = false;
    last_rejected_state_ = false;
    last_lost_master_ = false;
}

lx::result<void> lx::drm::kms_pipeline::ensure_props() {
#if defined(LUMEN_HAS_DRM)
    if (props_ready_ || !device_)
        return {};
    const int fd = device_->card_fd();
    const unsigned plane = desc_.primary_plane_id;
    if (fd < 0 || plane == 0)
        return drm_error(lx::drm_err::mode_invalid, "no primary plane");

    drmModeObjectProperties* props =
        drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!props)
        return drm_error(lx::drm_err::mode_invalid, "drmModeObjectGetProperties failed");
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
        if (!p)
            continue;
        if (std::strcmp(p->name, "CRTC_ID") == 0)
            prop_crtc_id_ = p->prop_id;
        else if (std::strcmp(p->name, "FB_ID") == 0)
            prop_fb_id_ = p->prop_id;
        else if (std::strcmp(p->name, "SRC_X") == 0)
            prop_src_x_ = p->prop_id;
        else if (std::strcmp(p->name, "SRC_Y") == 0)
            prop_src_y_ = p->prop_id;
        else if (std::strcmp(p->name, "SRC_W") == 0)
            prop_src_w_ = p->prop_id;
        else if (std::strcmp(p->name, "SRC_H") == 0)
            prop_src_h_ = p->prop_id;
        else if (std::strcmp(p->name, "CRTC_X") == 0)
            prop_crtc_x_ = p->prop_id;
        else if (std::strcmp(p->name, "CRTC_Y") == 0)
            prop_crtc_y_ = p->prop_id;
        else if (std::strcmp(p->name, "CRTC_W") == 0)
            prop_crtc_w_ = p->prop_id;
        else if (std::strcmp(p->name, "CRTC_H") == 0)
            prop_crtc_h_ = p->prop_id;
        else if (std::strcmp(p->name, "FB_DAMAGE_CLIPS") == 0) {
            prop_fb_damage_clips_ = p->prop_id;
            atomic_damage_ = true;
        } else if (std::strcmp(p->name, "IN_FENCE_FD") == 0)
            prop_in_fence_fd_ = p->prop_id;
        else if (std::strcmp(p->name, "OUT_FENCE_PTR") == 0)
            prop_out_fence_ptr_ = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    props_ready_ = prop_crtc_id_ && prop_fb_id_;
    if (!props_ready_)
        return drm_error(lx::drm_err::mode_invalid, "missing plane props");
    return {};
#else
    return lx::not_implemented("lx::drm::kms_pipeline::ensure_props");
#endif
}

bool lx::drm::kms_pipeline::ensure_modeset_props(int fd) {
#if defined(LUMEN_HAS_DRM)
    if (!modeset_props_ready_) {
        prop_crtc_mode_ = find_object_prop(fd, desc_.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        prop_crtc_active_ = find_object_prop(fd, desc_.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        prop_conn_crtc_ =
            find_object_prop(fd, desc_.connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        prop_crtc_vrr_ = find_object_prop(fd, desc_.crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED");
        // Optional: back to neutral, so another master's night-light LUT, colour matrix,
        // VRR, HDR signalling or plane rotation does not leak into this session (Lumen
        // manages none of them).
        neutral_count_ = 0;
        resolve_neutral(fd, desc_.crtc_id, DRM_MODE_OBJECT_CRTC, false);
        resolve_neutral(fd, desc_.connector_id, DRM_MODE_OBJECT_CONNECTOR, false);
        resolve_neutral(fd, desc_.primary_plane_id, DRM_MODE_OBJECT_PLANE, false);
        modeset_props_ready_ = true;
    }
    return prop_crtc_mode_ && prop_crtc_active_ && prop_conn_crtc_;
#else
    (void)fd;
    return false;
#endif
}

lx::result<void> lx::drm::kms_pipeline::commit(const atomic_commit_request& request) {
#if defined(LUMEN_HAS_DRM)
    if (!device_)
        return drm_error(lx::drm_err::page_flip_failed, "null kms device");
    const int fd = device_->card_fd();
    const unsigned fb = request.framebuffer_id ? request.framebuffer_id : framebuffer_id_;
    const unsigned plane = desc_.primary_plane_id;
    const unsigned crtc = desc_.crtc_id;
    const bool primary = request.update_primary;

    if (fd < 0 || !plane || !crtc || (primary && !fb)) {
        last_rejected_state_ = false;
        last_lost_master_ = false;
        return drm_error(lx::drm_err::page_flip_failed, "atomic commit missing fb/plane/crtc");
    }
    if (!primary && (!request.cursor || !modeset_attempted_ || !props_ready_)) {
        // Nothing to commit, or the CRTC is not ours yet — the first commit is the
        // modeset, and it carries a frame. Says nothing about the plane's state.
        last_rejected_state_ = false;
        last_lost_master_ = false;
        return drm_error(lx::drm_err::mode_invalid, "cursor-only commit before the first modeset");
    }

    if (auto props = ensure_props(); !props)
        return props.get_error();

    const unsigned width = mode_.width;
    const unsigned height = mode_.height;
    if (!width || !height)
        return drm_error(lx::drm_err::mode_invalid, "no mode for the pipeline");

    // A plane-only commit inherits whatever mode the console left on the CRTC, so the very
    // first commit sets the mode our framebuffer was sized for — and so does the first one
    // after a resume (`prepare_resume`) or a mode change. A failure the device caused
    // (EACCES: master went away again) leaves the modeset to the next commit; one the
    // kernel refused is not retried every frame.
    if (!modeset_attempted_ && !request.test_only) {
        modeset_entry entry{};
        entry.pipe = this;
        entry.enable = true;
        entry.framebuffer_id = fb;
        entry.in_fence_fd = request.in_fence_fd;
        entry.cursor = request.cursor && request.cursor->valid() ? request.cursor : nullptr;
        entry.user_tag = request.user_tag;
        entry.request_page_flip = request.request_page_flip;
        auto modeset = commit_modeset(*device_, &entry, 1, false);
        if (modeset || last_rejected_state_)
            modeset_attempted_ = true;
        return modeset;
    }

    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req)
        return drm_error(lx::drm_err::page_flip_failed, "drmModeAtomicAlloc failed");

    const bool with_cursor = request.cursor && request.cursor->dirty();
    if (with_cursor)
        request.cursor->append_props(req);

    // OUT_FENCE_PTR takes the address of a signed 32-bit slot that the kernel fills with a
    // ready-to-use sync_file FD — not a syncobj handle needing a separate export.
    int32_t out_fence_fd = -1;
    const bool want_out_fence =
        primary && !request.test_only && prop_out_fence_ptr_ && request.out_fence_fd_ptr;

    if (primary) {
        drmModeAtomicAddProperty(req, plane, prop_crtc_id_, crtc);
        drmModeAtomicAddProperty(req, plane, prop_fb_id_, fb);
        drmModeAtomicAddProperty(req, plane, prop_src_x_, 0);
        drmModeAtomicAddProperty(req, plane, prop_src_y_, 0);
        drmModeAtomicAddProperty(req, plane, prop_src_w_, static_cast<uint64_t>(width) << 16);
        drmModeAtomicAddProperty(req, plane, prop_src_h_, static_cast<uint64_t>(height) << 16);
        drmModeAtomicAddProperty(req, plane, prop_crtc_x_, 0);
        drmModeAtomicAddProperty(req, plane, prop_crtc_y_, 0);
        drmModeAtomicAddProperty(req, plane, prop_crtc_w_, width);
        drmModeAtomicAddProperty(req, plane, prop_crtc_h_, height);

        if (atomic_damage_ && prop_fb_damage_clips_ && !request.damage.full_frame &&
            request.damage.count > 0 && damage_blob_id_) {
            drmModeAtomicAddProperty(req, plane, prop_fb_damage_clips_, damage_blob_id_);
        }

        if (prop_in_fence_fd_ && request.in_fence_fd >= 0)
            drmModeAtomicAddProperty(req, plane, prop_in_fence_fd_,
                                     static_cast<uint64_t>(request.in_fence_fd));

        if (want_out_fence)
            drmModeAtomicAddProperty(req, plane, prop_out_fence_ptr_,
                                     reinterpret_cast<uint64_t>(&out_fence_fd));
    }

    // TEST_ONLY takes neither NONBLOCK semantics nor an event (an event is EINVAL with it).
    uint32_t flags = DRM_MODE_ATOMIC_TEST_ONLY;
    if (!request.test_only) {
        flags = DRM_MODE_ATOMIC_NONBLOCK;
        if (request.request_page_flip)
            flags |= DRM_MODE_PAGE_FLIP_EVENT;
        if (request.async)
            flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }

    // Raised before the ioctl, not after: the flip event can be drained on the UI thread
    // before drmModeAtomicCommit even returns here, and a late store would then re-raise
    // a flag no event is left to clear, wedging every later commit as "flip in flight".
    // Only a flag this commit raised is lowered again on failure — a flip already pending
    // still owns its event.
    const bool raises_flip = request.request_page_flip && !request.test_only &&
                             !flip_pending_.exchange(true, std::memory_order_acq_rel);
    if (raises_flip)
        note_inflight(request);
    const bool with_event = (flags & DRM_MODE_PAGE_FLIP_EVENT) != 0;
    flip_cookie& cookie = next_cookie(with_event);
    const int r = drmModeAtomicCommit(fd, req, flags, &cookie);
    // Capture errno before drmModeAtomicFree, which may clobber it.
    const int commit_errno = r != 0 ? errno : 0;
    drmModeAtomicFree(req);
    if (r != 0) {
        if (with_event && reg_slot_ < k_max_pipelines)
            cookie.disarm(1u << reg_slot_);
        if (raises_flip)
            flip_pending_.store(false, std::memory_order_release);
        set_failure(commit_errno);
        return drm_error(lx::drm_err::page_flip_failed, atomic_commit_message(commit_errno));
    }
    last_rejected_state_ = false;
    last_lost_master_ = false;
    if (request.test_only)
        return {};

    if (with_cursor)
        request.cursor->on_committed();

    if (want_out_fence)
        *request.out_fence_fd_ptr = out_fence_fd;

    return {};
#else
    (void)request;
    return lx::not_implemented("lx::drm::kms_pipeline::commit");
#endif
}

lx::result<void> lx::drm::kms_pipeline::commit_power_off(kms_cursor_plane* cursor) {
#if defined(LUMEN_HAS_DRM)
    const int fd = device_ ? device_->card_fd() : -1;
    const unsigned crtc = desc_.crtc_id;
    const unsigned plane = desc_.primary_plane_id;
    if (fd < 0 || !crtc || !modeset_props_ready_ || !prop_crtc_active_) {
        last_rejected_state_ = false;
        last_lost_master_ = false;
        return drm_error(lx::drm_err::mode_invalid, "power off before the first modeset");
    }
    if (flip_pending_.load(std::memory_order_acquire)) {
        last_rejected_state_ = false;
        last_lost_master_ = false;
        return drm_error(lx::drm_err::page_flip_failed, "power off while a flip is pending");
    }
    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) {
        last_rejected_state_ = false;
        last_lost_master_ = false;
        return drm_error(lx::drm_err::page_flip_failed, "drmModeAtomicAlloc failed");
    }
    // ACTIVE=0 alone is the atomic DPMS off: MODE_ID, the connector's CRTC and the planes'
    // state are kept, so the resources stay reserved and power-on cannot fail on them.
    drmModeAtomicAddProperty(req, crtc, prop_crtc_active_, 0);
    if (cursor && cursor->valid())
        cursor->append_off_props(req);
    const int essential = drmModeAtomicGetCursor(req);
    // Blocking, no event: the caller needs none, and drivers differ on sending one for a
    // CRTC that goes off. Nothing else is in flight (checked above).
    int r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    int commit_errno = r != 0 ? errno : 0;
    if (r != 0 && commit_errno == EINVAL && plane && props_ready_) {
        // A driver that wants an inactive CRTC's primary plane detached as well.
        drmModeAtomicSetCursor(req, essential);
        drmModeAtomicAddProperty(req, plane, prop_crtc_id_, 0);
        drmModeAtomicAddProperty(req, plane, prop_fb_id_, 0);
        r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        commit_errno = r != 0 ? errno : 0;
    }
    drmModeAtomicFree(req);
    if (r != 0) {
        set_failure(commit_errno);
        return drm_error(lx::drm_err::page_flip_failed, atomic_commit_message(commit_errno));
    }
    last_rejected_state_ = false;
    last_lost_master_ = false;
    return {};
#else
    (void)cursor;
    return lx::not_implemented("lx::drm::kms_pipeline::commit_power_off");
#endif
}

void lx::drm::kms_pipeline::set_page_flip_handler(page_flip_handler handler, void* user) {
    flip_handler_ = handler;
    flip_user_ = user;
}

bool lx::drm::kms_pipeline::supports_atomic_damage() const { return atomic_damage_; }

bool lx::drm::kms_pipeline::flip_pending() const {
    return flip_pending_.load(std::memory_order_acquire);
}

void lx::drm::kms_pipeline::build_damage_blob(const kms_damage_region& region) {
#if defined(LUMEN_HAS_DRM)
    if (!device_ || !atomic_damage_)
        return;
    const int fd = device_->card_fd();
    if (fd < 0)
        return;

    // The kernel validates every clip against the framebuffer — a negative origin or an
    // edge past it fails the whole commit with EINVAL, not just the clip — and a window
    // hanging off the output produces exactly that rectangle. The scanout buffer is sized
    // to the mode, so that is the bound.
    const long long max_x = mode_.width;
    const long long max_y = mode_.height;

    // FB_DAMAGE_CLIPS is an array of struct drm_mode_rect (four s32, exclusive x2/y2).
    // A blob whose length is not a multiple of that size is rejected with EINVAL.
    drm_mode_rect clips[16]{};
    unsigned n = 0;
    const unsigned limit = region.count < 16 ? region.count : 16;
    for (unsigned i = 0; i < limit; ++i) {
        const auto& r = region.rects[i];
        // 64-bit sums: "everything" arrives as INT32_MAX extents and would overflow.
        const long long x1 = r.x > 0 ? r.x : 0;
        const long long y1 = r.y > 0 ? r.y : 0;
        long long x2 = static_cast<long long>(r.x) + r.width;
        long long y2 = static_cast<long long>(r.y) + r.height;
        if (max_x > 0 && x2 > max_x)
            x2 = max_x;
        if (max_y > 0 && y2 > max_y)
            y2 = max_y;
        if (x2 <= x1 || y2 <= y1)
            continue;
        clips[n].x1 = static_cast<int32_t>(x1);
        clips[n].y1 = static_cast<int32_t>(y1);
        clips[n].x2 = static_cast<int32_t>(x2);
        clips[n].y2 = static_cast<int32_t>(y2);
        ++n;
    }

    uint32_t blob = 0;
    if (n > 0 && drmModeCreatePropertyBlob(fd, clips, sizeof(clips[0]) * n, &blob) != 0)
        blob = 0;
    // Replace unconditionally: keeping the old blob would attach a previous frame's damage
    // to this commit, so the driver would refresh the wrong region.
    if (damage_blob_id_)
        drmModeDestroyPropertyBlob(fd, damage_blob_id_);
    damage_blob_id_ = blob;
#else
    (void)region;
#endif
}

// ── commit_modeset ────────────────────────────────────────────────────────────

lx::result<void> lx::drm::commit_modeset(kms_device& device, const modeset_entry* entries,
                                         unsigned count, bool test_only) {
#if defined(LUMEN_HAS_DRM)
    const int fd = device.card_fd();
    if (fd < 0 || !entries || count == 0 || count > k_max_pipelines)
        return drm_error(lx::drm_err::mode_invalid, "modeset: nothing to commit");
    const auto fail_all = [&](int err, bool rejected) {
        for (unsigned i = 0; i < count; ++i) {
            if (!entries[i].pipe)
                continue;
            if (err)
                entries[i].pipe->set_failure(err);
            else {
                entries[i].pipe->last_rejected_state_ = rejected;
                entries[i].pipe->last_lost_master_ = false;
            }
        }
    };

    // Validate and resolve: every pipeline its plane props and modeset props; enabled ones a
    // mode and a framebuffer; none with a flip in flight (a CRTC takes one commit at a time).
    unsigned owned_crtcs = 0;
    for (unsigned i = 0; i < count; ++i) {
        const modeset_entry& e = entries[i];
        kms_pipeline* p = e.pipe;
        if (!p) {
            fail_all(0, true);
            return drm_error(lx::drm_err::mode_invalid, "modeset: null pipeline");
        }
        owned_crtcs |= 1u << p->desc_.crtc_index;
        if (auto props = p->ensure_props(); !props || !p->ensure_modeset_props(fd)) {
            fail_all(0, true);
            return drm_error(lx::drm_err::mode_invalid, "missing crtc/connector modeset props");
        }
        const kms_mode& want = e.mode ? *e.mode : p->mode_;
        if (e.enable && (!want.valid() || !e.framebuffer_id)) {
            fail_all(0, true);
            return drm_error(lx::drm_err::mode_invalid, "modeset: no mode or framebuffer");
        }
        if (!test_only && p->flip_pending_.load(std::memory_order_acquire)) {
            fail_all(0, false);
            return drm_error(lx::drm_err::page_flip_failed,
                             "drmModeAtomicCommit rejected: flip already pending (EBUSY)");
        }
    }
    // Other registered pipelines' CRTCs are ours too: never touched here.
    for (unsigned s = 0; s < k_max_pipelines; ++s)
        if (const kms_pipeline* p = device.pipelines_[s].load(std::memory_order_acquire))
            owned_crtcs |= 1u << p->desc_.crtc_index;
    const unsigned stale = device.stale_crtc_mask() & ~owned_crtcs;

    // Mode blobs, one per enabled entry.
    uint32_t blobs[k_max_pipelines]{};
    const auto destroy_blobs = [&] {
        for (unsigned i = 0; i < count; ++i)
            if (blobs[i])
                drmModeDestroyPropertyBlob(fd, blobs[i]);
    };
    for (unsigned i = 0; i < count; ++i) {
        if (!entries[i].enable)
            continue;
        const kms_mode& want = entries[i].mode ? *entries[i].mode : entries[i].pipe->mode_;
        if (drmModeCreatePropertyBlob(fd, want.kms.bytes, sizeof(want.kms.bytes), &blobs[i]) != 0) {
            destroy_blobs();
            fail_all(0, false);
            return drm_error(lx::drm_err::mode_invalid, "MODE_ID blob creation failed");
        }
    }

    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) {
        destroy_blobs();
        fail_all(0, false);
        return drm_error(lx::drm_err::page_flip_failed, "drmModeAtomicAlloc failed");
    }

    // Planes a previous master left on one of these CRTCs (an overlay, another cursor):
    // detached, or they would cover the frame. From the device's table — read at enumerate.
    const auto detach_other_planes = [&](unsigned crtc_id, unsigned keep_primary,
                                         unsigned keep_cursor) {
        for (unsigned k = 0; k < device.plane_count_; ++k) {
            const kms_plane_info& pl = device.planes_[k];
            if (pl.crtc_id != crtc_id || pl.id == keep_primary || pl.id == keep_cursor)
                continue;
            const unsigned p_crtc = find_object_prop(fd, pl.id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
            const unsigned p_fb = find_object_prop(fd, pl.id, DRM_MODE_OBJECT_PLANE, "FB_ID");
            if (p_crtc && p_fb) {
                drmModeAtomicAddProperty(req, pl.id, p_crtc, 0);
                drmModeAtomicAddProperty(req, pl.id, p_fb, 0);
            }
        }
    };

    bool any_event = false;
    for (unsigned i = 0; i < count; ++i) {
        const modeset_entry& e = entries[i];
        kms_pipeline& p = *e.pipe;
        const unsigned crtc = p.desc_.crtc_id;
        const unsigned plane = p.desc_.primary_plane_id;
        const unsigned conn = p.desc_.connector_id;
        const bool with_cursor = e.cursor && e.cursor->valid();
        const unsigned cursor_plane = with_cursor ? e.cursor->caps().plane_id : 0;
        if (!e.enable) {
            if (!e.connector_gone)
                drmModeAtomicAddProperty(req, conn, p.prop_conn_crtc_, 0);
            drmModeAtomicAddProperty(req, crtc, p.prop_crtc_active_, 0);
            drmModeAtomicAddProperty(req, crtc, p.prop_crtc_mode_, 0);
            drmModeAtomicAddProperty(req, plane, p.prop_crtc_id_, 0);
            drmModeAtomicAddProperty(req, plane, p.prop_fb_id_, 0);
            if (with_cursor)
                e.cursor->append_off_props(req);
            detach_other_planes(crtc, plane, cursor_plane);
            continue;
        }
        const kms_mode& want = e.mode ? *e.mode : p.mode_;
        const unsigned width = want.width;
        const unsigned height = want.height;
        drmModeAtomicAddProperty(req, conn, p.prop_conn_crtc_, crtc);
        drmModeAtomicAddProperty(req, crtc, p.prop_crtc_mode_, blobs[i]);
        drmModeAtomicAddProperty(req, crtc, p.prop_crtc_active_, 1);
        drmModeAtomicAddProperty(req, plane, p.prop_crtc_id_, crtc);
        drmModeAtomicAddProperty(req, plane, p.prop_fb_id_, e.framebuffer_id);
        drmModeAtomicAddProperty(req, plane, p.prop_src_x_, 0);
        drmModeAtomicAddProperty(req, plane, p.prop_src_y_, 0);
        drmModeAtomicAddProperty(req, plane, p.prop_src_w_, static_cast<uint64_t>(width) << 16);
        drmModeAtomicAddProperty(req, plane, p.prop_src_h_, static_cast<uint64_t>(height) << 16);
        drmModeAtomicAddProperty(req, plane, p.prop_crtc_x_, 0);
        drmModeAtomicAddProperty(req, plane, p.prop_crtc_y_, 0);
        drmModeAtomicAddProperty(req, plane, p.prop_crtc_w_, width);
        drmModeAtomicAddProperty(req, plane, p.prop_crtc_h_, height);
        // The composite may still be running on the GPU: the first frame waits for it too.
        if (p.prop_in_fence_fd_ && e.in_fence_fd >= 0)
            drmModeAtomicAddProperty(req, plane, p.prop_in_fence_fd_,
                                     static_cast<uint64_t>(e.in_fence_fd));
        // The cursor plane in full (visible where the pointer is, or off), so nothing
        // another master put there survives into the first frame.
        if (with_cursor)
            e.cursor->append_props(req);
        detach_other_planes(crtc, plane, cursor_plane);
        // Adaptive sync is part of the state asked for, not a neutral setting: stated here,
        // so a driver that refuses the neutral ones still gets it.
        if (p.prop_crtc_vrr_) {
            const bool vrr = e.vrr < 0 ? p.vrr_ : e.vrr != 0;
            drmModeAtomicAddProperty(req, crtc, p.prop_crtc_vrr_, vrr ? 1 : 0);
        } else if (e.vrr > 0 || (e.vrr < 0 && p.vrr_)) {
            drmModeAtomicFree(req);
            destroy_blobs();
            fail_all(0, true);
            return drm_error(lx::drm_err::test_rejected, "modeset: the CRTC has no VRR_ENABLED");
        }
        any_event = any_event || e.request_page_flip;
    }
    // CRTCs a previous master left on that nothing of ours drives: off, with their planes
    // and every connector still routed to them.
    for (unsigned k = 0; k < device.crtc_count_; ++k) {
        const kms_crtc_info& c = device.crtcs_[k];
        if (!(stale & (1u << c.index)))
            continue;
        const unsigned active = find_object_prop(fd, c.id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
        const unsigned mode_id = find_object_prop(fd, c.id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
        if (!active || !mode_id)
            continue;
        drmModeAtomicAddProperty(req, c.id, active, 0);
        drmModeAtomicAddProperty(req, c.id, mode_id, 0);
        detach_other_planes(c.id, 0, 0);
        // Routed now, read from the kernel: another master (fbcon after a VT switch) may
        // have moved connectors since any probe of ours. A connector left on a CRTC turned
        // off is refused ("enabled/connectors mismatch").
        if (drmModeRes* res = drmModeGetResources(fd)) {
            for (int j = 0; j < res->count_connectors; ++j) {
                const unsigned conn = res->connectors[j];
                bool ours = false;
                for (unsigned i = 0; i < count; ++i)
                    ours = ours || entries[i].pipe->desc_.connector_id == conn;
                if (ours)
                    continue;
                unsigned long long routed = 0;
                if (!object_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", routed) ||
                    routed != c.id)
                    continue;
                if (const unsigned prop =
                        find_object_prop(fd, conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"))
                    drmModeAtomicAddProperty(req, conn, prop, 0);
            }
            drmModeFreeResources(res);
        }
    }

    // The neutral settings last, so a driver that refuses one of them can be asked again
    // without (the request is cut back to `essential`).
    const int essential = drmModeAtomicGetCursor(req);
    for (unsigned i = 0; i < count; ++i) {
        const modeset_entry& e = entries[i];
        if (!e.enable)
            continue;
        kms_pipeline& p = *e.pipe;
        const bool with_cursor = e.cursor && e.cursor->valid();
        if (with_cursor && p.neutral_cursor_plane_ == 0) {
            p.neutral_cursor_plane_ = e.cursor->caps().plane_id;
            p.resolve_neutral(fd, p.neutral_cursor_plane_, DRM_MODE_OBJECT_PLANE, true);
        }
        for (unsigned k = 0; k < p.neutral_count_; ++k) {
            const auto& n = p.neutral_[k];
            // The cursor plane's only while it is part of this commit.
            if (n.object == p.neutral_cursor_plane_ && !with_cursor)
                continue;
            // VRR_ENABLED is stated with the essential state above.
            if (n.object == p.desc_.crtc_id && n.prop == p.prop_crtc_vrr_)
                continue;
            drmModeAtomicAddProperty(req, n.object, n.prop, n.value);
        }
    }
    const bool with_neutral = drmModeAtomicGetCursor(req) != essential;

    if (test_only) {
        int r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
                                    nullptr);
        int commit_errno = r != 0 ? errno : 0;
        if (r != 0 && commit_errno == EINVAL && with_neutral) {
            drmModeAtomicSetCursor(req, essential);
            r = drmModeAtomicCommit(fd, req,
                                    DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            commit_errno = r != 0 ? errno : 0;
        }
        drmModeAtomicFree(req);
        destroy_blobs();
        if (r != 0) {
            fail_all(commit_errno, false);
            return drm_error(lx::drm_err::test_rejected, atomic_commit_message(commit_errno));
        }
        fail_all(0, false);
        return {};
    }

    // The console's state of every CRTC we take, restored at exit.
    for (unsigned i = 0; i < count; ++i) {
        kms_pipeline& p = *entries[i].pipe;
        if (entries[i].enable && !p.saved_crtc_) {
            p.saved_crtc_ = drmModeGetCrtc(fd, p.desc_.crtc_id);
            p.saved_connector_ = p.desc_.connector_id;
        }
    }

    // One cookie for the whole commit; the device routes each CRTC's event by its id.
    flip_cookie& cookie = device.next_combined_cookie();
    unsigned mask = 0;
    bool raised[k_max_pipelines]{};
    for (unsigned i = 0; i < count; ++i) {
        kms_pipeline& p = *entries[i].pipe;
        // Only a commit that asks for events is owed them (one per CRTC in its state).
        if (any_event && p.reg_slot_ < k_max_pipelines) {
            mask |= 1u << p.reg_slot_;
            cookie.arm(p.reg_slot_, p.flip_generation_.load(std::memory_order_acquire));
        }
        if (entries[i].enable && entries[i].request_page_flip &&
            !p.flip_pending_.exchange(true, std::memory_order_acq_rel)) {
            raised[i] = true;
            atomic_commit_request note{};
            note.user_tag = entries[i].user_tag;
            note.update_primary = true;
            p.note_inflight(note);
        }
    }

    // Nonblocking, with an event: completion arrives on the card fd and is drained by the
    // event thread, like any flip.
    uint32_t flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK;
    if (any_event)
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
    int r = drmModeAtomicCommit(fd, req, flags, &cookie);
    int commit_errno = r != 0 ? errno : 0;
    bool blocking = false;
    // Refused: no event comes for it (a blocking retry carries none either).
    if (r != 0)
        cookie.disarm(mask);
    if (r != 0 && commit_errno == EINVAL) {
        // Some drivers take a modeset only blocking. Once more, without the event; the
        // completion is then reported through `dispatch_events`.
        r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        commit_errno = r != 0 ? errno : 0;
        if (r != 0 && commit_errno == EINVAL && with_neutral) {
            // A driver that takes one of the resets badly: the modeset matters more.
            drmModeAtomicSetCursor(req, essential);
            r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
            commit_errno = r != 0 ? errno : 0;
        }
        blocking = r == 0;
    }
    drmModeAtomicFree(req);
    if (r != 0) {
        for (unsigned i = 0; i < count; ++i)
            if (raised[i])
                entries[i].pipe->flip_pending_.store(false, std::memory_order_release);
        destroy_blobs();
        fail_all(commit_errno, false);
        return drm_error(lx::drm_err::page_flip_failed, atomic_commit_message(commit_errno));
    }

    for (unsigned k = 0; k < device.crtc_count_; ++k)
        if (stale & (1u << device.crtcs_[k].index))
            device.note_crtc_inactive(device.crtcs_[k].index);
    for (unsigned i = 0; i < count; ++i) {
        const modeset_entry& e = entries[i];
        kms_pipeline& p = *e.pipe;
        p.last_rejected_state_ = false;
        p.last_lost_master_ = false;
        // Ours now (lit or turned off by us): no longer a previous master's.
        device.stale_crtcs_.fetch_and(~(1u << p.desc_.crtc_index), std::memory_order_acq_rel);
        if (!e.enable) {
            p.modeset_attempted_ = true;
            if (e.cursor && e.cursor->valid())
                e.cursor->invalidate();
            continue;
        }
        p.modeset_attempted_ = true;
        if (e.cursor && e.cursor->valid())
            e.cursor->on_committed();
        // Whether flip timestamps are the CRTC's vblank timestamps: a driver without vblank
        // support (vmwgfx, some virtual GPUs) answers EOPNOTSUPP, and its flip events then
        // carry sequence 0 and a time sampled when the event was sent
        // (drm_crtc_send_vblank_event). Asked once, after the first modeset; any other
        // failure (the CRTC not on yet) leaves the assumption that it has them.
        if (!p.vblank_probed_) {
            p.vblank_probed_ = true;
            uint64_t seq = 0;
            uint64_t ns = 0;
            if (drmCrtcGetSequence(fd, p.desc_.crtc_id, &seq, &ns) != 0 && errno == EOPNOTSUPP)
                p.vblank_timestamps_.store(false, std::memory_order_release);
        }
        if (blocking && e.request_page_flip)
            p.synthetic_flip_.store(true, std::memory_order_release);
        else if (blocking && raised[i])
            p.flip_pending_.store(false, std::memory_order_release);
        if (p.mode_blob_id_)
            drmModeDestroyPropertyBlob(fd, p.mode_blob_id_);
        p.mode_blob_id_ = blobs[i];
        blobs[i] = 0;
    }
    return {};
#else
    (void)device;
    (void)entries;
    (void)count;
    (void)test_only;
    return lx::not_implemented("lx::drm::commit_modeset");
#endif
}
