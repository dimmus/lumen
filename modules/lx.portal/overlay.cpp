module;

// Bodies of `:overlay`. See overlay.cppm for the model.

#include <stdint.h>

#include <errno.h>

#include <poll.h>
#include <unistd.h>

module lx.portal;

import std;

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.text;
import lx.trace;
import lx.shell;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
import :compose;
import :chooser;
import :desktop;
import :overlay;

namespace lx::portal {

namespace {

// evdev codes (linux/input-event-codes.h); the engine's key_event carries xkb's (evdev+8).
constexpr unsigned k_key_esc = 1, k_key_tab = 15, k_key_enter = 28, k_key_kpenter = 96;
constexpr unsigned k_key_home = 102, k_key_up = 103, k_key_pageup = 104, k_key_end = 107;
constexpr unsigned k_key_down = 108, k_key_pagedown = 109;
constexpr unsigned k_btn_left = 0x110, k_btn_right = 0x111;

constexpr std::uint32_t k_border = 0xFFFFFFFFu;
constexpr std::uint32_t k_box = 0xFF202024u;
constexpr std::uint32_t k_row_selected = 0xFF2F5C9Cu;
/// The dim over the backdrop: the capture's channels at 140/256 — a black quad at alpha
/// 115/255 multiplies by 0.549 (the canvas multiplied by 0.547; within one step of a step).
constexpr std::uint32_t k_dim_quad = 0x73000000u;

constexpr std::uint32_t k_text = 0xFFEEEEEEu;
constexpr std::uint32_t k_text_dim = 0xFFA0A0A6u;
constexpr lx::text::font_desc k_font{"Sans", 10.5f, lx::text::weight::normal, lx::text::style::normal};
constexpr lx::text::font_desc k_font_bold{"Sans", 11.f, lx::text::weight::bold, lx::text::style::normal};
constexpr lx::text::font_desc k_font_small{"Sans", 9.f, lx::text::weight::normal, lx::text::style::normal};

constexpr int k_list_width = 560;
constexpr int k_list_head = 44;
constexpr int k_list_row = 40;
constexpr int k_list_foot = 30;
constexpr int k_list_rows = 10;

constexpr lx::trace::category log_overlay{"portal.overlay"};

overlay* g_describing = nullptr;
overlay::view* g_describing_view = nullptr;
struct describing_guard {
    describing_guard(overlay* o, overlay::view* v)
        : previous_{g_describing}, previous_view_{g_describing_view} {
        g_describing = o;
        g_describing_view = v;
    }
    ~describing_guard() {
        g_describing = previous_;
        g_describing_view = previous_view_;
    }
    describing_guard(const describing_guard&) = delete;
    describing_guard& operator=(const describing_guard&) = delete;

private:
    overlay* previous_;
    overlay::view* previous_view_;
};

[[nodiscard]] lx::ui::style at(lx::rect2i r) {
    return lx::ui::style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

[[nodiscard]] lx::ui::quad_style fill_quad(std::uint32_t argb, float radius = 0.f) {
    lx::ui::quad_style q{};
    q.background = lx::color::rgb(static_cast<float>((argb >> 16) & 0xFF) / 255.f,
                                  static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                                  static_cast<float>(argb & 0xFF) / 255.f,
                                  static_cast<float>((argb >> 24) & 0xFF) / 255.f);
    if (radius > 0.f)
        for (float& r : q.corner_radii)
            r = radius;
    return q;
}

[[nodiscard]] lx::ui::child overlay_text(const char* text, std::uint32_t argb,
                                         const lx::text::font_desc& font, lx::ui::element_key key,
                                         lx::text::text_align align = lx::text::text_align::start) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = font;
    t.color = lx::color::rgb(static_cast<float>((argb >> 16) & 0xFF) / 255.f,
                             static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                             static_cast<float>(argb & 0xFF) / 255.f, 1.f);
    t.wrap = lx::text::text_wrap::none;
    t.align = align;
    t.context = lx::ui::active_ui_resources().text;
    return lx::ui::text_block(t, key);
}

[[nodiscard]] lx::ui::child overlay_box(lx::ui::style layout, lx::ui::quad_style quad,
                                        std::span<const lx::ui::child> children,
                                        lx::ui::element_key key) {
    lx::shell::shell_box_props p{};
    p.control.layout = layout;
    p.rest = quad;
    return lx::shell::shell_box(p, children, key);
}

} // namespace

// ── The describing function and the root node (friends of `overlay`) ─────────

lx::ui::child lx::portal::overlay_describe(lx::ui::build_context&) {
    if (!g_describing)
        return nullptr;
    return g_describing->build_ui(*g_describing_view);
}

/// The root: focusable (the exclusive keyboard's keys are its), interactive (the pointer's
/// fallback over what no row took), the crosshair over the backdrops.
struct overlay_root_props {
    lx::shell::shell_box_props box{};
    overlay* self = nullptr;
    overlay::view* view_ptr = nullptr;
};

class overlay_root_node final : public lx::shell::shell_box_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "overlay_root"; }

    bool apply(const overlay_root_props& p) {
        self_ = p.self;
        view_ = p.view_ptr;
        return lx::shell::shell_box_node::apply(p.box);
    }

    [[nodiscard]] lx::ui::cursor_style cursor() const override {
        return self_ && view_ && view_->output >= 0 ? lx::ui::cursor_style::crosshair
                                                   : lx::shell::shell_box_node::cursor();
    }

    bool on_focus_key(lx::ui::key_event event, lx::ui::focus_manager& focus) override {
        (void)focus;
        if (self_ && view_)
            self_->on_root_key(*view_, event);
        return true; // the overlay owns every key while it has the keyboard
    }

    bool on_pointer(const lx::ui::pointer_event& e) override {
        if (self_ && view_)
            self_->on_root_pointer(*view_, e);
        return lx::shell::shell_box_node::on_pointer(e);
    }

private:
    overlay* self_ = nullptr;
    overlay::view* view_ = nullptr;
};

// ── The engine's start and the views ─────────────────────────────────────────

overlay::overlay(desktop& d) : desktop_{d} {}

lx::result<void> overlay::start_ui() {
    if (ui_started_)
        return {};
    if (auto connected = client_.connect({}); !connected) {
        log_overlay.error(connected.get_error());
        return connected.get_error();
    }
    if (auto started = ui_.start(client_, ui_loop_, {}); !started) {
        log_overlay.error(started.get_error());
        return started.get_error();
    }
    // The outputs' names the views' slots need (and a first drain of the socket).
    if (auto rt = client_.roundtrip(); !rt) {
        log_overlay.error(rt.get_error());
        return rt.get_error();
    }
    ui_started_ = true;
    return {};
}

unsigned overlay::slot_of(const std::string& output_name) const {
    const unsigned n = client_.output_count();
    for (unsigned i = 0; i < n; ++i)
        if (const lx::wayland::output* o = client_.output_at(i);
            o && std::strcmp(o->info().name, output_name.c_str()) == 0)
            return i;
    return ~0u;
}

lx::result<void> overlay::add_view(int output_index) {
    auto v = std::make_unique<view>();
    v->owner = this;
    v->output = output_index;
    lx::ui::host::surface_options so{};
    so.describe = &overlay_describe;
    if (output_index >= 0) {
        // The output's size: the configure names it (anchored to all edges); the shot's
        // logical rect is the fallback.
        const lx::rect2i& lr = shots_[static_cast<std::size_t>(output_index)].logical;
        so.size = {lr.width, lr.height};
    } else {
        const int rows = std::min<int>(k_list_rows, static_cast<int>(entries_.size()));
        so.size = {k_list_width, k_list_head + rows * k_list_row + k_list_foot};
    }
    so.frame_records = false;
    if (auto made = v->surface.create(ui_, so); !made) {
        log_overlay.error(made.get_error());
        return made.get_error();
    }
    lx::ui::host::layer_role_options lo{};
    lo.layer.level = lx::wayland::layer::overlay;
    lo.layer.name_space = "lumen-portal";
    lo.keyboard = lx::wayland::keyboard_interactivity::exclusive;
    lo.size = so.size;
    if (output_index >= 0) {
        lo.layer.output_slot = slot_of(shots_[static_cast<std::size_t>(output_index)].name);
        lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_bottom |
                    lx::wayland::anchor_left | lx::wayland::anchor_right;
        lo.exclusive_zone = -1;
    }
    if (auto made = v->role.create(v->surface, lo); !made) {
        log_overlay.error(made.get_error());
        v->surface.destroy();
        return made.get_error();
    }
    v->role.set_closed_handler(
        [](void* user) {
            // An output went (or the compositor dropped the overlay): nothing sensible to
            // finish with.
            static_cast<view*>(user)->owner->finish(
                {.what = overlay_result::kind::failed,
                 .reason = "overlay closed by the compositor"});
        },
        v.get());
    // The identity lives on the (heap-stable) view; the surface's keyboard-focus handler
    // focuses the root when the exclusive keyboard arrives.
    v->surface.set_keyboard_focus_handler(
        [](bool entered, void* user) {
            view& v = *static_cast<view*>(user);
            if (entered)
                v.owner->on_key_focus(v);
        },
        v.get());
    // The pointer's raw positions (surface coordinates): the pick's swatch follows them,
    // the selection's drag measures from them; the tree's hover handles the rest.
    v->surface.set_pointer_observer(
        [](const lx::wayland::pointer_frame& f, void* user) {
            view& v = *static_cast<view*>(user);
            overlay& o = *v.owner;
            using pf = lx::wayland::pointer_frame;
            if (f.mask & (pf::has_enter | pf::has_motion)) {
                o.pointer_view_ = &v;
                if (v.output >= 0 && v.output < static_cast<int>(o.shots_.size())) {
                    const lx::rect2i& r = o.shots_[static_cast<std::size_t>(v.output)].logical;
                    o.gx_ = r.x + f.x;
                    o.gy_ = r.y + f.y;
                } else {
                    o.gx_ = f.x;
                    o.gy_ = f.y;
                }
                o.pointer_known_ = true;
                o.pointer_moved();
            } else if (f.mask & pf::has_leave && &v == o.pointer_view_ && !o.pressed_) {
                o.pointer_view_ = nullptr;
                o.pointer_known_ = false;
                o.mark_all_dirty();
            }
        },
        v.get());
    if (auto shown = v->surface.show(); !shown) {
        log_overlay.error(shown.get_error());
        v->role.destroy();
        v->surface.destroy();
        return shown.get_error();
    }
    views_.push_back(std::move(v));
    return {};
}

lx::result<void> overlay::start_select(std::span<const output_shot> shots) {
    shots_ = shots;
    mode_ = mode::select;
    if (auto r = start_ui(); !r.ok())
        return r;
    for (std::size_t i = 0; i < shots.size(); ++i)
        if (auto r = add_view(static_cast<int>(i)); !r.ok())
            return r;
    return {};
}

lx::result<void> overlay::start_pick(std::span<const output_shot> shots) {
    shots_ = shots;
    mode_ = mode::pick;
    if (auto r = start_ui(); !r.ok())
        return r;
    for (std::size_t i = 0; i < shots.size(); ++i)
        if (auto r = add_view(static_cast<int>(i)); !r.ok())
            return r;
    return {};
}

lx::result<void> overlay::start_choose(std::vector<chooser_entry> entries, std::string heading) {
    if (entries.empty())
        return lx::invalid_argument("nothing to choose from");
    entries_ = std::move(entries);
    heading_ = std::move(heading);
    mode_ = mode::choose;
    if (auto r = start_ui(); !r.ok())
        return r;
    if (auto r = add_view(-1); !r.ok())
        return r;
    return {};
}

void overlay::finish(overlay_result r) {
    if (done())
        return;
    result_ = r;
    log_overlay.debug("overlay result {}", static_cast<int>(r.what));
}

void overlay::mark_all_dirty() {
    for (auto& v : views_)
        v->dirty = true;
}

// ── Input (the tree forwards it; the same state machine as ever) ─────────────

void overlay::on_key_focus(view& v) {
    // The tree's keys are the focused node's: the root takes the keyboard (programmatic —
    // no ring), as the launcher's does.
    if (lx::ui::ui_node* tree = v.surface.valid() ? v.surface.root().tree() : nullptr)
        (void)v.surface.root().focus().focus(*tree, lx::ui::focus_cause::programmatic);
}

void overlay::on_root_pointer(view& v, const lx::ui::pointer_event& e) {
    pointer_view_ = &v;
    // Surface coordinates → layout coordinates.
    if (v.output >= 0 && v.output < static_cast<int>(shots_.size())) {
        const lx::rect2i& r = shots_[static_cast<std::size_t>(v.output)].logical;
        gx_ = r.x + e.at.x;
        gy_ = r.y + e.at.y;
    } else {
        gx_ = e.at.x;
        gy_ = e.at.y;
    }
    pointer_known_ = true;
    if (e.kind == lx::ui::pointer_kind::press || e.kind == lx::ui::pointer_kind::release) {
        button(e.button == lx::ui::mouse_button::left ? k_btn_left : k_btn_right,
               e.kind == lx::ui::pointer_kind::press);
        return;
    }
    if (e.kind == lx::ui::pointer_kind::drag) {
        pointer_moved();
        return;
    }
    if (e.kind == lx::ui::pointer_kind::wheel && mode_ == mode::choose && e.dy != 0.f)
        key(e.dy > 0.f ? k_key_down : k_key_up);
}

void overlay::on_root_key(view& v, const lx::ui::key_event& e) {
    (void)v;
    if (!e.pressed)
        return;
    key(e.keycode - 8); // xkb keycode → evdev
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
            const int x0 = static_cast<int>(std::lround(sel_x0_)),
                      y0 = static_cast<int>(std::lround(sel_y0_));
            const int x1 = static_cast<int>(std::lround(sel_x1_)),
                      y1 = static_cast<int>(std::lround(sel_y1_));
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

void overlay::on_row_hover(const row_ref& ref, bool hovered) {
    if (mode_ != mode::choose || !hovered || ref.entry < 0)
        return;
    if (ref.entry >= static_cast<int>(entries_.size()) || ref.entry == selected_)
        return;
    selected_ = ref.entry;
    mark_all_dirty();
}

void overlay::on_row_event(const row_ref& ref, const lx::ui::pointer_event& e) {
    if (mode_ != mode::choose || e.kind != lx::ui::pointer_kind::release)
        return;
    if (ref.entry < 0 || ref.entry >= static_cast<int>(entries_.size()))
        return;
    finish({.what = overlay_result::kind::choice, .choice = ref.entry});
}

// ── The tree ─────────────────────────────────────────────────────────────────

const char* overlay::hint_text() const {
    return mode_ == mode::select
               ? "Drag to select an area  \xC2\xB7  click a screen  \xC2\xB7  Enter: all "
                 "screens  \xC2\xB7  Esc: cancel"
               : "Click to pick a colour  \xC2\xB7  Esc: cancel";
}

lx::gfx::atlas_tile_id overlay::backdrop_tile(view& v) {
    const output_shot& shot = shots_[static_cast<std::size_t>(v.output)];
    const lx::size2i device = v.surface.valid() ? v.surface.buffer_size() : lx::size2i{};
    if (device.width <= 0 || device.height <= 0 || shot.pixels.empty())
        return {};
    lx::gfx::atlas_tile place{};
    if (v.tile.valid() && v.tile_w == device.width && v.tile_h == device.height &&
        v.surface.atlas().lookup(v.tile, place))
        return v.tile; // composed and encoded already, still in the atlas
    // The capture composed once, at the surface's pixel size (it shows pixel for pixel).
    v.composed = compose(shots_, shot.logical,
                         static_cast<double>(device.width) / static_cast<double>(v.surface.logical_size().width));
    if (v.composed.empty() || v.composed.width > 2048 || v.composed.height > 2048) {
        // The atlas caps its RGBA pages at 2048² (two of them): a bigger output, or more
        // than two captured at once, has no tile.
        log_overlay.info("DEBUG tile: composed {}x{} rejected", v.composed.width, v.composed.height);
        return {};
    }
    v.tile_w = device.width;
    v.tile_h = device.height;
    const std::size_t n = static_cast<std::size_t>(v.composed.width) *
                          static_cast<std::size_t>(v.composed.height);
    v.rgba.resize(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t px = v.composed.pixels[i];
        v.rgba[i * 4 + 0] = static_cast<std::uint8_t>((px >> 16) & 0xFF);
        v.rgba[i * 4 + 1] = static_cast<std::uint8_t>((px >> 8) & 0xFF);
        v.rgba[i * 4 + 2] = static_cast<std::uint8_t>(px & 0xFF);
        v.rgba[i * 4 + 3] = static_cast<std::uint8_t>((px >> 24) & 0xFF);
    }
    auto made = v.surface.atlas().insert(
        lx::gfx::atlas_format::rgba8, static_cast<unsigned>(v.composed.width),
        static_cast<unsigned>(v.composed.height), std::span<const std::uint8_t>{v.rgba},
        static_cast<unsigned>(v.composed.width * 4));
    if (!made) {
        log_overlay.info("DEBUG tile: atlas insert failed: {}", made.error().message);
        return {};
    }
    v.tile = made.value();
    return v.tile;
}

lx::rect2i overlay::hint_box(const view& v) const {
    lx::text::shaped_line line{};
    const int tw = lx::text::shape_line(hint_text(), k_font, line) ? line.width_px : 0;
    const int w = v.surface.valid() ? v.surface.logical_size().width : 0;
    return {(w - (tw + 32)) / 2, 48, tw + 32, 34};
}

bool overlay::swatch_box(const view& v, lx::rect2i& box) const {
    if (mode_ != mode::pick || !pointer_known_ || pointer_view_ != &v || v.output < 0)
        return false;
    // 132×30 logical, right of and below the pointer, flipped at the output's edges.
    const lx::rect2i& lr = shots_[static_cast<std::size_t>(v.output)].logical;
    const int sw = 132, sh = 30;
    const int w = v.surface.valid() ? v.surface.logical_size().width : 0;
    const int h = v.surface.valid() ? v.surface.logical_size().height : 0;
    double x = gx_ - lr.x + 18, y = gy_ - lr.y + 18;
    if (x + sw > w)
        x = gx_ - lr.x - 18 - sw;
    if (y + sh > h)
        y = gy_ - lr.y - 18 - sh;
    box = {static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), sw, sh};
    return true;
}

lx::ui::child overlay::build_ui(view& v) {
    const lx::size2i logical = v.surface.valid() ? v.surface.logical_size() : lx::size2i{};
    const lx::rect2i whole{0, 0, logical.width, logical.height};
    const lx::size2i device = v.surface.valid() ? v.surface.buffer_size() : lx::size2i{};
    lx::ui::child kids[10];
    unsigned n = 0;

    if (v.output >= 0) {
        // The frozen capture, dimmed; the selection area shows it undimmed, bordered.
        const lx::gfx::atlas_tile_id tile = backdrop_tile(v);
        if (!tile.valid()) {
            finish({.what = overlay_result::kind::failed, .reason = "overlay backdrop"});
            return nullptr;
        }
        kids[n++] = lx::ui::image(v.surface.atlas(), tile, device, 0xA001u);
        kids[n++] = overlay_box(at(whole), fill_quad(k_dim_quad), {}, 0xA002u);

        if (mode_ == mode::select && has_sel_) {
            const lx::rect2i& lr = shots_[static_cast<std::size_t>(v.output)].logical;
            const lx::rect2i sel{
                static_cast<int>(std::lround(sel_x0_)) - lr.x,
                static_cast<int>(std::lround(sel_y0_)) - lr.y,
                static_cast<int>(std::lround(sel_x1_ - sel_x0_)),
                static_cast<int>(std::lround(sel_y1_ - sel_y0_))};
            if (sel.width > 0 && sel.height > 0) {
                // The bright capture inside the selection: the same tile in a clipped box,
                // an inner box at the selection's negated origin carrying the image, so its
                // pixels line up 1:1 with the surface's.
                lx::ui::child img[1] = {lx::ui::image(v.surface.atlas(), tile, device, 0xA003u)};
                lx::shell::shell_box_props inner_box{};
                inner_box.control.layout = at({-sel.x, -sel.y, logical.width, logical.height});
                lx::ui::child inner[1] = {lx::shell::shell_box(inner_box, img, 0xA004u)};
                lx::shell::shell_box_props clip_box{};
                clip_box.control.layout = at(sel).overflow_hidden();
                kids[n++] = lx::shell::shell_box(clip_box, inner, 0xA005u);
                constexpr int b = 2;
                kids[n++] = overlay_box(at({sel.x, sel.y, sel.width, b}), fill_quad(k_border), {}, 0xA006u);
                kids[n++] = overlay_box(at({sel.x, sel.y + sel.height - b, sel.width, b}),
                                        fill_quad(k_border), {}, 0xA007u);
                kids[n++] = overlay_box(at({sel.x, sel.y, b, sel.height}), fill_quad(k_border), {}, 0xA008u);
                kids[n++] = overlay_box(at({sel.x + sel.width - b, sel.y, b, sel.height}),
                                        fill_quad(k_border), {}, 0xA009u);
            }
        }

        const lx::rect2i hint = hint_box(v);
        if (hint.width > 0) {
            lx::ui::child line[1] = {overlay_text(hint_text(), k_text, k_font, 0xA009u)};
            kids[n++] = overlay_box(at(hint).flex_row().items_center().justify_center(),
                                    fill_quad(k_box), line, 0xA00Au);
        }

        if (mode_ == mode::pick) {
            lx::rect2i box{};
            std::uint32_t c = 0;
            if (swatch_box(v, box) && colour_at(shots_, gx_, gy_, c)) {
                char hex[16];
                std::snprintf(hex, sizeof hex, "#%02X%02X%02X", (c >> 16) & 0xFFu,
                              (c >> 8) & 0xFFu, c & 0xFFu);
                lx::ui::child parts[2];
                unsigned p = 0;
                parts[p++] = overlay_box(at({4, 4, 22, 22}), fill_quad(0xFF000000u | (c & 0xFFFFFFu)),
                                         {}, 0xA00Bu);
                parts[p++] = overlay_text(hex, k_text, k_font, 0xA00Cu);
                kids[n++] = overlay_box(at(box).flex_row().items_center(), fill_quad(k_box),
                                        {parts, p}, 0xA00Du);
            }
        }
    } else {
        // The chooser.
        lx::ui::child head[1] = {overlay_text(heading_.c_str(), k_text, k_font_bold, 0xA010u)};
        kids[n++] = overlay_box(at({16, 0, logical.width - 32, k_list_head}), {}, head, 0xA011u);
        const int count = static_cast<int>(entries_.size());
        for (int i = top_; i < count && i < top_ + k_list_rows; ++i) {
            const chooser_entry& e = entries_[static_cast<std::size_t>(i)];
            const int y = k_list_head + (i - top_) * k_list_row;
            const bool sel = i == selected_;
            const char* tag = e.type == chooser_entry::kind::monitor  ? "Screen"
                              : e.type == chooser_entry::kind::window ? "Window"
                                                                      : "";
            v.rows[i - top_] = {this, i};
            lx::ui::child parts[3];
            unsigned p = 0;
            parts[p++] = overlay_text(tag, k_text_dim, k_font_small, 0xA012u + static_cast<unsigned>(i) * 4);
            parts[p++] = overlay_text(e.title.c_str(), k_text, k_font, 0xA013u + static_cast<unsigned>(i) * 4);
            if (!e.detail.empty())
                parts[p++] = overlay_text(e.detail.c_str(), k_text_dim, k_font_small,
                                          0xA014u + static_cast<unsigned>(i) * 4,
                                          lx::text::text_align::end);
            lx::shell::shell_box_props row{};
            row.control.layout = at({8, y + 2, logical.width - 16, k_list_row - 4})
                                     .flex_row()
                                     .items_center()
                                     .gap(lx::layout::px(8.f))
                                     .overflow_hidden();
            row.control.interactive = true;
            if (sel)
                row.rest = fill_quad(k_row_selected, 3.f);
            row.notify = [](void* user, bool hovered) {
                const row_ref& r = *static_cast<row_ref*>(user);
                r.self->on_row_hover(r, hovered);
            };
            row.event = [](void* user, const lx::ui::pointer_event& ev) {
                const row_ref& r = *static_cast<row_ref*>(user);
                r.self->on_row_event(r, ev);
            };
            row.user = &v.rows[i - top_];
            kids[n++] = lx::shell::shell_box(row, {parts, p}, 0xA015u + static_cast<unsigned>(i) * 4);
        }
        lx::ui::child foot[1] = {overlay_text(
            "\xE2\x86\x91\xE2\x86\x93 choose  \xC2\xB7  Enter: share  \xC2\xB7  Esc: cancel",
            k_text_dim, k_font_small, 0xA016u)};
        kids[n++] = overlay_box(
            at({16, logical.height - k_list_foot, logical.width - 32, k_list_foot - 6}), {},
            foot, 0xA017u);
    }

    // The root: focusable and interactive (the pointer's fallback), the crosshair over
    // the backdrops riding its cursor().
    overlay_root_props root{};
    root.box.control.layout = lx::ui::style{}.w(lx::layout::percent(100.f)).h(
        lx::layout::percent(100.f));
    root.box.control.focusable = true;
    root.box.control.interactive = true;
    root.self = this;
    root.view_ptr = &v;
    return lx::ui::with_children(
        lx::ui::describe<overlay_root_node, overlay_root_props>(root, 0xA000u),
        {kids, n});
}

// ── Driving ──────────────────────────────────────────────────────────────────

void overlay::update() {
    if (done() || !ui_started_)
        return;
    for (auto& vp : views_) {
        view& v = *vp;
        if (!v.surface.valid())
            continue;
        if (v.dirty) {
            v.surface.root().set_describer(&overlay_describe);
            v.surface.root().add_repaint(
                {0, 0, v.surface.logical_size().width, v.surface.logical_size().height});
            v.dirty = false;
        }
        if (v.surface.frame_due()) {
            describing_guard guard{this, &v};
            v.surface.run_frame();
        }
    }
}

bool overlay::pump(int timeout_ms) {
    // The chooser's drive: both connections in one poll. After every dispatch the views'
    // frames run and their commits flush BEFORE the sleep — a configure answered only by
    // the next loop round would deadlock it (nothing inbound wakes the poll: the
    // compositor waits for our ack, the ack sits in the queue).
    const int raw = desktop_.fd();
    const int ui = ui_fd();
    if (raw < 0)
        return false;
    pollfd fds[2]{{raw, POLLIN, 0}, {ui, POLLIN, 0}};
    const nfds_t n = ui >= 0 ? 2 : 1;
    if (!pump_ui())
        return false;
    update();
    (void)pump_ui();
    if (poll(fds, n, timeout_ms) < 0 && errno != EINTR)
        return false;
    if (n == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
        if (!pump_ui())
            return false;
        update();
        (void)pump_ui();
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
        return desktop_.dispatch_readable();
    return desktop_.prepare();
}

int overlay::ui_fd() const {
    return ui_started_ ? client_.fd() : -1;
}

bool overlay::pump_ui() {
    if (!ui_started_)
        return true;
    if (auto dispatched = client_.dispatch(0); !dispatched) {
        log_overlay.error(dispatched.get_error());
        finish({.what = overlay_result::kind::failed, .reason = "the ui connection broke"});
        return false;
    }
    return true;
}

overlay::~overlay() {
    views_.clear(); // the surfaces and roles go before their connection
    if (ui_started_)
        ui_.stop();
}

} // namespace lx::portal
