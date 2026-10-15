module;

#include <atomic>
#include <chrono>

#if defined(LUMEN_HAS_DRM)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
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

export namespace lx::drm {

enum class connector_status { connected, disconnected, unknown };

struct mode {
    unsigned width = 0;
    unsigned height = 0;
    unsigned refresh_millihz = 60000;
    bool preferred = false;
};

struct connector_info {
    unsigned id = 0;
    unsigned crtc_id = 0;
    connector_status status = connector_status::unknown;
    const char* name = "";
};

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
    [[nodiscard]] static lx::result<kms_device> open_fd(lx::unique_fd card_fd);

    [[nodiscard]] unsigned connector_count() const;
    [[nodiscard]] connector_info connector(unsigned index) const;
    [[nodiscard]] lx::result<mode> active_mode(unsigned connector_index) const;

    /// Borrowed DRM master / card FD for KMS ioctls (never transfers ownership).
    [[nodiscard]] int card_fd() const;

    /// True when this FD holds DRM master. Modesetting and atomic commits require it, so
    /// a nested session (another compositor owns the card) can enumerate but not present.
    [[nodiscard]] bool is_master() const;

    /// Open an owning render-node FD suitable for Vulkan / GBM (`/dev/dri/renderD128`).
    [[nodiscard]] lx::result<lx::unique_fd> open_render_node() const;

    void set_active_mode(unsigned connector_index, const mode& m);

    /// Refresh connector/mode cache from the kernel.
    [[nodiscard]] lx::result<void> refresh();

    [[nodiscard]] unsigned crtc_id_for(unsigned connector_index) const;
    [[nodiscard]] unsigned primary_plane_id() const { return primary_plane_id_; }
    /// Index of the driving CRTC in the resource list — the bit planes' `possible_crtcs`
    /// masks are expressed in.
    [[nodiscard]] unsigned crtc_index() const { return crtc_index_; }
    /// DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT was accepted. Virtualised drivers hide their
    /// cursor plane from atomic clients that have not set it.
    [[nodiscard]] bool cursor_hotspot_cap() const { return cursor_hotspot_cap_; }

private:
    friend class kms_atomic_commit;
    friend class plane_manager;

    void release();
    [[nodiscard]] lx::result<void> init_from_fd();

    static constexpr unsigned k_max_connectors = 16;

    struct connector_slot {
        connector_info info{};
        mode active{};
        char name_buf[32]{};
        bool used = false;
    };

    lx::unique_fd card_fd_{};
    connector_slot connectors_[k_max_connectors]{};
    unsigned connector_count_ = 0;
    unsigned primary_plane_id_ = 0;
    unsigned crtc_id_ = 0;
    unsigned crtc_index_ = 0;
    bool cursor_hotspot_cap_ = false;
};

class plane_manager {
public:
    explicit plane_manager(kms_device& device);
    [[nodiscard]] bool can_direct_scanout(unsigned connector) const;

private:
    kms_device* device_ = nullptr;
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

lx::drm::kms_device::~kms_device() { release(); }

lx::drm::kms_device::kms_device(kms_device&& other) noexcept { *this = static_cast<kms_device&&>(other); }

lx::drm::kms_device& lx::drm::kms_device::operator=(kms_device&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    card_fd_ = static_cast<lx::unique_fd&&>(other.card_fd_);
    connector_count_ = other.connector_count_;
    primary_plane_id_ = other.primary_plane_id_;
    crtc_id_ = other.crtc_id_;
    crtc_index_ = other.crtc_index_;
    cursor_hotspot_cap_ = other.cursor_hotspot_cap_;
    for (unsigned i = 0; i < k_max_connectors; ++i)
        connectors_[i] = other.connectors_[i];
    other.connector_count_ = 0;
    other.primary_plane_id_ = 0;
    other.crtc_id_ = 0;
    other.crtc_index_ = 0;
    other.cursor_hotspot_cap_ = false;
    return *this;
}

void lx::drm::kms_device::release() {
    card_fd_.reset();
    connector_count_ = 0;
    primary_plane_id_ = 0;
    crtc_id_ = 0;
    crtc_index_ = 0;
    cursor_hotspot_cap_ = false;
    for (auto& c : connectors_)
        c = {};
}

lx::result<void> lx::drm::kms_device::init_from_fd() {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "invalid card fd");
    }
    // Non-blocking so drmHandleEvent / drm_read never stalls the UI thread when
    // there is no pending page-flip event.
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0)
        (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "DRM_CLIENT_CAP_ATOMIC failed");
    }
    (void)drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    // Declares that this client sets HOTSPOT_X/Y on cursor planes. Virtualised drivers
    // (vmwgfx, virtio-gpu, qxl, vboxvideo) only expose their cursor plane to atomic
    // clients that do, because the host draws the pointer and needs the hotspot to place
    // it. Other drivers reject the cap, which is expected and harmless.
    cursor_hotspot_cap_ = drmSetClientCap(fd, DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT, 1) == 0;
    return refresh();
#else
    return lx::not_implemented("lx::drm::kms_device::init_from_fd");
#endif
}

lx::result<lx::drm::kms_device> lx::drm::kms_device::open(const char* device_path) {
#if defined(LUMEN_HAS_DRM)
    const char* path = device_path ? device_path : "/dev/dri/card0";
    const int fd = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "failed to open DRM card");
    }
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

lx::result<void> lx::drm::kms_device::refresh() {
#if defined(LUMEN_HAS_DRM)
    const int fd = card_fd_.get();
    if (fd < 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "no card fd");
    }
    drmModeRes* res = drmModeGetResources(fd);
    if (!res) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                              "drmModeGetResources failed");
    }

    connector_count_ = 0;
    for (int i = 0; i < res->count_connectors && connector_count_ < k_max_connectors; ++i) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
            continue;
        auto& slot = connectors_[connector_count_];
        slot = {};
        slot.used = true;
        slot.info.id = conn->connector_id;
        std::snprintf(slot.name_buf, sizeof(slot.name_buf), "DP-%u", connector_count_);
        slot.info.name = slot.name_buf;
        if (conn->connection == DRM_MODE_CONNECTED)
            slot.info.status = connector_status::connected;
        else if (conn->connection == DRM_MODE_DISCONNECTED)
            slot.info.status = connector_status::disconnected;
        else
            slot.info.status = connector_status::unknown;

        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            drmModeModeInfo* preferred = &conn->modes[0];
            for (int m = 0; m < conn->count_modes; ++m) {
                if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                    preferred = &conn->modes[m];
                    break;
                }
            }
            slot.active.width = preferred->hdisplay;
            slot.active.height = preferred->vdisplay;
            slot.active.refresh_millihz =
                preferred->vrefresh ? preferred->vrefresh * 1000u : 60000u;
            slot.active.preferred = true;

            // Resolve CRTC: encoder → crtc.
            if (conn->encoder_id) {
                drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
                if (enc) {
                    slot.info.crtc_id = enc->crtc_id;
                    if (!crtc_id_ && enc->crtc_id)
                        crtc_id_ = enc->crtc_id;
                    drmModeFreeEncoder(enc);
                }
            }
            if (!slot.info.crtc_id && res->count_crtcs > 0) {
                slot.info.crtc_id = res->crtcs[0];
                if (!crtc_id_)
                    crtc_id_ = res->crtcs[0];
            }
        }
        drmModeFreeConnector(conn);
        ++connector_count_;
    }

    // `possible_crtcs` is a bitmask over the resource list's CRTC order, not over ids.
    crtc_index_ = 0;
    for (int i = 0; i < res->count_crtcs; ++i) {
        if (res->crtcs[i] == crtc_id_) {
            crtc_index_ = static_cast<unsigned>(i);
            break;
        }
    }

    // The primary plane for that CRTC, chosen by its `type`. Taking the first plane that
    // can reach the CRTC worked only while the primary happened to be enumerated first;
    // with cursor planes exposed (the hotspot cap) a cursor or overlay plane can precede it.
    primary_plane_id_ = 0;
    unsigned first_usable = 0;
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    if (planes) {
        for (uint32_t i = 0; i < planes->count_planes && primary_plane_id_ == 0; ++i) {
            drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
            if (!p)
                continue;
            const bool usable = crtc_id_ == 0 || (p->possible_crtcs & (1u << crtc_index_));
            const unsigned plane_id = p->plane_id;
            drmModeFreePlane(p);
            if (!usable)
                continue;
            if (!first_usable)
                first_usable = plane_id;
            drmModeObjectProperties* props =
                drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
            if (!props)
                continue;
            for (uint32_t k = 0; k < props->count_props; ++k) {
                drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[k]);
                if (!prop)
                    continue;
                const bool is_type = std::strcmp(prop->name, "type") == 0;
                drmModeFreeProperty(prop);
                if (is_type) {
                    if (props->prop_values[k] == DRM_PLANE_TYPE_PRIMARY)
                        primary_plane_id_ = plane_id;
                    break;
                }
            }
            drmModeFreeObjectProperties(props);
        }
        drmModeFreePlaneResources(planes);
    }
    // A driver without a `type` property (pre-universal-planes): the old rule.
    if (primary_plane_id_ == 0)
        primary_plane_id_ = first_usable;

    drmModeFreeResources(res);
    return {};
#else
    return lx::not_implemented("lx::drm::kms_device::refresh");
#endif
}

unsigned lx::drm::kms_device::connector_count() const { return connector_count_; }

lx::drm::connector_info lx::drm::kms_device::connector(unsigned index) const {
    if (index >= connector_count_ || !connectors_[index].used)
        return {};
    return connectors_[index].info;
}

lx::result<lx::drm::mode> lx::drm::kms_device::active_mode(unsigned connector_index) const {
    if (connector_index >= connector_count_ || !connectors_[connector_index].used) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "connector index out of range");
    }
    const auto& m = connectors_[connector_index].active;
    if (m.width == 0 || m.height == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no active mode");
    }
    return m;
}

int lx::drm::kms_device::card_fd() const { return card_fd_.get(); }

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
    return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::open_failed),
                          "no render node");
#else
    return lx::not_implemented("lx::drm::kms_device::open_render_node");
#endif
}

void lx::drm::kms_device::set_active_mode(unsigned connector_index, const mode& m) {
    if (connector_index < connector_count_ && connectors_[connector_index].used)
        connectors_[connector_index].active = m;
}

unsigned lx::drm::kms_device::crtc_id_for(unsigned connector_index) const {
    if (connector_index < connector_count_ && connectors_[connector_index].used)
        return connectors_[connector_index].info.crtc_id;
    return crtc_id_;
}

lx::drm::plane_manager::plane_manager(kms_device& device) : device_{&device} {}

bool lx::drm::plane_manager::can_direct_scanout(unsigned) const {
#if defined(LUMEN_HAS_DRM)
    return device_ && device_->primary_plane_id_ != 0 && device_->card_fd() >= 0;
#else
    return false;
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

// ── kms_atomic_commit (needs complete kms_device) ───────────────────────────

lx::drm::kms_atomic_commit::kms_atomic_commit(kms_device& device) : device_{&device} {}

lx::drm::kms_atomic_commit::~kms_atomic_commit() {
#if defined(LUMEN_HAS_DRM)
    const int fd = device_ ? device_->card_fd() : -1;
    if (fd >= 0) {
        if (saved_crtc_) {
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
}

void lx::drm::kms_atomic_commit::set_framebuffer(unsigned fb_id) { framebuffer_id_ = fb_id; }

void lx::drm::kms_atomic_commit::emit_flip(const atomic_commit_request& request, bool presented) {
    if (!flip_handler_)
        return;
    page_flip_event ev{};
    ev.sequence = ++flip_sequence_;
    ev.timestamp_ns = static_cast<lx::runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    ev.applied_damage = request.damage;
    ev.presented = presented;
    flip_handler_(ev, flip_user_);
}

lx::result<void> lx::drm::kms_atomic_commit::ensure_props() {
#if defined(LUMEN_HAS_DRM)
    if (props_ready_ || !device_)
        return {};
    const int fd = device_->card_fd();
    const unsigned plane = device_->primary_plane_id();
    if (fd < 0 || plane == 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no primary plane");
    }

    drmModeObjectProperties* props =
        drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!props) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "drmModeObjectGetProperties failed");
    }
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
    if (!props_ready_) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "missing plane props");
    }
    return {};
#else
    return lx::not_implemented("lx::drm::kms_atomic_commit::ensure_props");
#endif
}

#if defined(LUMEN_HAS_DRM)
namespace {
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

lx::result<void> lx::drm::kms_atomic_commit::apply_modeset(unsigned connector_index, unsigned crtc,
                                                           unsigned fb, unsigned width,
                                                           unsigned height) {
#if defined(LUMEN_HAS_DRM)
    const int fd = device_ ? device_->card_fd() : -1;
    const unsigned plane = device_ ? device_->primary_plane_id() : 0;
    const unsigned connector_id = device_ ? device_->connector(connector_index).id : 0;
    if (fd < 0 || !plane || !crtc || !connector_id || !fb) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "modeset missing connector/crtc/plane");
    }

    drmModeConnector* conn = drmModeGetConnector(fd, connector_id);
    if (!conn) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "connector query failed");
    }
    drmModeModeInfo chosen{};
    bool found = false;
    for (int m = 0; m < conn->count_modes && !found; ++m) {
        if (conn->modes[m].hdisplay == width && conn->modes[m].vdisplay == height) {
            chosen = conn->modes[m];
            found = true;
        }
    }
    drmModeFreeConnector(conn);
    if (!found) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no connector mode matches the composite size");
    }

    const unsigned crtc_mode_prop = find_object_prop(fd, crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    const unsigned crtc_active_prop = find_object_prop(fd, crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    const unsigned conn_crtc_prop =
        find_object_prop(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    if (!crtc_mode_prop || !crtc_active_prop || !conn_crtc_prop) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "missing crtc/connector modeset props");
    }

    if (!saved_crtc_) {
        saved_crtc_ = drmModeGetCrtc(fd, crtc);
        saved_connector_ = connector_id;
    }

    uint32_t blob = 0;
    if (drmModeCreatePropertyBlob(fd, &chosen, sizeof(chosen), &blob) != 0) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "MODE_ID blob creation failed");
    }

    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) {
        drmModeDestroyPropertyBlob(fd, blob);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "drmModeAtomicAlloc failed");
    }

    drmModeAtomicAddProperty(req, connector_id, conn_crtc_prop, crtc);
    drmModeAtomicAddProperty(req, crtc, crtc_mode_prop, blob);
    drmModeAtomicAddProperty(req, crtc, crtc_active_prop, 1);
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

    // Blocking and without a flip event: the mode must be live before flips are queued.
    const int r = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
    const int commit_errno = r != 0 ? errno : 0;
    drmModeAtomicFree(req);
    if (r != 0) {
        drmModeDestroyPropertyBlob(fd, blob);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              atomic_commit_message(commit_errno));
    }

    if (mode_blob_id_)
        drmModeDestroyPropertyBlob(fd, mode_blob_id_);
    mode_blob_id_ = blob;
    return {};
#else
    (void)connector_index;
    (void)crtc;
    (void)fb;
    (void)width;
    (void)height;
    return lx::not_implemented("lx::drm::kms_atomic_commit::apply_modeset");
#endif
}

lx::result<void> lx::drm::kms_atomic_commit::commit(const atomic_commit_request& request) {
#if defined(LUMEN_HAS_DRM)
    if (!device_) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "null kms device");
    }
    const int fd = device_->card_fd();
    const unsigned fb = request.framebuffer_id ? request.framebuffer_id : framebuffer_id_;
    const unsigned plane = device_->primary_plane_id();
    const unsigned crtc = device_->crtc_id_for(request.connector);
    const bool primary = request.update_primary;
    // Flip events and their failure notifications describe frames; a cursor-only or
    // test-only commit is not one.
    const bool frame_event = primary && request.request_page_flip && !request.test_only;

    if (fd < 0 || !plane || !crtc || (primary && !fb)) {
        if (frame_event)
            emit_flip(request, false);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "atomic commit missing fb/plane/crtc");
    }
    if (!primary && (!request.cursor || !modeset_attempted_ || !props_ready_)) {
        // Nothing to commit, or the CRTC is not ours yet — the first commit is the
        // modeset, and it carries a frame.
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "cursor-only commit before the first modeset");
    }

    if (auto props = ensure_props(); !props)
        return props.get_error();

    auto mode = device_->active_mode(request.connector);
    const unsigned width = mode ? mode.value().width : 0;
    const unsigned height = mode ? mode.value().height : 0;
    if (!width || !height) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::mode_invalid),
                              "no active mode for connector");
    }

    // A plane-only commit inherits whatever mode the console left on the CRTC, so the very
    // first commit sets the mode our framebuffer was sized for. On failure the flip path
    // still runs — degraded, but reported once rather than every frame.
    // The cursor plane is not part of it: it joins on the next commit.
    if (!modeset_attempted_ && !request.test_only) {
        modeset_attempted_ = true;
        auto modeset = apply_modeset(request.connector, crtc, fb, width, height);
        if (!modeset) {
            if (frame_event)
                emit_flip(request, false);
            return modeset.get_error();
        }
        if (frame_event)
            emit_flip(request, true);
        return {};
    }

    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "drmModeAtomicAlloc failed");
    }

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
    const int r = drmModeAtomicCommit(fd, req, flags, this);
    // Capture errno before drmModeAtomicFree, which may clobber it.
    const int commit_errno = r != 0 ? errno : 0;
    drmModeAtomicFree(req);
    if (r != 0) {
        if (raises_flip)
            flip_pending_.store(false, std::memory_order_release);
        last_rejected_state_ =
            commit_errno == EINVAL || commit_errno == ERANGE || commit_errno == ENOSPC;
        if (frame_event)
            emit_flip(request, false);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              atomic_commit_message(commit_errno));
    }
    last_rejected_state_ = false;
    if (request.test_only)
        return {};

    if (with_cursor)
        request.cursor->on_committed();

    if (want_out_fence)
        *request.out_fence_fd_ptr = out_fence_fd;

    return {};
#else
    (void)request;
    return lx::not_implemented("lx::drm::kms_atomic_commit::commit");
#endif
}

void lx::drm::kms_atomic_commit::dispatch_events() {
#if defined(LUMEN_HAS_DRM)
    if (!device_)
        return;
    const int fd = device_->card_fd();
    if (fd < 0)
        return;
    drmEventContext ctx{};
    ctx.version = 3;
    ctx.page_flip_handler2 = [](int /*fd*/, unsigned int /*sequence*/, unsigned int tv_sec,
                                unsigned int tv_usec, unsigned int /*crtc_id*/, void* user_data) {
        auto* self = static_cast<kms_atomic_commit*>(user_data);
        if (!self)
            return;
        // Clear before the handler check: the CRTC is free again even with no handler bound.
        self->flip_pending_.store(false, std::memory_order_release);
        if (!self->flip_handler_)
            return;
        page_flip_event ev{};
        ev.sequence = ++self->flip_sequence_;
        ev.timestamp_ns = static_cast<lx::runtime::clock_time>(tv_sec) * 1'000'000'000ull +
                          static_cast<lx::runtime::clock_time>(tv_usec) * 1'000ull;
        ev.presented = true;
        self->flip_handler_(ev, self->flip_user_);
    };
    (void)drmHandleEvent(fd, &ctx);
#endif
}

void lx::drm::kms_atomic_commit::set_page_flip_handler(page_flip_handler handler, void* user) {
    flip_handler_ = handler;
    flip_user_ = user;
}

bool lx::drm::kms_atomic_commit::supports_atomic_damage() const { return atomic_damage_; }

bool lx::drm::kms_atomic_commit::flip_pending() const {
    return flip_pending_.load(std::memory_order_acquire);
}

void lx::drm::kms_atomic_commit::build_damage_blob(const kms_damage_region& region,
                                                   unsigned connector_index) {
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
    long long max_x = 0;
    long long max_y = 0;
    if (auto mode = device_->active_mode(connector_index); mode) {
        max_x = mode.value().width;
        max_y = mode.value().height;
    }

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
