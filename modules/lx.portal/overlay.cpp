module;

// Bodies of `:overlay`. See overlay.cppm for the model.

#include <stdint.h>

#include <unistd.h>

#if defined(LUMEN_PORTAL_WAYLAND)
#include <wayland-client.h>
#include "cursor-shape-v1-client-protocol.h"
// The header names a parameter `namespace` (C); renamed for C++, as lx.shell does.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

module lx.portal;

import std;

import lx.foundation;
import lx.shell;
import lx.text;
import lx.trace;
import :compose;
import :chooser;
import :desktop;
import :overlay;

namespace lx::portal {

namespace {

// evdev codes (linux/input-event-codes.h); wl_keyboard.key carries them as they are.
constexpr unsigned k_key_esc = 1, k_key_tab = 15, k_key_enter = 28, k_key_kpenter = 96;
constexpr unsigned k_key_home = 102, k_key_up = 103, k_key_pageup = 104, k_key_end = 107;
constexpr unsigned k_key_down = 108, k_key_pagedown = 109;
constexpr unsigned k_btn_left = 0x110, k_btn_right = 0x111;

constexpr std::uint32_t k_border = 0xFFFFFFFFu;
constexpr std::uint32_t k_box = 0xFF202024u;
constexpr std::uint32_t k_row_selected = 0xFF2F5C9Cu;
constexpr unsigned k_dim = 140; // backdrop brightness, /256

constexpr lx::text::raster_color k_text{0xEE, 0xEE, 0xEE, 0xFF};
constexpr lx::text::raster_color k_text_dim{0xA0, 0xA0, 0xA6, 0xFF};
constexpr lx::text::font_desc k_font{"Sans", 10.5f, lx::text::weight::normal, lx::text::style::normal};
constexpr lx::text::font_desc k_font_bold{"Sans", 11.f, lx::text::weight::bold, lx::text::style::normal};
constexpr lx::text::font_desc k_font_small{"Sans", 9.f, lx::text::weight::normal, lx::text::style::normal};

constexpr int k_list_width = 560;
constexpr int k_list_head = 44;
constexpr int k_list_row = 40;
constexpr int k_list_foot = 30;
constexpr int k_list_rows = 10;

constexpr const char* k_category = "portal.overlay";

template <typename... Args>
void log_line(lx::trace::level lvl, std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lvl, k_category, fmt, std::forward<Args>(args)...);
}

lx::rect2i unite(lx::rect2i a, lx::rect2i b) {
    if (a.width <= 0 || a.height <= 0)
        return b;
    if (b.width <= 0 || b.height <= 0)
        return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.width, b.x + b.width), y1 = std::max(a.y + a.height, b.y + b.height);
    return {x0, y0, x1 - x0, y1 - y0};
}

lx::rect2i clip(lx::rect2i r, int w, int h) {
    const int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0);
    const int x1 = std::min(r.x + r.width, w), y1 = std::min(r.y + r.height, h);
    if (x1 <= x0 || y1 <= y0)
        return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

bool overlaps(lx::rect2i a, lx::rect2i b) {
    return a.width > 0 && b.width > 0 && a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

} // namespace

struct overlay::view {
    overlay* owner = nullptr;
    int output = -1; // index into the shots; -1 for the chooser
    void* surface = nullptr; // wl_surface*
    void* layer = nullptr;   // zwlr_layer_surface_v1*
    lx::shell::shm_canvas canvas;
    int width = 0, height = 0; // logical, as configured
    bool configured = false;
    bool dirty = true;
    // Backdrop at the canvas's pixel size (select, pick).
    image bright, dimmed;
    // What each buffer shows of the dynamic part (selection, swatch), pixels; whether the
    // buffer holds a full paint at all; which buffer was committed last.
    bool valid[2]{};
    lx::rect2i drawn[2]{};
    int last = -1;
    int pw = 0, ph = 0;
    lx::rect2i hint_px{};
};

#if defined(LUMEN_PORTAL_WAYLAND)

struct overlay::events {
    // ── layer surface ──
    static void configure(void* data, zwlr_layer_surface_v1* ls, uint32_t serial, uint32_t w, uint32_t h) {
        auto& v = *static_cast<view*>(data);
        zwlr_layer_surface_v1_ack_configure(ls, serial);
        const int nw = static_cast<int>(w), nh = static_cast<int>(h);
        if (nw <= 0 || nh <= 0)
            return;
        if (!v.configured || nw != v.width || nh != v.height) {
            v.width = nw;
            v.height = nh;
            if (!v.canvas.allocate(v.owner->desktop_.shm(), nw, nh, "lumen-portal-overlay")) {
                v.owner->finish({.what = overlay_result::kind::failed, .reason = "overlay buffers"});
                return;
            }
            v.valid[0] = v.valid[1] = false;
            v.last = -1;
        }
        v.configured = true;
        v.dirty = true;
    }
    static void layer_closed(void* data, zwlr_layer_surface_v1*) {
        auto& v = *static_cast<view*>(data);
        // The output went (or the compositor dropped the overlay): nothing sensible to
        // finish with.
        v.owner->finish({.what = overlay_result::kind::failed, .reason = "overlay closed by the compositor"});
    }

    // ── pointer ──
    static view* find(overlay& o, wl_surface* s) {
        for (auto& v : o.views_)
            if (v->surface == s)
                return v.get();
        return nullptr;
    }
    static void enter(void* data, wl_pointer*, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
        auto& o = *static_cast<overlay*>(data);
        o.pointer_view_ = find(o, s);
        if (o.cursor_device_)
            wp_cursor_shape_device_v1_set_shape(static_cast<wp_cursor_shape_device_v1*>(o.cursor_device_), serial,
                                                o.mode_ == mode::choose
                                                    ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
                                                    : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
        motion(data, nullptr, 0, x, y);
    }
    static void leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
        auto& o = *static_cast<overlay*>(data);
        if (o.pressed_)
            return; // the implicit grab keeps reporting to the pressed surface
        o.pointer_view_ = nullptr;
        o.pointer_known_ = false;
        o.mark_all_dirty();
    }
    static void motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto& o = *static_cast<overlay*>(data);
        view* v = o.pointer_view_;
        if (!v)
            return;
        const double lx_ = wl_fixed_to_double(x), ly = wl_fixed_to_double(y);
        if (v->output >= 0 && v->output < static_cast<int>(o.shots_.size())) {
            const lx::rect2i& r = o.shots_[static_cast<std::size_t>(v->output)].logical;
            o.gx_ = r.x + lx_;
            o.gy_ = r.y + ly;
        } else {
            o.gx_ = lx_;
            o.gy_ = ly;
        }
        o.pointer_known_ = true;
        o.pointer_moved();
    }
    static void pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t b, uint32_t state) {
        static_cast<overlay*>(data)->button(b, state == WL_POINTER_BUTTON_STATE_PRESSED);
    }
    static void axis(void* data, wl_pointer*, uint32_t, uint32_t a, wl_fixed_t value) {
        auto& o = *static_cast<overlay*>(data);
        if (o.mode_ != mode::choose || a != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        o.key(wl_fixed_to_double(value) > 0 ? k_key_down : k_key_up);
    }
    static void frame(void*, wl_pointer*) {}
    static void axis_source(void*, wl_pointer*, uint32_t) {}
    static void axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

    // ── keyboard ──
    static void keymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
    static void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
    static void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
    static void kb_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t k, uint32_t state) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            static_cast<overlay*>(data)->key(k);
    }
    static void modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
    static void repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

    static constexpr zwlr_layer_surface_v1_listener k_layer_listener = {.configure = configure,
                                                                         .closed = layer_closed};
    static constexpr wl_pointer_listener k_pointer_listener = {.enter = enter,
                                                               .leave = leave,
                                                               .motion = motion,
                                                               .button = pointer_button,
                                                               .axis = axis,
                                                               .frame = frame,
                                                               .axis_source = axis_source,
                                                               .axis_stop = axis_stop,
                                                               .axis_discrete = axis_discrete,
                                                               .axis_value120 = nullptr,
                                                               .axis_relative_direction = nullptr};
    static constexpr wl_keyboard_listener k_keyboard_listener = {.keymap = keymap,
                                                                 .enter = kb_enter,
                                                                 .leave = kb_leave,
                                                                 .key = kb_key,
                                                                 .modifiers = modifiers,
                                                                 .repeat_info = repeat_info};
};

overlay::overlay(desktop& d) : desktop_{d} {}

overlay::~overlay() {
    for (auto& v : views_) {
        v->canvas.free();
        v->canvas.release_scale();
        if (v->layer)
            zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(v->layer));
        if (v->surface)
            wl_surface_destroy(static_cast<wl_surface*>(v->surface));
    }
    views_.clear();
    if (cursor_device_)
        wp_cursor_shape_device_v1_destroy(static_cast<wp_cursor_shape_device_v1*>(cursor_device_));
    if (auto* p = static_cast<wl_pointer*>(pointer_)) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(p)) >= 3)
            wl_pointer_release(p);
        else
            wl_pointer_destroy(p);
    }
    if (auto* k = static_cast<wl_keyboard*>(keyboard_)) {
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(k)) >= 3)
            wl_keyboard_release(k);
        else
            wl_keyboard_destroy(k);
    }
    (void)desktop_.prepare();
}

lx::result<void> overlay::setup_input() {
    if (!desktop_.compositor() || !desktop_.shm() || !desktop_.layer_shell() || !desktop_.seat())
        return lx::make_error(
            lx::wayland_err::bind_failed,
            "the overlay needs wl_compositor, wl_shm, wl_seat and wlr-layer-shell");
    auto* seat = static_cast<wl_seat*>(desktop_.seat());
    const unsigned caps = desktop_.seat_caps();
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        auto* p = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(p, &events::k_pointer_listener, this);
        pointer_ = p;
        if (desktop_.cursor_shape())
            cursor_device_ = wp_cursor_shape_manager_v1_get_pointer(
                static_cast<wp_cursor_shape_manager_v1*>(desktop_.cursor_shape()), p);
    }
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
        auto* k = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(k, &events::k_keyboard_listener, this);
        keyboard_ = k;
    }
    return {};
}

lx::result<void> overlay::add_view(int output_index) {
    auto v = std::make_unique<view>();
    v->owner = this;
    v->output = output_index;
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(desktop_.compositor()));
    v->surface = surface;
    wl_output* out = nullptr;
    if (output_index >= 0) {
        const std::string& name = shots_[static_cast<std::size_t>(output_index)].name;
        for (const auto& o : desktop_.outputs())
            if (!o->gone && o->name == name)
                out = static_cast<wl_output*>(o->wl_output);
        if (!out)
            return lx::make_error(lx::wayland_err::bind_failed, "an output of the capture is gone");
    }
    // Fractional scale before the first buffer (the canvas sizes its buffers by it).
    v->canvas.attach_scale(desktop_.viewporter(), desktop_.fractional(), surface);
    auto* ls = zwlr_layer_shell_v1_get_layer_surface(static_cast<zwlr_layer_shell_v1*>(desktop_.layer_shell()),
                                                     surface, out, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                                     "lumen-portal");
    v->layer = ls;
    zwlr_layer_surface_v1_add_listener(ls, &events::k_layer_listener, v.get());
    if (output_index >= 0) {
        zwlr_layer_surface_v1_set_anchor(ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(ls, -1);
    } else {
        const int rows = std::min<int>(k_list_rows, static_cast<int>(entries_.size()));
        zwlr_layer_surface_v1_set_size(ls, k_list_width, k_list_head + rows * k_list_row + k_list_foot);
    }
    zwlr_layer_surface_v1_set_keyboard_interactivity(ls, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(surface);
    views_.push_back(std::move(v));
    return {};
}

lx::result<void> overlay::start_select(std::span<const output_shot> shots) {
    shots_ = shots;
    mode_ = mode::select;
    if (auto r = setup_input(); !r.ok())
        return r;
    for (std::size_t i = 0; i < shots.size(); ++i)
        if (auto r = add_view(static_cast<int>(i)); !r.ok())
            return r;
    (void)desktop_.prepare();
    return {};
}

lx::result<void> overlay::start_pick(std::span<const output_shot> shots) {
    shots_ = shots;
    mode_ = mode::pick;
    if (auto r = setup_input(); !r.ok())
        return r;
    for (std::size_t i = 0; i < shots.size(); ++i)
        if (auto r = add_view(static_cast<int>(i)); !r.ok())
            return r;
    (void)desktop_.prepare();
    return {};
}

lx::result<void> overlay::start_choose(std::vector<chooser_entry> entries, std::string heading) {
    if (entries.empty())
        return lx::invalid_argument("nothing to choose from");
    entries_ = std::move(entries);
    heading_ = std::move(heading);
    mode_ = mode::choose;
    if (auto r = setup_input(); !r.ok())
        return r;
    if (auto r = add_view(-1); !r.ok())
        return r;
    (void)desktop_.prepare();
    return {};
}

void overlay::finish(overlay_result r) {
    if (done())
        return;
    result_ = r;
    log_line(lx::trace::level::debug, "overlay result {}", static_cast<int>(r.what));
}

void overlay::mark_all_dirty() {
    for (auto& v : views_)
        v->dirty = true;
}

void overlay::pointer_moved() {
    if (mode_ == mode::select) {
        if (pressed_) {
            if (!dragging_ && (std::fabs(gx_ - press_x_) >= 3.0 || std::fabs(gy_ - press_y_) >= 3.0))
                dragging_ = true;
            if (dragging_) {
                sel_x0_ = std::min(press_x_, gx_);
                sel_y0_ = std::min(press_y_, gy_);
                sel_x1_ = std::max(press_x_, gx_);
                sel_y1_ = std::max(press_y_, gy_);
                has_sel_ = true;
                mark_all_dirty();
            }
        }
    } else if (mode_ == mode::pick) {
        mark_all_dirty();
    } else if (mode_ == mode::choose && pointer_view_) {
        const int row = (static_cast<int>(gy_) - k_list_head);
        if (row >= 0) {
            const int i = top_ + row / k_list_row;
            if (i >= top_ && i < static_cast<int>(entries_.size()) && i < top_ + k_list_rows && i != selected_) {
                selected_ = i;
                mark_all_dirty();
            }
        }
    }
}

void overlay::button(unsigned code, bool pressed) {
    if (done())
        return;
    if (code == k_btn_right && pressed && mode_ != mode::choose) {
        finish({.what = overlay_result::kind::cancelled});
        return;
    }
    if (code != k_btn_left)
        return;
    if (mode_ == mode::select) {
        if (pressed) {
            pressed_ = true;
            dragging_ = false;
            press_x_ = gx_;
            press_y_ = gy_;
            return;
        }
        if (!pressed_)
            return;
        pressed_ = false;
        if (dragging_) {
            // Corners rounded to the nearest layout pixel: the area is the distance dragged.
            const int x0 = static_cast<int>(std::lround(sel_x0_)), y0 = static_cast<int>(std::lround(sel_y0_));
            const int x1 = static_cast<int>(std::lround(sel_x1_)), y1 = static_cast<int>(std::lround(sel_y1_));
            if (x1 > x0 && y1 > y0) {
                finish({.what = overlay_result::kind::area, .area = {x0, y0, x1 - x0, y1 - y0}});
                return;
            }
        }
        const int o = shot_at(shots_, gx_, gy_);
        if (o >= 0)
            finish({.what = overlay_result::kind::output, .output = o});
        return;
    }
    if (mode_ == mode::pick) {
        if (!pressed)
            return;
        std::uint32_t c = 0;
        if (colour_at(shots_, gx_, gy_, c))
            finish({.what = overlay_result::kind::colour, .colour = c});
        return;
    }
    if (mode_ == mode::choose && !pressed && pointer_view_) {
        const int row = static_cast<int>(gy_) - k_list_head;
        if (row >= 0) {
            const int i = top_ + row / k_list_row;
            if (i < static_cast<int>(entries_.size()) && i < top_ + k_list_rows)
                finish({.what = overlay_result::kind::choice, .choice = i});
        }
    }
}

void overlay::key(unsigned code) {
    if (done())
        return;
    if (code == k_key_esc) {
        finish({.what = overlay_result::kind::cancelled});
        return;
    }
    if (mode_ == mode::select && (code == k_key_enter || code == k_key_kpenter)) {
        finish({.what = overlay_result::kind::all});
        return;
    }
    if (mode_ != mode::choose)
        return;
    const int n = static_cast<int>(entries_.size());
    int s = selected_;
    switch (code) {
    case k_key_enter:
    case k_key_kpenter: finish({.what = overlay_result::kind::choice, .choice = selected_}); return;
    case k_key_up: s = s > 0 ? s - 1 : 0; break;
    case k_key_down:
    case k_key_tab: s = s + 1 < n ? s + 1 : n - 1; break;
    case k_key_home: s = 0; break;
    case k_key_end: s = n - 1; break;
    case k_key_pageup: s = std::max(0, s - k_list_rows); break;
    case k_key_pagedown: s = std::min(n - 1, s + k_list_rows); break;
    default: return;
    }
    if (s != selected_) {
        selected_ = s;
        if (selected_ < top_)
            top_ = selected_;
        if (selected_ >= top_ + k_list_rows)
            top_ = selected_ - k_list_rows + 1;
        mark_all_dirty();
    }
}

lx::rect2i overlay::dynamic_rect(const view& v) const {
    if (v.output < 0 || v.width <= 0)
        return {};
    const lx::rect2i& lr = shots_[static_cast<std::size_t>(v.output)].logical;
    const double s = static_cast<double>(v.pw) / v.width;
    if (mode_ == mode::select && has_sel_) {
        const int b = std::max(1, static_cast<int>(std::lround(s)));
        const int x0 = static_cast<int>(std::lround((sel_x0_ - lr.x) * s)) - b;
        const int y0 = static_cast<int>(std::lround((sel_y0_ - lr.y) * s)) - b;
        const int x1 = static_cast<int>(std::lround((sel_x1_ - lr.x) * s)) + b;
        const int y1 = static_cast<int>(std::lround((sel_y1_ - lr.y) * s)) + b;
        return clip({x0, y0, x1 - x0, y1 - y0}, v.pw, v.ph);
    }
    lx::rect2i box{};
    if (swatch_box(v, box))
        return clip(v.canvas.pixel_rect(box), v.pw, v.ph);
    return {};
}

bool overlay::swatch_box(const view& v, lx::rect2i& box) const {
    if (mode_ != mode::pick || !pointer_known_ || pointer_view_ != &v || v.output < 0)
        return false;
    // 132×30 logical, right of and below the pointer, flipped at the output's edges.
    const lx::rect2i& lr = shots_[static_cast<std::size_t>(v.output)].logical;
    const int sw = 132, sh = 30;
    double x = gx_ - lr.x + 18, y = gy_ - lr.y + 18;
    if (x + sw > v.width)
        x = gx_ - lr.x - 18 - sw;
    if (y + sh > v.height)
        y = gy_ - lr.y - 18 - sh;
    box = {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), sw, sh};
    return true;
}

void overlay::paint_backdrop_view(view& v) {
    const output_shot& shot = shots_[static_cast<std::size_t>(v.output)];
    const lx::rect2i full = v.canvas.pixel_rect({0, 0, v.width, v.height});
    if (v.bright.width != full.width || v.bright.height != full.height) {
        v.pw = full.width;
        v.ph = full.height;
        // The backdrop at exactly the surface's pixel size: one compose per output.
        v.bright = compose(shots_, shot.logical, static_cast<double>(v.pw) / v.width);
        v.dimmed = v.bright;
        dim(v.dimmed, k_dim);
        v.valid[0] = v.valid[1] = false;
    }
    if (v.bright.width != v.pw || v.bright.height != v.ph)
        return;
    const int b = v.canvas.acquire();
    if (b < 0)
        return;
    const char* hint = mode_ == mode::select
                           ? "Drag to select an area  \xC2\xB7  click a screen  \xC2\xB7  Enter: all screens  \xC2\xB7  Esc: cancel"
                           : "Click to pick a colour  \xC2\xB7  Esc: cancel";
    const int tw = std::max(0, v.canvas.text_width(hint, k_font));
    const lx::rect2i hint_box{(v.width - (tw + 32)) / 2, 48, tw + 32, 34};
    v.hint_px = clip(v.canvas.pixel_rect(hint_box), v.pw, v.ph);
    const lx::rect2i dyn = dynamic_rect(v);
    lx::rect2i region;
    if (!v.valid[b]) {
        region = {0, 0, v.pw, v.ph};
    } else {
        region = unite(v.drawn[b], dyn);
        if (overlaps(region, v.hint_px))
            region = unite(region, v.hint_px);
    }
    region = clip(region, v.pw, v.ph);
    if (region.width <= 0 && b == v.last) {
        v.dirty = false;
        return;
    }
    std::uint32_t* px = v.canvas.pixels(b);
    if (!px)
        return;
    const auto copy_rows = [&](const image& from, lx::rect2i r) {
        r = clip(r, v.pw, v.ph);
        for (int y = r.y; y < r.y + r.height; ++y)
            std::memcpy(px + static_cast<std::size_t>(y) * v.pw + r.x,
                        from.pixels.data() + static_cast<std::size_t>(y) * v.pw + r.x,
                        static_cast<std::size_t>(r.width) * 4u);
    };
    const auto fill_px = [&](lx::rect2i r, std::uint32_t c) {
        r = clip(r, v.pw, v.ph);
        for (int y = r.y; y < r.y + r.height; ++y)
            std::fill_n(px + static_cast<std::size_t>(y) * v.pw + r.x, r.width, c);
    };
    copy_rows(v.dimmed, region);
    if (mode_ == mode::select && dyn.width > 0) {
        const double s = static_cast<double>(v.pw) / v.width;
        const int bw = std::max(1, static_cast<int>(std::lround(s)));
        const lx::rect2i inner{dyn.x + bw, dyn.y + bw, dyn.width - 2 * bw, dyn.height - 2 * bw};
        copy_rows(v.bright, inner);
        fill_px({dyn.x, dyn.y, dyn.width, bw}, k_border);
        fill_px({dyn.x, dyn.y + dyn.height - bw, dyn.width, bw}, k_border);
        fill_px({dyn.x, dyn.y, bw, dyn.height}, k_border);
        fill_px({dyn.x + dyn.width - bw, dyn.y, bw, dyn.height}, k_border);
    }
    if (overlaps(region, v.hint_px)) {
        v.canvas.fill(b, hint_box, k_box);
        (void)v.canvas.text(b, {hint_box.x + 16, hint_box.y, hint_box.width - 32, hint_box.height}, hint, k_text,
                            k_font);
    }
    if (mode_ == mode::pick && dyn.width > 0) {
        std::uint32_t c = 0;
        lx::rect2i box{};
        if (swatch_box(v, box) && colour_at(shots_, gx_, gy_, c)) {
            fill_px(dyn, k_box);
            v.canvas.fill(b, {box.x + 4, box.y + 4, 22, 22}, c);
            char hex[16];
            std::snprintf(hex, sizeof hex, "#%02X%02X%02X", (c >> 16) & 0xFFu, (c >> 8) & 0xFFu, c & 0xFFu);
            (void)v.canvas.text(b, {box.x + 34, box.y, 94, 30}, hex, k_text, k_font);
        }
    }
    const double s = static_cast<double>(v.pw) / v.width;
    const lx::rect2i damage_logical{static_cast<int>(std::floor(region.x / s)), static_cast<int>(std::floor(region.y / s)),
                                    static_cast<int>(std::ceil(region.width / s)) + 1,
                                    static_cast<int>(std::ceil(region.height / s)) + 1};
    v.canvas.commit(v.surface, b, &damage_logical, 1);
    v.valid[b] = true;
    v.drawn[b] = dyn;
    v.last = b;
    v.dirty = false;
}

void overlay::paint_chooser_view(view& v) {
    const int b = v.canvas.acquire();
    if (b < 0)
        return;
    const lx::rect2i all{0, 0, v.width, v.height};
    v.canvas.fill(b, all, lx::shell::k_canvas_bg);
    (void)v.canvas.text(b, {16, 0, v.width - 32, k_list_head}, heading_.c_str(), k_text, k_font_bold);
    const int n = static_cast<int>(entries_.size());
    for (int i = top_; i < n && i < top_ + k_list_rows; ++i) {
        const chooser_entry& e = entries_[static_cast<std::size_t>(i)];
        const int y = k_list_head + (i - top_) * k_list_row;
        if (i == selected_)
            v.canvas.fill(b, {8, y + 2, v.width - 16, k_list_row - 4}, k_row_selected);
        const char* tag = e.type == chooser_entry::kind::monitor  ? "Screen"
                          : e.type == chooser_entry::kind::window ? "Window"
                                                                  : "";
        (void)v.canvas.text(b, {18, y, 64, k_list_row}, tag, k_text_dim, k_font_small);
        const int title_w = e.detail.empty() ? v.width - 110 : (v.width - 110) * 3 / 5;
        (void)v.canvas.text(b, {86, y, title_w, k_list_row}, e.title.c_str(), k_text, k_font);
        if (!e.detail.empty())
            (void)v.canvas.text(b, {86 + title_w + 8, y, v.width - 110 - title_w - 8, k_list_row}, e.detail.c_str(),
                                k_text_dim, k_font_small, lx::shell::shm_canvas::align::right);
    }
    const int foot_y = v.height - k_list_foot;
    (void)v.canvas.text(b, {16, foot_y, v.width - 32, k_list_foot - 6},
                        "\xE2\x86\x91\xE2\x86\x93 choose  \xC2\xB7  Enter: share  \xC2\xB7  Esc: cancel", k_text_dim,
                        k_font_small);
    v.canvas.commit(v.surface, b, &all, 1);
    v.last = b;
    v.dirty = false;
}

void overlay::update() {
    if (done())
        return;
    for (auto& vp : views_) {
        view& v = *vp;
        if (!v.configured)
            continue;
        if (v.canvas.take_rescale()) {
            if (!v.canvas.allocate(desktop_.shm(), v.width, v.height, "lumen-portal-overlay")) {
                finish({.what = overlay_result::kind::failed, .reason = "overlay buffers"});
                return;
            }
            v.valid[0] = v.valid[1] = false;
            v.last = -1;
            v.dirty = true;
        }
        if (!v.dirty || v.canvas.frame_pending())
            continue;
        if (v.output >= 0)
            paint_backdrop_view(v);
        else
            paint_chooser_view(v);
    }
    (void)desktop_.prepare();
}

#else // !LUMEN_PORTAL_WAYLAND

struct overlay::events {};
overlay::overlay(desktop& d) : desktop_{d} {}
overlay::~overlay() = default;
lx::result<void> overlay::setup_input() { return lx::not_implemented("lx::portal::overlay"); }
lx::result<void> overlay::add_view(int) { return lx::not_implemented("lx::portal::overlay"); }
lx::result<void> overlay::start_select(std::span<const output_shot>) {
    return lx::not_implemented("lx::portal::overlay::start_select");
}
lx::result<void> overlay::start_pick(std::span<const output_shot>) {
    return lx::not_implemented("lx::portal::overlay::start_pick");
}
lx::result<void> overlay::start_choose(std::vector<chooser_entry>, std::string) {
    return lx::not_implemented("lx::portal::overlay::start_choose");
}
void overlay::finish(overlay_result r) { result_ = r; }
void overlay::paint_backdrop_view(view&) {}
void overlay::paint_chooser_view(view&) {}
lx::rect2i overlay::dynamic_rect(const view&) const { return {}; }
void overlay::pointer_moved() {}
void overlay::button(unsigned, bool) {}
void overlay::key(unsigned) {}
void overlay::mark_all_dirty() {}
bool overlay::swatch_box(const view&, lx::rect2i&) const { return false; }
void overlay::update() {}

#endif

} // namespace lx::portal
