// lx.ui:icon's rasteriser: librsvg (dlopen'd) behind LUMEN_HAS_RSVG. Its GLib and cairo
// headers stay out of the module interface.

module;

#if defined(LUMEN_HAS_RSVG)
#include <cairo.h>
#include <dlfcn.h>
#include <librsvg/rsvg.h>
#endif
#include <sys/stat.h>

module lx.ui;

import std;
import lx.foundation;
import lx.trace;

namespace {

#if defined(LUMEN_HAS_RSVG)
/// librsvg, cairo and GLib entry points, from one dlopen'd handle (see lx.shell:svg for why
/// it is not linked).
struct lx_ui_rsvg_api {
    decltype(&rsvg_handle_new_from_file) new_from_file = nullptr;
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

const lx_ui_rsvg_api& lx_ui_rsvg() {
    // Once, thread-safe (the worker may be first). Never closed: GLib cannot be unloaded.
    static const lx_ui_rsvg_api loaded = [] {
        lx_ui_rsvg_api a{};
        void* lib = ::dlopen("librsvg-2.so.2", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            lx::trace::logger::global().log(lx::trace::level::warn, "ui.icon",
                                            "librsvg-2.so.2 cannot be loaded: icons are not drawn");
            return a;
        }
        const auto sym = [lib]<class F>(F& slot, const char* name) {
            slot = reinterpret_cast<F>(::dlsym(lib, name));
            return slot != nullptr;
        };
        a.ok = sym(a.new_from_file, "rsvg_handle_new_from_file") &&
               sym(a.render_document, "rsvg_handle_render_document") &&
               sym(a.object_unref, "g_object_unref") && sym(a.error_free, "g_error_free") &&
               sym(a.surface_for_data, "cairo_image_surface_create_for_data") &&
               sym(a.create, "cairo_create") && sym(a.destroy, "cairo_destroy") &&
               sym(a.surface_flush, "cairo_surface_flush") &&
               sym(a.surface_destroy, "cairo_surface_destroy");
        if (!a.ok)
            lx::trace::logger::global().log(lx::trace::level::warn, "ui.icon",
                                            "librsvg lacks an entry point Lumen needs (2.52+): "
                                            "icons are not drawn");
        return a;
    }();
    return loaded;
}
#endif

} // namespace

bool lx::ui::icons_available() {
#if defined(LUMEN_HAS_RSVG)
    struct stat st{};
    const char* dir = icon_dir();
    return lx_ui_rsvg().ok && dir[0] && ::stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
#else
    return false;
#endif
}

lx::result<void> lx::ui::rasterize_icon(const char* path, unsigned size_px,
                                        std::span<std::uint8_t> coverage) {
    if (!path || size_px == 0 || size_px > icon_cache::k_max_px ||
        coverage.size() < std::size_t{size_px} * size_px)
        return lx::invalid_argument("rasterize_icon: bad size or buffer");
#if defined(LUMEN_HAS_RSVG)
    const lx_ui_rsvg_api& a = lx_ui_rsvg();
    if (!a.ok)
        return lx::make_error(lx::ui_err::icon_raster, "rasterize_icon: librsvg unavailable");
    GError* err = nullptr;
    RsvgHandle* h = a.new_from_file(path, &err);
    if (err)
        a.error_free(err);
    if (!h)
        return lx::make_error(lx::ui_err::icon_raster, "rasterize_icon: not an SVG librsvg reads");
    // 128² ARGB32 at most: 64 KiB on the stack, no heap of our own (librsvg's is its own).
    alignas(16) std::uint32_t argb[icon_cache::k_max_px * icon_cache::k_max_px];
    const int n = static_cast<int>(size_px);
    std::memset(argb, 0, std::size_t{size_px} * size_px * 4);
    cairo_surface_t* surf = a.surface_for_data(reinterpret_cast<unsigned char*>(argb),
                                               CAIRO_FORMAT_ARGB32, n, n, n * 4);
    cairo_t* cr = a.create(surf);
    const RsvgRectangle vp{0.0, 0.0, static_cast<double>(n), static_cast<double>(n)};
    err = nullptr;
    const bool ok = a.render_document(h, cr, &vp, &err) != 0;
    if (err)
        a.error_free(err);
    a.destroy(cr);
    a.surface_flush(surf);
    a.surface_destroy(surf);
    a.object_unref(h);
    if (!ok)
        return lx::make_error(lx::ui_err::icon_raster, "rasterize_icon: librsvg render failed");
    for (std::size_t i = 0; i < std::size_t{size_px} * size_px; ++i)
        coverage[i] = static_cast<std::uint8_t>(argb[i] >> 24u);
    return {};
#else
    (void)coverage;
    return lx::make_error(lx::ui_err::icon_raster, "rasterize_icon: built without librsvg");
#endif
}

