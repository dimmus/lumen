module;

#include <chrono>

#if defined(LUMEN_HAS_DRM)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

import lx.foundation;
import lx.runtime;

export module lx.drm;

export import :atomic;

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
    for (unsigned i = 0; i < k_max_connectors; ++i)
        connectors_[i] = other.connectors_[i];
    other.connector_count_ = 0;
    other.primary_plane_id_ = 0;
    other.crtc_id_ = 0;
    return *this;
}

void lx::drm::kms_device::release() {
    card_fd_.reset();
    connector_count_ = 0;
    primary_plane_id_ = 0;
    crtc_id_ = 0;
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

    // Primary plane for the first CRTC.
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    if (planes) {
        for (uint32_t i = 0; i < planes->count_planes; ++i) {
            drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
            if (!p)
                continue;
            // Prefer a plane that can use our CRTC.
            if (crtc_id_ == 0 || (p->possible_crtcs & 1u)) {
                primary_plane_id_ = p->plane_id;
                drmModeFreePlane(p);
                break;
            }
            drmModeFreePlane(p);
        }
        drmModeFreePlaneResources(planes);
    }

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

// ── kms_atomic_commit (needs complete kms_device) ───────────────────────────

lx::drm::kms_atomic_commit::kms_atomic_commit(kms_device& device) : device_{&device} {}

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

    if (fd < 0 || !fb || !plane || !crtc) {
        if (request.request_page_flip)
            emit_flip(request, false);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "atomic commit missing fb/plane/crtc");
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

    drmModeAtomicReq* req = drmModeAtomicAlloc();
    if (!req) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "drmModeAtomicAlloc failed");
    }

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

    // OUT_FENCE_PTR takes the address of a signed 32-bit slot that the kernel fills with a
    // ready-to-use sync_file FD — not a syncobj handle needing a separate export.
    int32_t out_fence_fd = -1;
    const bool want_out_fence = prop_out_fence_ptr_ && request.out_fence_fd_ptr;
    if (want_out_fence)
        drmModeAtomicAddProperty(req, plane, prop_out_fence_ptr_,
                                 reinterpret_cast<uint64_t>(&out_fence_fd));

    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
    if (request.request_page_flip)
        flags |= DRM_MODE_PAGE_FLIP_EVENT;
    if (request.async)
        flags |= DRM_MODE_PAGE_FLIP_ASYNC;

    const int r = drmModeAtomicCommit(fd, req, flags, this);
    drmModeAtomicFree(req);
    if (r != 0) {
        if (request.request_page_flip)
            emit_flip(request, false);
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::page_flip_failed),
                              "drmModeAtomicCommit failed");
    }

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
        if (!self || !self->flip_handler_)
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

void lx::drm::kms_atomic_commit::build_damage_blob(const kms_damage_region& region) {
#if defined(LUMEN_HAS_DRM)
    if (!device_ || !atomic_damage_ || region.count == 0)
        return;
    const int fd = device_->card_fd();
    if (fd < 0)
        return;
    struct clip {
        uint16_t x1, y1, x2, y2;
    };
    clip clips[16]{};
    const unsigned n = region.count < 16 ? region.count : 16;
    for (unsigned i = 0; i < n; ++i) {
        clips[i].x1 = static_cast<uint16_t>(region.rects[i].x);
        clips[i].y1 = static_cast<uint16_t>(region.rects[i].y);
        clips[i].x2 = static_cast<uint16_t>(region.rects[i].x + region.rects[i].width);
        clips[i].y2 = static_cast<uint16_t>(region.rects[i].y + region.rects[i].height);
    }
    uint32_t blob = 0;
    if (drmModeCreatePropertyBlob(fd, clips, sizeof(clip) * n, &blob) == 0) {
        if (damage_blob_id_)
            drmModeDestroyPropertyBlob(fd, damage_blob_id_);
        damage_blob_id_ = blob;
    }
#else
    (void)region;
#endif
}
