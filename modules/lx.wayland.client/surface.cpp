module;

// Bodies of lx.wayland.client:surface — wl_surface, buffers, shm pools, viewport, fractional
// scale, presentation feedback, explicit-sync points. The per-frame calls (attach,
// damage_buffer, request_frame, request_presentation_feedback, commit, the syncobj points)
// write into fixed members only; libwayland marshals each request into its own buffer.

#include "client_glue.h"

module lx.wayland.client;

import std;

import lx.foundation;
import lx.runtime;
import lx.trace;

namespace lx::wayland {

namespace {
constexpr lx::trace::category log_surface{"wayland.client.surface"};
} // namespace

#if LX_WLC_REAL

namespace {

const wl_buffer_listener k_buffer_listener = {
    .release = [](void* data, wl_buffer*) { static_cast<buffer*>(data)->on_release(); },
};

const zwp_linux_buffer_params_v1_listener k_params_listener = {
    // create_immed never sends `created`.
    .created = [](void*, zwp_linux_buffer_params_v1*, wl_buffer*) {},
    .failed = [](void* data, zwp_linux_buffer_params_v1*) { static_cast<buffer*>(data)->on_failed(); },
};

const wl_surface_listener k_surface_listener = {
    .enter = [](void* data, wl_surface*, wl_output* o) { static_cast<surface*>(data)->on_enter(o); },
    .leave = [](void* data, wl_surface*, wl_output* o) { static_cast<surface*>(data)->on_leave(o); },
    .preferred_buffer_scale =
        [](void* data, wl_surface*, std::int32_t scale) {
            static_cast<surface*>(data)->on_preferred_buffer_scale(scale);
        },
    .preferred_buffer_transform =
        [](void* data, wl_surface*, std::uint32_t transform) {
            static_cast<surface*>(data)->on_preferred_transform(transform);
        },
};

const wl_callback_listener k_frame_listener = {
    .done = [](void* data, wl_callback*,
               std::uint32_t time) { static_cast<surface*>(data)->on_frame_done(time); },
};

const wp_fractional_scale_v1_listener k_fractional_listener = {
    .preferred_scale = [](void* data, wp_fractional_scale_v1*,
                          std::uint32_t scale) { static_cast<surface*>(data)->on_fractional_scale(scale); },
};

const wp_presentation_feedback_listener k_feedback_listener = {
    .sync_output = [](void*, struct wp_presentation_feedback*, wl_output*) {},
    .presented =
        [](void* data, struct wp_presentation_feedback*, std::uint32_t sec_hi, std::uint32_t sec_lo,
           std::uint32_t nsec, std::uint32_t refresh, std::uint32_t seq_hi, std::uint32_t seq_lo,
           std::uint32_t flags) {
            auto* slot = static_cast<surface::feedback_slot*>(data);
            presentation_event e{};
            const std::uint64_t sec = (std::uint64_t{sec_hi} << 32) | sec_lo;
            e.presented = true;
            e.time_ns = sec * 1'000'000'000ull + nsec;
            e.refresh_ns = refresh;
            e.sequence = (std::uint64_t{seq_hi} << 32) | seq_lo;
            e.flags = flags;
            slot->owner->on_feedback(*slot, &e);
        },
    .discarded =
        [](void* data, struct wp_presentation_feedback*) {
            auto* slot = static_cast<surface::feedback_slot*>(data);
            slot->owner->on_feedback(*slot, nullptr);
        },
};

void tag(void* proxy) { wl_proxy_set_tag(static_cast<wl_proxy*>(proxy), &lx_wlc_tag); }

} // namespace

// ── shm_pool ──

shm_pool::~shm_pool() { destroy(); }

lx::result<void> shm_pool::create(display& d, std::size_t bytes) {
    d.check_affinity();
    if (pool_)
        return lx::invalid_argument("lx::wayland::shm_pool::create: already created");
    auto* shm = static_cast<wl_shm*>(d.native_global(global_kind::shm));
    if (!shm || bytes == 0 || bytes > static_cast<std::size_t>(INT32_MAX))
        return lx::invalid_argument("lx::wayland::shm_pool::create: no wl_shm or bad size");
    fd_ = ::memfd_create("lumen-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd_ < 0)
        return lx::make_error(lx::wayland_err::object_failed, "memfd_create failed");
    if (::ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
        destroy();
        return lx::make_error(lx::wayland_err::object_failed, "ftruncate of the shm pool failed");
    }
    // Never shrunk while the compositor maps it (a shrink would SIGBUS it).
    (void)::fcntl(fd_, F_ADD_SEALS, F_SEAL_SHRINK);
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
        destroy();
        return lx::make_error(lx::wayland_err::object_failed, "mmap of the shm pool failed");
    }
    data_ = static_cast<std::byte*>(p);
    size_ = bytes;
    pool_ = wl_shm_create_pool(shm, fd_, static_cast<std::int32_t>(bytes));
    if (!pool_) {
        destroy();
        return lx::make_error(lx::wayland_err::object_failed, "wl_shm.create_pool failed");
    }
    tag(pool_);
    return {};
}

lx::result<void> shm_pool::grow(std::size_t bytes) {
    if (!pool_)
        return lx::invalid_argument("lx::wayland::shm_pool::grow: no pool");
    if (bytes <= size_)
        return {};
    if (bytes > static_cast<std::size_t>(INT32_MAX) || ::ftruncate(fd_, static_cast<off_t>(bytes)) != 0)
        return lx::make_error(lx::wayland_err::object_failed, "growing the shm pool failed");
    void* p = ::mremap(data_, size_, bytes, MREMAP_MAYMOVE);
    if (p == MAP_FAILED)
        return lx::make_error(lx::wayland_err::object_failed, "mremap of the shm pool failed");
    data_ = static_cast<std::byte*>(p);
    size_ = bytes;
    wl_shm_pool_resize(pool_, static_cast<std::int32_t>(bytes));
    return {};
}

void shm_pool::destroy() {
    if (pool_)
        wl_shm_pool_destroy(pool_);
    pool_ = nullptr;
    if (data_)
        ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

// ── buffer ──

buffer::~buffer() { destroy(); }

lx::result<void> buffer::create_shm(shm_pool& pool, std::size_t offset, size2i size,
                                    std::int32_t stride, std::uint32_t format) {
    if (proxy_)
        return lx::invalid_argument("lx::wayland::buffer::create_shm: already created");
    if (!pool.valid() || size.empty() || stride < size.width ||
        offset + static_cast<std::size_t>(stride) * static_cast<std::size_t>(size.height) >
            pool.size())
        return lx::invalid_argument("lx::wayland::buffer::create_shm: outside the pool");
    proxy_ = wl_shm_pool_create_buffer(static_cast<wl_shm_pool*>(pool.native()),
                                       static_cast<std::int32_t>(offset), size.width, size.height,
                                       stride, format);
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "wl_shm_pool.create_buffer failed");
    tag(proxy_);
    wl_buffer_add_listener(proxy_, &k_buffer_listener, this);
    size_ = size;
    busy_ = failed_ = false;
    return {};
}

lx::result<void> buffer::create_dmabuf(display& d, std::span<const dmabuf_plane> planes,
                                       size2i size, std::uint32_t fourcc, std::uint64_t modifier,
                                       std::uint32_t flags) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::buffer::create_dmabuf: already created");
    auto* dmabuf = static_cast<zwp_linux_dmabuf_v1*>(d.native_global(global_kind::dmabuf));
    if (!dmabuf || d.version(global_kind::dmabuf) < 2)
        return lx::make_error(lx::wayland_err::missing_global,
                              "zwp_linux_dmabuf_v1 v2 (create_immed) is not available");
    if (planes.empty() || planes.size() > 4 || size.empty())
        return lx::invalid_argument("lx::wayland::buffer::create_dmabuf: 1-4 planes, a size");
    params_ = zwp_linux_dmabuf_v1_create_params(dmabuf);
    if (!params_)
        return lx::make_error(lx::wayland_err::object_failed, "create_params failed");
    zwp_linux_buffer_params_v1_add_listener(params_, &k_params_listener, this);
    for (std::size_t i = 0; i < planes.size(); ++i)
        zwp_linux_buffer_params_v1_add(params_, planes[i].fd, static_cast<std::uint32_t>(i),
                                       planes[i].offset, planes[i].stride,
                                       static_cast<std::uint32_t>(modifier >> 32),
                                       static_cast<std::uint32_t>(modifier & 0xFFFFFFFFu));
    proxy_ = zwp_linux_buffer_params_v1_create_immed(params_, size.width, size.height, fourcc,
                                                     flags);
    if (!proxy_) {
        zwp_linux_buffer_params_v1_destroy(params_);
        params_ = nullptr;
        return lx::make_error(lx::wayland_err::object_failed, "create_immed failed");
    }
    tag(proxy_);
    wl_buffer_add_listener(proxy_, &k_buffer_listener, this);
    size_ = size;
    busy_ = failed_ = false;
    return {};
}

lx::result<void> buffer::create_single_pixel(display& d, std::uint32_t r, std::uint32_t g,
                                             std::uint32_t b, std::uint32_t a) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::buffer::create_single_pixel: already created");
    auto* m = static_cast<wp_single_pixel_buffer_manager_v1*>(
        d.native_global(global_kind::single_pixel));
    if (!m)
        return lx::make_error(lx::wayland_err::missing_global,
                              "wp_single_pixel_buffer_manager_v1 is not available");
    proxy_ = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(m, r, g, b, a);
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "create_u32_rgba_buffer failed");
    tag(proxy_);
    wl_buffer_add_listener(proxy_, &k_buffer_listener, this);
    size_ = {1, 1};
    busy_ = failed_ = false;
    return {};
}

void buffer::destroy() {
    if (params_)
        zwp_linux_buffer_params_v1_destroy(params_);
    params_ = nullptr;
    if (proxy_)
        wl_buffer_destroy(proxy_);
    proxy_ = nullptr;
    busy_ = false;
}

void buffer::on_release() {
    busy_ = false;
    if (handler_)
        handler_->on_release(*this);
}

void buffer::on_failed() {
    failed_ = true;
    busy_ = false;
    log_surface.warn("the compositor refused a dmabuf ({}x{})", size_.width, size_.height);
    if (handler_)
        handler_->on_failed(*this);
}

// ── syncobj_timeline ──

syncobj_timeline::~syncobj_timeline() { destroy(); }

lx::result<void> syncobj_timeline::create(display& d, int syncobj_fd) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::syncobj_timeline::create: already created");
    auto* m = static_cast<wp_linux_drm_syncobj_manager_v1*>(d.native_global(global_kind::syncobj));
    if (!m)
        return lx::make_error(lx::wayland_err::missing_global,
                              "wp_linux_drm_syncobj_manager_v1 is not available");
    if (syncobj_fd < 0)
        return lx::invalid_argument("lx::wayland::syncobj_timeline::create: no fd");
    proxy_ = wp_linux_drm_syncobj_manager_v1_import_timeline(m, syncobj_fd);
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "import_timeline failed");
    tag(proxy_);
    return {};
}

void syncobj_timeline::destroy() {
    if (proxy_)
        wp_linux_drm_syncobj_timeline_v1_destroy(proxy_);
    proxy_ = nullptr;
}

// ── surface ──

surface::~surface() { destroy(); }

surface* surface::from_native(const void* wl) {
    if (!wl)
        return nullptr;
    auto* proxy = static_cast<wl_proxy*>(const_cast<void*>(wl));
    if (wl_proxy_get_tag(proxy) != &lx_wlc_tag)
        return nullptr;
    if (std::strcmp(wl_proxy_get_class(proxy), wl_surface_interface.name) != 0)
        return nullptr;
    return static_cast<surface*>(wl_proxy_get_user_data(proxy));
}

lx::result<void> surface::create(display& d) {
    d.check_affinity();
    if (proxy_)
        return lx::invalid_argument("lx::wayland::surface::create: already created");
    auto* compositor = static_cast<wl_compositor*>(d.native_global(global_kind::compositor));
    if (!compositor)
        return lx::make_error(lx::wayland_err::missing_global, "wl_compositor is not bound");
    proxy_ = wl_compositor_create_surface(compositor);
    if (!proxy_)
        return lx::make_error(lx::wayland_err::object_failed, "create_surface failed");
    display_ = &d;
    version_ = wl_surface_get_version(proxy_);
    tag(proxy_);
    wl_surface_add_listener(proxy_, &k_surface_listener, this);
    for (auto& f : feedback_)
        f = {};
    commit_id_ = 0;
    frames_done_ = 0;
    outputs_ = 0;
    fractional120_ = 0;
    buffer_scale_pref_ = 1;
    return {};
}

void surface::destroy() {
    for (auto& f : feedback_) {
        if (f.proxy)
            wp_presentation_feedback_destroy(f.proxy);
        f = {};
    }
    if (frame_)
        wl_callback_destroy(frame_);
    frame_ = nullptr;
    if (syncobj_)
        wp_linux_drm_syncobj_surface_v1_destroy(syncobj_);
    syncobj_ = nullptr;
    if (fractional_)
        wp_fractional_scale_v1_destroy(fractional_);
    fractional_ = nullptr;
    if (viewport_)
        wp_viewport_destroy(viewport_);
    viewport_ = nullptr;
    if (proxy_)
        wl_surface_destroy(proxy_);
    proxy_ = nullptr;
}

void surface::attach(buffer* b, point2i offset) {
    if (!proxy_)
        return;
    const bool use_offset_request = version_ >= WL_SURFACE_OFFSET_SINCE_VERSION;
    wl_surface_attach(proxy_, b ? static_cast<wl_buffer*>(b->native()) : nullptr,
                      use_offset_request ? 0 : offset.x, use_offset_request ? 0 : offset.y);
    if (use_offset_request && (offset.x != 0 || offset.y != 0))
        wl_surface_offset(proxy_, offset.x, offset.y);
    if (b)
        b->mark_busy();
}

void surface::damage_buffer(rect2i r) {
    if (!proxy_)
        return;
    if (version_ >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
        wl_surface_damage_buffer(proxy_, r.x, r.y, r.width, r.height);
    else
        wl_surface_damage(proxy_, 0, 0, INT32_MAX, INT32_MAX); // surface coordinates: all
}

void surface::set_buffer_scale(std::int32_t scale) {
    if (proxy_ && scale > 0)
        wl_surface_set_buffer_scale(proxy_, scale);
}

void surface::set_buffer_transform(std::int32_t transform) {
    if (proxy_)
        wl_surface_set_buffer_transform(proxy_, transform);
}

void surface::set_region(bool opaque, std::span<const rect2i> rects) {
    if (!proxy_ || !display_)
        return;
    auto* compositor =
        static_cast<wl_compositor*>(display_->native_global(global_kind::compositor));
    wl_region* region = nullptr;
    if (!rects.empty() && compositor) {
        region = wl_compositor_create_region(compositor);
        for (const rect2i& r : rects)
            wl_region_add(region, r.x, r.y, r.width, r.height);
    }
    if (opaque)
        wl_surface_set_opaque_region(proxy_, region);
    else
        wl_surface_set_input_region(proxy_, region);
    if (region)
        wl_region_destroy(region); // the surface keeps a copy of its state
}

void surface::set_opaque_region(std::span<const rect2i> rects) { set_region(true, rects); }

void surface::set_input_region(std::span<const rect2i> rects) {
    if (rects.empty()) {
        // An empty input region is "nothing": a region with no rects, not the null default.
        if (!proxy_ || !display_)
            return;
        auto* compositor =
            static_cast<wl_compositor*>(display_->native_global(global_kind::compositor));
        if (!compositor)
            return;
        wl_region* region = wl_compositor_create_region(compositor);
        wl_surface_set_input_region(proxy_, region);
        wl_region_destroy(region);
        return;
    }
    set_region(false, rects);
}

void surface::reset_input_region() {
    if (proxy_)
        wl_surface_set_input_region(proxy_, nullptr);
}

void surface::request_frame() {
    if (!proxy_ || frame_)
        return;
    frame_ = wl_surface_frame(proxy_);
    if (frame_)
        wl_callback_add_listener(frame_, &k_frame_listener, this);
}

void surface::on_frame_done(std::uint32_t time_ms) {
    if (frame_)
        wl_callback_destroy(frame_);
    frame_ = nullptr;
    ++frames_done_;
    if (handler_)
        handler_->on_frame(*this, time_ms);
}

bool surface::request_presentation_feedback() {
    if (!proxy_ || !display_)
        return false;
    auto* presentation =
        static_cast<wp_presentation*>(display_->native_global(global_kind::presentation));
    if (!presentation)
        return false;
    for (auto& f : feedback_) {
        if (f.proxy)
            continue;
        f.owner = this;
        f.proxy = wp_presentation_feedback(presentation, proxy_);
        if (!f.proxy)
            return false;
        f.armed = true;
        f.commit_id = 0;
        wp_presentation_feedback_add_listener(f.proxy, &k_feedback_listener, &f);
        return true;
    }
    // overflow_action::drop_newest: the oldest answers are the ones a latency join needs.
    ++feedback_dropped_;
    return false;
}

void surface::on_feedback(feedback_slot& slot, const presentation_event* presented) {
    presentation_event e = presented ? *presented : presentation_event{};
    e.commit_id = slot.commit_id;
    if (slot.proxy)
        wp_presentation_feedback_destroy(slot.proxy);
    slot.proxy = nullptr;
    slot.armed = false;
    if (handler_)
        handler_->on_presentation(*this, e);
}

unsigned surface::feedback_outstanding() const {
    unsigned n = 0;
    for (const auto& f : feedback_)
        n += f.proxy ? 1u : 0u;
    return n;
}

std::uint64_t surface::commit() {
    if (!proxy_)
        return 0;
    wl_surface_commit(proxy_);
    ++commit_id_;
    for (auto& f : feedback_)
        if (f.proxy && f.armed) {
            f.armed = false;
            f.commit_id = commit_id_;
        }
    return commit_id_;
}

lx::result<void> surface::enable_viewport() {
    if (viewport_)
        return {};
    if (!proxy_ || !display_)
        return lx::invalid_argument("lx::wayland::surface::enable_viewport: no surface");
    auto* vp = static_cast<wp_viewporter*>(display_->native_global(global_kind::viewporter));
    if (!vp)
        return lx::make_error(lx::wayland_err::missing_global, "wp_viewporter is not available");
    viewport_ = wp_viewporter_get_viewport(vp, proxy_);
    if (!viewport_)
        return lx::make_error(lx::wayland_err::object_failed, "get_viewport failed");
    tag(viewport_);
    return {};
}

void surface::set_viewport_source(double x, double y, double w, double h) {
    if (!viewport_)
        return;
    if (w < 0.0) {
        const wl_fixed_t unset = wl_fixed_from_int(-1);
        wp_viewport_set_source(viewport_, unset, unset, unset, unset);
        return;
    }
    wp_viewport_set_source(viewport_, wl_fixed_from_double(x), wl_fixed_from_double(y),
                           wl_fixed_from_double(w), wl_fixed_from_double(h));
}

void surface::set_viewport_destination(size2i size) {
    if (!viewport_)
        return;
    if (size.width <= 0 || size.height <= 0)
        wp_viewport_set_destination(viewport_, -1, -1);
    else
        wp_viewport_set_destination(viewport_, size.width, size.height);
}

lx::result<void> surface::enable_fractional_scale() {
    if (fractional_)
        return {};
    if (!proxy_ || !display_)
        return lx::invalid_argument("lx::wayland::surface::enable_fractional_scale: no surface");
    auto* m = static_cast<wp_fractional_scale_manager_v1*>(
        display_->native_global(global_kind::fractional_scale));
    if (!m)
        return lx::make_error(lx::wayland_err::missing_global,
                              "wp_fractional_scale_manager_v1 is not available");
    fractional_ = wp_fractional_scale_manager_v1_get_fractional_scale(m, proxy_);
    if (!fractional_)
        return lx::make_error(lx::wayland_err::object_failed, "get_fractional_scale failed");
    tag(fractional_);
    wp_fractional_scale_v1_add_listener(fractional_, &k_fractional_listener, this);
    return {};
}

lx::result<void> surface::enable_syncobj() {
    if (syncobj_)
        return {};
    if (!proxy_ || !display_)
        return lx::invalid_argument("lx::wayland::surface::enable_syncobj: no surface");
    auto* m = static_cast<wp_linux_drm_syncobj_manager_v1*>(
        display_->native_global(global_kind::syncobj));
    if (!m)
        return lx::make_error(lx::wayland_err::missing_global,
                              "wp_linux_drm_syncobj_manager_v1 is not available");
    syncobj_ = wp_linux_drm_syncobj_manager_v1_get_surface(m, proxy_);
    if (!syncobj_)
        return lx::make_error(lx::wayland_err::object_failed, "syncobj get_surface failed");
    tag(syncobj_);
    return {};
}

void surface::set_acquire_point(syncobj_timeline& t, std::uint64_t point) {
    if (!syncobj_ || !t.valid())
        return;
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(
        syncobj_, static_cast<wp_linux_drm_syncobj_timeline_v1*>(t.native()),
        static_cast<std::uint32_t>(point >> 32), static_cast<std::uint32_t>(point & 0xFFFFFFFFu));
}

void surface::set_release_point(syncobj_timeline& t, std::uint64_t point) {
    if (!syncobj_ || !t.valid())
        return;
    wp_linux_drm_syncobj_surface_v1_set_release_point(
        syncobj_, static_cast<wp_linux_drm_syncobj_timeline_v1*>(t.native()),
        static_cast<std::uint32_t>(point >> 32), static_cast<std::uint32_t>(point & 0xFFFFFFFFu));
}

std::uint32_t surface::preferred_scale120() const {
    if (fractional_ && fractional120_)
        return fractional120_;
    return static_cast<std::uint32_t>(std::max(1, buffer_scale_pref_)) * 120u;
}

void surface::on_enter(void* wl_output) {
    const unsigned slot = display_ ? display_->output_slot(wl_output) : ~0u;
    if (slot >= 32)
        return;
    outputs_ |= 1u << slot;
    if (handler_)
        handler_->on_output(*this, slot, true);
}

void surface::on_leave(void* wl_output) {
    const unsigned slot = display_ ? display_->output_slot(wl_output) : ~0u;
    if (slot >= 32)
        return;
    outputs_ &= ~(1u << slot);
    if (handler_)
        handler_->on_output(*this, slot, false);
}

void surface::on_preferred_buffer_scale(std::int32_t scale) {
    buffer_scale_pref_ = scale > 0 ? scale : 1;
    if (handler_ && !(fractional_ && fractional120_))
        handler_->on_preferred_scale(*this, preferred_scale120());
}

void surface::on_preferred_transform(std::uint32_t transform) {
    transform_pref_ = transform;
    if (handler_)
        handler_->on_preferred_transform(*this, transform);
}

void surface::on_fractional_scale(std::uint32_t scale120) {
    fractional120_ = scale120;
    if (handler_)
        handler_->on_preferred_scale(*this, scale120);
}

#else // !LX_WLC_REAL

shm_pool::~shm_pool() = default;
lx::result<void> shm_pool::create(display&, std::size_t) {
    return lx::not_implemented("lx::wayland::shm_pool::create");
}
lx::result<void> shm_pool::grow(std::size_t) { return lx::not_implemented("lx::wayland::shm_pool::grow"); }
void shm_pool::destroy() {}
buffer::~buffer() = default;
lx::result<void> buffer::create_shm(shm_pool&, std::size_t, size2i, std::int32_t, std::uint32_t) {
    return lx::not_implemented("lx::wayland::buffer::create_shm");
}
lx::result<void> buffer::create_dmabuf(display&, std::span<const dmabuf_plane>, size2i,
                                       std::uint32_t, std::uint64_t, std::uint32_t) {
    return lx::not_implemented("lx::wayland::buffer::create_dmabuf");
}
lx::result<void> buffer::create_single_pixel(display&, std::uint32_t, std::uint32_t,
                                             std::uint32_t, std::uint32_t) {
    return lx::not_implemented("lx::wayland::buffer::create_single_pixel");
}
void buffer::destroy() {}
void buffer::on_release() {}
void buffer::on_failed() { (void)log_surface; }
syncobj_timeline::~syncobj_timeline() = default;
lx::result<void> syncobj_timeline::create(display&, int) {
    return lx::not_implemented("lx::wayland::syncobj_timeline::create");
}
void syncobj_timeline::destroy() {}
surface::~surface() = default;
surface* surface::from_native(const void*) { return nullptr; }
lx::result<void> surface::create(display&) { return lx::not_implemented("lx::wayland::surface::create"); }
void surface::destroy() {}
void surface::attach(buffer*, point2i) {}
void surface::damage_buffer(rect2i) {}
void surface::set_buffer_scale(std::int32_t) {}
void surface::set_buffer_transform(std::int32_t) {}
void surface::set_region(bool, std::span<const rect2i>) {}
void surface::set_opaque_region(std::span<const rect2i>) {}
void surface::set_input_region(std::span<const rect2i>) {}
void surface::reset_input_region() {}
void surface::request_frame() {}
void surface::on_frame_done(std::uint32_t) {}
bool surface::request_presentation_feedback() { return false; }
void surface::on_feedback(feedback_slot&, const presentation_event*) {}
unsigned surface::feedback_outstanding() const { return 0; }
std::uint64_t surface::commit() { return 0; }
lx::result<void> surface::enable_viewport() { return lx::not_implemented("lx::wayland::surface::enable_viewport"); }
void surface::set_viewport_source(double, double, double, double) {}
void surface::set_viewport_destination(size2i) {}
lx::result<void> surface::enable_fractional_scale() {
    return lx::not_implemented("lx::wayland::surface::enable_fractional_scale");
}
lx::result<void> surface::enable_syncobj() { return lx::not_implemented("lx::wayland::surface::enable_syncobj"); }
void surface::set_acquire_point(syncobj_timeline&, std::uint64_t) {}
void surface::set_release_point(syncobj_timeline&, std::uint64_t) {}
std::uint32_t surface::preferred_scale120() const { return 120; }
void surface::on_enter(void*) {}
void surface::on_leave(void*) {}
void surface::on_preferred_buffer_scale(std::int32_t) {}
void surface::on_preferred_transform(std::uint32_t) {}
void surface::on_fractional_scale(std::uint32_t) {}

#endif // LX_WLC_REAL

} // namespace lx::wayland
