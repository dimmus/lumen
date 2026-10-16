module;

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(LUMEN_SHELL_WAYLAND)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "cursor-shape-v1-client-protocol.h"
// wayland-scanner names an argument `namespace`, a C++ keyword (Wayfire renames it the same
// way).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

import lx.foundation;
import lx.text;
import lx.trace;

export module lx.shell:panel;

import :task_model;
import :bridge;

export namespace lx::shell {

// ── Panel layout (pure) ───────────────────────────────────────────────────────
//
// One row of task entries, left to right in task order. Kept separate from drawing so
// both repaint and damage are a comparison of two layouts: what a buffer holds (or what
// was last committed) against what should be shown.

inline constexpr int k_panel_height = 32;
inline constexpr int k_panel_pad = 4;
inline constexpr int k_panel_gap = 4;
inline constexpr int k_entry_height = 24;
inline constexpr int k_entry_min_width = 48;
inline constexpr int k_entry_max_width = 220;
/// Room for the "+k" label of entries that do not fit.
inline constexpr int k_panel_more_width = 44;
/// The logout button at the right end.
inline constexpr int k_logout_width = 84;
inline constexpr unsigned k_max_panel_cells = 128;

enum panel_cell_flag : unsigned {
    cell_focused = 1u << 0,
    cell_hover = 1u << 1,
    cell_pressed = 1u << 2,
    /// The window asked for attention (an activation that was not granted focus).
    cell_attention = 1u << 3,
    /// Its client does not answer pings: the label is drawn dimmed.
    cell_not_responding = 1u << 4,
};

struct panel_cell {
    task_handle task{};
    unsigned label_rev = 0;
    int x = 0;
    int w = 0;
    unsigned flags = 0;
};

/// The logout button's input to a layout. Two clicks log out: the first arms it ("Confirm?"),
/// the second sends the request; moving off the button disarms it. No timer, so nothing
/// runs while the panel is idle, and a stray click cannot end the session.
struct panel_logout {
    bool shown = false; // the compositor supports zlm_shell_v1.request_logout
    bool armed = false;
    bool pressed = false;
};

enum panel_logout_flag : unsigned {
    logout_hover = 1u << 0,
    logout_pressed = 1u << 1,
    logout_armed = 1u << 2,
};

struct panel_layout {
    /// 0 = nothing (an unpainted buffer, nothing committed yet).
    int width = 0;
    int height = 0;
    int cell_y = 0;
    int cell_h = 0;
    unsigned count = 0;
    panel_cell cells[k_max_panel_cells]{};
    /// Entries that did not fit, shown as "+hidden" at more_x.
    unsigned hidden = 0;
    int more_x = 0;
    /// The logout button; logout_w 0 = not shown.
    int logout_x = 0;
    int logout_w = 0;
    unsigned logout_flags = 0;
};

/// Lays out `model`'s entries on a `width`×`height` panel. The pointer (surface
/// coordinates; negative = not on the panel) marks the entry under it as hovered;
/// `pressed` marks the entry a button went down on.
void layout_panel(const task_model& model, int width, int height, double pointer_x,
                  double pointer_y, task_handle pressed, panel_layout& out,
                  panel_logout logout = {});
/// Copies the used part of `from` (the cells in use, not all k_max_panel_cells).
///
/// Also a workaround: Clang 23.1 finds no viable copy assignment for this class here and
/// crashes in codegen when one is explicitly defaulted, so layouts are copied field-wise.
void copy_layout(panel_layout& to, const panel_layout& from);
/// Index of the cell under (x, y), or -1.
[[nodiscard]] int panel_hit(const panel_layout& layout, double x, double y);
/// Whether (x, y) is on the logout button.
[[nodiscard]] bool panel_hit_logout(const panel_layout& layout, double x, double y);
/// Rects of `to` that differ from `from`: the whole panel when the arrangement changed
/// (size, entry count, any position or width, the "+k" label), else one rect per entry
/// whose task, label or state changed. Returns how many were written (≤ cap; a full-panel
/// rect when they would not fit).
unsigned panel_diff(const panel_layout& from, const panel_layout& to, lx::rect2i* out,
                    unsigned cap);

// ── The panel ─────────────────────────────────────────────────────────────────

/// Globals the panel draws and takes input through (all `void*`: no Wayland type is
/// exported). `cursor_shape_manager` may be null.
struct panel_globals {
    void* compositor = nullptr;           // wl_compositor (v4+: damage_buffer)
    void* shm = nullptr;                  // wl_shm
    void* layer_shell = nullptr;          // zwlr_layer_shell_v1
    /// wl_seat (may be null: no input). Bind it right before `create`, which listens on it
    /// before anything is dispatched; its capabilities event would otherwise be lost.
    void* seat = nullptr;
    void* cursor_shape_manager = nullptr; // wp_cursor_shape_manager_v1
};

/// The shell's top panel: a wlr-layer-shell surface (layer top, anchored top/left/right,
/// 32 px, exclusive zone 32, no keyboard) listing one entry per window, the focused one
/// highlighted. A left click (press and release on the same entry) asks the compositor to
/// raise and activate that window.
///
/// Drawing is event driven: nothing runs while nothing changes. Model changes, pointer
/// motion and configures only mark the panel dirty; `after_dispatch()` draws at most once
/// per frame callback, repainting in its buffer only the entries that differ from what that
/// buffer holds and damaging only the entries that differ from the last commit.
class panel {
public:
    panel() = default;
    ~panel();
    panel(const panel&) = delete;
    panel& operator=(const panel&) = delete;

    /// Creates the layer surface and sends its initial commit.
    [[nodiscard]] lx::result<void> create(panel_globals globals, policy_bridge& bridge);
    void destroy();
    /// Call after every dispatch: consumes model changes and draws if needed.
    void after_dispatch();
    /// The compositor closed the layer surface (its output went away) or a buffer could
    /// not be made: the shell should exit and be restarted.
    [[nodiscard]] bool closed() const { return closed_; }
    [[nodiscard]] const panel_layout& committed() const { return committed_; }

    struct events;

private:
    friend struct events;
    struct buffer {
        void* wl = nullptr; // wl_buffer*
        std::uint32_t* pixels = nullptr;
        bool busy = false;
        panel_layout drawn{}; // what these pixels show (width 0: nothing valid)
    };

    [[nodiscard]] bool make_buffers(int width, int height);
    void free_buffers();
    void draw(bool force);
    void paint(buffer& b, const panel_layout& l, const lx::rect2i* rects, unsigned n);
    void paint_cell(buffer& b, const panel_layout& l, unsigned i);
    void paint_more(buffer& b, const panel_layout& l);
    void paint_logout(buffer& b, const panel_layout& l);
    void paint_text(buffer& b, int x, int y, int w, int h, const char* text,
                    lx::text::raster_color color);
    void pointer_moved(double x, double y);
    void log_layout(const panel_layout& l) const;

    panel_globals g_{};
    policy_bridge* bridge_ = nullptr;
    void* surface_ = nullptr;        // wl_surface*
    void* layer_ = nullptr;          // zwlr_layer_surface_v1*
    void* pointer_ = nullptr;        // wl_pointer*
    void* cursor_device_ = nullptr;  // wp_cursor_shape_device_v1*
    void* frame_ = nullptr;          // wl_callback*
    void* pool_ = nullptr;           // wl_shm_pool*
    void* pool_data_ = nullptr;
    std::size_t pool_size_ = 0;
    buffer buffers_[2]{};
    int last_buffer_ = -1;
    int width_ = 0;
    int height_ = 0;
    bool configured_ = false;
    bool resized_ = false;
    /// A configure was acked: the next draw commits even if nothing changed.
    bool must_commit_ = false;
    bool dirty_ = false;
    bool closed_ = false;
    bool font_warned_ = false;
    double pointer_x_ = -1.0;
    double pointer_y_ = -1.0;
    task_handle hovered_{};
    task_handle pressed_{};
    bool logout_hovered_ = false;
    bool logout_pressed_ = false;
    bool logout_armed_ = false;
    panel_layout committed_{};
    panel_layout next_{};
    lx::text::shaped_line line_{};
};

} // namespace lx::shell

// ── Layout ───────────────────────────────────────────────────────────────────

void lx::shell::layout_panel(const task_model& model, int width, int height, double px,
                             double py, task_handle pressed, panel_layout& out,
                             panel_logout logout) {
    out.width = width;
    out.height = height;
    out.cell_h = height < k_entry_height ? height : k_entry_height;
    out.cell_y = (height - out.cell_h) / 2;
    out.count = 0;
    out.hidden = 0;
    out.more_x = 0;
    out.logout_x = 0;
    out.logout_w = 0;
    out.logout_flags = 0;
    // The logout button takes the right end; entries share what is left.
    int reserve = 0;
    if (logout.shown && width >= 2 * k_panel_pad + k_logout_width + k_entry_min_width) {
        out.logout_w = k_logout_width;
        out.logout_x = width - k_panel_pad - k_logout_width;
        reserve = k_logout_width + k_panel_gap;
        if (logout.armed)
            out.logout_flags |= logout_armed;
        if (logout.pressed)
            out.logout_flags |= logout_pressed;
        if (px >= out.logout_x && px < out.logout_x + out.logout_w && py >= out.cell_y &&
            py < out.cell_y + out.cell_h)
            out.logout_flags |= logout_hover;
    }
    task_handle order[k_max_tasks];
    const unsigned n = model.ordered(order, k_max_tasks);
    if (n == 0 || width <= 2 * k_panel_pad + reserve)
        return;
    const int avail = width - 2 * k_panel_pad - reserve;
    const int ni = static_cast<int>(n);
    unsigned visible = n;
    int w = k_entry_min_width;
    if (ni * k_entry_min_width + (ni - 1) * k_panel_gap <= avail) {
        w = (avail - (ni - 1) * k_panel_gap) / ni;
        w = w < k_entry_min_width ? k_entry_min_width
                                  : (w > k_entry_max_width ? k_entry_max_width : w);
    } else {
        const int m = (avail - k_panel_more_width) / (k_entry_min_width + k_panel_gap);
        visible = m > 0 ? static_cast<unsigned>(m) : 0u;
    }
    if (visible > k_max_panel_cells)
        visible = k_max_panel_cells;
    const task_handle focused = model.focused();
    for (unsigned i = 0; i < visible; ++i) {
        panel_cell& c = out.cells[i];
        c.task = order[i];
        const task_entry* e = model.get(order[i]);
        c.label_rev = e ? e->label_rev : 0;
        c.x = k_panel_pad + static_cast<int>(i) * (w + k_panel_gap);
        c.w = w;
        c.flags = 0;
        if (c.task == focused)
            c.flags |= cell_focused;
        if (e && (e->flags & task_flag_demands_attention))
            c.flags |= cell_attention;
        if (e && (e->flags & task_flag_not_responding))
            c.flags |= cell_not_responding;
        if (pressed && c.task == pressed)
            c.flags |= cell_pressed;
        if (px >= c.x && px < c.x + c.w && py >= out.cell_y && py < out.cell_y + out.cell_h)
            c.flags |= cell_hover;
    }
    out.count = visible;
    out.hidden = n - visible;
    if (out.hidden)
        out.more_x = k_panel_pad + static_cast<int>(visible) * (w + k_panel_gap);
}

void lx::shell::copy_layout(panel_layout& to, const panel_layout& from) {
    to.width = from.width;
    to.height = from.height;
    to.cell_y = from.cell_y;
    to.cell_h = from.cell_h;
    to.count = from.count;
    to.hidden = from.hidden;
    to.more_x = from.more_x;
    to.logout_x = from.logout_x;
    to.logout_w = from.logout_w;
    to.logout_flags = from.logout_flags;
    for (unsigned i = 0; i < from.count && i < k_max_panel_cells; ++i) {
        panel_cell& d = to.cells[i];
        const panel_cell& c = from.cells[i];
        d.task.index = c.task.index;
        d.task.gen = c.task.gen;
        d.label_rev = c.label_rev;
        d.x = c.x;
        d.w = c.w;
        d.flags = c.flags;
    }
}

int lx::shell::panel_hit(const panel_layout& l, double x, double y) {
    if (y < l.cell_y || y >= l.cell_y + l.cell_h)
        return -1;
    for (unsigned i = 0; i < l.count; ++i)
        if (x >= l.cells[i].x && x < l.cells[i].x + l.cells[i].w)
            return static_cast<int>(i);
    return -1;
}

bool lx::shell::panel_hit_logout(const panel_layout& l, double x, double y) {
    return l.logout_w > 0 && x >= l.logout_x && x < l.logout_x + l.logout_w &&
           y >= l.cell_y && y < l.cell_y + l.cell_h;
}

unsigned lx::shell::panel_diff(const panel_layout& a, const panel_layout& b, lx::rect2i* out,
                               unsigned cap) {
    if (cap == 0 || b.width <= 0 || b.height <= 0)
        return 0;
    const lx::rect2i whole{0, 0, b.width, b.height};
    bool arrangement = a.width != b.width || a.height != b.height || a.count != b.count ||
                       a.hidden != b.hidden || a.more_x != b.more_x || a.cell_y != b.cell_y ||
                       a.cell_h != b.cell_h || a.logout_x != b.logout_x ||
                       a.logout_w != b.logout_w;
    for (unsigned i = 0; !arrangement && i < b.count; ++i)
        arrangement = a.cells[i].x != b.cells[i].x || a.cells[i].w != b.cells[i].w;
    if (arrangement) {
        out[0] = whole;
        return 1;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < b.count; ++i) {
        const panel_cell& x = a.cells[i];
        const panel_cell& y = b.cells[i];
        if (x.task == y.task && x.label_rev == y.label_rev && x.flags == y.flags)
            continue;
        if (n == cap) {
            out[0] = whole;
            return 1;
        }
        out[n++] = lx::rect2i{y.x, b.cell_y, y.w, b.cell_h};
    }
    if (b.logout_w > 0 && a.logout_flags != b.logout_flags) {
        if (n == cap) {
            out[0] = whole;
            return 1;
        }
        out[n++] = lx::rect2i{b.logout_x, b.cell_y, b.logout_w, b.cell_h};
    }
    return n;
}

// ── Drawing ──────────────────────────────────────────────────────────────────

namespace {

constexpr const char* k_panel_category = "shell.panel";
constexpr std::uint32_t k_bg = 0xFF1E1E22u;
constexpr std::uint32_t k_hover = 0xFF34343Au;
constexpr std::uint32_t k_pressed = 0xFF2A2A30u;
constexpr std::uint32_t k_focused = 0xFF3D6FB4u;
/// Demands attention: an amber accent, as GNOME's and KDE's task bars use.
constexpr std::uint32_t k_attention = 0xFFB4782Du;
/// The armed logout button: a second click ends the session.
constexpr std::uint32_t k_danger = 0xFFB43C3Cu;
constexpr lx::text::raster_color k_text{0xE6, 0xE6, 0xE6, 0xFF};
constexpr lx::text::raster_color k_text_focused{0xFF, 0xFF, 0xFF, 0xFF};
constexpr lx::text::raster_color k_text_more{0xA8, 0xA8, 0xAC, 0xFF};
constexpr lx::text::font_desc k_panel_font{"Sans", 10.f, lx::text::weight::normal,
                                           lx::text::style::normal};
/// Entry corner radius, anti-aliased against the panel background.
constexpr int k_radius = 4;
constexpr int k_text_pad = 8;

[[nodiscard]] bool panel_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void panel_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_panel_category, line);
}

[[nodiscard]] std::uint32_t mix(std::uint32_t over, std::uint32_t under, unsigned a /*0..255*/) {
    const unsigned ia = 255u - a;
    const unsigned r = (((over >> 16) & 0xFFu) * a + ((under >> 16) & 0xFFu) * ia + 127u) / 255u;
    const unsigned g = (((over >> 8) & 0xFFu) * a + ((under >> 8) & 0xFFu) * ia + 127u) / 255u;
    const unsigned b = ((over & 0xFFu) * a + (under & 0xFFu) * ia + 127u) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void fill(std::uint32_t* px, int stride, lx::rect2i r, std::uint32_t c) {
    for (int y = r.y; y < r.y + r.height; ++y) {
        std::uint32_t* row = px + static_cast<std::ptrdiff_t>(y) * stride + r.x;
        for (int x = 0; x < r.width; ++x)
            row[x] = c;
    }
}

/// A rounded rect over the panel background: corners blend by analytic pixel coverage.
void fill_rounded(std::uint32_t* px, int stride, lx::rect2i r, std::uint32_t c) {
    fill(px, stride, r, c);
    const int rad = r.width >= 2 * k_radius && r.height >= 2 * k_radius ? k_radius : 0;
    for (int cy = 0; cy < rad; ++cy) {
        for (int cx = 0; cx < rad; ++cx) {
            // Distance from the corner circle's centre to this pixel's centre.
            const float dx = static_cast<float>(rad) - (static_cast<float>(cx) + 0.5f);
            const float dy = static_cast<float>(rad) - (static_cast<float>(cy) + 0.5f);
            float cov = static_cast<float>(rad) - std::sqrt(dx * dx + dy * dy) + 0.5f;
            cov = cov < 0.f ? 0.f : (cov > 1.f ? 1.f : cov);
            const unsigned a = static_cast<unsigned>(cov * 255.f + 0.5f);
            const std::uint32_t v = mix(c, k_bg, a);
            const int xl = r.x + cx, xr = r.x + r.width - 1 - cx;
            const int yt = r.y + cy, yb = r.y + r.height - 1 - cy;
            px[static_cast<std::ptrdiff_t>(yt) * stride + xl] = v;
            px[static_cast<std::ptrdiff_t>(yt) * stride + xr] = v;
            px[static_cast<std::ptrdiff_t>(yb) * stride + xl] = v;
            px[static_cast<std::ptrdiff_t>(yb) * stride + xr] = v;
        }
    }
}

[[nodiscard]] bool intersects(lx::rect2i a, int x, int w) {
    return x < a.x + a.width && x + w > a.x;
}

} // namespace

void lx::shell::panel::paint_text(buffer& b, int x, int y, int w, int h, const char* text,
                                  lx::text::raster_color color) {
    if (w <= 0 || h <= 0)
        return;
    if (auto shaped = lx::text::shape_line(text, k_panel_font, line_); !shaped) {
        if (!font_warned_) {
            font_warned_ = true;
            lx::trace::logger::global().log_error(shaped.get_error(), k_panel_category);
            panel_log(lx::trace::level::warn, "no text rendering: entries are drawn unlabeled");
        }
        return;
    }
    const lx::text::line_fit fit = lx::text::fit_line(line_, w);
    // The target is the label's own box, so nothing can spill into a neighbour.
    lx::text::raster_target target{b.pixels + static_cast<std::ptrdiff_t>(y) * width_ + x, w, h,
                                   width_};
    const int baseline = (h - (line_.ascent_px + line_.descent_px)) / 2 + line_.ascent_px;
    (void)lx::text::render_line(line_, fit, color, target, {0, baseline});
}

void lx::shell::panel::paint_cell(buffer& b, const panel_layout& l, unsigned i) {
    const panel_cell& c = l.cells[i];
    const lx::rect2i box{c.x, l.cell_y, c.w, l.cell_h};
    fill(b.pixels, width_, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (c.flags & cell_pressed)
        bgc = k_pressed;
    else if (c.flags & cell_focused)
        bgc = k_focused;
    else if (c.flags & cell_attention)
        bgc = k_attention;
    else if (c.flags & cell_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        fill_rounded(b.pixels, width_, box, bgc);
    const task_entry* e = bridge_ ? bridge_->model().get(c.task) : nullptr;
    const char* label = "";
    if (e)
        label = e->title[0] ? e->title : (e->app_id[0] ? e->app_id : "untitled");
    paint_text(b, c.x + k_text_pad, l.cell_y, c.w - 2 * k_text_pad, l.cell_h, label,
               (c.flags & cell_not_responding) ? k_text_more
               : (c.flags & cell_focused)      ? k_text_focused
                                               : k_text);
}

void lx::shell::panel::paint_more(buffer& b, const panel_layout& l) {
    if (!l.hidden)
        return;
    char more[16];
    std::snprintf(more, sizeof(more), "+%u", l.hidden);
    const int right = l.logout_w > 0 ? l.logout_x - k_panel_gap : width_ - k_panel_pad;
    int w = right - l.more_x;
    w = w < k_panel_more_width ? w : k_panel_more_width;
    paint_text(b, l.more_x + 4, l.cell_y, w - 4, l.cell_h, more, k_text_more);
}

void lx::shell::panel::paint_logout(buffer& b, const panel_layout& l) {
    if (l.logout_w <= 0)
        return;
    const lx::rect2i box{l.logout_x, l.cell_y, l.logout_w, l.cell_h};
    fill(b.pixels, width_, box, k_bg);
    std::uint32_t bgc = k_bg;
    if (l.logout_flags & logout_pressed)
        bgc = k_pressed;
    else if (l.logout_flags & logout_armed)
        bgc = k_danger;
    else if (l.logout_flags & logout_hover)
        bgc = k_hover;
    if (bgc != k_bg)
        fill_rounded(b.pixels, width_, box, bgc);
    const bool armed = (l.logout_flags & logout_armed) != 0;
    paint_text(b, l.logout_x + k_text_pad, l.cell_y, l.logout_w - 2 * k_text_pad, l.cell_h,
               armed ? "Confirm?" : "Log out", armed ? k_text_focused : k_text_more);
}

void lx::shell::panel::paint(buffer& b, const panel_layout& l, const lx::rect2i* rects,
                             unsigned n) {
    for (unsigned r = 0; r < n; ++r) {
        const lx::rect2i& rect = rects[r];
        if (rect.width == l.width && rect.height == l.height) {
            // Everything: background, every entry, the "+k" label.
            fill(b.pixels, width_, rect, k_bg);
            for (unsigned i = 0; i < l.count; ++i)
                paint_cell(b, l, i);
            paint_more(b, l);
            paint_logout(b, l);
            return;
        }
        for (unsigned i = 0; i < l.count; ++i)
            if (intersects(rect, l.cells[i].x, l.cells[i].w))
                paint_cell(b, l, i);
        if (l.logout_w > 0 && intersects(rect, l.logout_x, l.logout_w))
            paint_logout(b, l);
    }
}

void lx::shell::panel::log_layout(const panel_layout& l) const {
    if (!panel_debug() || !bridge_)
        return;
    for (unsigned i = 0; i < l.count; ++i) {
        const task_entry* e = bridge_->model().get(l.cells[i].task);
        char line[384];
        std::snprintf(line, sizeof(line), "layout idx=%u x=%d w=%d handle=%u title=\"%s\"", i,
                      l.cells[i].x, l.cells[i].w, l.cells[i].task.index, e ? e->title : "");
        panel_log(lx::trace::level::debug, line);
    }
    if (l.hidden) {
        char line[64];
        std::snprintf(line, sizeof(line), "layout more=%u x=%d", l.hidden, l.more_x);
        panel_log(lx::trace::level::debug, line);
    }
}

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::panel::events {
    static panel& self(void* data) { return *static_cast<panel*>(data); }

    // ── zwlr_layer_surface_v1 ──
    static void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial,
                          uint32_t w, uint32_t h) {
        auto& p = self(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        const int nw = static_cast<int>(w);
        const int nh = h ? static_cast<int>(h) : k_panel_height;
        if (!p.configured_ || nw != p.width_ || nh != p.height_) {
            p.width_ = nw;
            p.height_ = nh;
            p.resized_ = true;
            char line[96];
            std::snprintf(line, sizeof(line), "configure serial=%u w=%d h=%d", serial, nw, nh);
            panel_log(lx::trace::level::info, line);
        }
        p.configured_ = true;
        // The ack takes effect with the next commit: make one even if nothing changed.
        p.must_commit_ = true;
    }
    static void closed(void* data, zwlr_layer_surface_v1*) {
        self(data).closed_ = true;
        panel_log(lx::trace::level::warn, "layer surface closed by the compositor");
    }

    // ── wl_callback (frame) ──
    static void frame_done(void* data, wl_callback* cb, uint32_t) {
        auto& p = self(data);
        wl_callback_destroy(cb);
        if (p.frame_ == cb)
            p.frame_ = nullptr;
    }

    // ── wl_buffer ──
    static void release(void* data, wl_buffer* wb) {
        auto& p = self(data);
        for (auto& b : p.buffers_)
            if (b.wl == wb)
                b.busy = false;
    }

    // ── wl_seat / wl_pointer ──
    static void seat_caps(void* data, wl_seat* seat, uint32_t caps);
    static void seat_name(void*, wl_seat*, const char*) {}
    static void enter(void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x,
                      wl_fixed_t y) {
        auto& p = self(data);
        if (p.cursor_device_)
            wp_cursor_shape_device_v1_set_shape(
                static_cast<wp_cursor_shape_device_v1*>(p.cursor_device_), serial,
                WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        p.pointer_moved(wl_fixed_to_double(x), wl_fixed_to_double(y));
    }
    static void leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
        auto& p = self(data);
        if (p.pressed_ || p.logout_pressed_ || p.logout_armed_)
            p.dirty_ = true;
        p.pressed_ = {};
        p.logout_pressed_ = false;
        p.logout_armed_ = false;
        p.pointer_moved(-1.0, -1.0);
    }
    static void motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        self(data).pointer_moved(wl_fixed_to_double(x), wl_fixed_to_double(y));
    }
    static void button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button,
                       uint32_t state) {
        constexpr uint32_t k_btn_left = 0x110;
        auto& p = self(data);
        if (button != k_btn_left)
            return;
        const bool on_logout = panel_hit_logout(p.committed_, p.pointer_x_, p.pointer_y_);
        if (state == WL_POINTER_BUTTON_STATE_PRESSED && on_logout) {
            p.logout_pressed_ = true;
            p.dirty_ = true;
            return;
        }
        if (state != WL_POINTER_BUTTON_STATE_PRESSED && p.logout_pressed_) {
            p.logout_pressed_ = false;
            p.dirty_ = true;
            if (!on_logout || !p.bridge_)
                return;
            if (!p.logout_armed_) {
                p.logout_armed_ = true; // first click: "Confirm?"
                panel_log(lx::trace::level::debug, "logout armed");
                return;
            }
            p.logout_armed_ = false;
            panel_log(lx::trace::level::info, "logout requested");
            p.bridge_->request_logout();
            return;
        }
        const int hit = panel_hit(p.committed_, p.pointer_x_, p.pointer_y_);
        const task_handle under = hit >= 0 ? p.committed_.cells[hit].task : task_handle{};
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            p.pressed_ = under;
            p.dirty_ = true;
            return;
        }
        const task_handle pressed = p.pressed_;
        p.pressed_ = {};
        p.dirty_ = true;
        if (!pressed || !(pressed == under) || !p.bridge_)
            return;
        if (panel_debug()) {
            const task_entry* e = p.bridge_->model().get(pressed);
            char line[384];
            std::snprintf(line, sizeof(line), "click idx=%d handle=%u title=\"%s\"", hit,
                          pressed.index, e ? e->title : "");
            panel_log(lx::trace::level::debug, line);
        }
        // Raise, then activate. The compositor's activate raises too; the panel still says
        // both so its intent stays explicit if focus and stacking policy ever separate.
        p.bridge_->request_raise(pressed);
        p.bridge_->request_activate(pressed);
    }
    static void axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
    static void frame(void*, wl_pointer*) {}
    static void axis_source(void*, wl_pointer*, uint32_t) {}
    static void axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}
};

namespace {

const zwlr_layer_surface_v1_listener k_layer_listener = {
    .configure = lx::shell::panel::events::configure,
    .closed = lx::shell::panel::events::closed,
};
const wl_callback_listener k_frame_listener = {.done = lx::shell::panel::events::frame_done};
const wl_buffer_listener k_buffer_listener = {.release = lx::shell::panel::events::release};
const wl_seat_listener k_seat_listener = {
    .capabilities = lx::shell::panel::events::seat_caps,
    .name = lx::shell::panel::events::seat_name,
};
// Bound at wl_seat v5 at most: every wl_pointer event up to v5 has a handler.
const wl_pointer_listener k_pointer_listener = {
    .enter = lx::shell::panel::events::enter,
    .leave = lx::shell::panel::events::leave,
    .motion = lx::shell::panel::events::motion,
    .button = lx::shell::panel::events::button,
    .axis = lx::shell::panel::events::axis,
    .frame = lx::shell::panel::events::frame,
    .axis_source = lx::shell::panel::events::axis_source,
    .axis_stop = lx::shell::panel::events::axis_stop,
    .axis_discrete = lx::shell::panel::events::axis_discrete,
};

} // namespace

void lx::shell::panel::events::seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto& p = self(data);
    const bool has = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has && !p.pointer_) {
        auto* ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ptr, &k_pointer_listener, &p);
        p.pointer_ = ptr;
        if (p.g_.cursor_shape_manager)
            p.cursor_device_ = wp_cursor_shape_manager_v1_get_pointer(
                static_cast<wp_cursor_shape_manager_v1*>(p.g_.cursor_shape_manager), ptr);
    } else if (!has && p.pointer_) {
        if (p.cursor_device_)
            wp_cursor_shape_device_v1_destroy(
                static_cast<wp_cursor_shape_device_v1*>(p.cursor_device_));
        p.cursor_device_ = nullptr;
        if (wl_pointer_get_version(static_cast<wl_pointer*>(p.pointer_)) >=
            WL_POINTER_RELEASE_SINCE_VERSION)
            wl_pointer_release(static_cast<wl_pointer*>(p.pointer_));
        else
            wl_pointer_destroy(static_cast<wl_pointer*>(p.pointer_));
        p.pointer_ = nullptr;
    }
}

void lx::shell::panel::pointer_moved(double x, double y) {
    pointer_x_ = x;
    pointer_y_ = y;
    // Hover is judged against what is on screen; a redraw only when it moves to another
    // entry, never per motion event.
    const int hit = panel_hit(committed_, x, y);
    const task_handle under = hit >= 0 ? committed_.cells[hit].task : task_handle{};
    if (!(under == hovered_)) {
        hovered_ = under;
        dirty_ = true;
    }
    const bool on_logout = panel_hit_logout(committed_, x, y);
    if (on_logout != logout_hovered_) {
        logout_hovered_ = on_logout;
        dirty_ = true;
        // Moving off the armed button cancels: confirmation is one deliberate gesture.
        if (!on_logout)
            logout_armed_ = false;
    }
}

lx::result<void> lx::shell::panel::create(panel_globals globals, policy_bridge& bridge) {
    if (!globals.compositor || !globals.shm || !globals.layer_shell)
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "panel needs wl_compositor, wl_shm and zwlr_layer_shell_v1");
    g_ = globals;
    bridge_ = &bridge;
    auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
    if (!surface)
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::bind_failed),
                              "wl_compositor.create_surface failed");
    surface_ = surface;
    auto* layer = zwlr_layer_shell_v1_get_layer_surface(
        static_cast<zwlr_layer_shell_v1*>(g_.layer_shell), surface, nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "lumen-panel");
    layer_ = layer;
    zwlr_layer_surface_v1_add_listener(layer, &k_layer_listener, this);
    zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(layer, 0, k_panel_height);
    zwlr_layer_surface_v1_set_exclusive_zone(layer, k_panel_height);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    // The panel is opaque everywhere: the compositor may skip blending and what is under it.
    if (auto* region = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        wl_region_add(region, 0, 0, 1 << 30, 1 << 30);
        wl_surface_set_opaque_region(surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(surface); // initial commit: no buffer, asks for a configure
    if (g_.seat)
        wl_seat_add_listener(static_cast<wl_seat*>(g_.seat), &k_seat_listener, this);
    return {};
}

bool lx::shell::panel::make_buffers(int width, int height) {
    free_buffers();
    const std::size_t stride = static_cast<std::size_t>(width) * 4u;
    const std::size_t one = stride * static_cast<std::size_t>(height);
    const std::size_t size = one * 2u;
    const int fd = memfd_create("lumen-panel", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return false;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return false;
    }
    // The compositor maps this pool too: it must not be able to shrink under it.
    (void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    auto* pool = wl_shm_create_pool(static_cast<wl_shm*>(g_.shm), fd, static_cast<int32_t>(size));
    close(fd);
    pool_ = pool;
    pool_data_ = data;
    pool_size_ = size;
    for (int i = 0; i < 2; ++i) {
        buffer& b = buffers_[i];
        auto* wb = wl_shm_pool_create_buffer(pool, static_cast<int32_t>(one) * i, width, height,
                                             static_cast<int32_t>(stride),
                                             WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(wb, &k_buffer_listener, this);
        b.wl = wb;
        b.pixels = reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(data) + one * i);
        b.busy = false;
        b.drawn.width = 0;
        b.drawn.count = 0;
    }
    last_buffer_ = -1;
    return true;
}

void lx::shell::panel::free_buffers() {
    for (auto& b : buffers_) {
        if (b.wl)
            wl_buffer_destroy(static_cast<wl_buffer*>(b.wl));
        b.wl = nullptr;
        b.pixels = nullptr;
        b.busy = false;
        b.drawn.width = 0;
        b.drawn.count = 0;
    }
    if (pool_)
        wl_shm_pool_destroy(static_cast<wl_shm_pool*>(pool_));
    pool_ = nullptr;
    if (pool_data_)
        munmap(pool_data_, pool_size_);
    pool_data_ = nullptr;
    pool_size_ = 0;
    last_buffer_ = -1;
}

void lx::shell::panel::after_dispatch() {
    if (!bridge_ || !surface_ || closed_)
        return;
    task_model& model = bridge_->model();
    // A snapshot in progress is never shown half-done; its changes wait for snapshot_done.
    if (model.in_snapshot())
        return;
    const unsigned changes = model.take_changes();
    if (changes & (task_change_list | task_change_label | task_change_focus | task_change_flags)) {
        dirty_ = true;
        // The entry under a still pointer may be a different one now.
        if (changes & task_change_list)
            pointer_moved(pointer_x_, pointer_y_);
        // A press on an entry that went away is no press.
        if (pressed_ && !model.get(pressed_))
            pressed_ = {};
    }
    if (!configured_)
        return;
    if (resized_ || must_commit_) {
        draw(true);
        return;
    }
    if (dirty_ && !frame_)
        draw(false);
}

void lx::shell::panel::draw(bool force) {
    if (resized_ || !pool_) {
        if (width_ <= 0 || height_ <= 0 || !make_buffers(width_, height_)) {
            panel_log(lx::trace::level::error, "cannot allocate panel buffers");
            closed_ = true;
            return;
        }
        committed_.width = 0; // nothing of this size is on screen yet
        committed_.count = 0;
        resized_ = false;
    }
    // Prefer the buffer committed last: it already shows most of the panel.
    int bi = -1;
    if (last_buffer_ >= 0 && !buffers_[last_buffer_].busy)
        bi = last_buffer_;
    else if (!buffers_[0].busy)
        bi = 0;
    else if (!buffers_[1].busy)
        bi = 1;
    if (bi < 0) {
        // Both still held by the compositor. An ack can still be answered with a bare
        // commit while the panel on screen has the configured size; anything else waits
        // for a release (dirty_ / must_commit_ stay set, and a release re-runs this).
        if (force && committed_.width == width_ && committed_.height == height_) {
            wl_surface_commit(static_cast<wl_surface*>(surface_));
            must_commit_ = false;
        }
        return;
    }
    buffer& b = buffers_[bi];

    layout_panel(bridge_->model(), width_, height_, pointer_x_, pointer_y_, pressed_, next_,
                 panel_logout{bridge_->can_logout(), logout_armed_, logout_pressed_});
    lx::rect2i damage[k_max_panel_cells];
    const unsigned nd = panel_diff(committed_, next_, damage, k_max_panel_cells);
    if (nd == 0 && !force) {
        dirty_ = false; // what is on screen is already right
        return;
    }
    lx::rect2i repaint[k_max_panel_cells];
    const unsigned nr = panel_diff(b.drawn, next_, repaint, k_max_panel_cells);
    paint(b, next_, repaint, nr);
    copy_layout(b.drawn, next_);

    const bool arrangement = nd == 1 && damage[0].width == next_.width &&
                             damage[0].height == next_.height;
    if (arrangement)
        log_layout(next_);

    auto* surface = static_cast<wl_surface*>(surface_);
    wl_surface_attach(surface, static_cast<wl_buffer*>(b.wl), 0, 0);
    for (unsigned i = 0; i < nd; ++i)
        wl_surface_damage_buffer(surface, damage[i].x, damage[i].y, damage[i].width,
                                 damage[i].height);
    auto* cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &k_frame_listener, this);
    frame_ = cb;
    wl_surface_commit(surface);
    b.busy = true;
    last_buffer_ = bi;
    copy_layout(committed_, next_);
    dirty_ = false;
    must_commit_ = false;

    if (panel_debug()) {
        int focused = -1;
        for (unsigned i = 0; i < next_.count; ++i)
            if (next_.cells[i].flags & cell_focused)
                focused = static_cast<int>(i);
        char line[160];
        std::snprintf(line, sizeof(line),
                      "draw entries=%u hidden=%u focused=%d damage=%u%s repaint=%u buffer=%d",
                      next_.count, next_.hidden, focused, nd, arrangement ? "(full)" : "", nr,
                      bi);
        panel_log(lx::trace::level::debug, line);
    }
}

void lx::shell::panel::destroy() {
    if (frame_)
        wl_callback_destroy(static_cast<wl_callback*>(frame_));
    frame_ = nullptr;
    if (cursor_device_)
        wp_cursor_shape_device_v1_destroy(static_cast<wp_cursor_shape_device_v1*>(cursor_device_));
    cursor_device_ = nullptr;
    if (pointer_) {
        if (wl_pointer_get_version(static_cast<wl_pointer*>(pointer_)) >=
            WL_POINTER_RELEASE_SINCE_VERSION)
            wl_pointer_release(static_cast<wl_pointer*>(pointer_));
        else
            wl_pointer_destroy(static_cast<wl_pointer*>(pointer_));
    }
    pointer_ = nullptr;
    if (layer_)
        zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(layer_));
    layer_ = nullptr;
    if (surface_)
        wl_surface_destroy(static_cast<wl_surface*>(surface_));
    surface_ = nullptr;
    free_buffers();
    bridge_ = nullptr;
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::panel::events {};

lx::result<void> lx::shell::panel::create(panel_globals, policy_bridge&) {
    return lx::not_implemented("lx::shell::panel::create");
}
void lx::shell::panel::destroy() {}
void lx::shell::panel::after_dispatch() {}
void lx::shell::panel::pointer_moved(double, double) {}
bool lx::shell::panel::make_buffers(int, int) { return false; }
void lx::shell::panel::free_buffers() {}
void lx::shell::panel::draw(bool) {}

#endif

lx::shell::panel::~panel() { destroy(); }
