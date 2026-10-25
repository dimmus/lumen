module;

export module lx.ui:popups;

import std;
import lx.foundation;
import lx.layout;
import lx.a11y;
import lx.runtime;

import :components;
import :control;
import :element;
import :events;
import :focus;
import :icon;
import :interact;
import :invalidate;
import :list;
import :node;
import :overlay;
import :style;
import :styled;
import :theme;

// ── Menus, popovers, modals and toasts (ADR-010 §5.6, on lx.ui:overlay) ──────────────────
//
// Menus (`menu`, crates/ui/src/components/context_menu.rs at zed a5c4ca8): an elevated
// surface (`elevation_2`: §5.2's elevated shadows, a 1 px `border.variant` border, rounded
// `lg` — styled_ext.rs `elevated`), `min_w(px(200.))`, the entries in a `List`
// (`py(Base04)`). An entry is an inset `ListItem` (outer `px(Base04)`, the box `px(Base06)`
// rounded `sm`, hover `ghost_element.hover`), a dense row on the menu's comfortable φ line
// (23 px): an optional `IconSize::Small` icon and the label (`gap_1p5`), the key binding
// `ml_4()` at the end (disabled with the entry). Separators are `ListSeparator`: 1 px
// `border.variant` with `my(Base06)`. A header is a `ListSubHeader`-like small muted label.
// A submenu entry ends in a chevron and opens its menu to the right (on a click, space,
// enter or the right arrow; left or Escape closes it), aligned so its first entry sits beside
// its trigger. The menu takes the focus while it is open (a trap) and the arrow keys move
// between its entries; activating one runs its action and closes the menu chain.
//
// `popover_menu(trigger, entries)` opens below its trigger (Zed's `PopoverMenu` default:
// anchored top-left to the trigger's bottom-left, −5 px for the menu's padding and border),
// flipping above or leftwards at the window's edges; `context_menu(target, entries)` opens at
// the pointer on a secondary press inside `target`. A modal (`modal`, modal.rs and the
// workspace's modal layer) is a `modal_surface` box (§5.2's four shadows, rounded `lg`) 5 rem
// below the window's top, centred; it occludes the window below (nothing under it takes the
// pointer), traps the focus, and Escape or a press outside it dismisses it. As in Zed it draws
// no scrim unless asked (`modal_builder::scrim`, Zed's `fade_out_background`):
// `elevated_surface.background` at 80 % over the window. Toasts (`toast`,
// crates/notifications/src/status_toast.rs, the workspace's toast layer) stack at the bottom
// centre and dismiss themselves after `duration_ms` (Zed's 10 s), the timer paused while the
// pointer is over one (resumed with at least Zed's 800 ms); each is Zed's status toast — the
// surface background, rounded `lg`, the elevation's 1 px `border.variant`, GPUI's `shadow_lg`,
// the icon in its status colour, a square muted close button.
//
// All of them fade in (§5.5: 100 ms for menus and tooltips, 150 ms for modals and toasts).

export namespace lx::ui {

enum class menu_entry_kind : std::uint8_t { item, separator, header, submenu };

/// One entry of a menu. Build them with `menu_item` / `menu_separator` / `menu_header` /
/// `menu_submenu` and the setters.
struct menu_entry {
    menu_entry_kind kind = menu_entry_kind::item;
    const char* text = nullptr;
    lx::ui::icon glyph = lx::ui::icon::circle;
    bool has_icon = false;
    bool is_disabled = false;
    /// −1: not a toggle; 0 / 1: a check mark's slot, empty or checked.
    std::int8_t toggled = -1;
    std::span<const char* const> keystrokes{};
    handler action{};
    int value = 0;
    std::span<const menu_entry> entries{};

    menu_entry& icon(lx::ui::icon i) { glyph = i; has_icon = true; return *this; }
    menu_entry& keys(std::span<const char* const> k) { keystrokes = k; return *this; }
    menu_entry& disabled(bool on = true) { is_disabled = on; return *this; }
    menu_entry& toggle(bool on) { toggled = on ? 1 : 0; return *this; }
    /// What `action` is called with.
    menu_entry& with_value(int v) { value = v; return *this; }
};

[[nodiscard]] menu_entry menu_item(const char* text, handler action = {});
[[nodiscard]] menu_entry menu_separator();
[[nodiscard]] menu_entry menu_header(const char* text);
[[nodiscard]] menu_entry menu_submenu(const char* text, std::span<const menu_entry> entries);

/// A menu's content (the elevated box and its entries): what `popover_menu` and
/// `context_menu` show.
[[nodiscard]] child menu(std::span<const menu_entry> entries, const char* name = nullptr);

/// A popover (a menu, or any content) below its trigger.
class popover_builder {
public:
    popover_builder(child trigger, std::span<const menu_entry> entries)
        : trigger_{trigger}, entries_{entries} {}
    /// Shown instead of a menu (a popover of anything).
    popover_builder& content(child c) { content_ = c; return *this; }
    popover_builder& open(bool on = true) { open_ = on; return *this; }
    /// Dismissed by an outside press, Escape or an activated entry.
    popover_builder& on_dismiss(handler h) { on_dismiss_ = h; return *this; }
    popover_builder& side(overlay_side s) { side_ = s; return *this; }
    popover_builder& align(overlay_align a) { align_ = a; return *this; }
    popover_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    child trigger_ = nullptr;
    std::span<const menu_entry> entries_{};
    child content_ = nullptr;
    bool open_ = false;
    handler on_dismiss_{};
    overlay_side side_ = overlay_side::below;
    overlay_align align_ = overlay_align::start;
    element_key key_ = 0;
};

/// Zed's `PopoverMenu`: `entries` in a menu below `trigger` while `open`.
[[nodiscard]] popover_builder popover_menu(child trigger, std::span<const menu_entry> entries);
/// A popover of `content` below `trigger` while `open` (no focus trap).
[[nodiscard]] popover_builder popover(child trigger, child content);

struct context_menu_props {
    int unused = 0;
};

/// A secondary press inside its first child opens its menu overlay at the pointer.
class context_menu_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;
    bool apply(const context_menu_props& props);
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    bool on_pointer(const pointer_event& event) override;
    /// The menu, open or not.
    [[nodiscard]] overlay_node* menu_overlay() const;
};

/// Zed's `right_click_menu`: `entries` at the pointer on a secondary press in `target`.
[[nodiscard]] child context_menu(child target, std::span<const menu_entry> entries,
                                 handler on_dismiss = {}, element_key key = 0);

class modal_builder {
public:
    explicit modal_builder(child body) : body_{body} {}
    modal_builder& title(const char* text) { title_ = text; return *this; }
    modal_builder& open(bool on = true) { open_ = on; return *this; }
    modal_builder& on_dismiss(handler h) { on_dismiss_ = h; return *this; }
    /// px at the rem (default 448: Zed's modals are `w(rems(28.))` and wider).
    modal_builder& width(float px) { width_ = px; return *this; }
    /// The footer's start and end slots (`ModalFooter`).
    modal_builder& footer(child start, child end) { footer_start_ = start; footer_end_ = end; return *this; }
    /// The header's close button (default on).
    modal_builder& dismiss_button(bool on) { dismiss_button_ = on; return *this; }
    /// Fade the window out under it (Zed's `ModalView::fade_out_background`, default off).
    modal_builder& scrim(bool on = true) { scrim_ = on; return *this; }
    modal_builder& name(const char* a11y) { name_ = a11y; return *this; }
    modal_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    child body_ = nullptr;
    const char* title_ = nullptr;
    bool open_ = false;
    bool dismiss_button_ = true;
    bool scrim_ = false;
    handler on_dismiss_{};
    float width_ = 448.f;
    child footer_start_ = nullptr;
    child footer_end_ = nullptr;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] modal_builder modal(child body);

/// Zed's workspace `DEFAULT_TOAST_DURATION` and `MINIMUM_RESUME_DURATION`.
inline constexpr std::uint32_t k_toast_duration_ms = 10'000;
inline constexpr std::uint32_t k_toast_resume_ms = 800;
inline constexpr std::uint32_t k_timer_toast = k_timer_user + 1;

struct toast_props {
    /// 0: stays until dismissed.
    std::uint32_t duration_ms = k_toast_duration_ms;
    handler on_dismiss{};
    int value = 0;
};

/// A toast's lifetime: dismisses itself (`on_dismiss(value)`) when its timer runs out,
/// paused while hovered. Wraps the toast's box.
class toast_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;
    toast_node();
    bool apply(const toast_props& props);
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    /// `shadow_lg`'s reach.
    [[nodiscard]] int ink_outset() const override;
    void on_timer(std::uint32_t tag) override;
    void on_interaction_changed() override;
    [[nodiscard]] a11y::node accessibility_node() const override;
    /// Dismisses it now (the close button).
    void dismiss();
    [[nodiscard]] bool dismissed() const { return dismissed_; }

private:
    toast_props props_{};
    std::uint64_t remaining_ns_ = 0;
    std::uint64_t deadline_ns_ = 0;
    bool started_ = false;
    bool dismissed_ = false;
};

class toast_builder {
public:
    explicit toast_builder(const char* text) : text_{text} {}
    /// The status triple (§5.1): the icon in its colour, the border its border.
    toast_builder& status(status_kind s) { status_ = s; has_status_ = true; return *this; }
    toast_builder& icon(lx::ui::icon i) { icon_ = i; has_icon_ = true; return *this; }
    toast_builder& duration_ms(std::uint32_t ms) { props_.duration_ms = ms; return *this; }
    toast_builder& on_dismiss(handler h) { props_.on_dismiss = h; return *this; }
    toast_builder& value(int v) { props_.value = v; return *this; }
    toast_builder& dismiss_button(bool on = true) { dismiss_button_ = on; return *this; }
    toast_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    const char* text_ = "";
    toast_props props_{};
    status_kind status_ = status_kind::info;
    lx::ui::icon icon_ = lx::ui::icon::info;
    bool has_status_ = false;
    bool has_icon_ = false;
    bool dismiss_button_ = false;
    element_key key_ = 0;
};

[[nodiscard]] toast_builder toast(const char* text);

/// The toasts, stacked at the window's bottom centre (newest last, at the bottom). No toasts:
/// the layer is closed.
[[nodiscard]] child toast_stack(std::span<const child> toasts, element_key key = 0);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

using namespace lx::ui;

[[nodiscard]] float lx_ui_pp_rem() { return ui_rem_px(); }

/// The menu's dense row: `TextSize::Default` on the comfortable φ line.
[[nodiscard]] int lx_ui_pp_row() { return list_row_height_px(list_item_spacing::dense, lx_ui_pp_rem()); }

[[nodiscard]] control_paint lx_ui_pp_item_paint() {
    control_paint p{};
    p.rest = {paint(tok::ghost_element_background), k_transparent};
    p.hover = {paint(tok::ghost_element_hover), k_transparent};
    p.active = {paint(tok::ghost_element_active), k_transparent};
    for (radius& r : p.corners)
        r = radius::sm;
    return p;
}

child lx_ui_pp_entries(std::span<const menu_entry> entries, const char* name);

[[nodiscard]] child lx_ui_pp_entry(const menu_entry& e) {
    const int row = lx_ui_pp_row();
    switch (e.kind) {
    case menu_entry_kind::separator: {
        // `ListSeparator`: `h_px().w_full().my(Base06).bg(border_variant)`.
        control_props c{};
        c.layout = style{}.h(lx::layout::px(1.f)).w_full().flex_shrink_0().my(sp::b06);
        c.paint = control_paint::fixed({paint(tok::border_variant), k_transparent});
        c.role = lx::a11y::role::separator;
        return control(c, {});
    }
    case menu_entry_kind::header: {
        control_props c{};
        c.layout = style{}.flex_row().items_center().h(lx::layout::px(static_cast<float>(row)))
                       .mx(sp::b04).px(sp::b06);
        const child l = label(e.text).size(label_size::small).color(tok::text_muted).single_line();
        return control(c, {&l, 1u});
    }
    case menu_entry_kind::item:
    case menu_entry_kind::submenu:
    default: break;
    }
    const bool disabled = e.is_disabled;
    const tok text_tok = disabled ? tok::text_disabled : tok::text;
    // context_menu.rs: an entry's icon `Default` (a toggle's check `Accent`), `Muted` when
    // disabled; a submenu entry's icon and chevron `Muted`.
    const tok icon_tok = disabled || e.kind == menu_entry_kind::submenu ? tok::icon_muted : tok::icon;
    const float small = 14.f * lx_ui_pp_rem() / 16.f; // IconSize::Small
    // The label: `h_flex().gap_1p5()` over the icon (or a toggle's check slot) and the label.
    child head[2]{};
    unsigned hn = 0;
    if (e.toggled >= 0) {
        const child check = icon_view(icon::check, small, disabled ? tok::icon_muted : tok::icon_accent);
        control_props slot{};
        slot.layout = style{}.flex_none().size(lx::layout::px(small));
        slot.paint.opacity = e.toggled ? 255 : 0; // `.when(!toggled, invisible)`
        head[hn++] = control(slot, {&check, 1u});
    } else if (e.has_icon) {
        head[hn++] = icon_view(e.glyph, small, icon_tok);
    }
    head[hn++] = label(e.text ? e.text : "").color(text_tok).truncate();
    control_props left{};
    left.layout = style{}.flex_row().items_center().gap(rems_from_px(6.f)).flex_grow(1.f)
                      .flex_shrink(1.f).min_w(lx::layout::px(0.f)).overflow_hidden();
    child parts[4]{};
    unsigned pn = 0;
    parts[pn++] = control(left, {head, hn});
    if (!e.keystrokes.empty()) {
        // `div().ml_4().child(binding.disabled(..))`.
        const child kb = key_binding(e.keystrokes).disabled(disabled);
        control_props wrap{};
        wrap.layout = style{}.flex_row().items_center().flex_none().ml(rems_from_px(16.f));
        parts[pn++] = control(wrap, {&kb, 1u});
    }
    if (e.kind == menu_entry_kind::submenu) {
        parts[pn++] = icon_view(icon::chevron_right, small, tok::icon_muted);
        // The submenu: right of the item, its first entry beside it (the menu's 4 px list
        // padding and 1 px border above it, the item's 4 px inset and the border beside it).
        overlay_props o{};
        o.kind = overlay_kind::menu;
        o.placement.side = overlay_side::right;
        o.placement.align = overlay_align::start;
        o.placement.gap = static_cast<int>(std::lround(4.f * lx_ui_pp_rem() / 16.f)) + 1;
        o.placement.offset = {0, -(static_cast<int>(std::lround(4.f * lx_ui_pp_rem() / 16.f)) + 1)};
        o.trap_focus = true;
        o.fade_ms = static_cast<std::uint16_t>(k_motion_fast_ms);
        parts[pn++] = overlay(o, lx_ui_pp_entries(e.entries, e.text));
    }
    control_props c{};
    // An inset `ListItem`: `px(Base04)` outside, the box `px(Base06).gap_1()` rounded `sm`.
    c.layout = style{}.flex_row().items_center().h(lx::layout::px(static_cast<float>(row)))
                   .mx(sp::b04).px(sp::b06).gap(rems_from_px(4.f)).justify_between();
    c.paint = lx_ui_pp_item_paint();
    c.on_activate = e.action;
    c.value = e.value;
    c.role = lx::a11y::role::menu_item;
    c.name = e.text;
    c.a11y_state = e.toggled == 1 ? static_cast<unsigned>(lx::a11y::state_flag::checked) : 0u;
    c.interactive = true;
    c.focusable = true;
    c.disabled = disabled;
    c.focus_ring = true;
    return control(c, {parts, pn});
}

child lx_ui_pp_entries(std::span<const menu_entry> entries, const char* name) {
    constexpr unsigned k_max = ui_node::k_max_children;
    child items[k_max]{};
    unsigned n = 0;
    for (const menu_entry& e : entries) {
        if (n == k_max)
            break; // a menu of more than 64 entries shows the first 64
        items[n++] = lx_ui_pp_entry(e);
    }
    // `List`: `v_flex().w_full().py(Base04)`; the arrow keys move along it (one focus trap).
    control_props list{};
    list.layout = style{}.flex_col().w_full().py(sp::b04);
    list.nav = nav_axis::vertical;
    list.role = lx::a11y::role::menu;
    list.name = name;
    const child body = control(list, {items, n});
    // `elevation_2`, `min_w(px(200.))`.
    return div(style{}.flex_col().min_w(rems_from_px(200.f)).bg(tok::elevated_surface_background)
                   .border_1(tok::border_variant).rounded(radius::lg)
                   .shadow(elevation::elevated_surface),
               {&body, 1u});
}

} // namespace

lx::ui::menu_entry lx::ui::menu_item(const char* text, handler action) {
    menu_entry e{};
    e.text = text;
    e.action = action;
    return e;
}

lx::ui::menu_entry lx::ui::menu_separator() {
    menu_entry e{};
    e.kind = menu_entry_kind::separator;
    return e;
}

lx::ui::menu_entry lx::ui::menu_header(const char* text) {
    menu_entry e{};
    e.kind = menu_entry_kind::header;
    e.text = text;
    return e;
}

lx::ui::menu_entry lx::ui::menu_submenu(const char* text, std::span<const menu_entry> entries) {
    menu_entry e{};
    e.kind = menu_entry_kind::submenu;
    e.text = text;
    e.entries = entries;
    return e;
}

lx::ui::child lx::ui::menu(std::span<const menu_entry> entries, const char* name) {
    return lx_ui_pp_entries(entries, name);
}

// ── popover ──

lx::ui::popover_builder::operator child() const {
    const bool is_menu = content_ == nullptr;
    overlay_props o{};
    o.kind = is_menu ? overlay_kind::menu : overlay_kind::popover;
    o.open = open_;
    o.placement.side = side_;
    o.placement.align = align_;
    // popover_menu.rs `resolved_offset`: `rems_from_px(5.)` outward for a start or end anchor.
    const int five = static_cast<int>(std::lround(5.f * lx_ui_pp_rem() / 16.f));
    if (side_ == overlay_side::below || side_ == overlay_side::above)
        o.placement.offset = {align_ == overlay_align::start ? -five
                              : align_ == overlay_align::end ? five
                                                             : 0,
                              0};
    o.trap_focus = is_menu;
    o.fade_ms = static_cast<std::uint16_t>(k_motion_fast_ms);
    o.on_dismiss = on_dismiss_;
    const child shown = is_menu ? lx_ui_pp_entries(entries_, nullptr) : content_;
    const child kids[2] = {trigger_, overlay(o, open_ ? shown : nullptr)};
    // The anchor: a box around the trigger alone (the overlay takes no space).
    control_props c{};
    c.layout = style{}.flex_row().items_center().flex_none();
    return control(c, kids, key_);
}

lx::ui::popover_builder lx::ui::popover_menu(child trigger, std::span<const menu_entry> entries) {
    return popover_builder{trigger, entries};
}

lx::ui::popover_builder lx::ui::popover(child trigger, child content) {
    return popover_builder{trigger, {}}.content(content);
}

// ── context menu ──

lx::ui::element_type_id lx::ui::context_menu_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::context_menu_node::type() const { return static_type(); }
const char* lx::ui::context_menu_node::type_name() const { return "context_menu"; }

bool lx::ui::context_menu_node::apply(const context_menu_props&) { return false; }

lx::size2i lx::ui::context_menu_node::measure(lx::layout::constraints c, size_hint hint) const {
    const ui_node* target = child_at(0);
    return target ? target->measure_cached(c, hint) : c.min;
}

lx::size2i lx::ui::context_menu_node::measure_content(lx::layout::constraints c,
                                                      size_hint hint) const {
    const ui_node* target = child_at(0);
    return target ? target->measure_cached(c, hint, sizing_mode::content) : c.min;
}

void lx::ui::context_menu_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    for (unsigned i = 0; i < child_count(); ++i)
        if (ui_node* c = child_at(i))
            c->place(bounds);
}

const lx::layout::flex_style* lx::ui::context_menu_node::flex_item_style() const {
    const ui_node* target = child_at(0);
    return target ? target->flex_item_style() : nullptr;
}

lx::ui::overlay_node* lx::ui::context_menu_node::menu_overlay() const {
    return child_overlay(*this, overlay_kind::menu);
}

bool lx::ui::context_menu_node::on_pointer(const pointer_event& e) {
    if (e.kind != pointer_kind::secondary)
        return false;
    overlay_node* o = menu_overlay();
    return o && o->open_at(e.at);
}

lx::ui::child lx::ui::context_menu(child target, std::span<const menu_entry> entries,
                                   handler on_dismiss, element_key key) {
    overlay_props o{};
    o.kind = overlay_kind::menu;
    o.anchor_point = true;
    o.trap_focus = true;
    o.fade_ms = static_cast<std::uint16_t>(k_motion_fast_ms);
    o.on_dismiss = on_dismiss;
    const child kids[2] = {target, overlay(o, lx_ui_pp_entries(entries, nullptr))};
    return with_children(describe<context_menu_node, context_menu_props>({}, key), kids);
}

// ── modal ──

lx::ui::modal_builder::operator child() const {
    const float rem = lx_ui_pp_rem();
    child parts[3]{};
    unsigned n = 0;
    if (title_ || dismiss_button_) {
        // `ModalHeader`: `h_flex().justify_between().w_full().px(Base12).pt(Base08)
        // .pb(Base04).gap(Base08)`, the headline `Small` (in `text`: Zed's `Headline` ignores
        // the `Muted` ModalHeader gives it), the close button small.
        child head[2]{};
        unsigned hn = 0;
        head[hn++] = headline(title_ ? title_ : "").size(headline_size::small);
        if (dismiss_button_) {
            const handler close = on_dismiss_;
            head[hn++] = icon_button(icon::x).icon_size(icon_size::small).on_press(close).name("Close");
        }
        parts[n++] = div(style{}.flex_row().items_center().justify_between().w_full().flex_none()
                             .min_w(lx::layout::px(0.f)).px(sp::b12).pt(sp::b08).pb(sp::b04)
                             .gap(sp::b08),
                         {head, hn});
    }
    // The body: `v_flex().w_full().flex_1().gap(Base08)`, a padded `Section`.
    parts[n++] = div(style{}.flex_col().w_full().gap(sp::b08).px(sp::b12).py(sp::b08), {&body_, body_ ? 1u : 0u});
    if (footer_start_ || footer_end_) {
        // `ModalFooter`: `h_flex().w_full().p(Base08).justify_between().gap_1().border_t_1()`.
        const child slots[2] = {div(style{}.flex_row(), {&footer_start_, footer_start_ ? 1u : 0u}),
                                div(style{}.flex_row().gap(rems_from_px(4.f)), {&footer_end_, footer_end_ ? 1u : 0u})};
        parts[n++] = div(style{}.flex_row().items_center().w_full().flex_none().p(sp::b08)
                             .justify_between().gap(rems_from_px(4.f)).border_t_1(tok::border_variant),
                         slots);
    }
    control_props dialog{};
    dialog.layout = style{}.flex_col().w_full();
    dialog.role = lx::a11y::role::dialog;
    dialog.name = name_ ? name_ : title_;
    const child body = control(dialog, {parts, n});
    const child box = div(style{}.flex_col().w(rems_from_px(width_))
                              .bg(tok::elevated_surface_background).border_1(tok::border_variant)
                              .rounded(radius::lg).shadow(elevation::modal_surface).overflow_hidden(),
                          {&body, 1u});
    overlay_props o{};
    o.kind = overlay_kind::modal;
    o.open = open_;
    o.placement.side = overlay_side::window_top;
    o.placement.window_top = static_cast<int>(std::lround(5.f * rem)); // `top_20()`
    o.occlude = true;
    o.scrim = scrim_;
    o.trap_focus = true;
    o.fade_ms = static_cast<std::uint16_t>(k_motion_ms);
    o.on_dismiss = on_dismiss_;
    return overlay(o, open_ ? box : nullptr, key_);
}

lx::ui::modal_builder lx::ui::modal(child body) { return modal_builder{body}; }

// ── toast ──

lx::ui::element_type_id lx::ui::toast_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::toast_node::type() const { return static_type(); }
const char* lx::ui::toast_node::type_name() const { return "toast"; }

lx::ui::toast_node::toast_node() { set_flag(node_flag::interactive, true); }

bool lx::ui::toast_node::apply(const toast_props& p) {
    const bool restart = !started_ || p.duration_ms != props_.duration_ms;
    props_ = p;
    if (restart && !dismissed_) {
        started_ = true;
        remaining_ns_ = std::uint64_t{p.duration_ms} * 1'000'000ull;
        if (p.duration_ms) {
            deadline_ns_ = ui_now_ns() + remaining_ns_;
            (void)ui_timers().schedule(*this, k_timer_toast, remaining_ns_);
        } else {
            ui_timers().cancel(*this, k_timer_toast);
        }
    }
    return false;
}

lx::size2i lx::ui::toast_node::measure(lx::layout::constraints c, size_hint hint) const {
    const ui_node* box = child_at(0);
    return box && !dismissed_ ? box->measure_cached(c, hint) : c.min;
}

lx::size2i lx::ui::toast_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    const ui_node* box = child_at(0);
    return box && !dismissed_ ? box->measure_cached(c, hint, sizing_mode::content) : c.min;
}

void lx::ui::toast_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    if (ui_node* box = child_at(0))
        box->place(bounds);
}

namespace {
/// GPUI's `shadow_lg` (Tailwind's `0 10px 15px -3px`, `0 4px 6px -4px`, black at 10 %).
constexpr struct {
    int dy;
    float blur;
    float spread;
} k_lx_ui_shadow_lg[2] = {{10, 15.f, -3.f}, {4, 6.f, -4.f}};
} // namespace

int lx::ui::toast_node::ink_outset() const {
    float reach = 0.f;
    for (const auto& s : k_lx_ui_shadow_lg)
        reach = std::max(reach, 3.f * s.blur + s.spread + static_cast<float>(std::abs(s.dy)));
    return static_cast<int>(std::ceil(reach));
}

void lx::ui::toast_node::emit(draw_scope& out) const {
    if (!visible_ || dismissed_)
        return;
    // status_toast.rs: `.elevation_3(cx)…shadow_lg()` — `shadow_lg` replaces the modal
    // shadows.
    if (const ui_node* box = child_at(0)) {
        const lx::rect2i b = box->bounds();
        const float r = radius_px(radius::lg, ui_rem_px());
        for (const auto& s : k_lx_ui_shadow_lg) {
            shadow_style sh{};
            sh.color = lx::color::rgb(0.f, 0.f, 0.f, 0.1f);
            for (float& c : sh.corner_radii)
                c = r;
            sh.blur_sigma = s.blur;
            sh.spread = s.spread;
            out.shadow({b.x, b.y + s.dy, b.width, b.height}, sh);
        }
    }
    ui_node::emit(out);
}

const lx::layout::flex_style* lx::ui::toast_node::flex_item_style() const {
    const ui_node* box = child_at(0);
    return box ? box->flex_item_style() : nullptr;
}

void lx::ui::toast_node::dismiss() {
    if (dismissed_)
        return;
    dismissed_ = true;
    ui_timers().cancel(*this, k_timer_toast);
    invalidate_layout(*this);
    if (props_.on_dismiss)
        props_.on_dismiss(props_.value);
}

void lx::ui::toast_node::on_timer(std::uint32_t tag) {
    if (tag == k_timer_toast && !has_flag(node_flag::hovered))
        dismiss();
}

void lx::ui::toast_node::on_interaction_changed() {
    if (props_.duration_ms && !dismissed_) {
        const std::uint64_t now = ui_now_ns();
        if (has_flag(node_flag::hovered)) {
            // Paused: what is left waits for the pointer to leave.
            remaining_ns_ = deadline_ns_ > now ? deadline_ns_ - now : 0;
            ui_timers().cancel(*this, k_timer_toast);
        } else {
            remaining_ns_ = std::max<std::uint64_t>(remaining_ns_, std::uint64_t{k_toast_resume_ms} * 1'000'000ull);
            deadline_ns_ = now + remaining_ns_;
            (void)ui_timers().schedule(*this, k_timer_toast, remaining_ns_);
        }
    }
    // A toast's look does not follow the hover: nothing to repaint.
}

lx::a11y::node lx::ui::toast_node::accessibility_node() const {
    return {a11y::role::alert, "", "", "", 0, const_cast<toast_node*>(this)};
}

lx::ui::toast_builder::operator child() const {
    const float rem = lx_ui_pp_rem();
    const status_colors sc = status_tokens(status_);
    child kids[3]{};
    unsigned n = 0;
    if (has_icon_ || has_status_)
        kids[n++] = icon_view(icon_, 14.f * rem / 16.f, has_status_ ? sc.color : tok::icon);
    kids[n++] = label(text_);
    if (dismiss_button_) {
        // The close button calls back like the timer would.
        const handler h = props_.on_dismiss;
        // status_toast.rs: `IconButton::new(.., IconName::Close).shape(Square)
        // .icon_size(Small).icon_color(Muted)`.
        kids[n++] = icon_button(icon::x).shape(icon_button_shape::square).icon_size(icon_size::small)
                        .icon_color(tok::icon_muted).on_press(h).name("Dismiss");
    }
    // status_toast.rs: `h_flex().elevation_3().gap_2().py_1p5().pl_2p5()` and `pr_2p5()` (`pr_1p5()`
    // with a button), `bg(surface_background)`, `shadow_lg()` (drawn by the toast node); the
    // elevation's 1 px `border.variant` (the status shows in the icon's colour only).
    style s = style{}.flex_row().items_center().flex_none().gap(rems_from_px(8.f))
                  .py(rems_from_px(6.f)).pl(rems_from_px(10.f))
                  .pr(rems_from_px(dismiss_button_ ? 6.f : 10.f))
                  .bg(tok::surface_background).rounded(radius::lg)
                  .border_1(tok::border_variant);
    const child box = div(s, {kids, n});
    return with_children(describe<toast_node, toast_props>(props_, key_), {&box, 1u});
}

lx::ui::toast_builder lx::ui::toast(const char* text) { return toast_builder{text}; }

lx::ui::child lx::ui::toast_stack(std::span<const child> toasts, element_key key) {
    overlay_props o{};
    o.kind = overlay_kind::toast;
    o.open = !toasts.empty();
    o.placement.side = overlay_side::window_bottom;
    o.dismiss_outside = false;
    o.dismiss_escape = false;
    o.fade_ms = static_cast<std::uint16_t>(k_motion_ms);
    const child stack = toasts.empty()
                            ? nullptr
                            : div(style{}.flex_col().items_center().gap(sp::b08), toasts);
    return overlay(o, stack, key);
}
