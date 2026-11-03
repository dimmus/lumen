module;

// The notification popups (`:notify_view`). See the partition.

#include <stdint.h>
#include <wayland-client.h>

module lx.shell;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
import :notifications;
import :notify_view;
import :view_ui;
import :icon_theme;

namespace {

constexpr lx::trace::category log_notify{"shell.notify"};
constexpr std::uint32_t k_card_bg = 0xFF24242Au;
constexpr std::uint32_t k_card_hover_bg = 0xFF2C2C33u;
constexpr std::uint32_t k_button_bg = 0xFF34343Au;
constexpr std::uint32_t k_button_hover = 0xFF3D6FB4u;
constexpr std::uint32_t k_critical = 0xFFE0312Bu;
constexpr std::uint32_t k_more_bg = 0xFF24242Au;
constexpr std::uint32_t k_summary_color = 0xFFF4F4F6u;
constexpr std::uint32_t k_body_color = 0xFFC8C8CEu;
constexpr std::uint32_t k_link_color = 0xFF8AB4F8u;
constexpr std::uint32_t k_dim_color = 0xFF9A9AA2u;
constexpr lx::text::font_desc k_summary_font{"Sans", 11.f, lx::text::weight::bold,
                                             lx::text::style::normal};
constexpr lx::text::font_desc k_body_font{"Sans", 10.f, lx::text::weight::normal,
                                          lx::text::style::normal};
constexpr lx::text::font_desc k_button_font{"Sans", 10.f, lx::text::weight::normal,
                                            lx::text::style::normal};
constexpr lx::text::font_desc k_close_font{"Sans", 11.f, lx::text::weight::normal,
                                           lx::text::style::normal};

lx::shell::notification_view* g_describing = nullptr;
struct describing_guard {
    explicit describing_guard(lx::shell::notification_view* v) : previous_{g_describing} {
        g_describing = v;
    }
    ~describing_guard() { g_describing = previous_; }
    describing_guard(const describing_guard&) = delete;
    describing_guard& operator=(const describing_guard&) = delete;

private:
    lx::shell::notification_view* previous_;
};

} // namespace

lx::ui::child lx::shell::notify_describe(lx::ui::build_context&) {
    lx::shell::notification_view* v = g_describing;
    return v ? v->build_ui() : nullptr;
}

namespace {

[[nodiscard]] bool contains(lx::rect2i r, double x, double y) {
    return r.width > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

/// One line of text in the stack's fonts, clipped to its box.
[[nodiscard]] lx::ui::child notify_text(const char* text, std::uint32_t argb,
                                        const lx::text::font_desc& font,
                                        lx::ui::element_key key) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = font;
    t.color = lx::shell::shell_color(argb);
    t.wrap = lx::text::text_wrap::none;
    t.context = lx::ui::active_ui_resources().text;
    return lx::ui::text_block(t, key);
}

/// A literal rounded box over `children` at an absolute rect (view_ui's).
[[nodiscard]] lx::ui::child notify_quad(lx::ui::style layout, std::uint32_t fill, int radius,
                                        std::span<const lx::ui::child> children,
                                        lx::ui::element_key key) {
    lx::shell::shell_box_props p{};
    p.control.layout = layout;
    if (fill) {
        lx::ui::quad_style q{};
        q.background = lx::shell::shell_color(fill);
        for (float& r : q.corner_radii)
            r = static_cast<float>(radius);
        p.rest = q;
    }
    return lx::shell::shell_box(p, children, key);
}

[[nodiscard]] lx::ui::style at(lx::rect2i r) {
    return lx::ui::style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

/// A line of text centred in and clipped to `r` — the canvas's `text` in a box: the
/// summary in its row, vertically centred, left-aligned, ellipsized at the rect's width
/// as the canvas's `fit_line` fitted it.
[[nodiscard]] lx::ui::child notify_text_at(lx::rect2i r, const char* text, std::uint32_t argb,
                                           const lx::text::font_desc& font,
                                           lx::ui::element_key text_key,
                                           lx::ui::element_key box_key) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = font;
    t.color = lx::shell::shell_color(argb);
    t.wrap = lx::text::text_wrap::none;
    t.ellipsize = true;
    t.context = lx::ui::active_ui_resources().text;
    const lx::ui::child inside[1] = {lx::ui::text_block(t, text_key)};
    lx::shell::shell_box_props box{};
    box.control.layout = at(r).flex_row().items_center().overflow_hidden();
    return lx::shell::shell_box(box, inside, box_key);
}

/// The markup spans as the text_block's attrs: a link's tag carries its colour.
[[nodiscard]] std::span<const lx::text::text_attr> body_attrs(
    const lx::shell::rich_text& body, std::vector<lx::text::text_attr>& out) {
    out.clear();
    out.reserve(body.spans.size());
    for (const lx::text::text_span& s : body.spans) {
        lx::text::text_attr a{};
        a.begin = s.begin;
        a.end = s.end;
        a.weight = s.weight;
        a.style = s.style;
        a.underline = s.underline ? lx::text::text_underline::single
                                  : lx::text::text_underline::none;
        if (s.tag > 0) {
            a.color = k_link_color & 0xFFFFFFu; // sRGB 0xRRGGBBAA, straight alpha
            a.role = lx::text::span_role::link;
        }
        a.tag = s.tag;
        out.push_back(a);
    }
    return {out};
}

} // namespace

// ── Layout (unchanged; pure) ─────────────────────────────────────────────────

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

namespace {

[[nodiscard]] std::uint32_t anchor_of(lx::shell::notify_position p) {
    using lx::shell::notify_position;
    using namespace lx::wayland;
    switch (p) {
    case notify_position::top_right: return anchor_top | anchor_right;
    case notify_position::top_center: return anchor_top;
    case notify_position::top_left: return anchor_top | anchor_left;
    case notify_position::bottom_right: return anchor_bottom | anchor_right;
    case notify_position::bottom_left: return anchor_bottom | anchor_left;
    }
    return anchor_top | anchor_right;
}

} // namespace

void lx::shell::notification_view::init(notify_globals g, notify_icon_fn icons, void* user) {
    g_ = g;
    icons_ = icons;
    user_ = user;
}

lx::shell::notification_view::card_cache* lx::shell::notification_view::cache_for(
    const notification& n) {
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
            const double s = v.surface_.valid() ? v.surface_.scale() : 1.0;
            if (c.rev != note.rev || c.width != width || c.scale != s) {
                c.rev = note.rev;
                c.width = width;
                c.scale = s;
                // As the canvas wrapped: the font and the width at device pixels, the height
                // back to logical (the text_block lays out at logical and scales itself).
                lx::text::font_desc font = k_body_font;
                font.size_pt = static_cast<float>(font.size_pt * s);
                c.body_h = 0;
                if (!note.body.text.empty() &&
                    lx::text::wrap_text(note.body.text, note.body.spans, font,
                                        static_cast<int>(width * s), k_card_body_lines, c.body) &&
                    c.body.line_count > 0)
                    c.body_h = static_cast<int>(
                        std::ceil(static_cast<int>(c.body.line_count) * c.body.line_height_px / s));
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
            const int px = static_cast<int>(std::lround(k_card_icon * (v.surface_.valid()
                                                                           ? v.surface_.scale()
                                                                           : 1.f)));
            return v.icons_ && v.icons_(note, px, v.user_) != nullptr;
        },
        layout_);
    dirty_ = true;
}

void lx::shell::notification_view::set_position(notify_position p) {
    if (p == position_)
        return;
    position_ = p;
    close(); // made again where it now belongs at the next update
}


void lx::shell::notification_view::update(const notification_center& c, unsigned output_slot) {
    center_ = &c;
    const notification* shown[1];
    const unsigned n = c.shown(shown, 1);
    if (n == 0) {
        if (is_open())
            close();
        center_rev_ = c.rev();
        return;
    }
    if (is_open() && output_slot != output_)
        close();
    const bool overlay = c.critical_shown();
    if (is_open() && c.rev() == center_rev_ && overlay == overlay_)
        return;
    center_rev_ = c.rev();
    if (!is_open()) {
        if (!g_.ui || !g_.ui->started())
            return;
        relayout();
        lx::ui::host::surface_options so{};
        so.describe = &notify_describe;
        so.size = {layout_.width, layout_.height > 0 ? layout_.height : 1};
        // Alpha cards over whatever is beneath; no opaque region.
        so.opaque = false;
        so.frame_records = false;
        if (auto made = surface_.create(*g_.ui, so); !made) {
            log_notify.error(made.get_error());
            return;
        }
        lx::ui::host::layer_role_options lo{};
        lo.layer.output_slot = output_slot;
        lo.layer.level = overlay ? lx::wayland::layer::overlay : lx::wayland::layer::top;
        lo.layer.name_space = "lumen-notifications";
        lo.size = so.size;
        lo.anchor = anchor_of(position_);
        for (std::int32_t& m : lo.margin)
            m = k_notify_margin;
        // 0: kept clear of the panel's zone, reserving nothing.
        lo.exclusive_zone = 0;
        lo.keyboard = lx::wayland::keyboard_interactivity::none;
        if (auto made = role_.create(surface_, lo); !made) {
            log_notify.error(made.get_error());
            surface_.destroy();
            return;
        }
        role_.set_closed_handler(
            [](void* user) { static_cast<notification_view*>(user)->on_layer_closed(); }, this);
        if (auto shown_ok = surface_.show(); !shown_ok) {
            log_notify.error(shown_ok.get_error());
            role_.destroy();
            surface_.destroy();
            return;
        }
        output_ = output_slot;
        overlay_ = overlay;
        want_w_ = so.size.width;
        want_h_ = so.size.height;
        drawn_w_ = drawn_h_ = 0;
        dirty_ = true;
        log_notify.info("notifications: shown ({} card{}, {} layer)", n, n == 1 ? "" : "s",
                        overlay ? "overlay" : "top");
        return;
    }
    if (overlay != overlay_) {
        overlay_ = overlay;
        log_notify.info("notifications: {} layer (a critical one {})", overlay ? "overlay" : "top",
                        overlay ? "shown" : "gone");
        // zwlr_layer_surface_v1 v2: the layer changes with the next commit (not sent below).
        (void)role_.set_layer(overlay ? lx::wayland::layer::overlay : lx::wayland::layer::top);
    }
    relayout();
    if (layout_.width != want_w_ || layout_.height != want_h_)
        apply_size();
}

void lx::shell::notification_view::apply_size() {
    want_w_ = layout_.width;
    want_h_ = layout_.height > 0 ? layout_.height : 1;
    role_.layer().set_size({want_w_, want_h_});
    surface_.commit_state(); // the configure that answers resizes the surface
}

void lx::shell::notification_view::close() {
    if (!is_open())
        return;
    const unsigned frames = static_cast<unsigned>(surface_.stats().frames);
    role_.destroy();
    surface_.destroy();
    output_ = ~0u;
    hover_card_ = hover_part_ = -1;
    pressed_ = {};
    drawn_w_ = drawn_h_ = 0;
    dirty_ = false;
    log_notify.info("notifications: hidden ({} frame{})", frames, frames == 1 ? "" : "s");
}

void lx::shell::notification_view::on_layer_closed() {
    // Its output went: shown again on another one at the next update.
    close();
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
                    const double s = e->scale > 0.0 ? e->scale : 1.0;
                    const int p = lx::text::wrapped_piece_at(
                        e->body, static_cast<int>((x - c.body.x) * s),
                        static_cast<int>((y - c.body.y) * s));
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

void lx::shell::notification_view::on_part_hover(const part_ref& ref, bool hovered) {
    // The pointer's part changed: what is on screen follows (the card's and the buttons'
    // backgrounds ride the engine's hover states; the ×'s colour is ours).
    if (hovered) {
        if (ref.card != hover_card_ || ref.part != hover_part_) {
            hover_card_ = ref.card;
            hover_part_ = ref.part;
            dirty_ = true;
        }
    } else if (ref.card == hover_card_ && ref.part == hover_part_) {
        hover_card_ = -1;
        hover_part_ = -1;
        pressed_ = {};
        dirty_ = true;
    }
    if (input_.hover)
        input_.hover(hover_card_ >= 0, input_.user);
}

void lx::shell::notification_view::on_part_event(const part_ref& ref, const lx::ui::pointer_event& e) {
    pointer_x_ = e.at.x;
    pointer_y_ = e.at.y;
    switch (e.kind) {
    case lx::ui::pointer_kind::press:
        pressed_ = hit_at(e.at.x, e.at.y);
        break;
    case lx::ui::pointer_kind::release:
        if (e.button == lx::ui::mouse_button::left) {
            const hit h = hit_at(e.at.x, e.at.y);
            finish_click(ref.card, h.card, h.part, pressed_.card, pressed_.part, 0x110);
        }
        pressed_ = {};
        break;
    // The engine delivers non-left buttons on their press only: dismiss at once.
    case lx::ui::pointer_kind::secondary:
        finish_click(ref.card, ref.card, ref.part, ref.card, ref.part, 0x111);
        break;
    case lx::ui::pointer_kind::middle:
        finish_click(ref.card, ref.card, ref.part, ref.card, ref.part, 0x112);
        break;
    default:
        break;
    }
}

void lx::shell::notification_view::finish_click(int card, int hit_card, int part, int press_card,
                                                int press_part, unsigned button) {
    if (card < 0 || card != hit_card || card != press_card || part != press_part || !center_)
        return;
    const card_layout& c = layout_.cards[card];
    const notification* n = center_->find(c.id);
    if (!n)
        return;
    notify_click out{};
    out.id = c.id;
    constexpr unsigned k_left = 0x110, k_right = 0x111, k_middle = 0x112;
    if (button == k_right || button == k_middle) {
        out.what = notify_click::kind::dismiss;
    } else if (button != k_left || part < 0) {
        return;
    } else if (part == 1) {
        out.what = notify_click::kind::dismiss;
    } else if (part >= 2 && part < 2 + static_cast<int>(k_card_buttons)) {
        const unsigned b = static_cast<unsigned>(part - 2);
        const int a = c.button_action[b];
        if (a < 0) {
            out.what = notify_click::kind::more;
            out.anchor = c.button[b];
        } else if (static_cast<unsigned>(a) < n->req.actions.size()) {
            out.what = notify_click::kind::invoke;
            out.arg = n->req.actions[static_cast<unsigned>(a)].key;
        }
    } else if (part >= 10) {
        for (const auto& e : cache_)
            if (e->id == c.id) {
                const unsigned piece = static_cast<unsigned>(part - 10);
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
    if (out.what == notify_click::kind::none)
        return;
    if (input_.click)
        input_.click(out, g_.ui ? g_.ui->last_press_serial() : 0u, input_.user);
}

lx::ui::child lx::shell::notification_view::build_ui() {
    // The `draw cards=…` line and each card's parts, as the canvas logged them: the smoke
    // test clicks the parts from these (n_card parses them).
    if (log_notify.enabled(lx::trace::level::debug)) {
        log_notify.debug("notifications: draw cards={} more={} {}x{}", layout_.count, layout_.more,
                         layout_.width, layout_.height);
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

    const float scale = surface_.valid() ? surface_.scale() : 1.f;
    const int icon_px = static_cast<int>(std::lround(k_card_icon * scale));
    lx::ui::child cards[k_max_cards + 1];
    unsigned n = 0;

    for (unsigned i = 0; i < layout_.count && n < k_max_cards; ++i) {
        const card_layout& c = layout_.cards[i];
        const notification* note = center_ ? center_->find(c.id) : nullptr;
        if (!note)
            continue;
        part_ref* const refs = part_refs_ + i * k_parts_per_card;
        lx::ui::child parts[3 + k_card_buttons];
        unsigned p = 0;

        // The critical stripe down the left edge.
        if (c.critical)
            parts[p++] = notify_quad(at({c.box.x, c.box.y + k_card_radius, 4,
                                         c.box.height - 2 * k_card_radius}),
                                     k_critical, 0, {}, 0x6100'0000u + i);
        // The icon: the notification's own image-data resampled, else the resolver's.
        if (c.has_icon) {
            const std::uint32_t* pixels = nullptr;
            card_cache* cache = nullptr;
            for (const auto& e : cache_)
                if (e->id == c.id)
                    cache = e.get();
            if (note->req.image.size > 0 && cache) {
                if (cache->image_px != icon_px) {
                    cache->image.assign(static_cast<std::size_t>(icon_px) *
                                            static_cast<std::size_t>(icon_px),
                                        0u);
                    resample_icon(note->req.image.pixels.data(), note->req.image.size,
                                  cache->image.data(), icon_px);
                    cache->image_px = icon_px;
                }
                pixels = cache->image.data();
            } else if (icons_) {
                pixels = icons_(*note, icon_px, user_);
            }
            if (pixels && surface_.valid()) {
                std::uint8_t rgba[64 * 64 * 4];
                for (int k = 0; k < icon_px * icon_px; ++k) {
                    const std::uint32_t v = pixels[k];
                    rgba[k * 4 + 0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
                    rgba[k * 4 + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                    rgba[k * 4 + 2] = static_cast<std::uint8_t>(v & 0xFF);
                    rgba[k * 4 + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
                }
                if (auto tile = surface_.atlas().insert(
                        lx::gfx::atlas_format::rgba8, static_cast<unsigned>(icon_px),
                        static_cast<unsigned>(icon_px),
                        std::span<const std::uint8_t>{rgba,
                                                      static_cast<std::size_t>(icon_px * icon_px * 4)},
                        static_cast<unsigned>(icon_px * 4))) {
                    // The icon at its rect, the raster at the pixels it lands on (no clip:
                    // at a scale over 1 the raster is the rect's own device pixels).
                    const lx::ui::child image[1] = {
                        lx::ui::image(surface_.atlas(), tile.value(), {icon_px, icon_px},
                                      0x6101'0000u + i)};
                    lx::shell::shell_box_props icon_at{};
                    icon_at.control.layout = at(c.icon);
                    parts[p++] = lx::shell::shell_box(icon_at, image, 0x6101'1000u + i);
                }
            }
        }
        // The summary, one bold line centred in its row, ellipsized at the card's width.
        if (c.summary.width > 0)
            parts[p++] = notify_text_at(c.summary, note->req.summary.c_str(), k_summary_color,
                                        k_summary_font, 0x6102'0000u + i, 0x6102'1000u + i);
        // The close button: dim ×, brighter under the pointer (its disc is the hover quad).
        {
            refs[1] = {this, static_cast<int>(i), 1};
            const bool close_hover = hover_card_ == static_cast<int>(i) && hover_part_ == 1;
            lx::shell::shell_box_props close{};
            close.control.layout = at(c.close).flex_row().items_center().justify_center();
            close.control.interactive = true;
            lx::ui::quad_style disc{};
            disc.background = shell_color(k_button_bg);
            for (float& r : disc.corner_radii)
                r = static_cast<float>(k_card_close / 2);
            close.hover = disc;
            close.notify = [](void* user, bool hovered) {
                static_cast<part_ref*>(user)->view->on_part_hover(*static_cast<part_ref*>(user),
                                                                  hovered);
            };
            close.event = [](void* user, const lx::ui::pointer_event& e) {
                static_cast<part_ref*>(user)->view->on_part_event(*static_cast<part_ref*>(user), e);
            };
            close.user = &refs[1];
            const lx::ui::child x[1] = {notify_text("\xC3\x97", close_hover ? k_summary_color
                                                                           : k_dim_color,
                                                    k_close_font, 0x6103'0000u + i)};
            parts[p++] = lx::shell::shell_box(close, x, 0x6103'1000u + i);
        }
        // The body: the markup as attrs, links coloured, wrapped to the card's lines —
        // at its rect, its wrap width the rect's own, clipped to it as `draw_wrapped` was.
        if (c.body.width > 0) {
            std::vector<lx::text::text_attr> attrs;
            lx::ui::text_block_props t{};
            t.text = note->body.text.c_str();
            t.font = k_body_font;
            t.color = shell_color(k_body_color);
            t.wrap = lx::text::text_wrap::word;
            t.max_lines = k_card_body_lines;
            t.attrs = body_attrs(note->body, attrs);
            t.context = lx::ui::active_ui_resources().text;
            const lx::ui::child wrapped[1] = {lx::ui::text_block(t, 0x6104'0000u + i)};
            lx::shell::shell_box_props body_at{};
            body_at.control.layout = at(c.body).overflow_hidden();
            parts[p++] = lx::shell::shell_box(body_at, wrapped, 0x6104'1000u + i);
        }
        // The card's box: interactive (the body's clicks are its default action), its
        // background the engine's hover state.
        {
            refs[0] = {this, static_cast<int>(i), 0};
            lx::shell::shell_box_props card{};
            card.control.layout = at(c.box);
            card.control.interactive = true;
            lx::ui::quad_style rest{}, hover{};
            rest.background = shell_color(k_card_bg);
            hover.background = shell_color(k_card_hover_bg);
            for (float& r : rest.corner_radii)
                r = static_cast<float>(k_card_radius);
            for (float& r : hover.corner_radii)
                r = static_cast<float>(k_card_radius);
            card.rest = rest;
            card.hover = hover;
            card.notify = [](void* user, bool hovered) {
                static_cast<part_ref*>(user)->view->on_part_hover(*static_cast<part_ref*>(user),
                                                                  hovered);
            };
            card.event = [](void* user, const lx::ui::pointer_event& e) {
                static_cast<part_ref*>(user)->view->on_part_event(*static_cast<part_ref*>(user), e);
            };
            card.user = &refs[0];
            // The buttons above the card's background (their own interactive boxes).
            lx::ui::child inside[3 + k_card_buttons];
            unsigned m = 0;
            for (unsigned b = 0; b < c.buttons; ++b) {
                refs[2 + b] = {this, static_cast<int>(i), 2 + static_cast<int>(b)};
                const int a = c.button_action[b];
                const char* label = a < 0 ? "More\xE2\x80\xA6"
                                          : note->req.actions[static_cast<unsigned>(a)].label.c_str();
                lx::shell::shell_box_props button{};
                button.control.layout = at(c.button[b]).flex_row().items_center().justify_center();
                button.control.interactive = true;
                lx::ui::quad_style rest_b{}, hover_b{};
                rest_b.background = shell_color(k_button_bg);
                hover_b.background = shell_color(k_button_hover);
                for (float& r : rest_b.corner_radii)
                    r = 6.f;
                for (float& r : hover_b.corner_radii)
                    r = 6.f;
                button.rest = rest_b;
                button.hover = hover_b;
                button.notify = [](void* user, bool hovered) {
                    static_cast<part_ref*>(user)->view->on_part_hover(*static_cast<part_ref*>(user),
                                                                      hovered);
                };
                button.event = [](void* user, const lx::ui::pointer_event& e) {
                    static_cast<part_ref*>(user)->view->on_part_event(*static_cast<part_ref*>(user),
                                                                      e);
                };
                button.user = &refs[2 + b];
                const lx::ui::child text[1] = {
                    notify_text(label, k_summary_color, k_button_font, 0x6105'0000u + i * 8 + b)};
                inside[m++] = lx::shell::shell_box(button, text, 0x6105'1000u + i * 8 + b);
            }
            for (unsigned k = 0; k < p; ++k)
                inside[m++] = parts[k];
            cards[n++] = lx::shell::shell_box(card, {inside, m}, 0x6100'1000u + i);
        }
    }

    // The "+n more" line under the waiting ones (a label; the canvas took no clicks on it).
    if (layout_.more > 0) {
        char text[32];
        std::snprintf(text, sizeof(text), "+%u more", layout_.more);
        const lx::ui::child line[1] = {notify_text(text, k_body_color, k_button_font, 0x6200u)};
        lx::ui::quad_style q{};
        q.background = shell_color(k_more_bg);
        for (float& r : q.corner_radii)
            r = static_cast<float>(layout_.more_box.height / 2);
        lx::shell::shell_box_props more{};
        more.control.layout = at(layout_.more_box).flex_row().items_center().justify_center();
        more.rest = q;
        cards[n++] = lx::shell::shell_box(more, line, 0x6201u);
    }

    // The root: nothing over the cards (clicks between them go through — the input region).
    lx::shell::shell_box_props root_props{};
    root_props.control.layout = lx::ui::style{}
                                    .w(lx::layout::percent(100.f))
                                    .h(lx::layout::percent(100.f));
    return lx::shell::shell_box(root_props, {cards, n}, 0x6000u);
}

void lx::shell::notification_view::request_repaint() {
    if (!surface_.valid())
        return;
    surface_.root().set_describer(&notify_describe);
    surface_.root().add_repaint({0, 0, layout_.width, layout_.height});
}

void lx::shell::notification_view::after_dispatch() {
    if (!surface_.valid())
        return;
    // Draw at the size the tree last described; a pending configure redraws at its own size.
    if (dirty_ || layout_.width != drawn_w_ || layout_.height != drawn_h_) {
        // The input region is the cards (clicks between them reach what is beneath).
        if (layout_.width != drawn_w_ || layout_.height != drawn_h_) {
            lx::rect2i input[k_max_cards];
            unsigned n = 0;
            for (unsigned i = 0; i < layout_.count; ++i)
                input[n++] = layout_.cards[i].box;
            surface_.set_input_region({input, n});
            drawn_w_ = layout_.width;
            drawn_h_ = layout_.height;
        }
        request_repaint();
        dirty_ = false;
    }
    // The surface's frame loop: a frame when its frame callback allows one and the ledger
    // has work (our repaint, the engine's hover damage, a configure).
    if (surface_.frame_due()) {
        describing_guard guard{this};
        surface_.run_frame();
    }
}


lx::shell::notification_view::~notification_view() { close(); }
