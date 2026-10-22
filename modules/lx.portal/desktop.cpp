module;

// Bodies of `:desktop`. See desktop.cppm for the model.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(LUMEN_PORTAL_WAYLAND)
#include <wayland-client.h>
#include "cursor-shape-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
// The header names a parameter `namespace` (C); renamed for C++, as lx.shell does.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#include "xdg-output-unstable-v1-client-protocol.h"
#endif

module lx.portal;

import lx.foundation;
import lx.trace;
import :compose;
import :desktop;

namespace lx::portal {

#if defined(LUMEN_PORTAL_WAYLAND)

namespace {

constexpr const char* k_category = "portal.wayland";

void log_line(lx::trace::level lvl, const char* fmt, auto... args) {
    if constexpr (sizeof...(args) == 0) {
        lx::trace::logger::global().log(lvl, k_category, fmt);
    } else {
        char buf[512];
        std::snprintf(buf, sizeof buf, fmt, args...);
        lx::trace::logger::global().log(lvl, k_category, buf);
    }
}

double now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

} // namespace

/// One capture in flight: its session, the shm buffer it attaches, the frame's outcome.
struct desktop::capture_job {
    desktop_output* output = nullptr; // null for a toplevel
    ext_image_capture_source_v1* source = nullptr;
    ext_image_copy_capture_session_v1* session = nullptr;
    ext_image_copy_capture_frame_v1* frame = nullptr;
    wl_shm* shm = nullptr;
    // Constraints (the latest `done` set).
    lx::size2i size{};
    bool has_xrgb = false;
    bool has_argb = false;
    bool constraints_done = false;
    // Buffer.
    wl_buffer* buffer = nullptr;
    void* map = nullptr;
    std::size_t map_size = 0;
    lx::size2i buffer_size{};
    // Outcome.
    int transform = 0;
    bool ready = false;
    bool failed = false;
    bool stopped = false;
    unsigned fail_reason = 0;
    int retries = 0;

    void drop_buffer() {
        if (buffer)
            wl_buffer_destroy(buffer);
        buffer = nullptr;
        if (map)
            munmap(map, map_size);
        map = nullptr;
        map_size = 0;
    }
    void destroy() {
        if (frame)
            ext_image_copy_capture_frame_v1_destroy(frame);
        frame = nullptr;
        if (session)
            ext_image_copy_capture_session_v1_destroy(session);
        session = nullptr;
        if (source)
            ext_image_capture_source_v1_destroy(source);
        source = nullptr;
        drop_buffer();
    }
    bool finished() const { return ready || failed || stopped; }
    bool start_frame();
};

struct desktop::events {
    // ── registry ──
    static void global(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
        auto& d = *static_cast<desktop*>(data);
        const auto bind = [&](const wl_interface* i, uint32_t want) {
            return wl_registry_bind(reg, name, i, version < want ? version : want);
        };
        if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
            d.compositor_ = bind(&wl_compositor_interface, 4);
        } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
            d.shm_ = bind(&wl_shm_interface, 1);
        } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !d.seat_) {
            auto* seat = static_cast<wl_seat*>(bind(&wl_seat_interface, 5));
            d.seat_ = seat;
            wl_seat_add_listener(seat, &k_seat_listener, &d);
        } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
            auto o = std::make_unique<desktop_output>();
            o->global = name;
            auto* wo = static_cast<wl_output*>(bind(&wl_output_interface, 4));
            o->wl_output = wo;
            wl_output_add_listener(wo, &k_output_listener, o.get());
            if (d.xdg_output_manager_)
                attach_xdg_output(d, *o);
            d.outputs_.push_back(std::move(o));
        } else if (std::strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
            d.xdg_output_manager_ = bind(&zxdg_output_manager_v1_interface, 3);
            for (auto& o : d.outputs_)
                if (!o->xdg_output)
                    attach_xdg_output(d, *o);
        } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
            d.layer_shell_ = bind(&zwlr_layer_shell_v1_interface, 4);
        } else if (std::strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
            d.cursor_shape_ = bind(&wp_cursor_shape_manager_v1_interface, 1);
        } else if (std::strcmp(iface, wp_viewporter_interface.name) == 0) {
            d.viewporter_ = bind(&wp_viewporter_interface, 1);
        } else if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
            d.fractional_ = bind(&wp_fractional_scale_manager_v1_interface, 1);
        } else if (std::strcmp(iface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
            auto* list = static_cast<ext_foreign_toplevel_list_v1*>(
                bind(&ext_foreign_toplevel_list_v1_interface, 1));
            d.toplevel_list_ = list;
            ext_foreign_toplevel_list_v1_add_listener(list, &k_list_listener, &d);
        } else if (std::strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
            d.output_sources_ = bind(&ext_output_image_capture_source_manager_v1_interface, 1);
        } else if (std::strcmp(iface, ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
            d.toplevel_sources_ = bind(&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
        } else if (std::strcmp(iface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
            d.copy_manager_ = bind(&ext_image_copy_capture_manager_v1_interface, 1);
        }
    }
    static void global_remove(void* data, wl_registry*, uint32_t name) {
        auto& d = *static_cast<desktop*>(data);
        for (auto& o : d.outputs_)
            if (o->global == name)
                o->gone = true;
    }
    static void attach_xdg_output(desktop& d, desktop_output& o) {
        auto* xo = zxdg_output_manager_v1_get_xdg_output(
            static_cast<zxdg_output_manager_v1*>(d.xdg_output_manager_), static_cast<wl_output*>(o.wl_output));
        zxdg_output_v1_add_listener(xo, &k_xdg_output_listener, &o);
        o.xdg_output = xo;
    }

    // ── wl_seat ──
    static void seat_capabilities(void* data, wl_seat*, uint32_t caps) {
        static_cast<desktop*>(data)->seat_caps_ = caps;
    }
    static void seat_name(void*, wl_seat*, const char*) {}

    // ── wl_output ──
    static void geometry(void* data, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                         const char*, const char*, int32_t transform) {
        auto& o = *static_cast<desktop_output*>(data);
        o.transform = transform;
        if (!o.xdg_output) {
            o.logical.x = x;
            o.logical.y = y;
        }
    }
    static void mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
        auto& o = *static_cast<desktop_output*>(data);
        if (flags & WL_OUTPUT_MODE_CURRENT)
            o.mode = {w, h};
    }
    static void output_done(void* data, wl_output*) {
        auto& o = *static_cast<desktop_output*>(data);
        if (o.xdg_output || o.mode.empty())
            return;
        // Without xdg-output: the integer-scale layout size, turned with the output.
        const bool turned = (o.transform & 1) != 0;
        const int s = o.scale > 0 ? o.scale : 1;
        o.logical.width = (turned ? o.mode.height : o.mode.width) / s;
        o.logical.height = (turned ? o.mode.width : o.mode.height) / s;
    }
    static void scale(void* data, wl_output*, int32_t factor) {
        static_cast<desktop_output*>(data)->scale = factor;
    }
    static void name(void* data, wl_output*, const char* n) {
        static_cast<desktop_output*>(data)->name = n ? n : "";
    }
    static void description(void* data, wl_output*, const char* desc) {
        static_cast<desktop_output*>(data)->description = desc ? desc : "";
    }

    // ── xdg_output ──
    static void logical_position(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
        auto& o = *static_cast<desktop_output*>(data);
        o.logical.x = x;
        o.logical.y = y;
    }
    static void logical_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) {
        auto& o = *static_cast<desktop_output*>(data);
        o.logical.width = w;
        o.logical.height = h;
    }
    static void xdg_done(void*, zxdg_output_v1*) {}
    static void xdg_name(void* data, zxdg_output_v1*, const char* n) {
        auto& o = *static_cast<desktop_output*>(data);
        if (o.name.empty() && n)
            o.name = n;
    }
    static void xdg_description(void* data, zxdg_output_v1*, const char* desc) {
        auto& o = *static_cast<desktop_output*>(data);
        if (o.description.empty() && desc)
            o.description = desc;
    }

    // ── toplevel list ──
    static void toplevel(void* data, ext_foreign_toplevel_list_v1*, ext_foreign_toplevel_handle_v1* h) {
        auto& d = *static_cast<desktop*>(data);
        auto t = std::make_unique<desktop_toplevel>();
        t->handle = h;
        ext_foreign_toplevel_handle_v1_add_listener(h, &k_handle_listener, t.get());
        d.toplevels_.push_back(std::move(t));
    }
    static void finished(void* data, ext_foreign_toplevel_list_v1* list) {
        auto& d = *static_cast<desktop*>(data);
        ext_foreign_toplevel_list_v1_destroy(list);
        if (d.toplevel_list_ == list)
            d.toplevel_list_ = nullptr;
    }
    static void closed(void* data, ext_foreign_toplevel_handle_v1*) {
        static_cast<desktop_toplevel*>(data)->closed = true;
    }
    static void handle_done(void*, ext_foreign_toplevel_handle_v1*) {}
    static void title(void* data, ext_foreign_toplevel_handle_v1*, const char* t) {
        static_cast<desktop_toplevel*>(data)->title = t ? t : "";
    }
    static void app_id(void* data, ext_foreign_toplevel_handle_v1*, const char* a) {
        static_cast<desktop_toplevel*>(data)->app_id = a ? a : "";
    }
    static void identifier(void* data, ext_foreign_toplevel_handle_v1*, const char* id) {
        static_cast<desktop_toplevel*>(data)->identifier = id ? id : "";
    }

    // ── capture session ──
    static void buffer_size(void* data, ext_image_copy_capture_session_v1*, uint32_t w, uint32_t h) {
        auto& j = *static_cast<capture_job*>(data);
        j.size = {static_cast<int>(w), static_cast<int>(h)};
        j.has_xrgb = j.has_argb = false;
    }
    static void shm_format(void* data, ext_image_copy_capture_session_v1*, uint32_t format) {
        auto& j = *static_cast<capture_job*>(data);
        if (format == WL_SHM_FORMAT_XRGB8888)
            j.has_xrgb = true;
        else if (format == WL_SHM_FORMAT_ARGB8888)
            j.has_argb = true;
    }
    static void dmabuf_device(void*, ext_image_copy_capture_session_v1*, wl_array*) {}
    static void dmabuf_format(void*, ext_image_copy_capture_session_v1*, uint32_t, wl_array*) {}
    static void session_done(void* data, ext_image_copy_capture_session_v1*) {
        auto& j = *static_cast<capture_job*>(data);
        j.constraints_done = true;
        // The first set of constraints, or new ones after a failed frame: (re)start.
        if (!j.frame && !j.finished() && !j.start_frame())
            j.failed = true;
    }
    static void session_stopped(void* data, ext_image_copy_capture_session_v1*) {
        static_cast<capture_job*>(data)->stopped = true;
    }

    // ── capture frame ──
    static void transform(void* data, ext_image_copy_capture_frame_v1*, uint32_t t) {
        static_cast<capture_job*>(data)->transform = static_cast<int>(t);
    }
    static void damage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t, int32_t, int32_t) {}
    static void presentation_time(void*, ext_image_copy_capture_frame_v1*, uint32_t, uint32_t, uint32_t) {}
    static void ready(void* data, ext_image_copy_capture_frame_v1*) {
        static_cast<capture_job*>(data)->ready = true;
    }
    static void failed(void* data, ext_image_copy_capture_frame_v1* frame, uint32_t reason) {
        auto& j = *static_cast<capture_job*>(data);
        ext_image_copy_capture_frame_v1_destroy(frame);
        if (j.frame == frame)
            j.frame = nullptr;
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS && j.retries < 3) {
            // New constraints follow (and end in `done`, which starts the next frame).
            ++j.retries;
            j.constraints_done = false;
            return;
        }
        j.fail_reason = reason;
        if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED)
            j.stopped = true;
        else
            j.failed = true;
    }

    static constexpr wl_registry_listener k_registry_listener = {.global = global,
                                                                 .global_remove = global_remove};
    static constexpr wl_seat_listener k_seat_listener = {.capabilities = seat_capabilities,
                                                         .name = seat_name};
    static constexpr wl_output_listener k_output_listener = {.geometry = geometry,
                                                             .mode = mode,
                                                             .done = output_done,
                                                             .scale = scale,
                                                             .name = name,
                                                             .description = description};
    static constexpr zxdg_output_v1_listener k_xdg_output_listener = {.logical_position = logical_position,
                                                                      .logical_size = logical_size,
                                                                      .done = xdg_done,
                                                                      .name = xdg_name,
                                                                      .description = xdg_description};
    static constexpr ext_foreign_toplevel_list_v1_listener k_list_listener = {.toplevel = toplevel,
                                                                              .finished = finished};
    static constexpr ext_foreign_toplevel_handle_v1_listener k_handle_listener = {.closed = closed,
                                                                                  .done = handle_done,
                                                                                  .title = title,
                                                                                  .app_id = app_id,
                                                                                  .identifier = identifier};
    static constexpr ext_image_copy_capture_session_v1_listener k_session_listener = {
        .buffer_size = buffer_size,
        .shm_format = shm_format,
        .dmabuf_device = dmabuf_device,
        .dmabuf_format = dmabuf_format,
        .done = session_done,
        .stopped = session_stopped};
    static constexpr ext_image_copy_capture_frame_v1_listener k_frame_listener = {
        .transform = transform,
        .damage = damage,
        .presentation_time = presentation_time,
        .ready = ready,
        .failed = failed};
};

bool desktop::capture_job::start_frame() {
    if (size.empty() || (!has_xrgb && !has_argb) || !shm)
        return false;
    if (!buffer || buffer_size.width != size.width || buffer_size.height != size.height) {
        drop_buffer();
        const std::size_t stride = static_cast<std::size_t>(size.width) * 4u;
        const std::size_t bytes = stride * static_cast<std::size_t>(size.height);
        const int fd = memfd_create("lumen-portal-capture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0)
            return false;
        if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
            close(fd);
            return false;
        }
        (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
        void* m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            return false;
        }
        wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(bytes));
        close(fd);
        buffer = wl_shm_pool_create_buffer(pool, 0, size.width, size.height, static_cast<int32_t>(stride),
                                           has_xrgb ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        map = m;
        map_size = bytes;
        buffer_size = size;
    }
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    ext_image_copy_capture_frame_v1_add_listener(frame, &events::k_frame_listener, this);
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    // A fresh buffer: all of it is stale.
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, size.width, size.height);
    ext_image_copy_capture_frame_v1_capture(frame);
    return true;
}

desktop::~desktop() {
    for (auto& t : toplevels_)
        if (t->handle)
            ext_foreign_toplevel_handle_v1_destroy(static_cast<ext_foreign_toplevel_handle_v1*>(t->handle));
    if (toplevel_list_)
        ext_foreign_toplevel_list_v1_destroy(static_cast<ext_foreign_toplevel_list_v1*>(toplevel_list_));
    for (auto& o : outputs_) {
        if (o->xdg_output)
            zxdg_output_v1_destroy(static_cast<zxdg_output_v1*>(o->xdg_output));
        if (auto* wo = static_cast<wl_output*>(o->wl_output)) {
            if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(wo)) >= 3)
                wl_output_release(wo);
            else
                wl_output_destroy(wo);
        }
    }
    if (copy_manager_)
        ext_image_copy_capture_manager_v1_destroy(static_cast<ext_image_copy_capture_manager_v1*>(copy_manager_));
    if (output_sources_)
        ext_output_image_capture_source_manager_v1_destroy(
            static_cast<ext_output_image_capture_source_manager_v1*>(output_sources_));
    if (toplevel_sources_)
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(
            static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(toplevel_sources_));
    if (xdg_output_manager_)
        zxdg_output_manager_v1_destroy(static_cast<zxdg_output_manager_v1*>(xdg_output_manager_));
    if (fractional_)
        wp_fractional_scale_manager_v1_destroy(static_cast<wp_fractional_scale_manager_v1*>(fractional_));
    if (viewporter_)
        wp_viewporter_destroy(static_cast<wp_viewporter*>(viewporter_));
    if (cursor_shape_)
        wp_cursor_shape_manager_v1_destroy(static_cast<wp_cursor_shape_manager_v1*>(cursor_shape_));
    if (layer_shell_)
        zwlr_layer_shell_v1_destroy(static_cast<zwlr_layer_shell_v1*>(layer_shell_));
    if (auto* seat = static_cast<wl_seat*>(seat_)) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(seat)) >= 5)
            wl_seat_release(seat);
        else
            wl_seat_destroy(seat);
    }
    if (shm_)
        wl_shm_destroy(static_cast<wl_shm*>(shm_));
    if (compositor_)
        wl_compositor_destroy(static_cast<wl_compositor*>(compositor_));
    if (registry_)
        wl_registry_destroy(static_cast<wl_registry*>(registry_));
    if (display_) {
        wl_display_flush(static_cast<wl_display*>(display_));
        wl_display_disconnect(static_cast<wl_display*>(display_));
    }
}

lx::result<std::unique_ptr<desktop>> desktop::connect(const char* name) {
    auto d = std::make_unique<desktop>();
    auto* dpy = wl_display_connect(name);
    if (!dpy)
        return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::bind_failed),
                              "cannot connect to the Wayland display");
    d->display_ = dpy;
    auto* reg = wl_display_get_registry(dpy);
    d->registry_ = reg;
    wl_registry_add_listener(reg, &events::k_registry_listener, d.get());
    // Globals, then their first events (outputs, xdg-outputs, the toplevel list).
    if (wl_display_roundtrip(dpy) < 0 || wl_display_roundtrip(dpy) < 0)
        return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::dispatch_failed),
                              "the Wayland display went away during setup");
    return d;
}

int desktop::fd() const { return display_ ? wl_display_get_fd(static_cast<wl_display*>(display_)) : -1; }

bool desktop::prepare() {
    auto* dpy = static_cast<wl_display*>(display_);
    if (broken_ || wl_display_dispatch_pending(dpy) < 0 || (wl_display_flush(dpy) < 0 && errno != EAGAIN))
        broken_ = true;
    return !broken_;
}

bool desktop::dispatch_readable() {
    auto* dpy = static_cast<wl_display*>(display_);
    if (broken_)
        return false;
    if (wl_display_prepare_read(dpy) != 0) {
        if (wl_display_dispatch_pending(dpy) < 0)
            broken_ = true;
        return !broken_;
    }
    if (wl_display_read_events(dpy) < 0 || wl_display_dispatch_pending(dpy) < 0)
        broken_ = true;
    return !broken_;
}

bool desktop::pump(int timeout_ms) {
    auto* dpy = static_cast<wl_display*>(display_);
    if (broken_)
        return false;
    while (wl_display_prepare_read(dpy) != 0)
        if (wl_display_dispatch_pending(dpy) < 0) {
            broken_ = true;
            return false;
        }
    if (wl_display_flush(dpy) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(dpy);
        broken_ = true;
        return false;
    }
    pollfd pfd{wl_display_get_fd(dpy), POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) > 0) {
        if (wl_display_read_events(dpy) < 0) {
            broken_ = true;
            return false;
        }
    } else {
        wl_display_cancel_read(dpy);
    }
    if (wl_display_dispatch_pending(dpy) < 0)
        broken_ = true;
    return !broken_;
}

bool desktop::can_capture_outputs() const { return copy_manager_ && output_sources_ && shm_; }
bool desktop::can_capture_toplevels() const { return copy_manager_ && toplevel_sources_ && shm_; }

lx::result<void> desktop::run_captures(std::vector<capture_job*>& jobs, int timeout_ms) {
    const double deadline = now_ms() + timeout_ms;
    for (;;) {
        bool all = true;
        for (capture_job* j : jobs)
            all = all && j->finished();
        if (all)
            break;
        const double left = deadline - now_ms();
        if (left <= 0)
            return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::dispatch_failed),
                                  "the compositor did not deliver a captured frame in time");
        if (!pump(static_cast<int>(left) + 1))
            return lx::make_error(lx::error_domain::wayland,
                                  static_cast<int>(lx::wayland_err::client_disconnected),
                                  "the Wayland connection broke during a capture");
    }
    for (capture_job* j : jobs) {
        if (j->stopped)
            return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::protocol_violation),
                                  "the compositor stopped the capture (output gone or session locked)");
        if (j->failed)
            return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::protocol_violation),
                                  "the compositor failed the captured frame");
    }
    return {};
}

lx::result<std::vector<output_shot>> desktop::capture_outputs(bool paint_cursor, int timeout_ms) {
    if (!can_capture_outputs())
        return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::bind_failed),
                              "the compositor offers no ext-image-copy-capture output source");
    std::vector<std::unique_ptr<capture_job>> owned;
    std::vector<capture_job*> jobs;
    for (auto& o : outputs_) {
        if (o->gone || o->logical.width <= 0 || o->logical.height <= 0)
            continue;
        auto j = std::make_unique<capture_job>();
        j->output = o.get();
        j->shm = static_cast<wl_shm*>(shm_);
        j->source = ext_output_image_capture_source_manager_v1_create_source(
            static_cast<ext_output_image_capture_source_manager_v1*>(output_sources_),
            static_cast<wl_output*>(o->wl_output));
        j->session = ext_image_copy_capture_manager_v1_create_session(
            static_cast<ext_image_copy_capture_manager_v1*>(copy_manager_), j->source,
            paint_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
        ext_image_copy_capture_session_v1_add_listener(j->session, &events::k_session_listener, j.get());
        jobs.push_back(j.get());
        owned.push_back(std::move(j));
    }
    if (jobs.empty())
        return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::bind_failed),
                              "no output to capture");
    const double t0 = now_ms();
    const lx::result<void> ran = run_captures(jobs, timeout_ms);
    std::vector<output_shot> shots;
    if (ran.ok()) {
        for (capture_job* j : jobs) {
            output_shot s;
            s.name = j->output->name;
            s.description = j->output->description;
            s.logical = j->output->logical;
            s.pixels = upright(static_cast<const std::uint32_t*>(j->map), j->buffer_size.width,
                               j->buffer_size.height, j->buffer_size.width,
                               static_cast<lx::buffer_transform>(j->transform & 7));
            log_line(lx::trace::level::debug, "captured output %s %dx%d transform=%d layout=%d,%d %dx%d",
                     s.name.c_str(), j->buffer_size.width, j->buffer_size.height, j->transform,
                     s.logical.x, s.logical.y, s.logical.width, s.logical.height);
            shots.push_back(std::move(s));
        }
        log_line(lx::trace::level::debug, "captured %zu output(s) in %.1f ms", shots.size(), now_ms() - t0);
    }
    for (capture_job* j : jobs)
        j->destroy();
    (void)prepare();
    if (!ran.ok())
        return ran.get_error();
    return shots;
}

lx::result<image> desktop::capture_toplevel(std::string_view identifier, bool paint_cursor, int timeout_ms) {
    if (!can_capture_toplevels())
        return lx::make_error(lx::error_domain::wayland, static_cast<int>(lx::wayland_err::bind_failed),
                              "the compositor offers no ext-image-copy-capture toplevel source");
    desktop_toplevel* target = nullptr;
    for (auto& t : toplevels_)
        if (!t->closed && t->identifier == identifier)
            target = t.get();
    if (!target)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "the chosen window is gone");
    capture_job j;
    j.shm = static_cast<wl_shm*>(shm_);
    j.source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(toplevel_sources_),
        static_cast<ext_foreign_toplevel_handle_v1*>(target->handle));
    j.session = ext_image_copy_capture_manager_v1_create_session(
        static_cast<ext_image_copy_capture_manager_v1*>(copy_manager_), j.source,
        paint_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
    ext_image_copy_capture_session_v1_add_listener(j.session, &events::k_session_listener, &j);
    std::vector<capture_job*> jobs{&j};
    const lx::result<void> ran = run_captures(jobs, timeout_ms);
    image out;
    if (ran.ok())
        out = upright(static_cast<const std::uint32_t*>(j.map), j.buffer_size.width, j.buffer_size.height,
                      j.buffer_size.width, static_cast<lx::buffer_transform>(j.transform & 7));
    j.destroy();
    (void)prepare();
    if (!ran.ok())
        return ran.get_error();
    return out;
}

#else // !LUMEN_PORTAL_WAYLAND

struct desktop::capture_job {};
struct desktop::events {};
desktop::~desktop() = default;
lx::result<std::unique_ptr<desktop>> desktop::connect(const char*) {
    return lx::not_implemented("lx::portal::desktop::connect");
}
int desktop::fd() const { return -1; }
bool desktop::prepare() { return false; }
bool desktop::dispatch_readable() { return false; }
bool desktop::pump(int) { return false; }
bool desktop::can_capture_outputs() const { return false; }
bool desktop::can_capture_toplevels() const { return false; }
lx::result<void> desktop::run_captures(std::vector<capture_job*>&, int) {
    return lx::not_implemented("lx::portal::desktop::run_captures");
}
lx::result<std::vector<output_shot>> desktop::capture_outputs(bool, int) {
    return lx::not_implemented("lx::portal::desktop::capture_outputs");
}
lx::result<image> desktop::capture_toplevel(std::string_view, bool, int) {
    return lx::not_implemented("lx::portal::desktop::capture_toplevel");
}

#endif

} // namespace lx::portal
