module;

export module lx.ui:components;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.a11y;
import lx.text;

import :control;
import :element;
import :focus;
import :icon;
import :interact;
import :node;
import :overlay;
import :style;
import :styled;
import :text_node;
import :theme;

// ── The first half of ADR-010 §5.6's component set, as Zed ships it ──────────────────────
//
// Every component is a builder: a value that collects options and describes a subtree of
// `control_node`s, `text_block_node`s and `icon_node`s when it is converted to a `child`
// (inside a describe, like `div` or `flex`), so it composes with the decorators
// (`button("Save") | grow()`) and with `div`'s child arrays. One shape across the set:
//
//     button("Save").style(button_style::filled).icon(icon::check).on_press(save)
//     icon_button(icon::x).shape(icon_button_shape::square).on_press(close)
//     toggle_button("Grid").selected(grid).on_toggle(set_grid)   // value: the next state
//     checkbox(toggle_state::selected).label("Wrap lines").on_toggle(set_wrap)
//     toggle_switch(on).label("Wi-Fi").on_toggle(set_wifi)
//     tab("main.rs").selected(true).position(0, 3, 0).on_select(show)  // value: the index
//
// Common options: `.disabled()`, `.selected()`, `.tab_index(i)` (0 tree order, < 0 out of
// the tab sequence), `.name(a11y)` and `.key(k)` (reconcile identity). Handlers are `handler`s
// (a function pointer, a reducer's `bind(host, action)`, a small trivially copyable lambda),
// called with the control's value.
//
// Values: every size, padding, gap, radius, token and state rule is Zed's, from the file of
// crates/ui (zed-industries/zed a5c4ca8) cited at each builder — Zed's `rems_from_px(n)` is
// `rems_from_px(n)` here (n px at the 16 px rem), its `DynamicSpacing::BaseNN` is `sp::bNN`
// (density-dependent, §5.3). Where Lumen departs from Zed it says so: the focus ring is the
// 1 px `border.focused` border on the control's box for every control (§5.5; Zed varies it per
// component), and a hover only changes a background or border, never a label (§5.5; Zed's
// `Transparent` button mutes its label on hover).
//
// Labels and icons draw with `active_ui_resources()` (the app's text engine, glyph cache and
// icon cache). Text colours and fonts are resolved against the active theme when described;
// a theme change describes again.

export namespace lx::ui {

/// Zed's `rems_from_px`: `px` at the default 16 px rem, as a rem length (it scales with the
/// UI font size).
[[nodiscard]] constexpr length rems_from_px(float px) { return lx::layout::rem(px / 16.f); }

// ── label, headline (label/label.rs, label/label_like.rs, styles/typography.rs) ──

/// `LabelSize`: `TextSize::{XSmall, Small, Default, Large}` — 10, 12, 14, 16 px at the rem.
enum class label_size : std::uint8_t { xsmall, small, normal, large };
/// `LineHeightStyle`: `TextLabel` is GPUI's default line height (φ), `UiLabel` 1.
enum class line_height_style : std::uint8_t { text_label, ui_label };

[[nodiscard]] float label_size_px(label_size s, float rem_px = 16.f);

class label_builder {
public:
    explicit label_builder(const char* text) : text_{text} {}
    /// `Color`: a text token (`text` default, `text_muted`, `text_accent`, `error` …).
    label_builder& color(tok t) { color_ = t; return *this; }
    label_builder& size(lx::ui::label_size s) { size_ = s; custom_px_ = 0.f; return *this; }
    /// `LabelSize::Custom`, in px at the rem.
    label_builder& size_px(float px) { custom_px_ = px; return *this; }
    label_builder& bold(bool on = true) { bold_ = on; return *this; }
    label_builder& italic(bool on = true) { italic_ = on; return *this; }
    label_builder& line_height(line_height_style s) { line_height_ = s == line_height_style::ui_label ? 1.f : 0.f; return *this; }
    /// Line height relative to the size.
    label_builder& line_height(float relative) { line_height_ = relative; return *this; }
    /// No wrapping (`whitespace_nowrap`).
    label_builder& single_line(bool on = true) { single_line_ = on; return *this; }
    /// One line, ellipsised at the end (`truncate`).
    label_builder& truncate(bool on = true) { truncate_ = on; return *this; }
    label_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    const char* text_ = "";
    tok color_ = tok::text;
    lx::ui::label_size size_ = lx::ui::label_size::normal;
    float custom_px_ = 0.f;
    /// 0: φ (`TextLabel`).
    float line_height_ = 0.f;
    bool bold_ = false;
    bool italic_ = false;
    bool single_line_ = false;
    bool truncate_ = false;
    element_key key_ = 0;
};

[[nodiscard]] label_builder label(const char* text);

/// `HeadlineSize`: 0.88, 1, 1.125 (default), 1.27, 1.43 rem; line height 1.6 rem.
enum class headline_size : std::uint8_t { xsmall, small, medium, large, xlarge };

class headline_builder {
public:
    explicit headline_builder(const char* text) : text_{text} {}
    headline_builder& size(headline_size s) { size_ = s; return *this; }
    headline_builder& color(tok t) { color_ = t; return *this; }
    headline_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    const char* text_ = "";
    headline_size size_ = headline_size::medium;
    tok color_ = tok::text;
    element_key key_ = 0;
};

[[nodiscard]] headline_builder headline(const char* text);

// ── buttons (button/button_like.rs, button.rs, icon_button.rs, toggle_button.rs) ──

/// `ButtonStyle`. `subtle` (Zed's default) is the ghost button: `ghost_element.*`, invisible
/// until hovered; `ghost` names it as the ADR does.
enum class button_style : std::uint8_t {
    filled,
    tinted,
    outlined,
    outlined_ghost,
    subtle,
    transparent,
    ghost = subtle,
};
/// `TintColor`: the status triple a tinted button takes.
enum class tint_color : std::uint8_t { accent, error, warning, success };
/// `ButtonSize`: 32, 28, 22 (default), 18, 16 px tall.
enum class button_size : std::uint8_t { large, medium, normal, compact, none };
/// `IconSize`: 10, 12, 14, 16 (default), 48 px.
enum class icon_size : std::uint8_t { indicator, xsmall, small, medium, xlarge };
enum class icon_position : std::uint8_t { start, end };
/// `IconButtonShape`: `square` is the icon plus `b02` each side, both ways; `wide` (Zed's
/// default) a button of the size's height and the icon plus the button's padding.
enum class icon_button_shape : std::uint8_t { square, wide };

[[nodiscard]] float button_height_px(button_size s);
[[nodiscard]] float icon_size_px(icon_size s);

class button_builder {
public:
    enum class kind : std::uint8_t { text, icon_only, toggle };

    button_builder(kind k, const char* label, lx::ui::icon id) : kind_{k}, label_{label}, icon_{id} {
        has_icon_ = k == kind::icon_only;
    }

    button_builder& style(button_style s) { style_ = s; return *this; }
    /// A tinted button (`ButtonStyle::Tinted`).
    button_builder& tint(tint_color t) { style_ = button_style::tinted; tint_ = t; return *this; }
    button_builder& size(button_size s) { size_ = s; return *this; }
    /// The icon beside the label (`start_icon` / `end_icon`), or an icon button's icon.
    button_builder& icon(lx::ui::icon id, icon_position at = icon_position::start) {
        icon_ = id;
        has_icon_ = true;
        icon_at_ = at;
        return *this;
    }
    button_builder& icon_size(lx::ui::icon_size s) { icon_size_ = s; return *this; }
    button_builder& icon_color(tok t) { icon_color_ = t; return *this; }
    button_builder& label_size(lx::ui::label_size s) { label_size_ = s; return *this; }
    button_builder& label_color(tok t) { label_color_ = t; return *this; }
    button_builder& shape(icon_button_shape s) { shape_ = s; return *this; }
    button_builder& full_width(bool on = true) { full_width_ = on; return *this; }
    button_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    button_builder& selected(bool on = true) { selected_ = on; return *this; }
    /// Called with 0 on a click, space or enter.
    button_builder& on_press(handler h) { on_press_ = h; return *this; }
    /// A toggle button's: called with the next state (1 on, 0 off).
    button_builder& on_toggle(handler h) { on_press_ = h; return *this; }
    button_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    button_builder& name(const char* a11y) { name_ = a11y; return *this; }
    button_builder& key(element_key k) { key_ = k; return *this; }
    /// A tooltip after the 500 ms hover delay (`Tooltip::text`).
    button_builder& tooltip(const char* text) { tooltip_ = text; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    const char* tooltip_ = nullptr;
    kind kind_ = kind::text;
    const char* label_ = nullptr;
    lx::ui::icon icon_ = lx::ui::icon::circle;
    bool has_icon_ = false;
    icon_position icon_at_ = icon_position::start;
    button_style style_ = button_style::subtle;
    tint_color tint_ = tint_color::accent;
    button_size size_ = button_size::normal;
    lx::ui::icon_size icon_size_ = lx::ui::icon_size::medium;
    tok icon_color_ = tok::icon;
    lx::ui::label_size label_size_ = lx::ui::label_size::normal;
    tok label_color_ = tok::text;
    icon_button_shape shape_ = icon_button_shape::wide;
    bool full_width_ = false;
    bool disabled_ = false;
    bool selected_ = false;
    handler on_press_{};
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] button_builder button(const char* label);
[[nodiscard]] button_builder icon_button(icon id);

/// A tooltip (tooltip.rs): an overlay child a control opens at the pointer when its 500 ms
/// hover delay runs out (the control needs `control_props::hover_intent`; a button's
/// `.tooltip(text)` sets both) and closes when the hover or a press ends. Zed's
/// `tooltip_container`: 8 px right of and 10 px below the pointer (`pl_2().pt_2p5()`), an
/// elevated surface (`elevation_2`: the §5.2 shadows, 1 px `border.variant`; rounded `md`
/// per §5.5 where Zed rounds `lg`) with `py_1().px_2()`, the title in `text` at
/// `TextSize::Default` up to `max_w_72` (288 px) wide, the meta line `Small` and muted. It
/// fades in over 100 ms; the pointer passes through it.
[[nodiscard]] child tooltip(const char* text, const char* meta = nullptr);
/// Zed's toggle button (a `ToggleButtonGroup` entry): selected, it takes the tinted accent
/// style and an accent label; `on_toggle` gets the next state.
[[nodiscard]] button_builder toggle_button(const char* label);

// ── checkbox, switch (toggle.rs) ──

/// `ToggleState`.
enum class toggle_state : std::uint8_t { unselected, indeterminate, selected };

class checkbox_builder {
public:
    explicit checkbox_builder(toggle_state s) : state_{s} {}
    checkbox_builder& label(const char* text) { label_ = text; return *this; }
    checkbox_builder& label_size(lx::ui::label_size s) { label_size_ = s; return *this; }
    checkbox_builder& label_color(tok t) { label_color_ = t; return *this; }
    /// `fill()`: `element.background` behind the box instead of the ghost background.
    checkbox_builder& fill(bool on = true) { filled_ = on; return *this; }
    checkbox_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    /// Called with the next state: 1 checked, 0 unchecked.
    checkbox_builder& on_toggle(handler h) { on_toggle_ = h; return *this; }
    checkbox_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    checkbox_builder& name(const char* a11y) { name_ = a11y; return *this; }
    checkbox_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    toggle_state state_ = toggle_state::unselected;
    const char* label_ = nullptr;
    lx::ui::label_size label_size_ = lx::ui::label_size::normal;
    tok label_color_ = tok::text_muted;
    bool filled_ = false;
    bool disabled_ = false;
    handler on_toggle_{};
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] checkbox_builder checkbox(toggle_state state);
[[nodiscard]] checkbox_builder checkbox(bool checked);

enum class switch_label_position : std::uint8_t { start, end };

class switch_builder {
public:
    explicit switch_builder(bool on) : on_{on} {}
    switch_builder& label(const char* text) { label_ = text; return *this; }
    switch_builder& label_position(switch_label_position p) { label_at_ = p; return *this; }
    switch_builder& label_size(lx::ui::label_size s) { label_size_ = s; return *this; }
    switch_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    /// Called with the next state.
    switch_builder& on_toggle(handler h) { on_toggle_ = h; return *this; }
    switch_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    switch_builder& name(const char* a11y) { name_ = a11y; return *this; }
    switch_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    bool on_ = false;
    const char* label_ = nullptr;
    switch_label_position label_at_ = switch_label_position::end;
    lx::ui::label_size label_size_ = lx::ui::label_size::small;
    bool disabled_ = false;
    handler on_toggle_{};
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

/// Zed's `Switch` (`switch` is a C++ keyword).
[[nodiscard]] switch_builder toggle_switch(bool on);

// ── divider (divider.rs), progress (progress/progress_bar.rs), avatar (avatar.rs) ──

/// `DividerColor`: `border`, `border` at 60 %, `border.variant` (default).
enum class divider_color : std::uint8_t { border, border_faded, border_variant };

class divider_builder {
public:
    explicit divider_builder(bool vertical) : vertical_{vertical} {}
    divider_builder& color(divider_color c) { color_ = c; return *this; }
    /// `inset()`: 6 px in from each end.
    divider_builder& inset(bool on = true) { inset_ = on; return *this; }
    divider_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    bool vertical_ = false;
    bool inset_ = false;
    divider_color color_ = divider_color::border_variant;
    element_key key_ = 0;
};

[[nodiscard]] divider_builder divider();
[[nodiscard]] divider_builder vertical_divider();

class progress_builder {
public:
    progress_builder(float value, float max_value) : value_{value}, max_{max_value} {}
    progress_builder& fg(tok t) { fg_ = t; return *this; }
    progress_builder& bg(tok t) { bg_ = t; return *this; }
    progress_builder& over(tok t) { over_ = t; return *this; }
    progress_builder& name(const char* a11y) { name_ = a11y; return *this; }
    progress_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    float value_ = 0.f;
    float max_ = 1.f;
    tok fg_ = tok::info;
    tok bg_ = tok::background;
    tok over_ = tok::error;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

/// `ProgressBar`: the bar fills `value / max_value` (at least 2 %); past the max it turns
/// `error`.
[[nodiscard]] progress_builder progress_bar(float value, float max_value);

class avatar_builder {
public:
    avatar_builder(const char* initials, lx::gfx::atlas* atlas, lx::gfx::atlas_tile_id image)
        : initials_{initials}, atlas_{atlas}, image_{image} {}
    /// The picture's size in px at the rem (default 16: `rems(1.)`).
    avatar_builder& size(float px) { size_px_ = px; return *this; }
    /// A 1 px ring in `t` around it (`border_color`).
    avatar_builder& border(tok t) { border_ = t; return *this; }
    avatar_builder& name(const char* a11y) { name_ = a11y; return *this; }
    avatar_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    const char* initials_ = nullptr;
    lx::gfx::atlas* atlas_ = nullptr;
    lx::gfx::atlas_tile_id image_{};
    float size_px_ = 16.f;
    tok border_ = tok::none;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

/// A round avatar with `initials` (null or empty: Zed's fallback, the person icon).
[[nodiscard]] avatar_builder avatar(const char* initials);
/// A round avatar showing an RGBA atlas image.
[[nodiscard]] avatar_builder avatar(lx::gfx::atlas& atlas, lx::gfx::atlas_tile_id image);

// ── key bindings (keybinding.rs, keybinding_hint.rs) ──

/// One keystroke as Zed spells it on Linux: modifiers and key joined by '-' ("Ctrl-S").
class key_binding_builder {
public:
    explicit key_binding_builder(std::span<const char* const> keystrokes) : keys_{keystrokes} {}
    /// The key text size in px (default `TextSize::Default`, 14).
    key_binding_builder& size(float px) { size_px_ = px; return *this; }
    key_binding_builder& color(tok t) { color_ = t; return *this; }
    key_binding_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    key_binding_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const char* const> keys_{};
    float size_px_ = 14.f;
    tok color_ = tok::text_muted;
    bool disabled_ = false;
    element_key key_ = 0;
};

[[nodiscard]] key_binding_builder key_binding(std::span<const char* const> keystrokes);

class key_binding_hint_builder {
public:
    key_binding_hint_builder(std::span<const char* const> keystrokes, tok surface)
        : keys_{keystrokes}, surface_{surface} {}
    key_binding_hint_builder& prefix(const char* text) { prefix_ = text; return *this; }
    key_binding_hint_builder& suffix(const char* text) { suffix_ = text; return *this; }
    /// The hint's text size in px (default `TextSize::Small`, 12); the keys are 2 px smaller.
    key_binding_hint_builder& size(float px) { size_px_ = px; return *this; }
    key_binding_hint_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const char* const> keys_{};
    tok surface_ = tok::surface_background;
    const char* prefix_ = nullptr;
    const char* suffix_ = nullptr;
    float size_px_ = 12.f;
    element_key key_ = 0;
};

/// `KeybindingHint`: the keys in a keycap — a rounded box tinted from the `surface` it sits
/// on — with an optional italic prefix and suffix.
[[nodiscard]] key_binding_hint_builder key_binding_hint(std::span<const char* const> keystrokes,
                                                        tok surface);

// ── tabs (tab.rs, tab_bar.rs) ──

/// `TabPosition`: first, a middle tab before / at / after the selected one, last.
enum class tab_position : std::uint8_t { first, middle_before, middle_selected, middle_after, last };

class tab_builder {
public:
    explicit tab_builder(const char* label) : label_{label} {}
    tab_builder& selected(bool on = true) { selected_ = on; return *this; }
    tab_builder& position(tab_position p) { position_ = p; return *this; }
    /// Zed's rule from the tab's index among `count` and the selected index; also sets the
    /// value `on_select` gets and the selection.
    tab_builder& position(unsigned index, unsigned count, unsigned selected_index);
    tab_builder& start_slot(child c) { start_ = c; return *this; }
    tab_builder& end_slot(child c) { end_ = c; return *this; }
    /// Called with the tab's value (its index with `position(index, …)`).
    tab_builder& on_select(handler h) { on_select_ = h; return *this; }
    tab_builder& value(int v) { value_ = v; return *this; }
    tab_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    tab_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); has_tab_index_ = true; return *this; }
    tab_builder& name(const char* a11y) { name_ = a11y; return *this; }
    tab_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    const char* label_ = "";
    bool selected_ = false;
    tab_position position_ = tab_position::first;
    child start_ = nullptr;
    child end_ = nullptr;
    handler on_select_{};
    int value_ = 0;
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    bool has_tab_index_ = false;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] tab_builder tab(const char* label);

class tab_bar_builder {
public:
    explicit tab_bar_builder(std::span<const child> tabs) : tabs_{tabs} {}
    /// Controls before the tabs (`start_children`) and after them (`end_children`).
    tab_bar_builder& start(std::span<const child> c) { start_ = c; return *this; }
    tab_bar_builder& end(std::span<const child> c) { end_ = c; return *this; }
    tab_bar_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const child> tabs_{};
    std::span<const child> start_{};
    std::span<const child> end_{};
    element_key key_ = 0;
};

/// `TabBar`: the tabs in a row the arrow keys move through (one tab stop: the selected tab).
[[nodiscard]] tab_bar_builder tab_bar(std::span<const child> tabs);

// ── panel (workspace/toolbar.rs), status bar (workspace/status_bar.rs) ──

class panel_builder {
public:
    explicit panel_builder(child body) : body_{body} {}
    panel_builder& toolbar_start(std::span<const child> c) { start_ = c; return *this; }
    panel_builder& toolbar_end(std::span<const child> c) { end_ = c; return *this; }
    panel_builder& name(const char* a11y) { name_ = a11y; return *this; }
    panel_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    child body_ = nullptr;
    std::span<const child> start_{};
    std::span<const child> end_{};
    const char* name_ = nullptr;
    element_key key_ = 0;
};

/// A dock panel: a toolbar (Zed's `Toolbar`: `toolbar.background`, a `border.variant` rule
/// below) over a body on `panel.background`. The toolbar is left out when it has no items.
[[nodiscard]] panel_builder panel(child body);

class status_bar_builder {
public:
    status_bar_builder(std::span<const child> left, std::span<const child> right)
        : left_{left}, right_{right} {}
    status_bar_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const child> left_{};
    std::span<const child> right_{};
    element_key key_ = 0;
};

/// `StatusBar`: items at either end on `status_bar.background`; a toolbar for assistive
/// technology whose controls the arrow keys move between.
[[nodiscard]] status_bar_builder status_bar(std::span<const child> left,
                                            std::span<const child> right);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

using namespace lx::ui;

/// The UI family of the active theme (§5.4), else lx.text's.
[[nodiscard]] const char* lx_ui_ui_family() {
    const char* f = active_theme().fonts().ui_family;
    return f && f[0] ? f : lx::text::k_ui_family;
}

/// A text block's props for one run of UI text.
[[nodiscard]] text_block_props lx_ui_text(const char* text, float size_px, tok color,
                                          float line_height, bool bold = false,
                                          bool italic = false, bool wrap = false,
                                          bool ellipsize = false) {
    text_block_props p{};
    p.text = text ? text : "";
    p.font = {lx_ui_ui_family(), size_px * 0.75f,
              bold ? lx::text::weight::bold : lx::text::weight::normal,
              italic ? lx::text::style::italic : lx::text::style::normal};
    p.color = active_theme().color(color);
    p.line_height = line_height;
    p.wrap = wrap ? lx::text::text_wrap::word : lx::text::text_wrap::none;
    p.ellipsize = ellipsize;
    p.max_lines = ellipsize ? 1u : 0u;
    p.context = active_ui_resources().text;
    return p;
}

/// GPUI's default line height, relative (φ, `TextStyle::default`).
[[nodiscard]] float lx_ui_phi_line() {
    const float l = active_theme().fonts().ui_line_height;
    return l > 0.f ? l : k_phi;
}

[[nodiscard]] float lx_ui_rem() { return ui_rem_px(); }

/// An `h_flex()`: a flex row with its items centred across.
[[nodiscard]] style lx_ui_h_flex() { return style{}.flex_row().items_center(); }
/// A `v_flex()`: a flex column.
[[nodiscard]] style lx_ui_v_flex() { return style{}.flex_col(); }

[[nodiscard]] child lx_ui_box(const style& s, const control_paint& p, std::span<const child> kids,
                              element_key key = 0) {
    control_props c{};
    c.layout = s;
    c.paint = p;
    return control(c, kids, key);
}

[[nodiscard]] control_paint lx_ui_rounded(control_paint p, radius r) {
    for (radius& c : p.corners)
        c = r;
    return p;
}

[[nodiscard]] control_paint lx_ui_bordered(control_paint p, std::uint8_t w) {
    for (std::uint8_t& b : p.border_widths)
        b = w;
    return p;
}

[[nodiscard]] tok lx_ui_icon_token(tok text_token) {
    switch (text_token) {
    case tok::text: return tok::icon;
    case tok::text_muted: return tok::icon_muted;
    case tok::text_disabled: return tok::icon_disabled;
    case tok::text_placeholder: return tok::icon_placeholder;
    case tok::text_accent: return tok::icon_accent;
    default: return text_token;
    }
}

[[nodiscard]] status_colors lx_ui_tint(tint_color t) {
    switch (t) {
    case tint_color::error: return status_tokens(status_kind::error);
    case tint_color::warning: return status_tokens(status_kind::warning);
    case tint_color::success: return status_tokens(status_kind::success);
    case tint_color::accent:
    default: return status_tokens(status_kind::info);
    }
}

/// button_like.rs `ButtonStyle::{enabled, hovered, active}` for a layer-less button. A
/// disabled button keeps its enabled look and loses hover and active
/// (`.bg(style.enabled(..))`, `.when(!self.disabled, hover / active)`).
[[nodiscard]] control_paint lx_ui_button_paint(button_style s, tint_color t, bool dark) {
    control_paint p{};
    switch (s) {
    case button_style::filled:
        p.rest = {paint(tok::element_background), k_transparent};
        p.hover = {paint(tok::element_background, 0.5f), k_transparent}; // fade_out(0.5)
        p.active = {paint(tok::element_active), k_transparent};
        break;
    case button_style::tinted: {
        const status_colors c = lx_ui_tint(t);
        p.rest = {paint(c.background), paint(c.border)};
        // `theme.darken(background, 0.05, 0.2)`: HSL lightness − 0.05 light, − 0.2 dark.
        p.hover = {paint_darkened(c.background, dark ? 0.2f : 0.05f), paint(c.border)};
        p.active = p.rest;
        break;
    }
    case button_style::outlined:
        p.rest = {paint(tok::element_background), paint(tok::border_variant)};
        p.hover = {paint(tok::ghost_element_hover), paint(tok::border)};
        p.active = {paint(tok::element_active), paint(tok::border_variant)};
        p = lx_ui_bordered(p, 1);
        break;
    case button_style::outlined_ghost:
        p.rest = {k_transparent, paint(tok::border_variant)};
        p.hover = {paint(tok::ghost_element_hover), paint(tok::border)};
        p.active = {k_transparent, paint(tok::border_variant)};
        p = lx_ui_bordered(p, 1);
        break;
    case button_style::subtle:
        p.rest = {paint(tok::ghost_element_background), k_transparent};
        p.hover = {paint(tok::ghost_element_hover), k_transparent};
        p.active = {paint(tok::ghost_element_active), k_transparent};
        break;
    case button_style::transparent:
        break;
    }
    return lx_ui_rounded(p, radius::sm); // `rounded_sm` on every corner (ButtonLikeRounding::ALL)
}

[[nodiscard]] dim lx_ui_button_padding(button_size s) {
    switch (s) {
    case button_size::large:
    case button_size::medium: return sp::b08;
    case button_size::none: return lx::layout::px(1.f); // `px_px()`
    default: return sp::b04;
    }
}

[[nodiscard]] unsigned lx_ui_a11y(lx::a11y::state_flag f) { return static_cast<unsigned>(f); }

} // namespace

// ── label, headline ──

float lx::ui::label_size_px(label_size s, float rem_px) {
    const float k = rem_px / 16.f;
    switch (s) {
    case label_size::xsmall: return 10.f * k;
    case label_size::small: return 12.f * k;
    case label_size::large: return 16.f * k;
    case label_size::normal:
    default: return 14.f * k;
    }
}

lx::ui::label_builder::operator child() const {
    const float px = custom_px_ > 0.f ? custom_px_ * lx_ui_rem() / 16.f : label_size_px(size_, lx_ui_rem());
    const float lh = line_height_ > 0.f ? line_height_ : lx_ui_phi_line();
    const bool one_line = single_line_ || truncate_;
    return text_block(lx_ui_text(text_, px, color_, lh, bold_, italic_, !one_line, truncate_), key_);
}

lx::ui::label_builder lx::ui::label(const char* text) { return label_builder{text}; }

lx::ui::headline_builder::operator child() const {
    static constexpr float k_rems[5] = {0.88f, 1.f, 1.125f, 1.27f, 1.43f};
    const float rem = lx_ui_rem();
    const float px = k_rems[static_cast<unsigned>(size_)] * rem;
    // `line_height(rems(1.6))`: 1.6 rem whatever the size.
    return text_block(lx_ui_text(text_, px, color_, 1.6f * rem / px, false, false, true), key_);
}

lx::ui::headline_builder lx::ui::headline(const char* text) { return headline_builder{text}; }

// ── buttons ──

float lx::ui::button_height_px(button_size s) {
    switch (s) {
    case button_size::large: return 32.f;
    case button_size::medium: return 28.f;
    case button_size::compact: return 18.f;
    case button_size::none: return 16.f;
    case button_size::normal:
    default: return 22.f;
    }
}

float lx::ui::icon_size_px(icon_size s) {
    switch (s) {
    case icon_size::indicator: return 10.f;
    case icon_size::xsmall: return 12.f;
    case icon_size::small: return 14.f;
    case icon_size::xlarge: return 48.f;
    case icon_size::medium:
    default: return 16.f;
    }
}

lx::ui::button_builder::operator child() const {
    const bool dark = active_theme().appearance_mode() == appearance::dark;
    const bool toggle = kind_ == kind::toggle;
    // `selected_style`: a selected toggle button is `Tinted(Accent)`.
    const button_style look = toggle && selected_ ? button_style::tinted : style_;
    const tint_color tint = toggle && selected_ ? tint_color::accent : tint_;

    control_props c{};
    c.layout = lx_ui_h_flex().flex_none().gap(sp::b04).px(lx_ui_button_padding(size_))
                   .h(rems_from_px(button_height_px(size_)));
    // `full_width()`: `w_full().justify_center()` (a toggle group's entries are; a lone
    // toggle button sizes to its content).
    if (full_width_)
        c.layout.w_full().justify_center();
    if (kind_ == kind::icon_only && shape_ == icon_button_shape::square) {
        // `IconSize::square`: the icon plus `Base02` on each side, both ways.
        const float side = icon_size_px(icon_size_) +
                           2.f * spacing_px(sp::b02, ui_density(), lx_ui_rem());
        c.layout.w(rems_from_px(side * 16.f / lx_ui_rem())).h(rems_from_px(side * 16.f / lx_ui_rem()))
            .justify_center();
    }
    c.paint = lx_ui_button_paint(look, tint, dark);
    c.on_activate = on_press_;
    c.value = toggle ? (selected_ ? 0 : 1) : 0;
    c.tab_index = tab_index_;
    c.role = toggle ? lx::a11y::role::toggle_button : lx::a11y::role::button;
    c.name = name_ ? name_ : label_;
    if (toggle && selected_)
        c.a11y_state = lx_ui_a11y(lx::a11y::state_flag::pressed);
    else if (selected_)
        c.a11y_state = lx_ui_a11y(lx::a11y::state_flag::selected);
    c.interactive = true;
    c.focusable = true;
    c.disabled = disabled_;
    c.selected = selected_;
    c.focus_ring = true;

    // Label and icon colours: disabled, selected (`Color::Selected`, the accent), or as set.
    tok label_tok = label_color_;
    tok icon_tok = icon_color_;
    if (disabled_) {
        label_tok = tok::text_disabled;
        icon_tok = tok::icon_disabled;
    } else if (selected_) {
        label_tok = tok::text_accent;
        icon_tok = tok::icon_accent;
    } else if (toggle) {
        icon_tok = tok::icon_muted; // toggle_button.rs: an unselected entry's icon is muted
    }

    c.hover_intent = tooltip_ != nullptr;
    child kids[4]{};
    unsigned n = 0;
    // toggle_button.rs entries show an `XSmall` icon; a button's icon is its `IconSize`.
    const float icon_px = icon_size_px(toggle ? lx::ui::icon_size::xsmall : icon_size_);
    const child icon_child = has_icon_ ? lx::ui::icon_view(icon_, icon_px * lx_ui_rem() / 16.f, icon_tok)
                                       : nullptr;
    const child text_child =
        label_ ? text_block(lx_ui_text(label_, label_size_px(label_size_, lx_ui_rem()), label_tok,
                                       lx_ui_phi_line()))
               : nullptr;
    if (icon_child && icon_at_ == icon_position::start)
        kids[n++] = icon_child;
    if (text_child)
        kids[n++] = text_child;
    if (icon_child && icon_at_ == icon_position::end)
        kids[n++] = icon_child;
    if (tooltip_)
        kids[n++] = lx::ui::tooltip(tooltip_);
    if (toggle) {
        // The entry's content: `h_flex().w_full().px_2().gap_1p5().justify_center()`.
        const child inner = lx_ui_box(lx_ui_h_flex().flex_grow(1.f).px(rems_from_px(8.f))
                                          .gap(rems_from_px(6.f)).justify_center().flex_none(),
                                      control_paint{}, {kids, n});
        return control(c, {&inner, 1u}, key_);
    }
    return control(c, {kids, n}, key_);
}

lx::ui::button_builder lx::ui::button(const char* label) {
    return button_builder{button_builder::kind::text, label, icon::circle};
}

lx::ui::child lx::ui::tooltip(const char* text, const char* meta) {
    overlay_props o{};
    o.kind = overlay_kind::tooltip;
    o.hit_through = true;
    o.dismiss_outside = true;
    o.dismiss_escape = true;
    o.fade_ms = static_cast<std::uint16_t>(k_motion_fast_ms);
    // `div().pl_2().pt_2p5()` around the box, from the pointer.
    const float rem = lx_ui_rem();
    o.placement.offset = {static_cast<int>(std::lround(8.f * rem / 16.f)),
                          static_cast<int>(std::lround(10.f * rem / 16.f))};
    const style cap = style{}.max_w(rems_from_px(288.f));
    child rows[2]{};
    unsigned n = 0;
    const child title = label(text);
    rows[n++] = div(cap, {&title, 1u});
    if (meta) {
        const child m = label(meta).size(label_size::small).color(tok::text_muted);
        rows[n++] = div(cap, {&m, 1u});
    }
    const child box = div(style{}.flex_col().py(rems_from_px(4.f)).px(rems_from_px(8.f))
                              .bg(tok::elevated_surface_background).border_1(tok::border_variant)
                              .rounded(radius::md).shadow(elevation::elevated_surface),
                          {rows, n});
    return overlay(o, box);
}

lx::ui::button_builder lx::ui::icon_button(icon id) {
    return button_builder{button_builder::kind::icon_only, nullptr, id};
}

lx::ui::button_builder lx::ui::toggle_button(const char* label) {
    return button_builder{button_builder::kind::toggle, label, icon::circle};
}

// ── checkbox (toggle.rs `Checkbox`) ──

lx::ui::checkbox_builder::operator child() const {
    const bool checked = state_ == toggle_state::selected;
    // The clickable row: `h_flex().gap(Base06)`, on_click toggles; focusable here (Zed's
    // checkbox has no tab stop) so it is keyboard-operable, its ring drawn on the box.
    control_props row{};
    row.layout = lx_ui_h_flex().gap(sp::b06);
    row.on_activate = on_toggle_;
    row.value = checked ? 0 : 1;
    row.tab_index = tab_index_;
    row.role = lx::a11y::role::checkbox;
    row.name = name_ ? name_ : label_;
    row.a11y_state = checked ? lx_ui_a11y(lx::a11y::state_flag::checked)
                     : state_ == toggle_state::indeterminate ? lx_ui_a11y(lx::a11y::state_flag::mixed)
                                                             : 0u;
    row.interactive = true;
    row.focusable = true;
    row.disabled = disabled_;
    row.selected = checked;

    // The box: `m_1().size_4().rounded_xs().border_1()`, ghost or element background, border
    // `border` (disabled: `border.variant` over `element.disabled` at 60 %), the border at
    // 70 % while the row is hovered (`group_hover`).
    const tok border_tok = disabled_ ? tok::border_variant : tok::border;
    box_paint rest{filled_ ? paint(tok::element_background) : paint(tok::ghost_element_background),
                   paint(border_tok)};
    if (disabled_)
        rest.bg = paint(tok::element_disabled, 0.6f);
    control_paint bp{};
    bp.rest = rest;
    bp.hover = bp.active = {rest.bg, paint(border_tok, 0.7f)};
    bp = lx_ui_bordered(lx_ui_rounded(bp, radius::xs), 1);
    control_props box{};
    box.layout = style{}.flex_row().flex_none().justify_center().items_center()
                     .m(rems_from_px(4.f)).size(rems_from_px(16.f));
    box.paint = bp;
    box.group_paint = true;
    box.focus_ring = true;
    child mark = nullptr;
    const tok mark_tok = disabled_ ? tok::icon_disabled : tok::icon_accent; // `Color::Selected`
    if (checked)
        mark = icon_view(icon::check, 14.f * lx_ui_rem() / 16.f, mark_tok); // `IconSize::Small`
    else if (state_ == toggle_state::indeterminate)
        mark = icon_view(icon::minus, 14.f * lx_ui_rem() / 16.f, mark_tok); // `IconName::Dash`
    const child box_child = control(box, {&mark, mark ? 1u : 0u});
    // `h_flex().size(Checkbox::container_size() = 20px).justify_center()`.
    const child holder = lx_ui_box(lx_ui_h_flex().size(rems_from_px(20.f)).justify_center(),
                                   control_paint{}, {&box_child, 1u});
    child kids[2] = {holder, nullptr};
    unsigned n = 1;
    if (label_)
        kids[n++] = text_block(lx_ui_text(label_, label_size_px(label_size_, lx_ui_rem()),
                                          disabled_ ? tok::text_disabled : label_color_,
                                          lx_ui_phi_line()));
    return control(row, {kids, n}, key_);
}

lx::ui::checkbox_builder lx::ui::checkbox(toggle_state state) { return checkbox_builder{state}; }
lx::ui::checkbox_builder lx::ui::checkbox(bool checked) {
    return checkbox_builder{checked ? toggle_state::selected : toggle_state::unselected};
}

// ── switch (toggle.rs `Switch`) ──

lx::ui::switch_builder::operator child() const {
    const bool dark = active_theme().appearance_mode() == appearance::dark;
    const float adjust = dark ? 1.f : 1.5f; // `is_light(cx)`: hover tints 1.5× in light themes
    control_props row{};
    row.layout = lx_ui_h_flex().gap(sp::b06);
    row.on_activate = on_toggle_;
    row.value = on_ ? 0 : 1;
    row.tab_index = tab_index_;
    row.role = lx::a11y::role::switch_control;
    row.name = name_ ? name_ : label_;
    row.a11y_state = on_ ? lx_ui_a11y(lx::a11y::state_flag::checked) : 0u;
    row.interactive = true;
    row.focusable = true;
    row.disabled = disabled_;
    row.selected = on_;

    // The switch: `p(px(1)).border_2().border_color(border.transparent).rounded_full()`; its
    // focus ring is the box (Zed turns that 2 px border `border.focused`).
    control_props sw{};
    sw.layout = lx_ui_h_flex().p(lx::layout::px(1.f));
    sw.paint = lx_ui_bordered(lx_ui_rounded(control_paint::fixed({k_transparent,
                                                                   paint(tok::border_transparent)}),
                                            radius::full),
                              2);
    sw.group_paint = true;
    sw.focus_ring = true;

    // The track: `w(Base32).h(Base20).rounded_full().px(Base02).border_1()`; off:
    // `element.disabled` and `border`; on: `info` at 40 % and `text.accent` at 20 %. Hovered,
    // the background takes `text` at 5 % (off) or 16 % (on) on top.
    const paint_color bg = on_ ? paint(tok::info, 0.4f) : paint(tok::element_disabled);
    const paint_color border = on_ ? paint(tok::text_accent, 0.2f) : paint(tok::border);
    const float tint = (on_ ? 0.16f : 0.05f) * adjust;
    control_paint tp{};
    tp.rest = {bg, border};
    if (disabled_)
        tp.hover = tp.active = tp.rest;
    else
        tp.hover = tp.active = {paint_blend(bg.base, tok::text, tint, static_cast<float>(bg.base_alpha) / 255.f),
                                border};
    tp = lx_ui_bordered(lx_ui_rounded(tp, radius::full), 1);
    control_props track{};
    track.layout = lx_ui_h_flex().w(sp::b32).h(sp::b20).px(sp::b02);
    if (on_)
        track.layout.justify_end();
    track.paint = tp;
    track.group_paint = true;

    // The thumb: `size(Base12).rounded_full().bg(text)` at 1 (on), 0.5 (off), 0.2 (disabled).
    control_paint th = lx_ui_rounded(control_paint::fixed({paint(tok::text), k_transparent}),
                                     radius::full);
    th.opacity = alpha_255(disabled_ ? 0.2f : on_ ? 1.f : 0.5f);
    const child thumb = lx_ui_box(style{}.flex_none().size(sp::b12), th, {});
    const child track_child = control(track, {&thumb, 1u});
    const child switch_child = control(sw, {&track_child, 1u});

    child kids[2]{};
    unsigned n = 0;
    const child text_child =
        label_ ? text_block(lx_ui_text(label_, label_size_px(label_size_, lx_ui_rem()),
                                       disabled_ ? tok::text_disabled : tok::text, lx_ui_phi_line()))
               : nullptr;
    if (text_child && label_at_ == switch_label_position::start)
        kids[n++] = text_child;
    kids[n++] = switch_child;
    if (text_child && label_at_ == switch_label_position::end)
        kids[n++] = text_child;
    return control(row, {kids, n}, key_);
}

lx::ui::switch_builder lx::ui::toggle_switch(bool on) { return switch_builder{on}; }

// ── divider ──

lx::ui::divider_builder::operator child() const {
    tok t = tok::border_variant;
    float a = 1.f;
    if (color_ == divider_color::border)
        t = tok::border;
    else if (color_ == divider_color::border_faded) {
        t = tok::border;
        a = 0.6f;
    }
    // Horizontal: `min_w_0().h_px().max_h_px().w_full()`, inset `mx_1p5()`; vertical:
    // `min_w_0().w_px().h_4()`, inset `my_1p5()`.
    style s{};
    if (vertical_) {
        s.min_w(lx::layout::px(0.f)).w(lx::layout::px(1.f)).h(rems_from_px(16.f)).flex_none();
        if (inset_)
            s.my(rems_from_px(6.f));
    } else {
        s.min_w(lx::layout::px(0.f)).h(lx::layout::px(1.f)).max_h(lx::layout::px(1.f)).w_full()
            .flex_shrink_0();
        if (inset_)
            s.mx(rems_from_px(6.f));
    }
    control_props c{};
    c.layout = s;
    c.paint = control_paint::fixed({paint(t, a), k_transparent});
    c.role = lx::a11y::role::separator;
    return control(c, {}, key_);
}

lx::ui::divider_builder lx::ui::divider() { return divider_builder{false}; }
lx::ui::divider_builder lx::ui::vertical_divider() { return divider_builder{true}; }

// ── progress bar ──

lx::ui::progress_builder::operator child() const {
    const float ratio = max_ > 0.f ? value_ / max_ : 0.f;
    const float fill = std::clamp(ratio, 0.02f, 1.f);
    // The track: `w_full().h_2().p_0p5().rounded_full().bg(background)` with a (0, 1) shadow
    // of black at 8 %; the fill: `h_full().rounded_full().w(relative(fill))`, `info`, or
    // `error` past the max.
    control_paint track = lx_ui_rounded(control_paint::fixed({paint(bg_), k_transparent}), radius::full);
    track.shadow_y = 1;
    track.shadow_blur = 0;
    // `gpui::black().opacity(0.08)`: no token is black, so the track's colour darkened to
    // lightness 0, at 8 %.
    track.shadow = {bg_, tok::none, alpha_255(0.08f), 0, 255};
    control_paint bar = lx_ui_rounded(control_paint::fixed({paint(value_ > max_ ? over_ : fg_),
                                                             k_transparent}),
                                      radius::full);
    const child fill_child = lx_ui_box(style{}.h_full().w(lx::layout::percent(fill * 100.f)), bar, {});
    control_props c{};
    c.layout = style{}.flex_row().w_full().h(rems_from_px(8.f)).p(rems_from_px(2.f));
    c.paint = track;
    c.role = lx::a11y::role::progress_bar;
    c.name = name_;
    return control(c, {&fill_child, 1u}, key_);
}

lx::ui::progress_builder lx::ui::progress_bar(float value, float max_value) {
    return progress_builder{value, max_value};
}

// ── avatar ──

lx::ui::avatar_builder::operator child() const {
    const float rem = lx_ui_rem();
    const float bw = border_ != tok::none ? 1.f : 0.f;
    // `size(image_size + border * 2).rounded_full()`, the picture `size_full().rounded_full()`
    // on `element.disabled`; with no picture, `IconName::Person`, muted, `IconSize::Small`.
    control_paint p = lx_ui_rounded(control_paint::fixed({paint(tok::element_disabled),
                                                          border_ != tok::none ? paint(border_)
                                                                               : k_transparent}),
                                    radius::full);
    if (bw > 0.f)
        p = lx_ui_bordered(p, 1);
    control_props c{};
    c.layout = lx_ui_h_flex().justify_center().flex_none()
                   .size(rems_from_px(size_px_ + 2.f * bw)).overflow_hidden();
    c.paint = p;
    c.role = lx::a11y::role::image;
    c.name = name_ ? name_ : initials_;
    child content = nullptr;
    const float px = size_px_ * rem / 16.f;
    if (atlas_ && image_.valid()) {
        content = describe<image_node, image_props>({atlas_, image_, px * 0.5f});
        content = lx_ui_box(style{}.size_full(), control_paint{}, {&content, 1u});
    } else if (initials_ && initials_[0]) {
        // Not in Zed (its avatar always has a picture): initials, muted, at 45 % of the size.
        content = text_block(lx_ui_text(initials_, std::max(px * 0.45f, 6.f), tok::text_muted, 1.f));
    } else {
        content = icon_view(icon::user, 14.f * rem / 16.f, tok::icon_muted);
    }
    return control(c, {&content, 1u}, key_);
}

lx::ui::avatar_builder lx::ui::avatar(const char* initials) {
    return avatar_builder{initials, nullptr, {}};
}

lx::ui::avatar_builder lx::ui::avatar(lx::gfx::atlas& atlas, lx::gfx::atlas_tile_id image) {
    return avatar_builder{nullptr, &atlas, image};
}

// ── key bindings ──

namespace {

/// One `Key`: `h(size).text_size(size).line_height(1)`, a single character `w(size)`
/// centred, longer text `px_0p5()`.
[[nodiscard]] child lx_ui_key(const char* text, float size_px, tok color) {
    const bool single = text && text[0] && !text[1];
    const float rem = lx_ui_rem();
    style s = lx_ui_h_flex().flex_none().h(rems_from_px(size_px * 16.f / rem));
    if (single)
        s.w(rems_from_px(size_px * 16.f / rem)).justify_center();
    else
        s.px(rems_from_px(2.f));
    const child t = text_block(lx_ui_text(text, size_px, color, 1.f));
    return lx_ui_box(s, control_paint{}, {&t, 1u});
}

/// `KeyBinding`: each keystroke `h_flex().flex_none().py_0p5().rounded_xs()`, `Base04` apart.
[[nodiscard]] child lx_ui_keys(std::span<const char* const> keys, float size_px, tok color,
                               element_key key) {
    constexpr unsigned k_max = 8;
    child strokes[k_max]{};
    unsigned n = 0;
    for (const char* k : keys) {
        if (n == k_max)
            break;
        const child one = lx_ui_key(k, size_px, color);
        strokes[n++] = lx_ui_box(lx_ui_h_flex().flex_none().py(rems_from_px(2.f)), control_paint{},
                                 {&one, 1u});
    }
    control_props c{};
    c.layout = lx_ui_h_flex().flex_none().gap(sp::b04);
    c.role = lx::a11y::role::key_shortcut;
    c.name = keys.empty() ? nullptr : keys[0];
    return control(c, {strokes, n}, key);
}

} // namespace

lx::ui::key_binding_builder::operator child() const {
    const float px = size_px_ * lx_ui_rem() / 16.f;
    return lx_ui_keys(keys_, px, disabled_ ? tok::text_disabled : color_, key_);
}

lx::ui::key_binding_builder lx::ui::key_binding(std::span<const char* const> keystrokes) {
    return key_binding_builder{keystrokes};
}

lx::ui::key_binding_hint_builder::operator child() const {
    const bool light = active_theme().appearance_mode() == appearance::light;
    const float rem = lx_ui_rem();
    const float px = size_px_ * rem / 16.f;
    // The cap: `rounded_sm().px_0p5().mr_0p5().border_1()`, border = the surface + `text` at
    // 16 % (8 % light), background = the surface + `text.accent` at 10 % (5 %), a (0, 1)
    // shadow of `text` at 8 % (4 %); the keys `size - 2px`.
    control_paint cap{};
    cap.rest = cap.hover = cap.active = {paint_blend(surface_, tok::text_accent, light ? 0.05f : 0.1f),
                                         paint_blend(surface_, tok::text, light ? 0.08f : 0.16f)};
    cap = lx_ui_bordered(lx_ui_rounded(cap, radius::sm), 1);
    cap.shadow_y = 1;
    cap.shadow = paint(tok::text, light ? 0.04f : 0.08f);
    const child keys = lx_ui_keys(keys_, px - 2.f * rem / 16.f, tok::text_muted, 0);
    const child cap_child = lx_ui_box(lx_ui_h_flex().flex_none().px(rems_from_px(2.f))
                                          .mr(rems_from_px(2.f)),
                                      cap, {&keys, 1u});
    child kids[3]{};
    unsigned n = 0;
    // `h_flex().gap_1()`, italic, `text.disabled`, `TextSize::Small`.
    if (prefix_)
        kids[n++] = text_block(lx_ui_text(prefix_, px, tok::text_disabled, lx_ui_phi_line(), false, true));
    kids[n++] = cap_child;
    if (suffix_)
        kids[n++] = text_block(lx_ui_text(suffix_, px, tok::text_disabled, lx_ui_phi_line(), false, true));
    return lx_ui_box(lx_ui_h_flex().flex_none().gap(rems_from_px(4.f)), control_paint{}, {kids, n}, key_);
}

lx::ui::key_binding_hint_builder lx::ui::key_binding_hint(std::span<const char* const> keystrokes,
                                                          tok surface) {
    return key_binding_hint_builder{keystrokes, surface};
}

// ── tabs ──

lx::ui::tab_builder& lx::ui::tab_builder::position(unsigned index, unsigned count,
                                                   unsigned selected_index) {
    value_ = static_cast<int>(index);
    selected_ = index == selected_index;
    if (index == 0)
        position_ = tab_position::first;
    else if (index + 1 >= count)
        position_ = tab_position::last;
    else if (index < selected_index)
        position_ = tab_position::middle_before;
    else if (index == selected_index)
        position_ = tab_position::middle_selected;
    else
        position_ = tab_position::middle_after;
    return *this;
}

lx::ui::tab_builder::operator child() const {
    const float rem = lx_ui_rem();
    const float container = spacing_px(sp::b32, ui_density(), rem); // `Tab::container_height`
    // tab.rs: the selected tab `tab.active_background` and `text`, the others
    // `tab.inactive_background` and `text.muted`; borders in `border` by position (no hover
    // style: Zed binds `_tab_hover_bg` and does not use it).
    control_paint p = control_paint::fixed({paint(selected_ ? tok::tab_active_background
                                                            : tok::tab_inactive_background),
                                            paint(tok::border)});
    style s = lx_ui_h_flex().flex_none().h(lx::layout::px(container));
    const auto edges = [&](bool l, bool r, bool b) {
        p.border_widths[3] = l ? 1 : 0;
        p.border_widths[1] = r ? 1 : 0;
        p.border_widths[2] = b ? 1 : 0;
    };
    const length one = lx::layout::px(1.f);
    switch (position_) {
    case tab_position::first:
        if (selected_) {
            s.pl(one).pb(one);
            edges(false, true, false);
        } else {
            s.pl(one).pr(one);
            edges(false, false, true);
        }
        break;
    case tab_position::last:
        if (selected_) {
            s.pb(one);
            edges(true, true, false);
        } else {
            s.pl(one);
            edges(false, true, true);
        }
        break;
    case tab_position::middle_selected:
        s.pb(one);
        edges(true, true, false);
        break;
    case tab_position::middle_before:
        s.pr(one);
        edges(true, false, true);
        break;
    case tab_position::middle_after:
        s.pl(one);
        edges(false, true, true);
        break;
    }
    control_props c{};
    c.layout = s;
    c.paint = p;
    c.on_activate = on_select_;
    c.value = value_;
    // One tab stop per bar (the selected tab); arrows move within the bar.
    c.tab_index = has_tab_index_ ? tab_index_ : static_cast<std::int16_t>(selected_ ? 0 : -1);
    c.role = lx::a11y::role::tab;
    c.name = name_ ? name_ : label_;
    c.a11y_state = selected_ ? lx_ui_a11y(lx::a11y::state_flag::selected) : 0u;
    c.interactive = true;
    c.focusable = true;
    c.disabled = disabled_;
    c.selected = selected_;
    c.focus_ring = true;

    // The content: `h_flex().h(content_height = Base32 − 1px).px(Base04).gap(Base04)`, the
    // start slot 12 px square, the end slot 14 px square, both centred.
    const child start = lx_ui_box(lx_ui_h_flex().flex_none().size(rems_from_px(12.f)).justify_center(),
                                  control_paint{}, {&start_, start_ ? 1u : 0u});
    const child end = lx_ui_box(lx_ui_h_flex().flex_none().size(rems_from_px(14.f)).justify_center(),
                                control_paint{}, {&end_, end_ ? 1u : 0u});
    const child text = text_block(lx_ui_text(label_, label_size_px(label_size::normal, rem),
                                             disabled_ ? tok::text_disabled
                                             : selected_ ? tok::text
                                                         : tok::text_muted,
                                             lx_ui_phi_line()));
    const child parts[3] = {start, text, end};
    const child content = lx_ui_box(lx_ui_h_flex().h(lx::layout::px(container - 1.f)).px(sp::b04)
                                        .gap(sp::b04),
                                    control_paint{}, parts);
    return control(c, {&content, 1u}, key_);
}

lx::ui::tab_builder lx::ui::tab(const char* label) { return tab_builder{label}; }

lx::ui::tab_bar_builder::operator child() const {
    const float container = spacing_px(sp::b32, ui_density(), lx_ui_rem());
    // tab_bar.rs: `flex().flex_none().w_full().h(Tab::container_height).bg(tab_bar.background)`;
    // start / end groups `h_flex().flex_none().gap(Base04).px(Base06).border_b_1()` with a
    // `border_r_1` / `border_l_1`; the tabs over a full-size absolute box drawing the bottom
    // rule, in an `h_flex().flex_grow_1()` the arrow keys move along.
    const control_paint group_l = [] {
        control_paint p = control_paint::fixed({k_transparent, paint(tok::border)});
        p.border_widths[2] = 1;
        p.border_widths[1] = 1;
        return p;
    }();
    const control_paint group_r = [] {
        control_paint p = control_paint::fixed({k_transparent, paint(tok::border)});
        p.border_widths[2] = 1;
        p.border_widths[3] = 1;
        return p;
    }();
    control_paint rule = control_paint::fixed({k_transparent, paint(tok::border)});
    rule.border_widths[2] = 1;
    const style group_style = lx_ui_h_flex().flex_none().gap(sp::b04).px(sp::b06);

    control_props row{};
    row.layout = lx_ui_h_flex().flex_grow(1.f);
    row.nav = nav_axis::horizontal;
    row.role = lx::a11y::role::tab_list;
    const child tabs_row = control(row, tabs_);
    const child bottom_rule = lx_ui_box(style{}.absolute().top(lx::layout::px(0.f))
                                            .left(lx::layout::px(0.f)).size_full(),
                                        rule, {});
    const child area_kids[2] = {bottom_rule, tabs_row};
    const child area = lx_ui_box(style{}.flex_row().relative().flex_1().h_full().overflow_hidden(),
                                 control_paint{}, area_kids);
    child kids[3]{};
    unsigned n = 0;
    if (!start_.empty())
        kids[n++] = lx_ui_box(group_style, group_l, start_);
    kids[n++] = area;
    if (!end_.empty())
        kids[n++] = lx_ui_box(group_style, group_r, end_);
    control_props bar{};
    bar.layout = style{}.flex_row().flex_none().w_full().h(lx::layout::px(container));
    bar.paint = control_paint::fixed({paint(tok::tab_bar_background), k_transparent});
    return control(bar, {kids, n}, key_);
}

lx::ui::tab_bar_builder lx::ui::tab_bar(std::span<const child> tabs) { return tab_bar_builder{tabs}; }

// ── panel, status bar ──

lx::ui::panel_builder::operator child() const {
    child kids[2]{};
    unsigned n = 0;
    if (!start_.empty() || !end_.empty()) {
        // toolbar.rs: `v_flex().py(Base06).px(Base08).gap(Base06).border_b_1()
        // .border_color(border.variant).bg(toolbar.background)` over one
        // `h_flex().items_start().justify_between().gap(Base08)` row: the left items
        // `h_flex().min_h_8().flex_auto().justify_start()`, the right `h_flex().h_8()`.
        control_paint tb = control_paint::fixed({paint(tok::toolbar_background), paint(tok::border_variant)});
        tb.border_widths[2] = 1;
        const child left = lx_ui_box(lx_ui_h_flex().min_h(rems_from_px(32.f)).flex_grow(1.f)
                                         .flex_shrink(1.f).justify_start().overflow_hidden()
                                         .gap(sp::b04),
                                     control_paint{}, start_);
        const child right = lx_ui_box(lx_ui_h_flex().h(rems_from_px(32.f)).gap(sp::b04).flex_none(),
                                      control_paint{}, end_);
        const child row_kids[2] = {left, right};
        const child row = lx_ui_box(style{}.flex_row().items_start().justify_between().gap(sp::b08),
                                    control_paint{}, row_kids);
        control_props toolbar{};
        toolbar.layout = lx_ui_v_flex().py(sp::b06).px(sp::b08).gap(sp::b06).flex_none();
        toolbar.paint = tb;
        toolbar.role = lx::a11y::role::toolbar;
        toolbar.nav = nav_axis::horizontal;
        kids[n++] = control(toolbar, {&row, 1u});
    }
    kids[n++] = lx_ui_box(lx_ui_v_flex().flex_1().min_h(lx::layout::px(0.f)).overflow_hidden(),
                          control_paint{}, {&body_, body_ ? 1u : 0u});
    control_props c{};
    c.layout = lx_ui_v_flex().size_full();
    c.paint = control_paint::fixed({paint(tok::panel_background), k_transparent});
    c.role = lx::a11y::role::panel;
    c.name = name_;
    return control(c, {kids, n}, key_);
}

lx::ui::panel_builder lx::ui::panel(child body) { return panel_builder{body}; }

lx::ui::status_bar_builder::operator child() const {
    // status_bar.rs: `h_flex().w_full().justify_between().gap(Base08).p(Base04)
    // .bg(status_bar.background)`, a toolbar whose controls the arrow keys move between; the
    // left tools `h_flex().gap_1().min_w_0().overflow_x_hidden()`, the right
    // `h_flex().flex_shrink_0().gap_1()`.
    const child left = lx_ui_box(lx_ui_h_flex().gap(rems_from_px(4.f)).min_w(lx::layout::px(0.f))
                                     .overflow_hidden(),
                                 control_paint{}, left_);
    const child right = lx_ui_box(lx_ui_h_flex().flex_shrink_0().gap(rems_from_px(4.f)),
                                  control_paint{}, right_);
    const child kids[2] = {left, right};
    control_props c{};
    c.layout = lx_ui_h_flex().w_full().justify_between().gap(sp::b08).p(sp::b04).flex_none();
    c.paint = control_paint::fixed({paint(tok::status_bar_background), k_transparent});
    c.role = lx::a11y::role::status_bar;
    c.name = "Status bar";
    c.nav = nav_axis::horizontal;
    return control(c, kids, key_);
}

lx::ui::status_bar_builder lx::ui::status_bar(std::span<const child> left,
                                              std::span<const child> right) {
    return status_bar_builder{left, right};
}
