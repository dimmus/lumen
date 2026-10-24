module;

#if defined(LUMEN_HAS_RSVG)
#include <cairo.h>
#include <dlfcn.h>
#include <librsvg/rsvg.h>
#endif

module lx.shell;

import std;
import lx.foundation;
import lx.trace;

namespace {
#if defined(LUMEN_HAS_RSVG)
/// The librsvg, cairo and GLib entry points the shell uses, from one dlopen'd handle
/// (dlsym searches the library's own dependencies too, which is where cairo and GLib are).
struct rsvg_api {
    decltype(&rsvg_handle_new_from_file) new_from_file = nullptr;
    decltype(&rsvg_handle_get_intrinsic_size_in_pixels) intrinsic_size = nullptr;
    decltype(&rsvg_handle_render_document) render_document = nullptr;
    decltype(&g_object_unref) object_unref = nullptr;
    decltype(&g_error_free) error_free = nullptr;
    decltype(&cairo_image_surface_create_for_data) surface_for_data = nullptr;
    decltype(&cairo_create) create = nullptr;
    decltype(&cairo_destroy) destroy = nullptr;
    decltype(&cairo_surface_flush) surface_flush = nullptr;
    decltype(&cairo_surface_destroy) surface_destroy = nullptr;
    bool ok = false;
};

const rsvg_api& api() {
    // Once, on first use (thread-safe: the background renders on a thread of its own).
    // Never closed: GLib does not support being unloaded.
    static const rsvg_api loaded = [] {
        rsvg_api a{};
        void* lib = ::dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            lx::trace::logger::global().log(lx::trace::level::warn, "shell",
                                            "librsvg-2.so.2 cannot be loaded: SVG icons and "
                                            "backgrounds are not drawn");
            return a;
        }
        const auto sym = [lib]<class F>(F& slot, const char* name) {
            slot = reinterpret_cast<F>(::dlsym(lib, name));
            return slot != nullptr;
        };
        a.ok = sym(a.new_from_file, "rsvg_handle_new_from_file") &&
               sym(a.intrinsic_size, "rsvg_handle_get_intrinsic_size_in_pixels") &&
               sym(a.render_document, "rsvg_handle_render_document") &&
               sym(a.object_unref, "g_object_unref") && sym(a.error_free, "g_error_free") &&
               sym(a.surface_for_data, "cairo_image_surface_create_for_data") &&
               sym(a.create, "cairo_create") && sym(a.destroy, "cairo_destroy") &&
               sym(a.surface_flush, "cairo_surface_flush") &&
               sym(a.surface_destroy, "cairo_surface_destroy");
        if (!a.ok)
            lx::trace::logger::global().log(lx::trace::level::warn, "shell",
                                            "librsvg lacks an entry point Lumen needs (2.52+): "
                                            "SVG icons and backgrounds are not drawn");
        return a;
    }();
    return loaded;
}
#endif
} // namespace

bool lx::shell::svg_document::open(const char* path) {
    reset();
#if defined(LUMEN_HAS_RSVG)
    const rsvg_api& a = api();
    if (!a.ok || !path)
        return false;
    GError* err = nullptr;
    handle_ = a.new_from_file(path, &err);
    if (err)
        a.error_free(err);
    return handle_ != nullptr;
#else
    (void)path;
    return false;
#endif
}

bool lx::shell::svg_document::intrinsic_size(double& width, double& height) const {
#if defined(LUMEN_HAS_RSVG)
    if (!handle_)
        return false;
    width = height = 0.0;
    return api().intrinsic_size(static_cast<RsvgHandle*>(handle_), &width, &height) != 0 &&
           width > 0.0 && height > 0.0;
#else
    (void)width;
    (void)height;
    return false;
#endif
}

bool lx::shell::svg_document::render(unsigned char* pixels, int width, int height, int stride,
                                     lx::rect2f viewport) const {
#if defined(LUMEN_HAS_RSVG)
    if (!handle_ || !pixels || width <= 0 || height <= 0)
        return false;
    const rsvg_api& a = api();
    cairo_surface_t* surf =
        a.surface_for_data(pixels, CAIRO_FORMAT_ARGB32, width, height, stride);
    cairo_t* cr = a.create(surf);
    const RsvgRectangle vp{viewport.x, viewport.y, viewport.width, viewport.height};
    GError* err = nullptr;
    const bool ok = a.render_document(static_cast<RsvgHandle*>(handle_), cr, &vp, &err) != 0;
    if (err)
        a.error_free(err);
    a.destroy(cr);
    a.surface_flush(surf);
    a.surface_destroy(surf);
    return ok;
#else
    (void)pixels;
    (void)width;
    (void)height;
    (void)stride;
    (void)viewport;
    return false;
#endif
}

void lx::shell::svg_document::reset() {
#if defined(LUMEN_HAS_RSVG)
    if (handle_)
        api().object_unref(handle_);
#endif
    handle_ = nullptr;
}
