module;

// The notification popups (`:notify_view`). See the partition.

#include <stdint.h>

#if defined(LUMEN_SHELL_WAYLAND)
#include <wayland-client.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

module lx.shell;

import std;
import lx.foundation;
import lx.text;
import lx.trace;
import :notifications;
import :notify_view;
import :shm_canvas;
import :icon_theme;

namespace {

constexpr lx::trace::category log_notify{"shell.notify"};
constexpr std::uint32_t k_card_bg = 0xFF24242Au;
constexpr std::uint32_t k_card_hover_bg = 0xFF2C2C33u;
constexpr std::uint32_t k_button_bg = 0xFF34343Au;
constexpr std::uint32_t k_button_hover = 0xFF3D6FB4u;
constexpr std::uint32_t k_critical = 0xFFE0312Bu;
constexpr std::uint32_t k_more_bg = 0xFF24242Au;
constexpr lx::text::raster_color k_summary_color{0xF4, 0xF4, 0xF6, 0xFF};
constexpr lx::text::raster_color k_body_color{0xC8, 0xC8, 0xCE, 0xFF};
constexpr lx::text::raster_color k_link_color{0x8A, 0xB4, 0xF8, 0xFF};
constexpr lx::text::raster_color k_dim_color{0x9A, 0x9A, 0xA2, 0xFF};
constexpr lx::text::font_desc k_summary_font{"Sans", 11.f, lx::text::weight::bold,
                                             lx::text::style::normal};
constexpr lx::text::font_desc k_body_font{"Sans", 10.f, lx::text::weight::normal,
                                          lx::text::style::normal};
constexpr lx::text::font_desc k_button_font{"Sans", 10.f, lx::text::weight::normal,
                                            lx::text::style::normal};
constexpr lx::text::font_desc k_close_font{"Sans", 11.f, lx::text::weight::normal,
                                           lx::text::style::normal};

[[nodiscard]] bool contains(lx::rect2i r, double x, double y) {
    return r.width > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

lx::text::raster_color body_color(int, std::uint16_t tag, void*) {
    return tag > 0 ? k_link_color : k_body_color;
}

} // namespace

// ── Layout ───────────────────────────────────────────────────────────────────

unsigned lx::shell::notify_button_actions(const notification& n, int* out, unsigned cap) {
    unsigned count = 0;
    int all[16];
    for (unsigned i = 0; i < n.req.actions.size() && count < 16; ++i)
        if (n.req.actions[i].key != "default")
            all[count++] = static_cast<int>(i);
    if (count <= cap) {
        for (unsigned i = 0; i < count; ++i)
            out[i] = all[i];
        return count;
    }
    // More than fit: the first cap−1, then "More…".
    for (unsigned i = 0; i + 1 < cap; ++i)
        out[i] = all[i];
    out[cap - 1] = -1;
    return cap;
}

void lx::shell::layout_notifications(const notification* const* shown, unsigned n, unsigned waiting,
                                     notify_position position, notify_body_fn body_height,
                                     void* user, bool (*has_icon)(const notification& n, void* user),
                                     notify_layout& out) {
    out.width = k_card_width;
    out.count = 0;
    out.more = waiting;
    out.more_box = {};
    const bool bottom = position == notify_position::bottom_right ||
                        position == notify_position::bottom_left;
    int heights[k_max_cards];
    for (unsigned i = 0; i < n && i < k_max_cards; ++i) {
        const notification& note = *shown[i];
        card_layout& c = out.cards[out.count++];
        c = {};
        c.id = note.id;
        c.rev = note.rev;
        c.critical = note.req.urgency == notify_urgency::critical;
        c.has_icon = has_icon && has_icon(note, user);
        const int text_x = k_card_pad + (c.has_icon ? k_card_icon + k_card_pad : 0);
        const int text_w = k_card_width - text_x - k_card_pad;
        c.close = {k_card_width - 6 - k_card_close, 6, k_card_close, k_card_close};
        int y = k_card_pad;
        if (!note.req.summary.empty()) {
            c.summary = {text_x, y, text_w - k_card_close + 4, k_card_summary_h};
            y += k_card_summary_h;
        }
        const int bh = note.body.text.empty() || !body_height ? 0 : body_height(note, text_w, user);
        if (bh > 0) {
            y += c.summary.width > 0 ? 4 : 0;
            c.body = {text_x, y, c.summary.width > 0 ? text_w : text_w - k_card_close + 4, bh};
            y += bh;
        }
        int idx[k_card_buttons];
        c.buttons = notify_button_actions(note, idx, k_card_buttons);
        if (c.buttons > 0) {
            y += 10;
            const int inner = k_card_width - 2 * k_card_pad;
            const int gap = 8;
            const int bw = (inner - gap * static_cast<int>(c.buttons - 1)) / static_cast<int>(c.buttons);
            const int top_of_buttons = c.has_icon && y < k_card_pad + k_card_icon + 10
                                           ? k_card_pad + k_card_icon + 10
                                           : y;
            for (unsigned b = 0; b < c.buttons; ++b) {
                c.button[b] = {k_card_pad + static_cast<int>(b) * (bw + gap), top_of_buttons, bw,
                               k_card_button_h};
                c.button_action[b] = idx[b];
            }
            y = top_of_buttons + k_card_button_h;
        }
        if (c.has_icon) {
            c.icon = {k_card_pad, k_card_pad, k_card_icon, k_card_icon};
            if (y < k_card_pad + k_card_icon)
                y = k_card_pad + k_card_icon;
        }
        if (y < k_card_pad + k_card_close)
            y = k_card_pad + k_card_close;
        heights[out.count - 1] = y + k_card_pad;
    }
    // Stacked: the newest at the anchor edge (top or bottom), the "+n more" line after the
    // cards, away from it.
    int y = 0;
    const auto place = [&](card_layout& c, int h) {
        const int dy = y;
        c.box = {0, dy, k_card_width, h};
        const auto shift = [dy](lx::rect2i& r) {
            if (r.width > 0)
                r.y += dy;
        };
        shift(c.icon);
        shift(c.summary);
        shift(c.body);
        shift(c.close);
        for (unsigned b = 0; b < c.buttons; ++b)
            shift(c.button[b]);
        y += h + k_card_gap;
    };
    if (bottom) {
        if (waiting) {
            out.more_box = {k_card_width / 2 - 50, y, 100, k_card_more_h};
            y += k_card_more_h + k_card_gap;
        }
        for (unsigned i = out.count; i-- > 0;)
            place(out.cards[i], heights[i]);
    } else {
        for (unsigned i = 0; i < out.count; ++i)
            place(out.cards[i], heights[i]);
        if (waiting) {
            out.more_box = {k_card_width / 2 - 50, y, 100, k_card_more_h};
            y += k_card_more_h + k_card_gap;
        }
    }
    out.height = y > 0 ? y - k_card_gap : 0;
}

// ── The view ─────────────────────────────────────────────────────────────────

lx::shell::notification_view::card_cache* lx::shell::notification_view::cache_for(const notification& n) {
    card_cache* c = nullptr;
    for (auto& e : cache_)
        if (e->id == n.id)
            c = e.get();
    if (!c) {
        cache_.push_back(std::make_unique<card_cache>());
        c = cache_.back().get();
        c->id = n.id;
        c->rev = 0;
    }
    return c;
}

void lx::shell::notification_view::relayout() {
    if (!center_)
        return;
    const notification* shown[k_max_cards];
    const unsigned n = center_->shown(shown, k_max_cards);
    // Caches of cards no longer shown go.
    std::erase_if(cache_, [&](const std::unique_ptr<card_cache>& c) {
        for (unsigned i = 0; i < n; ++i)
            if (shown[i]->id == c->id)
                return false;
        return true;
    });
    layout_notifications(
        shown, n, center_->waiting(), position_,
        [](const notification& note, int width, void* user) {
            auto& v = *static_cast<notification_view*>(user);
            card_cache& c = *v.cache_for(note);
            const double s = v.canvas_.scale_factor();
            if (c.rev != note.rev || c.width != width || c.scale != s) {
                c.rev = note.rev;
                c.width = width;
                c.scale = s;
                c.body_h = v.canvas_.wrap(note.body.text, note.body.spans, k_body_font, width,
                                          k_card_body_lines, c.body);
                c.image.clear();
                c.image_px = 0;
            }
            return c.body_h;
        },
        this,
        [](const notification& note, void* user) {
            auto& v = *static_cast<notification_view*>(user);
            if (note.req.image.size > 0)
                return true;
            const int px = v.canvas_.pixel_rect({0, 0, k_card_icon, k_card_icon}).width;
            return v.icons_ && v.icons_(note, px, v.user_) != nullptr;
        },
        layout_);
    dirty_ = true;
}

lx::shell::notification_view::hit lx::shell::notification_view::hit_at(double x, double y) const {
    hit h{};
    for (unsigned i = 0; i < layout_.count; ++i) {
        const card_layout& c = layout_.cards[i];
        if (!contains(c.box, x, y))
            continue;
        h.card = static_cast<int>(i);
        h.part = 0;
        if (contains(c.close, x, y)) {
            h.part = 1;
            return h;
        }
        for (unsigned b = 0; b < c.buttons; ++b)
            if (contains(c.button[b], x, y)) {
                h.part = 2 + static_cast<int>(b);
                return h;
            }
        if (contains(c.body, x, y)) {
            for (const auto& e : cache_)
                if (e->id == c.id) {
                    const int p = canvas_.wrapped_hit(e->body, x - c.body.x, y - c.body.y);
                    const notification* n = center_ ? center_->find(c.id) : nullptr;
                    if (p >= 0 && n && e->body.pieces[p].span < n->body.spans.size() &&
                        n->body.spans[e->body.pieces[p].span].tag > 0)
                        h.part = 10 + p;
                }
        }
        return h;
    }
    return h;
}

void lx::shell::notification_view::pointer_enter(double x, double y) { pointer_motion(x, y); }

void lx::shell::notification_view::pointer_leave() {
    pointer_x_ = pointer_y_ = -1.0;
    if (hover_card_ >= 0 || hover_part_ >= 0)
        dirty_ = true;
    hover_card_ = hover_part_ = -1;
    pressed_ = {};
}

void lx::shell::notification_view::pointer_motion(double x, double y) {
    pointer_x_ = x;
    pointer_y_ = y;
    const hit h = hit_at(x, y);
    // Only a change of part repaints (hover feedback), never per motion event.
    const int part = h.part >= 10 ? 10 : h.part;
    if (h.card != hover_card_ || part != hover_part_) {
        hover_card_ = h.card;
        hover_part_ = part;
        dirty_ = true;
    }
}

lx::shell::notify_click lx::shell::notification_view::pointer_button(unsigned button, bool down) {
    notify_click out{};
    if (down) {
        pressed_ = hit_at(pointer_x_, pointer_y_);
        return out;
    }
    const hit h = hit_at(pointer_x_, pointer_y_);
    const hit p = pressed_;
    pressed_ = {};
    if (h.card < 0 || h.card != p.card || !center_)
        return out;
    const card_layout& c = layout_.cards[h.card];
    const notification* n = center_->find(c.id);
    if (!n)
        return out;
    out.id = c.id;
    constexpr unsigned k_left = 0x110, k_right = 0x111, k_middle = 0x112;
    if (button == k_right || button == k_middle) {
        out.what = notify_click::kind::dismiss;
        return out;
    }
    if (button != k_left || h.part != p.part)
        return out;
    if (h.part == 1) {
        out.what = notify_click::kind::dismiss;
    } else if (h.part >= 2 && h.part < 2 + static_cast<int>(k_card_buttons)) {
        const unsigned b = static_cast<unsigned>(h.part - 2);
        const int a = c.button_action[b];
        if (a < 0) {
            out.what = notify_click::kind::more;
            out.anchor = c.button[b];
        } else if (static_cast<unsigned>(a) < n->req.actions.size()) {
            out.what = notify_click::kind::invoke;
            out.arg = n->req.actions[static_cast<unsigned>(a)].key;
        }
    } else if (h.part >= 10) {
        for (const auto& e : cache_)
            if (e->id == c.id) {
                const unsigned piece = static_cast<unsigned>(h.part - 10);
                const unsigned span = e->body.pieces[piece].span;
                const unsigned tag = span < n->body.spans.size() ? n->body.spans[span].tag : 0u;
                if (tag > 0 && tag - 1 < n->body.links.size()) {
                    out.what = notify_click::kind::link;
                    out.arg = n->body.links[tag - 1];
                }
            }
    } else {
        // The card itself: its default action, else it goes (reason 2).
        bool has_default = false;
        for (const notify_action& a : n->req.actions)
            has_default = has_default || a.key == "default";
        if (has_default && center_->can_invoke(c.id, "default")) {
            out.what = notify_click::kind::invoke;
            out.arg = "default";
        } else {
            out.what = notify_click::kind::dismiss;
        }
    }
    return out;
}

void lx::shell::notification_view::paint_card(int bi, const card_layout& c, const notification& n) {
    const bool hovered = hover_card_ >= 0 && layout_.cards[hover_card_].id == c.id;
    const std::uint32_t bg = hovered ? k_card_hover_bg : k_card_bg;
    canvas_.fill_rounded(bi, c.box, bg, 0u, k_card_radius);
    if (c.critical)
        canvas_.fill(bi, {c.box.x, c.box.y + k_card_radius, 4, c.box.height - 2 * k_card_radius},
                     k_critical);
    card_cache* cache = nullptr;
    for (auto& e : cache_)
        if (e->id == c.id)
            cache = e.get();
    if (c.has_icon) {
        const int px = canvas_.pixel_rect(c.icon).width;
        const std::uint32_t* pixels = nullptr;
        if (n.req.image.size > 0 && cache) {
            if (cache->image_px != px) {
                cache->image.assign(static_cast<std::size_t>(px) * static_cast<std::size_t>(px), 0u);
                resample_icon(n.req.image.pixels.data(), n.req.image.size, cache->image.data(), px);
                cache->image_px = px;
            }
            pixels = cache->image.data();
        } else if (icons_) {
            pixels = icons_(n, px, user_);
        }
        if (pixels)
            canvas_.blit(bi, c.icon, pixels, px);
    }
    if (c.summary.width > 0)
        (void)canvas_.text(bi, c.summary, n.req.summary.c_str(), k_summary_color, k_summary_font);
    // The close button: dim, brighter (and a disc behind it) under the pointer.
    const bool close_hover = hovered && hover_part_ == 1;
    if (close_hover)
        canvas_.fill_rounded(bi, c.close, k_button_bg, bg, k_card_close / 2);
    {
        const char* x = "\xC3\x97"; // ×
        const int tw = canvas_.text_width(x, k_close_font);
        const int tx = c.close.x + (tw > 0 && tw < c.close.width ? (c.close.width - tw) / 2 : 4);
        (void)canvas_.text(bi, {tx, c.close.y, c.close.x + c.close.width - tx, c.close.height}, x,
                           close_hover ? k_summary_color : k_dim_color, k_close_font);
    }
    if (c.body.width > 0 && cache)
        canvas_.draw_wrapped(bi, c.body, n.body.text, n.body.spans, cache->body, k_body_font,
                             &body_color, nullptr);
    for (unsigned b = 0; b < c.buttons; ++b) {
        const lx::rect2i& r = c.button[b];
        const bool on = hovered && hover_part_ == 2 + static_cast<int>(b);
        canvas_.fill_rounded(bi, r, on ? k_button_hover : k_button_bg, bg, 6);
        const int a = c.button_action[b];
        const char* label = a < 0 ? "More\xE2\x80\xA6"
                                  : n.req.actions[static_cast<unsigned>(a)].label.c_str();
        const int tw = canvas_.text_width(label, k_button_font);
        const int tx = tw > 0 && tw < r.width - 12 ? r.x + (r.width - tw) / 2 : r.x + 6;
        (void)canvas_.text(bi, {tx, r.y, r.x + r.width - 6 - tx, r.height}, label, k_summary_color,
                           k_button_font);
    }
}

void lx::shell::notification_view::draw() {
    const int bi = canvas_.acquire();
    if (bi < 0)
        return; // a release re-runs this
    canvas_.clear(bi);
    for (unsigned i = 0; i < layout_.count; ++i)
        if (const notification* n = center_ ? center_->find(layout_.cards[i].id) : nullptr)
            paint_card(bi, layout_.cards[i], *n);
    if (layout_.more > 0) {
        const lx::rect2i& r = layout_.more_box;
        canvas_.fill_rounded(bi, r, k_more_bg, 0u, r.height / 2);
        char text[32];
        std::snprintf(text, sizeof(text), "+%u more", layout_.more);
        const int tw = canvas_.text_width(text, k_button_font);
        const int tx = tw > 0 && tw < r.width ? r.x + (r.width - tw) / 2 : r.x + 8;
        (void)canvas_.text(bi, {tx, r.y, r.x + r.width - tx, r.height}, text, k_body_color,
                           k_button_font);
    }
    const lx::rect2i whole{0, 0, layout_.width, layout_.height};
    canvas_.commit(surface_, bi, &whole, 1);
    ++frames_;
    dirty_ = false;
    if (log_notify.enabled(lx::trace::level::debug)) {
        log_notify.debug("notifications: draw cards={} more={} {}x{}", layout_.count, layout_.more,
                         layout_.width, layout_.height);
        // Where each card's parts are (surface coordinates): the smoke test clicks them.
        for (unsigned i = 0; i < layout_.count; ++i) {
            const card_layout& c = layout_.cards[i];
            char buttons[160] = "";
            char* end = buttons;
            char* const last = buttons + sizeof(buttons) - 1;
            for (unsigned b = 0; b < c.buttons && end < last; ++b)
                end = std::format_to_n(end, last - end, "{}{},{},{},{}", b ? ";" : "",
                                       c.button[b].x, c.button[b].y, c.button[b].width,
                                       c.button[b].height)
                          .out;
            log_notify.debug(
                "notifications: card id={} box={},{},{},{} close={},{},{},{} buttons={}", c.id,
                c.box.x, c.box.y, c.box.width, c.box.height, c.close.x, c.close.y, c.close.width,
                c.close.height, buttons[0] ? buttons : "-");
        }
    }
}

#if defined(LUMEN_SHELL_WAYLAND)

struct lx::shell::notification_view::events {
    static notification_view& self(void* data) { return *static_cast<notification_view*>(data); }
    static void configure(void* data, zwlr_layer_surface_v1* layer, uint32_t serial, uint32_t w,
                          uint32_t h) {
        auto& v = self(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        v.conf_w_ = w ? static_cast<int>(w) : v.want_w_;
        v.conf_h_ = h ? static_cast<int>(h) : v.want_h_;
        v.configured_ = true;
        v.dirty_ = true;
    }
    static void closed(void* data, zwlr_layer_surface_v1*) {
        // Its output went: shown again on another one at the next update.
        self(data).close();
    }
};

namespace {

const zwlr_layer_surface_v1_listener k_notify_layer_listener = {
    .configure = lx::shell::notification_view::events::configure,
    .closed = lx::shell::notification_view::events::closed,
};

uint32_t anchor_of(lx::shell::notify_position p) {
    using lx::shell::notify_position;
    switch (p) {
    case notify_position::top_right:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    case notify_position::top_center: return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    case notify_position::top_left:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    case notify_position::bottom_right:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    case notify_position::bottom_left:
        return ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    }
    return ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
}

} // namespace

void lx::shell::notification_view::init(notify_globals g, notify_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    user_ = user;
    canvas_.set_alpha(true);
}

void lx::shell::notification_view::set_position(notify_position p) {
    if (p == position_)
        return;
    position_ = p;
    close(); // made again where it now belongs at the next update
    center_rev_ = 0;
}

void lx::shell::notification_view::update(const notification_center& c, void* output) {
    center_ = &c;
    const notification* shown[k_max_cards];
    const unsigned n = c.shown(shown, k_max_cards);
    if (n == 0) {
        if (surface_)
            close();
        center_rev_ = c.rev();
        return;
    }
    if (surface_ && output != output_)
        close();
    const bool overlay = c.critical_shown();
    if (surface_ && c.rev() == center_rev_ && overlay == overlay_)
        return;
    center_rev_ = c.rev();
    if (!surface_) {
        if (!g_.compositor || !g_.layer_shell || !g_.shm)
            return;
        auto* surface = wl_compositor_create_surface(static_cast<wl_compositor*>(g_.compositor));
        if (!surface)
            return;
        surface_ = surface;
        output_ = output;
        overlay_ = overlay;
        auto* layer = zwlr_layer_shell_v1_get_layer_surface(
            static_cast<zwlr_layer_shell_v1*>(g_.layer_shell), surface,
            static_cast<wl_output*>(output),
            overlay ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY : ZWLR_LAYER_SHELL_V1_LAYER_TOP,
            "lumen-notifications");
        layer_ = layer;
        zwlr_layer_surface_v1_add_listener(layer, &k_notify_layer_listener, this);
        canvas_.attach_scale(g_.viewporter, g_.fractional, surface);
        zwlr_layer_surface_v1_set_anchor(layer, anchor_of(position_));
        zwlr_layer_surface_v1_set_margin(layer, k_notify_margin, k_notify_margin,
                                         k_notify_margin, k_notify_margin);
        // 0: kept clear of the panel's zone, reserving nothing.
        zwlr_layer_surface_v1_set_exclusive_zone(layer, 0);
        zwlr_layer_surface_v1_set_keyboard_interactivity(
            layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        configured_ = false;
        conf_w_ = conf_h_ = want_w_ = want_h_ = 0;
        frames_ = 0;
        relayout();
        apply_size();
        wl_surface_commit(surface);
        log_notify.info("notifications: shown ({} card{}, {} layer)", n, n == 1 ? "" : "s",
                        overlay ? "overlay" : "top");
        return;
    }
    if (overlay != overlay_) {
        overlay_ = overlay;
        log_notify.info("notifications: {} layer (a critical one {})", overlay ? "overlay" : "top",
                        overlay ? "shown" : "gone");
        // zwlr_layer_surface_v1 v2: the layer changes with the next commit.
        if (zwlr_layer_surface_v1_get_version(static_cast<zwlr_layer_surface_v1*>(layer_)) >= 2)
            zwlr_layer_surface_v1_set_layer(static_cast<zwlr_layer_surface_v1*>(layer_),
                                            overlay ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                                                    : ZWLR_LAYER_SHELL_V1_LAYER_TOP);
    }
    relayout();
    if (layout_.width != want_w_ || layout_.height != want_h_) {
        apply_size();
        wl_surface_commit(static_cast<wl_surface*>(surface_));
    }
}

void lx::shell::notification_view::apply_size() {
    want_w_ = layout_.width;
    want_h_ = layout_.height > 0 ? layout_.height : 1;
    zwlr_layer_surface_v1_set_size(static_cast<zwlr_layer_surface_v1*>(layer_),
                                   static_cast<uint32_t>(want_w_), static_cast<uint32_t>(want_h_));
}

void lx::shell::notification_view::close() {
    if (!surface_)
        return;
    canvas_.free();
    canvas_.release_scale();
    (void)canvas_.take_rescale();
    if (layer_)
        zwlr_layer_surface_v1_destroy(static_cast<zwlr_layer_surface_v1*>(layer_));
    layer_ = nullptr;
    wl_surface_destroy(static_cast<wl_surface*>(surface_));
    surface_ = nullptr;
    output_ = nullptr;
    configured_ = false;
    hover_card_ = hover_part_ = -1;
    pressed_ = {};
    log_notify.info("notifications: hidden ({} frame{})", frames_, frames_ == 1 ? "" : "s");
}

void lx::shell::notification_view::after_dispatch() {
    if (!surface_ || !configured_)
        return;
    if (canvas_.take_rescale()) {
        // Another preferred scale: the bodies wrap again at the new pixel size.
        for (auto& c : cache_)
            c->rev = 0;
        relayout();
        canvas_.free();
    }
    // Draw at the size configured for the current layout only.
    if (conf_w_ != layout_.width || conf_h_ != (layout_.height > 0 ? layout_.height : 1))
        return;
    if (!dirty_ || canvas_.frame_pending())
        return;
    if (!canvas_.allocated() || canvas_.width() != conf_w_ || canvas_.height() != conf_h_) {
        if (!canvas_.allocate(g_.shm, conf_w_, conf_h_, "lumen-notifications")) {
            log_notify.error("cannot allocate notification buffers");
            close();
            return;
        }
    }
    auto* surface = static_cast<wl_surface*>(surface_);
    // Input and opaque regions: the cards (clicks between them go through).
    if (auto* input = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        for (unsigned i = 0; i < layout_.count; ++i) {
            const lx::rect2i& b = layout_.cards[i].box;
            wl_region_add(input, b.x, b.y, b.width, b.height);
        }
        if (layout_.more)
            wl_region_add(input, layout_.more_box.x, layout_.more_box.y, layout_.more_box.width,
                          layout_.more_box.height);
        wl_surface_set_input_region(surface, input);
        wl_region_destroy(input);
    }
    if (auto* opaque = wl_compositor_create_region(static_cast<wl_compositor*>(g_.compositor))) {
        for (unsigned i = 0; i < layout_.count; ++i) {
            const lx::rect2i& b = layout_.cards[i].box;
            wl_region_add(opaque, b.x + k_card_radius, b.y, b.width - 2 * k_card_radius, b.height);
            wl_region_add(opaque, b.x, b.y + k_card_radius, b.width, b.height - 2 * k_card_radius);
        }
        wl_surface_set_opaque_region(surface, opaque);
        wl_region_destroy(opaque);
    }
    draw();
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::notification_view::events {};
void lx::shell::notification_view::init(notify_globals g, notify_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    user_ = user;
}
void lx::shell::notification_view::set_position(notify_position p) { position_ = p; }
void lx::shell::notification_view::update(const notification_center& c, void*) { center_ = &c; }
void lx::shell::notification_view::apply_size() {}
void lx::shell::notification_view::close() {}
void lx::shell::notification_view::after_dispatch() {}

#endif

lx::shell::notification_view::~notification_view() { close(); }
