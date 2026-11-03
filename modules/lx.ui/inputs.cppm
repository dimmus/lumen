module;

export module lx.ui:inputs;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.a11y;
import lx.text;

import :control;
import :element;
import :events;
import :focus;
import :icon;
import :interact;
import :invalidate;
import :node;
import :popups;
import :style;
import :styled;
import :text_input;
import :text_node;
import :theme;

// ── ADR-013 Phase A: the value-input controls the settings surface needs ─────────────────
//
// The second half of the component set: `slider`, `segmented`, `select`, `stepper` and
// `swatch_row`, built in the same idiom as `:components` — builders that describe
// `control_node` subtrees (Zed's language: `element.*` states, 1 px borders, `rounded_sm` /
// `md`, ghost hovers, the switch's fill and thumb look), from ADR-013 §5.4. They live in
// their own partition beside `:components`, which components.cppm has no room to grow into;
// `:popups` is imported for the select's dropdown.
//
//     slider(0.55, 0.1, 0.9).steps(80).on_change(set_ratio)   // on_change: the step index
//     segmented(words, selected).on_select(set_word)          // on_select: the index
//     select(words, selected).open(open).on_select(pick).on_open(show).on_dismiss(close)
//     drop_down(words, selected).open(open).on_select(pick)   // Zed's dropdown look
//     stepper(buffer, value).range(1, 64).on_change(apply)    // buttons pass +1 / -1
//     spinner(buffer, value).range(1, 64).on_change(apply)    // Zed's number-input look
//     swatch_row(swatches, selected).on_select(pick)
//
// Handlers are `handler`s (the engine's int currency): a slider reports the quantized step
// index, a segmented / select / swatch row the chosen index, a stepper's buttons ±1 — every
// caller reconstructs its own value from the index it gave the builder. Values are owned by
// the app (a reducer); the controls are views, so a re-describe repaints nothing they did
// not change.

export namespace lx::ui {

// ── slider ────────────────────────────────────────────────────────────────────────────────

/// A slider's props (`slider`).
struct slider_props {
    float value = 0.f;
    float min = 0.f;
    float max = 1.f;
    /// The quantization: `on_change` reports `round((value − min) / (max − min) * steps)`,
    /// and the thumb paints the value that index names. `steps` is the number of divisions
    /// — the indices run 0..steps — so 100 gives percent steps. ≥ 1.
    int steps = 100;
    lx::ui::style layout{};
    handler on_change{};
    bool disabled = false;
    std::int16_t tab_index = 0;
    const char* name = nullptr;
    element_key key = 0;
};

class slider_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    /// True when the layout changed; a value or look change damages the bounds.
    bool apply(const slider_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] int focus_order() const override { return tab_index_; }
    bool on_focus_key(key_event event, focus_manager& focus) override;
    bool on_pointer(const pointer_event& event) override;
    [[nodiscard]] a11y::node accessibility_node() const override;

    /// The value the thumb paints: the quantized one (the step index's), in [min, max].
    [[nodiscard]] float painted_value() const { return painted_; }
    /// The step index the last change reported (tests).
    [[nodiscard]] int changes() const { return changes_; }

private:
    /// The step index of `v`, and `v` of `index`.
    [[nodiscard]] int index_of(float v) const;
    [[nodiscard]] float value_of(int index) const;
    /// Sets the value from an x position in local coordinates; reports one change.
    void set_from_x(int x);
    /// Steps by `delta` indices (a wheel notch, an arrow key); reports one change.
    void step(int delta);
    void resolve();

    lx::layout::flex_style flex_{};
    lx::ui::style layout_{};
    float value_ = 0.f;
    float min_ = 0.f;
    float max_ = 1.f;
    float painted_ = 0.f;
    int steps_ = 100;
    int changes_ = 0;
    handler on_change_{};
    std::int16_t tab_index_ = 0;
    bool disabled_ = false;
    bool applied_ = false;
    std::uint64_t generation_ = 0;
    /// The accessible name, copied (props live in the frame arena only).
    char name_[48]{};
    /// The three resolved paints: track, fill (hover), thumb (rest, hover), plus the
    /// disabled fill and thumb.
    packed_quad_style track_{};
    packed_quad_style fill_{};
    packed_quad_style fill_hover_{};
    packed_quad_style thumb_{};
    packed_quad_style thumb_hover_{};
    packed_quad_style fill_disabled_{};
    packed_quad_style thumb_disabled_{};
    float radius_ = 0.f;
    char value_text_[32]{};
};

/// A horizontal slider: Zed's switch look stretched over a 4 px track — `element_disabled`
/// track, `info` at 40 % fill (hovered 60 %), a 12 px `text` thumb, full rounding. Press or
/// drag anywhere sets the value from the pointer; the wheel and the arrow keys step, home
/// and end go to the ends. The style's height is the hit area (default 20 px); the width
/// comes from the row it sits in.
class slider_builder {
public:
    slider_builder(float value, float min, float max) : value_{value}, min_{min}, max_{max} {}
    /// The quantization (default 100: percent steps).
    slider_builder& steps(int n) { steps_ = n; return *this; }
    /// Called with the quantized step index (`0..steps`) on every change.
    slider_builder& on_change(handler h) { on_change_ = h; return *this; }
    /// The item style (default: grow across the row, 20 px tall — the hit area).
    slider_builder& layout(const lx::ui::style& s) { layout_ = s; has_layout_ = true; return *this; }
    slider_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    slider_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    slider_builder& name(const char* a11y) { name_ = a11y; return *this; }
    slider_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    float value_ = 0.f;
    float min_ = 0.f;
    float max_ = 1.f;
    int steps_ = 100;
    lx::ui::style layout_{};
    bool has_layout_ = false;
    handler on_change_{};
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] slider_builder slider(float value, float min, float max);

// ── segmented ─────────────────────────────────────────────────────────────────────────────

/// Zed's `ToggleButtonGroup` as a radio group: the options in a row on an
/// `element_background` container rounded `md`, each segment a padded pill — the selected
/// one tinted accent (the toggle button's selected look) with an accent label, the rest
/// ghost with muted labels. One tab stop (the selected segment); the arrow keys move within
/// the group, space / enter / a click select. `on_select` gets the index.
class segmented_builder {
public:
    segmented_builder(std::span<const char* const> options, int selected)
        : options_{options}, selected_{selected} {}
    segmented_builder& on_select(handler h) { on_select_ = h; return *this; }
    segmented_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    /// The group's tab stop; segments take it by roving (a selected segment's 0, the rest −1).
    segmented_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    segmented_builder& name(const char* a11y) { name_ = a11y; return *this; }
    segmented_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const char* const> options_{};
    int selected_ = 0;
    handler on_select_{};
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] segmented_builder segmented(std::span<const char* const> options, int selected);

// ── select ────────────────────────────────────────────────────────────────────────────────

inline constexpr unsigned k_select_max_options = 32; ///< the dropdown truncates past this

/// A dropdown: the value control (an `element_background` box with a `border_variant`
/// border, rounded `md`, the current word and a chevron) that opens a `popover_menu` of the
/// options below it while `open`. The open state is the app's (a bool in its reducer):
/// `.on_open` fires on a click / space / enter on the control, `.on_dismiss` when the
/// popover closes for any other reason, `.on_select` with the chosen index. Past
/// `k_select_max_options` the list truncates; a longer list wants a popover of a `list`
/// (the displays pane builds one). At most 64 entries render (a menu's cap).
class select_builder {
public:
    select_builder(std::span<const char* const> options, int selected)
        : options_{options}, selected_{selected} {}
    select_builder& open(bool on = true) { open_ = on; return *this; }
    select_builder& on_select(handler h) { on_select_ = h; return *this; }
    select_builder& on_open(handler h) { on_open_ = h; return *this; }
    select_builder& on_dismiss(handler h) { on_dismiss_ = h; return *this; }
    /// Shown instead of the current word when no option is selected (index −1).
    select_builder& placeholder(const char* text) { placeholder_ = text; return *this; }
    select_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    select_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    select_builder& name(const char* a11y) { name_ = a11y; return *this; }
    select_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const char* const> options_{};
    int selected_ = -1;
    bool open_ = false;
    handler on_select_{};
    handler on_open_{};
    handler on_dismiss_{};
    const char* placeholder_ = nullptr;
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] select_builder select(std::span<const char* const> options, int selected);

// ── drop_down ─────────────────────────────────────────────────────────────────────────────

/// Zed's dropdown (`dropdown.rs`): a bordered trigger — the current word and the stacked
/// up / down chevrons — that opens a `popover_menu` of the options below it while `open`,
/// the current one carrying its check mark at the row's right edge. The state and the
/// handlers are `select`'s (`.on_open` on a click / space / enter, `.on_dismiss` when the
/// popover closes for any other reason, `.on_select` with the chosen index); the look is
/// the dropdown's: the trigger hugs its content to a `min_width` floor instead of filling
/// the row. At most 64 entries render (a menu's cap).
class drop_down_builder {
public:
    drop_down_builder(std::span<const char* const> options, int selected)
        : options_{options}, selected_{selected} {}
    drop_down_builder& open(bool on = true) { open_ = on; return *this; }
    drop_down_builder& on_select(handler h) { on_select_ = h; return *this; }
    drop_down_builder& on_open(handler h) { on_open_ = h; return *this; }
    drop_down_builder& on_dismiss(handler h) { on_dismiss_ = h; return *this; }
    /// Shown instead of the current word when no option is selected (index −1).
    drop_down_builder& placeholder(const char* text) { placeholder_ = text; return *this; }
    /// The trigger's width floor, px at the rem (default 150; the content can exceed it).
    drop_down_builder& min_width(float px) { min_width_ = px; return *this; }
    drop_down_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    drop_down_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    drop_down_builder& name(const char* a11y) { name_ = a11y; return *this; }
    drop_down_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const char* const> options_{};
    int selected_ = -1;
    bool open_ = false;
    handler on_select_{};
    handler on_open_{};
    handler on_dismiss_{};
    const char* placeholder_ = nullptr;
    float min_width_ = 150.f;
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] drop_down_builder drop_down(std::span<const char* const> options, int selected);

// ── stepper ───────────────────────────────────────────────────────────────────────────────

/// A number field with − / + buttons: the minus and plus ghosts flank a 64 px text input on
/// the app's `text_buffer`. The buttons report `on_change(−1)` / `on_change(+1)` (the app
/// rewrites the buffer and clamps); a submit reports `on_change(0)` (the app parses it).
/// `range` only disables the buttons at the ends — the value's truth is the buffer's.
class stepper_builder {
public:
    stepper_builder(text_buffer& buffer, int value) : buffer_{&buffer}, value_{value} {}
    stepper_builder& range(int min, int max) {
        min_ = min;
        max_ = max;
        has_range_ = true;
        return *this;
    }
    stepper_builder& on_change(handler h) { on_change_ = h; return *this; }
    stepper_builder& on_submit(handler h) { on_submit_ = h; return *this; }
    stepper_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    stepper_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    stepper_builder& name(const char* a11y) { name_ = a11y; return *this; }
    stepper_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    text_buffer* buffer_ = nullptr;
    int value_ = 0;
    bool has_range_ = false;
    int min_ = 0;
    int max_ = 0;
    handler on_change_{};
    handler on_submit_{};
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] stepper_builder stepper(text_buffer& buffer, int value);

// ── spinner ───────────────────────────────────────────────────────────────────────────────

/// Zed's number input (`number_input.rs`): one bordered box — the − button, a 1 px rule,
/// the value on the app's `text_buffer` (a borderless, editable field), a rule, the +
/// button. The buttons report `on_change(−1)` / `on_change(+1)` (the app rewrites the
/// buffer and clamps); a submit reports `on_change(0)` (the app parses it). `range` only
/// disables the buttons at the ends — the value's truth is the buffer's. The box hugs its
/// content to a 150 px floor.
class spinner_builder {
public:
    spinner_builder(text_buffer& buffer, int value) : buffer_{&buffer}, value_{value} {}
    spinner_builder& range(int min, int max) {
        min_ = min;
        max_ = max;
        has_range_ = true;
        return *this;
    }
    spinner_builder& on_change(handler h) { on_change_ = h; return *this; }
    spinner_builder& on_submit(handler h) { on_submit_ = h; return *this; }
    spinner_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    spinner_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    spinner_builder& name(const char* a11y) { name_ = a11y; return *this; }
    spinner_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    text_buffer* buffer_ = nullptr;
    int value_ = 0;
    bool has_range_ = false;
    int min_ = 0;
    int max_ = 0;
    handler on_change_{};
    handler on_submit_{};
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] spinner_builder spinner(text_buffer& buffer, int value);

// ── swatch ────────────────────────────────────────────────────────────────────────────────

/// One colour chip. Not a token — an arbitrary sRGB colour (a config file's `#RRGGBB`, a
/// display minimap's rectangle), which `control_paint` cannot carry: this node paints one
/// rounded quad.
struct swatch_props {
    lx::color color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    /// The corner radius, px (default 4: `radius::sm`).
    float corner_px = 4.f;
    /// A 1 px `border` ring (a chip on a like-coloured background).
    bool ring = false;
    bool selected = false;
    bool disabled = false;
    lx::ui::style layout{};
    handler on_select{};
    int value = 0;
    std::int16_t tab_index = 0;
    const char* name = nullptr;
    element_key key = 0;
};

class swatch_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;
    bool apply(const swatch_props& props);
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] int focus_order() const override { return props_.tab_index; }
    bool on_focus_key(key_event event, focus_manager& focus) override;
    bool activate() override;
    [[nodiscard]] a11y::node accessibility_node() const override;

private:
    void resolve();

    swatch_props props_{};
    lx::layout::flex_style flex_{};
    packed_quad_style chip_{};
    float radius_ = 0.f;
    bool applied_ = false;
    std::uint64_t generation_ = 0;
};

/// A row of `swatch`es (ADR-013 §5.4): the chips 20 px, `sp::b03` apart, the selected one
/// carrying a `text_accent` ring and drawn with a `check` glyph; hover lifts a ghost chip's
/// ring to `border`. Click / space / enter select. One tab stop (the selected chip).
struct swatch {
    const char* label = nullptr; ///< the accessible name (also the tooltip's text)
    lx::color color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
};

class swatch_row_builder {
public:
    swatch_row_builder(std::span<const swatch> swatches, int selected)
        : swatches_{swatches}, selected_{selected} {}
    swatch_row_builder& on_select(handler h) { on_select_ = h; return *this; }
    swatch_row_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    swatch_row_builder& tab_index(int i) { tab_index_ = static_cast<std::int16_t>(i); return *this; }
    swatch_row_builder& name(const char* a11y) { name_ = a11y; return *this; }
    swatch_row_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    std::span<const swatch> swatches_{};
    int selected_ = -1;
    handler on_select_{};
    bool disabled_ = false;
    std::int16_t tab_index_ = 0;
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] swatch_row_builder swatch_row(std::span<const swatch> swatches, int selected);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────────

namespace {

// The bits of :components' local vocabulary this file needs (they are file-local there).
[[nodiscard]] float lx_in_rem() { return ui_rem_px(); }

/// px at the 16 px rem, as a rem length.
[[nodiscard]] lx::layout::length lx_in_px16(float px) { return lx::layout::rem(px / 16.f); }

[[nodiscard]] lx::ui::style lx_in_h_flex() { return lx::ui::style{}.flex_row().items_center(); }

[[nodiscard]] lx::ui::control_paint lx_in_rounded(lx::ui::control_paint p, radius r) {
    for (radius& c : p.corners)
        c = r;
    return p;
}

[[nodiscard]] lx::ui::control_paint lx_in_bordered(lx::ui::control_paint p, std::uint8_t w) {
    for (std::uint8_t& b : p.border_widths)
        b = w;
    return p;
}

[[nodiscard]] lx::ui::child lx_in_box(const lx::ui::style& s, const lx::ui::control_paint& p,
                                      std::span<const lx::ui::child> kids,
                                      element_key key = 0) {
    lx::ui::control_props c{};
    c.layout = s;
    c.paint = p;
    return control(c, kids, key);
}

[[nodiscard]] text_block_props lx_in_text(const char* text, float size_px, tok color,
                                          float line_height, bool bold = false) {
    text_block_props p{};
    p.text = text ? text : "";
    const char* family = active_theme().fonts().ui_family;
    p.font = {family && family[0] ? family : lx::text::k_ui_family,
              size_px * 0.75f, bold ? lx::text::weight::bold : lx::text::weight::normal,
              lx::text::style::normal};
    p.color = active_theme().color(color);
    p.line_height = line_height;
    p.wrap = lx::text::text_wrap::none;
    p.context = active_ui_resources().text;
    return p;
}

[[nodiscard]] float lx_in_text_px(float px) { return px * lx_in_rem() / 16.f; }

[[nodiscard]] unsigned lx_in_a11y(lx::a11y::state_flag f) {
    return static_cast<unsigned>(f);
}

/// The slider's geometry, in px at the rem: a 20 px hit area, 4 px track, 12 px thumb, 2 px
/// of track padding each side of it.
constexpr float k_sl_hit = 20.f;
constexpr float k_sl_track = 4.f;
constexpr float k_sl_thumb = 12.f;
constexpr float k_sl_pad = 2.f;
constexpr int k_sl_min_w = 96;

/// The stepper's field, px at the rem.
constexpr float k_st_field_w = 64.f;
constexpr float k_st_h = 28.f;

/// The swatch chip and its row, px at the rem.
constexpr float k_sw_chip = 20.f;
constexpr float k_sw_gap = 6.f;

/// One ghost icon square, the stepper's − / + (an icon button's compact look): 24×22,
/// rounded `sm`, ghost hover, the icon 14 px centred. `on_press` gets `value`.
[[nodiscard]] lx::ui::child lx_in_ghost_icon(lx::ui::icon id, int value, handler on_press,
                                             bool disabled, const char* a11y_name) {
    const float rem = lx_in_rem();
    lx::ui::control_props c{};
    c.layout = lx_in_h_flex().flex_none().justify_center()
                   .w(lx_in_px16(24.f)).h(lx_in_px16(22.f));
    lx::ui::control_paint p{};
    p.rest = {paint(tok::ghost_element_background), k_transparent};
    p.hover = {paint(tok::ghost_element_hover), k_transparent};
    p.active = {paint(tok::ghost_element_active), k_transparent};
    c.paint = lx_in_rounded(p, radius::sm);
    c.on_activate = on_press;
    c.value = value;
    c.role = lx::a11y::role::button;
    c.name = a11y_name;
    c.interactive = true;
    c.focusable = true;
    // The field beside it is the row's tab stop; the buttons are pointer / arrow work.
    c.tab_index = -1;
    c.disabled = disabled;
    c.focus_ring = true;
    lx::ui::child glyph = icon_view(id, 14.f * rem / 16.f,
                                    disabled ? tok::icon_disabled : tok::icon_muted);
    return control(c, {&glyph, 1u});
}

} // namespace

// ── slider_node ────────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::slider_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::slider_node::type() const { return static_type(); }
const char* lx::ui::slider_node::type_name() const { return "slider"; }

int lx::ui::slider_node::index_of(float v) const {
    const float span = max_ - min_;
    if (span <= 0.f || steps_ <= 0)
        return 0;
    const float t = std::clamp((v - min_) / span, 0.f, 1.f);
    return static_cast<int>(t * static_cast<float>(steps_) + 0.5f);
}

float lx::ui::slider_node::value_of(int index) const {
    const float span = max_ - min_;
    if (steps_ <= 0)
        return min_;
    const float t = static_cast<float>(std::clamp(index, 0, steps_)) / static_cast<float>(steps_);
    return min_ + span * t;
}

void lx::ui::slider_node::resolve() {
    const theme& t = active_theme();
    const bool disabled = disabled_;
    const float rem = lx_in_rem();
    radius_ = k_sl_track * rem / 16.f * 0.5f;
    const float thumb_radius = k_sl_thumb * rem / 16.f * 0.5f;

    quad_style q{};
    q.corner_radii[0] = q.corner_radii[1] = q.corner_radii[2] = q.corner_radii[3] = radius_;
    q.background = t.color(tok::element_disabled);
    track_ = pack_quad_style(q);

    const paint_color fills[3] = {paint(tok::info, 0.4f), paint(tok::info, 0.6f),
                                  paint(tok::info, 0.15f)};
    const paint_color* fill = disabled ? &fills[2] : &fills[0];
    q.background = t.color(fill->base);
    q.background.a *= static_cast<float>(fill->base_alpha) / 255.f;
    (disabled ? fill_disabled_ : fill_) = pack_quad_style(q);
    if (!disabled) {
        q.background = t.color(fills[1].base);
        q.background.a *= static_cast<float>(fills[1].base_alpha) / 255.f;
        fill_hover_ = pack_quad_style(q);
    }

    const float thumb_alpha = disabled ? 0.2f : 1.f;
    q.corner_radii[0] = q.corner_radii[1] = q.corner_radii[2] = q.corner_radii[3] = thumb_radius;
    q.background = t.color(tok::text);
    q.background.a *= thumb_alpha;
    (disabled ? thumb_disabled_ : thumb_) = pack_quad_style(q);
    if (!disabled) {
        q.background = t.color(tok::text);
        thumb_hover_ = pack_quad_style(q);
    }
}

bool lx::ui::slider_node::apply(const slider_props& p) {
    const std::uint64_t generation = theme_generation();
    const bool new_look = generation != generation_ || !applied_ || p.disabled != disabled_;
    bool layout_changed = false;
    if (new_look || !(p.layout == layout_)) {
        const lx::layout::flex_style flex =
            resolve_layout(p.layout, {ui_density(), ui_rem_px()});
        layout_changed = !(flex == flex_) || !applied_;
        flex_ = flex;
        layout_ = p.layout;
    }
    value_ = std::clamp(p.value, p.min, p.max);
    min_ = p.min;
    max_ = std::max(p.max, p.min);
    steps_ = std::max(p.steps, 1);
    const float painted = value_of(index_of(value_));
    const bool value_changed = painted != painted_;
    painted_ = painted;
    on_change_ = p.on_change;
    tab_index_ = p.tab_index;
    disabled_ = p.disabled;
    std::size_t n = p.name ? std::strlen(p.name) : 0;
    if (n >= sizeof(name_))
        n = sizeof(name_) - 1;
    if (n)
        std::memcpy(name_, p.name, n);
    name_[n] = '\0';
    set_flag(node_flag::interactive, !p.disabled);
    set_flag(node_flag::focusable, !p.disabled);
    set_flag(node_flag::disabled, p.disabled);
    if (new_look) {
        generation_ = generation;
        resolve();
    }
    // The accessible value, the painted one rounded to the decimal the quantization names.
    if (max_ > min_) {
        const float span = max_ - min_;
        const float scale = span <= 10.f ? 100.f : span <= 100.f ? 10.f : 1.f;
        const double shown = std::round(static_cast<double>(painted_) * scale) / scale;
        // `scale` decimals: 100 → "0.55", 10 → "12.5", 1 → "300".
        std::snprintf(value_text_, sizeof(value_text_),
                      scale >= 100.f ? "%.2f" : scale >= 10.f ? "%.1f" : "%.0f", shown);
    } else {
        value_text_[0] = '0';
        value_text_[1] = '\0';
    }
    applied_ = true;
    if (!layout_changed && (new_look || value_changed))
        invalidate_paint(*this);
    return layout_changed;
}

lx::size2i lx::ui::slider_node::measure(lx::layout::constraints c, size_hint) const {
    constexpr int unbounded = lx::layout::constraints{}.max.width;
    const int h = static_cast<int>(k_sl_hit * lx_in_rem() / 16.f);
    lx::size2i s{c.max.width < unbounded ? c.max.width : k_sl_min_w, h};
    s.width = std::clamp(s.width, c.min.width, std::max(c.min.width, c.max.width));
    s.height = std::clamp(s.height, c.min.height, std::max(c.min.height, c.max.height));
    return s;
}

lx::size2i lx::ui::slider_node::measure_content(lx::layout::constraints c,
                                                size_hint hint) const {
    return measure(c, hint);
}

void lx::ui::slider_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
}

const lx::layout::flex_style* lx::ui::slider_node::flex_item_style() const { return &flex_; }

void lx::ui::slider_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    const float scale = lx_in_rem() / 16.f;
    const int hit = std::max(bounds_.height, 1);
    const int track_y = bounds_.y + (hit - static_cast<int>(k_sl_track * scale)) / 2;
    const int track_h = std::max(static_cast<int>(k_sl_track * scale), 2);
    const int pad = static_cast<int>(k_sl_pad * scale);
    const int reach = std::max(bounds_.width - 2 * pad, 1);
    const float t = max_ > min_
                        ? std::clamp((painted_ - min_) / (max_ - min_), 0.f, 1.f)
                        : 0.f;
    const int thumb_x = bounds_.x + pad + static_cast<int>(t * static_cast<float>(reach));
    const int thumb_d = std::max(static_cast<int>(k_sl_thumb * scale), 6);

    const bool hovered = !disabled_ && has_flag(node_flag::hovered);
    const packed_quad_style& fill = disabled_ ? fill_disabled_ : hovered ? fill_hover_ : fill_;
    const packed_quad_style& thumb = disabled_ ? thumb_disabled_ : hovered ? thumb_hover_ : thumb_;
    out.quad({bounds_.x + pad, track_y, reach, track_h}, track_);
    const int fill_w = thumb_x + thumb_d / 2 - (bounds_.x + pad);
    if (fill_w > 0)
        out.quad({bounds_.x + pad, track_y, std::min(fill_w, reach), track_h}, fill);
    const int thumb_y = bounds_.y + (hit - thumb_d) / 2;
    out.quad({thumb_x - thumb_d / 2, thumb_y, thumb_d, thumb_d}, thumb);
    if (!disabled_ && has_flag(node_flag::focused) && has_flag(node_flag::focus_ring)) {
        const float radii[4] = {radius_, radius_, radius_, radius_};
        draw_focus_ring(out, bounds_, radii, 1.f);
    }
}

void lx::ui::slider_node::set_from_x(int x) {
    const float scale = lx_in_rem() / 16.f;
    const int pad = static_cast<int>(k_sl_pad * scale);
    const int reach = std::max(bounds_.width - 2 * pad, 1);
    const float t = static_cast<float>(std::clamp(x - bounds_.x - pad, 0, reach)) /
                    static_cast<float>(reach);
    const float v = min_ + (max_ - min_) * t;
    const int index = index_of(v);
    if (index != index_of(painted_)) {
        painted_ = value_of(index);
        invalidate_paint(*this);
    } else {
        painted_ = value_of(index);
    }
    ++changes_;
    if (on_change_)
        on_change_(index);
}

void lx::ui::slider_node::step(int delta) {
    const int index = std::clamp(index_of(painted_) + delta, 0, std::max(steps_, 1));
    painted_ = value_of(index);
    invalidate_paint(*this);
    ++changes_;
    if (on_change_)
        on_change_(index);
}

bool lx::ui::slider_node::on_focus_key(key_event event, focus_manager&) {
    if (disabled_ || !event.pressed)
        return false;
    switch (event.key) {
    case key::left:
    case key::down:
        step(-1);
        return true;
    case key::right:
    case key::up:
        step(1);
        return true;
    case key::home:
        step(-steps_ - 1);
        return true;
    case key::end:
        step(steps_ + 1);
        return true;
    default:
        return false;
    }
}

bool lx::ui::slider_node::on_pointer(const pointer_event& e) {
    if (disabled_)
        return false;
    switch (e.kind) {
    case pointer_kind::press:
    case pointer_kind::drag: {
        const lx::point2i p = local_point(*this, e.at);
        set_from_x(p.x);
        return true;
    }
    case pointer_kind::release:
        return true;
    case pointer_kind::wheel:
        step(e.dy > 0.f ? -1 : 1);
        return true;
    default:
        return false;
    }
}

lx::a11y::node lx::ui::slider_node::accessibility_node() const {
    unsigned state = 0;
    if (has_flag(node_flag::focused))
        state |= lx_in_a11y(lx::a11y::state_flag::focused);
    if (disabled_)
        state |= lx_in_a11y(lx::a11y::state_flag::disabled);
    return {lx::a11y::role::slider, name_[0] ? name_ : "", "", value_text_, state,
            const_cast<slider_node*>(this)};
}

lx::ui::slider_builder::operator child() const {
    slider_props p{};
    p.value = std::clamp(value_, min_, max_);
    p.min = min_;
    p.max = std::max(max_, min_);
    p.steps = std::max(steps_, 1);
    p.layout = has_layout_ ? layout_
                           : lx::ui::style{}.flex_row().flex_grow(1.f).h(lx_in_px16(k_sl_hit))
                                 .min_w(lx::layout::px(0.f));
    p.on_change = on_change_;
    p.disabled = disabled_;
    p.tab_index = tab_index_;
    p.name = name_;
    return describe<slider_node, slider_props>(p, key_);
}

lx::ui::slider_builder lx::ui::slider(float value, float min, float max) {
    return slider_builder{value, min, max};
}

// ── segmented ──────────────────────────────────────────────────────────────────────────

lx::ui::segmented_builder::operator child() const {
    const unsigned n = std::min<unsigned>(options_.size(), k_select_max_options);
    if (n == 0)
        return nullptr;
    build_context* ctx = build_context::current();
    const auto** kids = ctx ? ctx->alloc_children(n) : nullptr;
    if (!kids)
        return nullptr;
    const bool dark = active_theme().appearance_mode() == appearance::dark;
    for (unsigned i = 0; i < n; ++i) {
        const bool selected = static_cast<int>(i) == selected_;
        lx::ui::control_props seg{};
        seg.layout = lx_in_h_flex().flex_none().justify_center()
                         .px(sp::b08).h(lx_in_px16(24.f))
                         .rounded(radius::sm);
        lx::ui::control_paint p{};
        if (selected && !disabled_) {
            // toggle_button's selected look: the tinted accent (status info) triple.
            const status_colors c = status_tokens(status_kind::info);
            p.rest = {paint(c.background), paint(c.border)};
            p.hover = {paint_darkened(c.background, dark ? 0.2f : 0.05f), paint(c.border)};
            p.active = p.rest;
        } else {
            p.rest = {paint(tok::ghost_element_background), k_transparent};
            p.hover = {paint(tok::ghost_element_hover), k_transparent};
            p.active = {paint(tok::ghost_element_active), k_transparent};
        }
        seg.paint = p;
        seg.on_activate = on_select_;
        seg.value = static_cast<int>(i);
        seg.role = lx::a11y::role::radio_item;
        seg.name = options_[i];
        seg.a11y_state = selected ? lx_in_a11y(lx::a11y::state_flag::selected) : 0u;
        seg.interactive = !disabled_;
        // Roving focus: the selected segment holds the group's one tab stop.
        seg.tab_index = disabled_ ? -1 : (selected ? tab_index_ : -1);
        seg.focusable = !disabled_;
        seg.disabled = disabled_;
        seg.focus_ring = true;
        const char* word = options_[i];
        lx::ui::child text = text_block(lx_in_text(
            word, lx_in_text_px(12.f),
            disabled_ ? tok::text_disabled : selected ? tok::text_accent : tok::text_muted,
            1.f, selected));
        kids[i] = control(seg, {&text, 1u});
    }
    // The container: `element.background` rounded `md`, `p(Base02)`, the arrows move within.
    lx::ui::control_props group{};
    group.layout = lx_in_h_flex().flex_none().p(lx_in_px16(2.f));
    group.paint = lx_in_rounded(
        lx::ui::control_paint::fixed({paint(tok::element_background), k_transparent}),
        radius::md);
    group.nav = nav_axis::horizontal;
    group.role = lx::a11y::role::radio_group;
    group.name = name_;
    group.disabled = disabled_;
    group.tab_index = tab_index_;
    return control(group, {kids, n}, key_);
}

lx::ui::segmented_builder lx::ui::segmented(std::span<const char* const> options, int selected) {
    return segmented_builder{options, selected};
}

// ── select ─────────────────────────────────────────────────────────────────────────────

lx::ui::select_builder::operator child() const {
    const unsigned n = std::min<unsigned>(options_.size(), k_select_max_options);
    const bool dark = active_theme().appearance_mode() == appearance::dark;
    const float rem = lx_in_rem();
    const bool has_selection = selected_ >= 0 && static_cast<unsigned>(selected_) < n;
    const char* current = has_selection ? options_[static_cast<unsigned>(selected_)] : nullptr;

    lx::ui::control_props trigger{};
    trigger.layout = lx_in_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                         .justify_between().h(lx_in_px16(k_st_h)).px(sp::b08);
    lx::ui::control_paint p{};
    p.rest = {paint(tok::element_background), paint(tok::border_variant)};
    p.hover = {paint(tok::element_background), paint(tok::border)};
    p.active = {paint(tok::element_active), paint(tok::border_variant)};
    trigger.paint = lx_in_rounded(lx_in_bordered(p, 1), radius::md);
    trigger.on_activate = on_open_;
    trigger.tab_index = disabled_ ? -1 : tab_index_;
    trigger.role = lx::a11y::role::button;
    trigger.name = name_ ? name_ : (current ? current : placeholder_);
    trigger.a11y_state = open_ ? lx_in_a11y(lx::a11y::state_flag::expanded) : 0u;
    trigger.interactive = !disabled_;
    trigger.focusable = !disabled_;
    trigger.disabled = disabled_;
    trigger.focus_ring = true;
    lx::ui::child words[2]{};
    words[0] = text_block(lx_in_text(
        current ? current : (placeholder_ ? placeholder_ : ""),
        lx_in_text_px(12.f),
        disabled_ ? tok::text_disabled : current ? tok::text : tok::text_placeholder, 1.f));
    lx::ui::child chevron =
        icon_view(open_ ? lx::ui::icon::chevron_up : lx::ui::icon::chevron_down,
                  14.f * rem / 16.f, disabled_ ? tok::icon_disabled : tok::icon_muted);
    lx::ui::child trigger_child = control(trigger, words);

    if (n == 0)
        return trigger_child;

    // The dropdown: one checked entry per word, `on_select` with the index. The array lives
    // on this describe's stack; the popover consumes it synchronously.
    const unsigned listed = std::min<unsigned>(n, 64); // a menu's truncation cap
    std::array<menu_entry, 64> entries{};
    for (unsigned i = 0; i < listed; ++i) {
        entries[i] = menu_item(options_[i], on_select_);
        entries[i].toggle(static_cast<int>(i) == selected_);
        entries[i].with_value(static_cast<int>(i));
    }
    popover_builder drop = popover_menu(trigger_child,
                                        std::span<const menu_entry>{entries.data(), listed});
    drop.open(open_ && !disabled_);
    drop.on_dismiss(on_dismiss_);
    drop.key(key_);
    // The control, its trigger and its dropdown in one column so the popover tracks it.
    lx::ui::control_props wrap{};
    wrap.layout = lx::ui::style{}.flex_col().flex_grow(1.f).flex_shrink(1.f)
                      .min_w(lx::layout::px(0.f));
    lx::ui::child drop_child = drop;
    lx::ui::child kids[2] = {trigger_child, drop_child};
    return control(wrap, {kids, 2u}, key_);
}

lx::ui::select_builder lx::ui::select(std::span<const char* const> options, int selected) {
    return select_builder{options, selected};
}

// ── drop_down ──────────────────────────────────────────────────────────────────────────

lx::ui::drop_down_builder::operator child() const {
    const unsigned n = std::min<unsigned>(options_.size(), k_select_max_options);
    const float rem = lx_in_rem();
    const bool has_selection = selected_ >= 0 && static_cast<unsigned>(selected_) < n;
    const char* current = has_selection ? options_[static_cast<unsigned>(selected_)] : nullptr;

    // The trigger: a bordered box the content sizes (to the `min_width` floor), the word
    // left and the stacked chevrons right.
    lx::ui::control_props trigger{};
    trigger.layout = lx_in_h_flex().flex_none().min_w(lx_in_px16(min_width_))
                         .justify_between().h(lx_in_px16(26.f)).px(sp::b08).gap(sp::b04);
    lx::ui::control_paint p{};
    p.rest = {paint(tok::element_background), paint(tok::border_variant)};
    p.hover = {paint(tok::element_background), paint(tok::border)};
    p.active = {paint(tok::element_active), paint(tok::border_variant)};
    trigger.paint = lx_in_rounded(lx_in_bordered(p, 1), radius::md);
    trigger.on_activate = on_open_;
    trigger.tab_index = disabled_ ? -1 : tab_index_;
    trigger.role = lx::a11y::role::button;
    trigger.name = name_ ? name_ : (current ? current : placeholder_);
    trigger.a11y_state = open_ ? lx_in_a11y(lx::a11y::state_flag::expanded) : 0u;
    trigger.interactive = !disabled_;
    trigger.focusable = !disabled_;
    trigger.disabled = disabled_;
    trigger.focus_ring = true;
    lx::ui::child word = text_block(lx_in_text(
        current ? current : (placeholder_ ? placeholder_ : ""),
        lx_in_text_px(13.f),
        disabled_ ? tok::text_disabled : current ? tok::text : tok::text_placeholder, 1.f));
    // The dropdown glyph: the up and down chevrons stacked, Zed's `ChevronUpDown`.
    lx::ui::child chevrons_kids[2] = {
        icon_view(lx::ui::icon::chevron_up, 9.f * rem / 16.f,
                  disabled_ ? tok::icon_disabled : tok::icon_muted),
        icon_view(lx::ui::icon::chevron_down, 9.f * rem / 16.f,
                  disabled_ ? tok::icon_disabled : tok::icon_muted),
    };
    lx::ui::control_props chevrons{};
    chevrons.layout = lx::ui::style{}.flex_none().flex_col().justify_center().items_center();
    lx::ui::child chevrons_child = control(chevrons, {chevrons_kids, 2u});
    lx::ui::child trigger_parts[2] = {word, chevrons_child};
    lx::ui::child trigger_child = control(trigger, {trigger_parts, 2u});

    if (n == 0)
        return trigger_child;

    // The dropdown: one entry per word, the check at the row's right edge, `on_select` with
    // the index. The array lives on this describe's stack; the popover consumes it at once.
    const unsigned listed = std::min<unsigned>(n, 64); // a menu's truncation cap
    std::array<menu_entry, 64> entries{};
    for (unsigned i = 0; i < listed; ++i) {
        entries[i] = menu_item(options_[i], on_select_);
        entries[i].toggle(static_cast<int>(i) == selected_);
        entries[i].check_end();
        entries[i].with_value(static_cast<int>(i));
    }
    popover_builder drop = popover_menu(trigger_child,
                                        std::span<const menu_entry>{entries.data(), listed});
    drop.open(open_ && !disabled_);
    drop.on_dismiss(on_dismiss_);
    drop.key(key_);
    // The control, its trigger and its dropdown in one column so the popover tracks it.
    lx::ui::control_props wrap{};
    wrap.layout = lx::ui::style{}.flex_col().items_start().flex_none();
    lx::ui::child drop_child = drop;
    lx::ui::child kids[2] = {trigger_child, drop_child};
    return control(wrap, {kids, 2u}, key_);
}

lx::ui::drop_down_builder lx::ui::drop_down(std::span<const char* const> options, int selected) {
    return drop_down_builder{options, selected};
}

// ── stepper ────────────────────────────────────────────────────────────────────────────

lx::ui::stepper_builder::operator child() const {
    const bool at_min = has_range_ && value_ <= min_;
    const bool at_max = has_range_ && value_ >= max_;
    lx::ui::child minus = lx_in_ghost_icon(lx::ui::icon::minus, -1, on_change_,
                                           disabled_ || at_min, "Decrease");
    text_input_builder field = text_input().buffer(*buffer_)
                                  .layout(lx_in_h_flex().flex_none().w(lx_in_px16(k_st_field_w))
                                              .h(lx_in_px16(k_st_h)))
                                  .max_bytes(16)
                                  .on_submit(on_submit_)
                                  .disabled(disabled_)
                                  .tab_index(tab_index_)
                                  .name(name_);
    lx::ui::child field_child = field;
    lx::ui::child plus = lx_in_ghost_icon(lx::ui::icon::plus, 1, on_change_,
                                          disabled_ || at_max, "Increase");
    lx::ui::control_props row{};
    row.layout = lx_in_h_flex().flex_none().gap(sp::b02);
    row.role = lx::a11y::role::spin_button;
    row.name = name_;
    lx::ui::child kids[3] = {minus, field_child, plus};
    return control(row, {kids, 3u}, key_);
}

lx::ui::stepper_builder lx::ui::stepper(text_buffer& buffer, int value) {
    return stepper_builder{buffer, value};
}

// ── spinner ────────────────────────────────────────────────────────────────────────────

lx::ui::spinner_builder::operator child() const {
    const bool at_min = has_range_ && value_ <= min_;
    const bool at_max = has_range_ && value_ >= max_;
    lx::ui::child minus = lx_in_ghost_icon(lx::ui::icon::minus, -1, on_change_,
                                           disabled_ || at_min, "Decrease");
    text_input_builder field = text_input().buffer(*buffer_)
                                  .framed(false)
                                  .layout(lx_in_h_flex().flex_none().min_w(lx::layout::px(0.f))
                                              .h(lx_in_px16(k_st_h)))
                                  .max_bytes(16)
                                  .on_submit(on_submit_)
                                  .disabled(disabled_)
                                  .tab_index(tab_index_)
                                  .name(name_);
    lx::ui::child field_child = field;
    // The value centred between the rules: the field hugs its text, a growing container
    // centres it (number_input.rs centres the value).
    lx::ui::control_props centre{};
    centre.layout = lx_in_h_flex().flex_grow(1.f).flex_shrink(1.f)
                        .min_w(lx::layout::px(0.f)).justify_center();
    lx::ui::child field_box = control(centre, {&field_child, 1u});
    lx::ui::child plus = lx_in_ghost_icon(lx::ui::icon::plus, 1, on_change_,
                                          disabled_ || at_max, "Increase");
    // The rules between the cells: 1 px, the variant border, 20 px tall, centred.
    lx::ui::control_props rule{};
    rule.layout = lx::ui::style{}.flex_none().w(lx::layout::px(1.f)).h(lx_in_px16(20.f));
    rule.paint = control_paint::fixed({paint(tok::border_variant), k_transparent});
    lx::ui::child left_rule = control(rule, {});
    lx::ui::child right_rule = control(rule, {});
    lx::ui::child kids[5] = {minus, left_rule, field_box, right_rule, plus};
    lx::ui::control_props box{};
    box.layout = lx_in_h_flex().flex_none().min_w(lx_in_px16(110.f)).h(lx_in_px16(k_st_h))
                     .rounded(radius::md);
    lx::ui::control_paint paint_{};
    paint_.rest = {paint(tok::element_background), paint(tok::border_variant)};
    paint_.hover = paint_.active = paint_.rest;
    box.paint = lx_in_rounded(lx_in_bordered(paint_, 1), radius::md);
    box.role = lx::a11y::role::spin_button;
    box.name = name_;
    box.disabled = disabled_;
    return control(box, {kids, 5u}, key_);
}

lx::ui::spinner_builder lx::ui::spinner(text_buffer& buffer, int value) {
    return spinner_builder{buffer, value};
}

// ── swatch_node ────────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::swatch_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::swatch_node::type() const { return static_type(); }
const char* lx::ui::swatch_node::type_name() const { return "swatch"; }

void lx::ui::swatch_node::resolve() {
    const theme& t = active_theme();
    quad_style q{};
    const float r = props_.corner_px * lx_in_rem() / 16.f;
    radius_ = r;
    for (float& c : q.corner_radii)
        c = r;
    q.background = props_.color;
    if (props_.ring || props_.selected) {
        q.border_color = t.color(props_.selected ? tok::text_accent : tok::border);
        for (float& w : q.border_widths)
            w = props_.selected ? 2.f : 1.f;
    }
    chip_ = pack_quad_style(q);
}

bool lx::ui::swatch_node::apply(const swatch_props& p) {
    const std::uint64_t generation = theme_generation();
    const bool color_changed = p.color.r != props_.color.r || p.color.g != props_.color.g ||
                               p.color.b != props_.color.b || p.color.a != props_.color.a ||
                               p.color.space != props_.color.space ||
                               p.color.encoding != props_.color.encoding;
    const bool new_look = generation != generation_ || !applied_ || color_changed ||
                          p.corner_px != props_.corner_px || p.ring != props_.ring ||
                          p.selected != props_.selected || p.disabled != props_.disabled;
    bool layout_changed = false;
    if (new_look || !(p.layout == props_.layout)) {
        const lx::layout::flex_style flex =
            resolve_layout(p.layout, {ui_density(), ui_rem_px()});
        layout_changed = !(flex == flex_) || !applied_;
        flex_ = flex;
    }
    props_ = p;
    set_flag(node_flag::interactive, !p.disabled);
    set_flag(node_flag::focusable, !p.disabled);
    set_flag(node_flag::disabled, p.disabled);
    if (new_look) {
        generation_ = generation;
        resolve();
    }
    applied_ = true;
    if (!layout_changed && new_look)
        invalidate_paint(*this);
    return layout_changed;
}

lx::size2i lx::ui::swatch_node::measure(lx::layout::constraints c, size_hint) const {
    const int side = static_cast<int>(k_sw_chip * lx_in_rem() / 16.f);
    lx::size2i s{side, side};
    s.width = std::clamp(s.width, c.min.width, std::max(c.min.width, c.max.width));
    s.height = std::clamp(s.height, c.min.height, std::max(c.min.height, c.max.height));
    return s;
}

lx::size2i lx::ui::swatch_node::measure_content(lx::layout::constraints c,
                                                size_hint hint) const {
    return measure(c, hint);
}

void lx::ui::swatch_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
}

const lx::layout::flex_style* lx::ui::swatch_node::flex_item_style() const { return &flex_; }

void lx::ui::swatch_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    out.quad(bounds_, chip_);
    if (!props_.disabled && has_flag(node_flag::focused) && has_flag(node_flag::focus_ring)) {
        const float radii[4] = {radius_, radius_, radius_, radius_};
        draw_focus_ring(out, bounds_, radii, 1.f);
    }
}

bool lx::ui::swatch_node::on_focus_key(key_event event, focus_manager&) {
    if (props_.disabled || !event.pressed || (event.key != key::space && event.key != key::enter))
        return false;
    if (props_.on_select)
        props_.on_select(props_.value);
    return true;
}

bool lx::ui::swatch_node::activate() {
    if (props_.disabled)
        return false;
    if (props_.on_select)
        props_.on_select(props_.value);
    return true;
}

lx::a11y::node lx::ui::swatch_node::accessibility_node() const {
    unsigned state = 0;
    if (props_.selected)
        state |= lx_in_a11y(lx::a11y::state_flag::selected);
    if (has_flag(node_flag::focused))
        state |= lx_in_a11y(lx::a11y::state_flag::focused);
    if (props_.disabled)
        state |= lx_in_a11y(lx::a11y::state_flag::disabled);
    return {lx::a11y::role::button, props_.name ? props_.name : "", "", "", state,
            const_cast<swatch_node*>(this)};
}

lx::ui::swatch_row_builder::operator child() const {
    const unsigned n = std::min<unsigned>(swatches_.size(), 64);
    if (n == 0)
        return nullptr;
    build_context* ctx = build_context::current();
    const auto** kids = ctx ? ctx->alloc_children(n) : nullptr;
    if (!kids)
        return nullptr;
    for (unsigned i = 0; i < n; ++i) {
        swatch_props p{};
        p.color = swatches_[i].color;
        p.selected = static_cast<int>(i) == selected_;
        p.disabled = disabled_;
        p.on_select = on_select_;
        p.value = static_cast<int>(i);
        p.tab_index = disabled_ ? -1
                    : static_cast<int>(i) == selected_ ? tab_index_ : -1;
        p.name = swatches_[i].label;
        kids[i] = describe<swatch_node, swatch_props>(p);
    }
    lx::ui::control_props row{};
    row.layout = lx_in_h_flex().flex_none().gap(lx_in_px16(k_sw_gap));
    row.nav = nav_axis::horizontal;
    row.role = lx::a11y::role::radio_group;
    row.name = name_;
    return control(row, {kids, n}, key_);
}

lx::ui::swatch_row_builder lx::ui::swatch_row(std::span<const swatch> swatches, int selected) {
    return swatch_row_builder{swatches, selected};
}
