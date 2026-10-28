module;

#include <stdint.h>

#if defined(LUMEN_HAS_DRM)
#include <drm_fourcc.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif
#include <stddef.h>

import lx.foundation;

export module lx.drm:cursor;

import std;

export namespace lx::drm {

struct cursor_plane_caps {
    unsigned plane_id = 0;
    /// Size of the buffers the plane scans out. Every image is padded to it: drivers such
    /// as vmwgfx accept nothing but the advertised cursor size.
    unsigned width = 0;
    unsigned height = 0;
    /// The plane has HOTSPOT_X/HOTSPOT_Y. Virtualised drivers (vmwgfx, virtio-gpu, qxl)
    /// expose their cursor plane to atomic clients only with these, and the host draws the
    /// pointer from them — without the hotspot it would click one hotspot away.
    bool hotspot = false;
};

/// The KMS cursor plane: a pointer image positioned by the display engine, so moving the
/// pointer costs a few plane properties on the next atomic commit and no composite at all.
///
/// Two ARGB8888 dumb buffers alternate. A new image is written into the one the plane is
/// not scanning out and shown by the commit that switches FB_ID, so an image change never
/// tears and the driver re-reads the image (it keys that on the framebuffer changing).
///
/// Render affinity after `create`. Takes the card fd and CRTC as plain values rather than a
/// `kms_device` so this partition stays independent of the facade.
class kms_cursor_plane {
public:
    kms_cursor_plane() = default;
    ~kms_cursor_plane();

    kms_cursor_plane(const kms_cursor_plane&) = delete;
    kms_cursor_plane& operator=(const kms_cursor_plane&) = delete;
    kms_cursor_plane(kms_cursor_plane&& other) noexcept;
    kms_cursor_plane& operator=(kms_cursor_plane&& other) noexcept;

    /// Finds a cursor-type plane usable on `crtc_index` that scans out ARGB8888 (exactly
    /// `plane_id` when nonzero — the one the CRTC allocation chose), sizes its buffers from
    /// SIZE_HINTS (the first hint that fits `max_image_*`) or else
    /// DRM_CAP_CURSOR_WIDTH/HEIGHT, and allocates both buffers. The card fd is borrowed and
    /// must outlive the plane.
    [[nodiscard]] static lx::result<kms_cursor_plane> create(int card_fd, unsigned crtc_id,
                                                             unsigned crtc_index,
                                                             unsigned max_image_w,
                                                             unsigned max_image_h,
                                                             unsigned plane_id = 0);

    [[nodiscard]] bool valid() const { return fd_ >= 0 && fbs_[0].fb != 0 && fbs_[1].fb != 0; }
    [[nodiscard]] cursor_plane_caps caps() const { return caps_; }

    /// Writes a premultiplied ARGB8888 image (w, h <= caps) into the buffer the plane is not
    /// scanning out, zero-padding the rest, and makes it the one the next commit shows.
    /// Precondition: no flip is pending — otherwise that buffer may still be on screen.
    [[nodiscard]] lx::result<void> stage_image(const std::uint32_t* argb, unsigned w, unsigned h,
                                               unsigned stride_px, int hot_x, int hot_y);
    /// Pointer (hotspot) position in CRTC coordinates.
    void set_position(int pointer_x, int pointer_y);
    void set_visible(bool visible);
    [[nodiscard]] bool visible() const { return visible_; }

    /// The state wanted differs from what the last successful commit put on the plane.
    [[nodiscard]] bool dirty() const;
    /// The plane is on, or is to be turned on — so a commit of it alone pulls in its CRTC.
    /// A commit with no CRTC cannot carry a page-flip event (EINVAL), which is why a
    /// cursor-only commit is made only when this holds.
    [[nodiscard]] bool involves_crtc() const { return valid() && (visible_ || hw_fb_ >= 0); }
    /// Adds the plane's properties to a `drmModeAtomicReq*` (kept out of this interface).
    void append_props(void* drm_atomic_req) const;
    /// Adds properties that take the plane off its CRTC (CRTC_ID 0, FB_ID 0) without changing
    /// what is wanted — for a commit that turns the CRTC off (display power). Follow a
    /// successful one with `invalidate`, so the next commit states the plane in full again.
    void append_off_props(void* drm_atomic_req) const;
    /// The commit carrying `append_props` succeeded: what was wanted is now what is shown.
    void on_committed();
    /// What the hardware shows is unknown (another DRM master had the CRTC): the next commit
    /// states the plane in full, either way. Staged images and capabilities are kept.
    void invalidate() {
        never_committed_ = true;
        hw_fb_ = -1;
    }

private:
    struct dumb_buffer {
        unsigned handle = 0;
        unsigned fb = 0;
        unsigned pitch = 0;
        unsigned long long size = 0;
        unsigned char* map = nullptr;
    };

    void release();

    int fd_ = -1;
    unsigned crtc_ = 0;
    cursor_plane_caps caps_{};
    dumb_buffer fbs_[2]{};

    unsigned p_crtc_id_ = 0, p_fb_id_ = 0;
    unsigned p_src_x_ = 0, p_src_y_ = 0, p_src_w_ = 0, p_src_h_ = 0;
    unsigned p_crtc_x_ = 0, p_crtc_y_ = 0, p_crtc_w_ = 0, p_crtc_h_ = 0;
    unsigned p_hot_x_ = 0, p_hot_y_ = 0;

    // Wanted.
    unsigned pending_ = 0;
    int x_ = 0, y_ = 0, hot_x_ = 0, hot_y_ = 0;
    bool visible_ = false;
    // On the hardware since the last successful commit. `hw_fb_` is -1 while disabled.
    int hw_fb_ = -1;
    int hw_x_ = 0, hw_y_ = 0, hw_hot_x_ = 0, hw_hot_y_ = 0;
    /// The plane has never been committed: the first commit must state it either way.
    bool never_committed_ = true;
};

} // namespace lx::drm


lx::drm::kms_cursor_plane::~kms_cursor_plane() { release(); }

lx::drm::kms_cursor_plane::kms_cursor_plane(kms_cursor_plane&& other) noexcept {
    *this = std::move(other);
}

lx::drm::kms_cursor_plane& lx::drm::kms_cursor_plane::operator=(kms_cursor_plane&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    fd_ = other.fd_;
    crtc_ = other.crtc_;
    caps_ = other.caps_;
    fbs_[0] = other.fbs_[0];
    fbs_[1] = other.fbs_[1];
    p_crtc_id_ = other.p_crtc_id_;
    p_fb_id_ = other.p_fb_id_;
    p_src_x_ = other.p_src_x_;
    p_src_y_ = other.p_src_y_;
    p_src_w_ = other.p_src_w_;
    p_src_h_ = other.p_src_h_;
    p_crtc_x_ = other.p_crtc_x_;
    p_crtc_y_ = other.p_crtc_y_;
    p_crtc_w_ = other.p_crtc_w_;
    p_crtc_h_ = other.p_crtc_h_;
    p_hot_x_ = other.p_hot_x_;
    p_hot_y_ = other.p_hot_y_;
    pending_ = other.pending_;
    x_ = other.x_;
    y_ = other.y_;
    hot_x_ = other.hot_x_;
    hot_y_ = other.hot_y_;
    visible_ = other.visible_;
    hw_fb_ = other.hw_fb_;
    hw_x_ = other.hw_x_;
    hw_y_ = other.hw_y_;
    hw_hot_x_ = other.hw_hot_x_;
    hw_hot_y_ = other.hw_hot_y_;
    never_committed_ = other.never_committed_;
    other.fd_ = -1;
    other.fbs_[0] = {};
    other.fbs_[1] = {};
    other.caps_ = {};
    return *this;
}

void lx::drm::kms_cursor_plane::release() {
#if defined(LUMEN_HAS_DRM)
    // Removing a framebuffer the plane still scans out makes the kernel disable the plane,
    // so tearing down here also takes the pointer off the screen. RMFB, not the exit's
    // CLOSEFB: a pointer frozen where it was would sit over the console or the next master's
    // first frames, and turning a cursor plane off leaves its CRTC lit (no flash).
    for (auto& b : fbs_) {
        if (b.map && b.size)
            munmap(b.map, static_cast<size_t>(b.size));
        if (fd_ >= 0 && b.fb)
            (void)drmModeRmFB(fd_, b.fb);
        if (fd_ >= 0 && b.handle)
            (void)drmModeDestroyDumbBuffer(fd_, b.handle);
        b = {};
    }
#endif
    fd_ = -1;
}

#if defined(LUMEN_HAS_DRM)
namespace {

/// One entry of a SIZE_HINTS blob (uapi `struct drm_plane_size_hint`, which older libdrm
/// headers lack).
struct plane_size_hint {
    std::uint16_t width;
    std::uint16_t height;
};

[[nodiscard]] bool create_dumb(int fd, unsigned w, unsigned h, unsigned& handle, unsigned& fb,
                               unsigned& pitch, unsigned long long& size, unsigned char*& map) {
    uint32_t hnd = 0;
    uint32_t pit = 0;
    uint64_t sz = 0;
    if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &hnd, &pit, &sz) != 0 || hnd == 0)
        return false;
    handle = hnd;
    pitch = pit;
    size = sz;
    uint64_t offset = 0;
    if (drmModeMapDumbBuffer(fd, hnd, &offset) != 0)
        return false;
    void* mapped = mmap(nullptr, static_cast<size_t>(sz), PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        static_cast<off_t>(offset));
    if (mapped == MAP_FAILED)
        return false;
    map = static_cast<unsigned char*>(mapped);
    std::memset(map, 0, static_cast<size_t>(sz));
    uint32_t handles[4] = {hnd, 0, 0, 0};
    uint32_t pitches[4] = {pit, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint32_t id = 0;
    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &id, 0) != 0 ||
        id == 0)
        return false;
    fb = id;
    return true;
}

} // namespace
#endif

lx::result<lx::drm::kms_cursor_plane> lx::drm::kms_cursor_plane::create(int fd, unsigned crtc_id,
                                                                        unsigned crtc_index,
                                                                        unsigned max_image_w,
                                                                        unsigned max_image_h,
                                                                        unsigned want_plane) {
#if defined(LUMEN_HAS_DRM)
    if (fd < 0 || crtc_id == 0 || crtc_index >= 32) {
        return lx::make_error(lx::drm_err::mode_invalid, "cursor plane: no CRTC");
    }
    drmModePlaneRes* planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        return lx::make_error(lx::drm_err::mode_invalid,
                              "cursor plane: drmModeGetPlaneResources failed");
    }

    kms_cursor_plane out{};
    unsigned size_hints_blob = 0;
    for (uint32_t i = 0; i < planes->count_planes && out.caps_.plane_id == 0; ++i) {
        drmModePlane* p = drmModeGetPlane(fd, planes->planes[i]);
        if (!p)
            continue;
        bool argb = false;
        for (uint32_t f = 0; f < p->count_formats; ++f)
            argb = argb || p->formats[f] == DRM_FORMAT_ARGB8888;
        const bool on_crtc = (p->possible_crtcs & (1u << crtc_index)) != 0;
        const unsigned plane_id = p->plane_id;
        drmModeFreePlane(p);
        if (!argb || !on_crtc || (want_plane != 0 && plane_id != want_plane))
            continue;

        drmModeObjectProperties* props = drmModeObjectGetProperties(fd, plane_id,
                                                                    DRM_MODE_OBJECT_PLANE);
        if (!props)
            continue;
        kms_cursor_plane candidate{};
        bool is_cursor = false;
        unsigned hints = 0;
        for (uint32_t k = 0; k < props->count_props; ++k) {
            drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[k]);
            if (!prop)
                continue;
            const char* n = prop->name;
            const unsigned id = prop->prop_id;
            if (std::strcmp(n, "type") == 0)
                is_cursor = props->prop_values[k] == DRM_PLANE_TYPE_CURSOR;
            else if (std::strcmp(n, "CRTC_ID") == 0)
                candidate.p_crtc_id_ = id;
            else if (std::strcmp(n, "FB_ID") == 0)
                candidate.p_fb_id_ = id;
            else if (std::strcmp(n, "SRC_X") == 0)
                candidate.p_src_x_ = id;
            else if (std::strcmp(n, "SRC_Y") == 0)
                candidate.p_src_y_ = id;
            else if (std::strcmp(n, "SRC_W") == 0)
                candidate.p_src_w_ = id;
            else if (std::strcmp(n, "SRC_H") == 0)
                candidate.p_src_h_ = id;
            else if (std::strcmp(n, "CRTC_X") == 0)
                candidate.p_crtc_x_ = id;
            else if (std::strcmp(n, "CRTC_Y") == 0)
                candidate.p_crtc_y_ = id;
            else if (std::strcmp(n, "CRTC_W") == 0)
                candidate.p_crtc_w_ = id;
            else if (std::strcmp(n, "CRTC_H") == 0)
                candidate.p_crtc_h_ = id;
            else if (std::strcmp(n, "HOTSPOT_X") == 0)
                candidate.p_hot_x_ = id;
            else if (std::strcmp(n, "HOTSPOT_Y") == 0)
                candidate.p_hot_y_ = id;
            else if (std::strcmp(n, "SIZE_HINTS") == 0)
                hints = static_cast<unsigned>(props->prop_values[k]);
            drmModeFreeProperty(prop);
        }
        drmModeFreeObjectProperties(props);
        const bool complete = candidate.p_crtc_id_ && candidate.p_fb_id_ && candidate.p_src_x_ &&
                              candidate.p_src_y_ && candidate.p_src_w_ && candidate.p_src_h_ &&
                              candidate.p_crtc_x_ && candidate.p_crtc_y_ && candidate.p_crtc_w_ &&
                              candidate.p_crtc_h_;
        if (!is_cursor || !complete)
            continue;
        out = std::move(candidate);
        out.caps_.plane_id = plane_id;
        out.caps_.hotspot = out.p_hot_x_ != 0 && out.p_hot_y_ != 0;
        size_hints_blob = hints;
    }
    drmModeFreePlaneResources(planes);

    if (out.caps_.plane_id == 0) {
        return lx::make_error(lx::drm_err::mode_invalid, "no cursor plane for this CRTC");
    }

    // SIZE_HINTS lists the sizes the plane takes in order of preference; the first one the
    // largest image fits is the one to use. Without it the legacy cursor caps are the only
    // size the driver is known to accept.
    unsigned width = 0;
    unsigned height = 0;
    if (size_hints_blob) {
        if (drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, size_hints_blob)) {
            const unsigned n = blob->length / sizeof(plane_size_hint);
            const auto* list = static_cast<const plane_size_hint*>(blob->data);
            for (unsigned i = 0; i < n && width == 0; ++i) {
                if (list[i].width >= max_image_w && list[i].height >= max_image_h) {
                    width = list[i].width;
                    height = list[i].height;
                }
            }
            // Nothing fits: take the largest, and let bigger images fall back to software.
            if (width == 0) {
                for (unsigned i = 0; i < n; ++i) {
                    if (list[i].width * list[i].height > width * height) {
                        width = list[i].width;
                        height = list[i].height;
                    }
                }
            }
            drmModeFreePropertyBlob(blob);
        }
    }
    if (width == 0 || height == 0) {
        uint64_t cap_w = 0;
        uint64_t cap_h = 0;
        width = drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &cap_w) == 0 && cap_w ? static_cast<unsigned>(cap_w) : 64u;
        height = drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &cap_h) == 0 && cap_h ? static_cast<unsigned>(cap_h) : 64u;
    }
    out.caps_.width = width;
    out.caps_.height = height;
    out.crtc_ = crtc_id;
    out.fd_ = fd;

    for (auto& b : out.fbs_) {
        if (!create_dumb(fd, width, height, b.handle, b.fb, b.pitch, b.size, b.map)) {
            return lx::make_error(lx::drm_err::open_failed,
                                  "cursor plane: dumb buffer allocation failed");
        }
    }
    return out;
#else
    (void)fd;
    (void)crtc_id;
    (void)crtc_index;
    (void)max_image_w;
    (void)max_image_h;
    (void)want_plane;
    return lx::not_implemented("lx::drm::kms_cursor_plane::create");
#endif
}

lx::result<void> lx::drm::kms_cursor_plane::stage_image(const std::uint32_t* argb, unsigned w,
                                                        unsigned h, unsigned stride_px, int hot_x,
                                                        int hot_y) {
#if defined(LUMEN_HAS_DRM)
    if (!valid() || !argb || w == 0 || h == 0 || w > caps_.width || h > caps_.height ||
        stride_px < w) {
        return lx::invalid_argument("cursor image does not fit the cursor plane");
    }
    // Never the buffer on screen. With the plane off, either is free.
    const unsigned target = hw_fb_ >= 0 ? static_cast<unsigned>(hw_fb_ ^ 1) : pending_;
    dumb_buffer& b = fbs_[target];
    // The mapping is write-combining: whole rows, written once, front to back, never read.
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
    for (unsigned y = 0; y < caps_.height; ++y) {
        unsigned char* row = b.map + static_cast<std::size_t>(y) * b.pitch;
        if (y < h) {
            std::memcpy(row, argb + static_cast<std::size_t>(y) * stride_px, row_bytes);
            std::memset(row + row_bytes, 0, static_cast<std::size_t>(caps_.width) * 4u - row_bytes);
        } else {
            std::memset(row, 0, static_cast<std::size_t>(caps_.width) * 4u);
        }
    }
    pending_ = target;
    hot_x_ = hot_x < 0 ? 0 : (hot_x >= static_cast<int>(w) ? static_cast<int>(w) - 1 : hot_x);
    hot_y_ = hot_y < 0 ? 0 : (hot_y >= static_cast<int>(h) ? static_cast<int>(h) - 1 : hot_y);
    return {};
#else
    (void)argb;
    (void)w;
    (void)h;
    (void)stride_px;
    (void)hot_x;
    (void)hot_y;
    return lx::not_implemented("lx::drm::kms_cursor_plane::stage_image");
#endif
}

void lx::drm::kms_cursor_plane::set_position(int pointer_x, int pointer_y) {
    x_ = pointer_x;
    y_ = pointer_y;
}

void lx::drm::kms_cursor_plane::set_visible(bool visible) { visible_ = visible; }

bool lx::drm::kms_cursor_plane::dirty() const {
    if (!valid())
        return false;
    if (never_committed_)
        return true;
    if (!visible_)
        return hw_fb_ >= 0;
    return hw_fb_ != static_cast<int>(pending_) || hw_x_ != x_ - hot_x_ || hw_y_ != y_ - hot_y_ ||
           hw_hot_x_ != hot_x_ || hw_hot_y_ != hot_y_;
}

void lx::drm::kms_cursor_plane::append_props(void* drm_atomic_req) const {
#if defined(LUMEN_HAS_DRM)
    auto* req = static_cast<drmModeAtomicReq*>(drm_atomic_req);
    if (!req || !valid())
        return;
    const unsigned plane = caps_.plane_id;
    if (!visible_) {
        drmModeAtomicAddProperty(req, plane, p_fb_id_, 0);
        drmModeAtomicAddProperty(req, plane, p_crtc_id_, 0);
        return;
    }
    // CRTC_X/Y are signed: a pointer at the left or top edge puts the image partly off the
    // CRTC, which the plane clips.
    const auto signed_prop = [](int v) {
        return static_cast<uint64_t>(static_cast<int64_t>(v));
    };
    drmModeAtomicAddProperty(req, plane, p_crtc_id_, crtc_);
    drmModeAtomicAddProperty(req, plane, p_fb_id_, fbs_[pending_].fb);
    drmModeAtomicAddProperty(req, plane, p_src_x_, 0);
    drmModeAtomicAddProperty(req, plane, p_src_y_, 0);
    drmModeAtomicAddProperty(req, plane, p_src_w_, static_cast<uint64_t>(caps_.width) << 16);
    drmModeAtomicAddProperty(req, plane, p_src_h_, static_cast<uint64_t>(caps_.height) << 16);
    drmModeAtomicAddProperty(req, plane, p_crtc_x_, signed_prop(x_ - hot_x_));
    drmModeAtomicAddProperty(req, plane, p_crtc_y_, signed_prop(y_ - hot_y_));
    drmModeAtomicAddProperty(req, plane, p_crtc_w_, caps_.width);
    drmModeAtomicAddProperty(req, plane, p_crtc_h_, caps_.height);
    if (caps_.hotspot) {
        drmModeAtomicAddProperty(req, plane, p_hot_x_, signed_prop(hot_x_));
        drmModeAtomicAddProperty(req, plane, p_hot_y_, signed_prop(hot_y_));
    }
#else
    (void)drm_atomic_req;
#endif
}

void lx::drm::kms_cursor_plane::append_off_props(void* drm_atomic_req) const {
#if defined(LUMEN_HAS_DRM)
    auto* req = static_cast<drmModeAtomicReq*>(drm_atomic_req);
    if (!req || !valid())
        return;
    drmModeAtomicAddProperty(req, caps_.plane_id, p_fb_id_, 0);
    drmModeAtomicAddProperty(req, caps_.plane_id, p_crtc_id_, 0);
#else
    (void)drm_atomic_req;
#endif
}

void lx::drm::kms_cursor_plane::on_committed() {
    never_committed_ = false;
    if (!visible_) {
        hw_fb_ = -1;
        return;
    }
    hw_fb_ = static_cast<int>(pending_);
    hw_x_ = x_ - hot_x_;
    hw_y_ = y_ - hot_y_;
    hw_hot_x_ = hot_x_;
    hw_hot_y_ = hot_y_;
}
