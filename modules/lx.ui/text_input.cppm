module;

export module lx.ui:text_input;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.a11y;
import lx.runtime;
import lx.text;
import lx.trace;

import :clipboard;
import :components;
import :control;
import :element;
import :events;
import :focus;
import :interact;
import :invalidate;
import :node;
import :style;
import :text_node;
import :theme;

namespace {

constexpr lx::trace::category log_ui{"ui"};

} // namespace

// ── Single-line text input (ADR-010 §5.6) ────────────────────────────────────────────────
//
// `text_input` edits UTF-8 in a fixed-capacity buffer — the app's (`text_buffer`, edited in
// place) or the node's own — and shows it through a `text_block_node` child it owns (the
// reconciler leaves it alone). Every position is a grapheme boundary of lx.text's layout
// (`text_layout::next_caret` / `prev_caret`, UAX #29): arrows step a grapheme (a ZWJ emoji
// sequence is one step), backspace and delete remove one, ctrl+arrows jump words, home and
// end go to the ends, shift extends the selection, a press places the caret and a drag
// selects (a double press selects a word, a triple everything). Left and right follow the
// paragraph's direction (in a right-to-left paragraph left moves forward); the caret is drawn
// where `text_layout::caret` puts the position — the Phase 2 hit testing, bidi-correct at a
// direction boundary. Ctrl+c / x / v go through `clipboard::global()`; ctrl+a selects all;
// enter calls `on_submit`.
//
// IME: `ime_target` is what text-input-v3 will drive in Phase 4. One `ime_update` is one
// `done` event's batch, applied in the protocol's order — `delete_surrounding_text`, then
// `commit_string`, then `preedit_string` — and `ime_query` answers `set_surrounding_text`
// and `set_cursor_rectangle`. The preedit is shown at the caret, underlined, with its own
// cursor; it is not part of the text until committed.
//
// Look (Zed's `InputField`, crates/ui_input/src/input_field.rs at a5c4ca8): `min_w(192px)`,
// `min_h_8()` (32 px), `px_2()` / `py_1p5()` (8 / 6 px), `rounded_md()`, a 1 px border
// `border.variant` turning `border.focused` while focused, background `editor.background`;
// text in `text`, the placeholder in `text.placeholder`, both `TextSize::Default` (14 px) on the
// 1.2 line Zed's single-line editor sets (editor.rs `ErasedEditorImpl::render`, the UI font:
// 17 px, so `min_h_8` governs and the field is 32 px). The caret is Zed's bar
// cursor, 2 px in `players[0].cursor`; the selection `players[0].selection`. The caret blinks
// at Zed's 500 ms (`BlinkManager`) on a UI timer — no frames while it waits — restarting solid
// after each edit or move, and stops when the focus leaves. Text wider than the field scrolls
// horizontally to keep the caret in view (by origin, no relayout).
//
// Capacity: `text_input_props::max_bytes`, at most the buffer's capacity and at most
// `k_text_input_capacity` (what the display holds); text longer than that — an app buffer
// assigned more, a smaller `max_bytes` — is cut to it at a character boundary when the props
// are applied. An insertion that does not fit follows `overflow`: `reject`
// refuses it whole, `drop_newest` / `warn_and_drop` keep what fits (cut at a character
// boundary); `rejected()` counts both. The preedit has `k_preedit_capacity`. Nothing
// allocates per keystroke: the text, display and preedit buffers are fixed, the layout keeps
// its storage. UI affinity.

export namespace lx::ui {

inline constexpr std::uint32_t k_text_input_capacity = 1024;
inline constexpr std::uint32_t k_preedit_capacity = 256;
/// Zed's `BlinkManager` interval.
inline constexpr std::uint32_t k_caret_blink_ms = 500;
/// The timer tag of the caret's blink.
inline constexpr std::uint32_t k_timer_caret_blink = k_timer_user;

/// App-owned UTF-8 a text input edits in place. `storage.size() - 1` bytes of text (a NUL
/// is kept after it). UI affinity.
struct text_buffer {
    std::span<char> storage{};
    std::uint32_t size = 0;
    /// Bumped by every change (the input's and `assign`'s): an input re-reads the text when
    /// it moved on without it.
    std::uint32_t revision = 0;

    [[nodiscard]] std::string_view view() const { return {storage.data(), size}; }
    [[nodiscard]] std::uint32_t capacity() const {
        return storage.empty() ? 0u : static_cast<std::uint32_t>(storage.size() - 1);
    }
    /// The app replaces the text. False (nothing changes) when it does not fit.
    bool assign(std::string_view text);
};

/// One `zwp_text_input_v3.done` batch (lengths in bytes).
struct ime_update {
    /// The new preedit (empty: none) and its cursor within it: −1 for both hides the caret;
    /// begin ≠ end highlights that range.
    std::string_view preedit{};
    std::int32_t preedit_cursor_begin = -1;
    std::int32_t preedit_cursor_end = -1;
    /// Text to insert at the caret (replacing the selection).
    std::string_view commit{};
    /// Bytes to remove before and after the caret first (`delete_surrounding_text`).
    std::uint32_t delete_before = 0;
    std::uint32_t delete_after = 0;
};

/// What text-input-v3 tells the input method about the field.
struct ime_state {
    bool enabled = false;          ///< the field has the focus
    std::string_view surrounding{}; ///< the committed text
    std::uint32_t cursor = 0;      ///< byte offsets into it
    std::uint32_t anchor = 0;
    lx::rect2i cursor_rect{};      ///< the caret, surface coordinates
};

/// The input-method side of a text field (Phase 4's `zwp_text_input_v3` binding drives it).
class ime_target {
public:
    /// Applies one `done` batch: delete around the caret, insert the commit, show the
    /// preedit.
    virtual void ime_apply(const ime_update& update) = 0;
    /// The input method went away: the preedit is dropped (not committed).
    virtual void ime_reset() = 0;
    [[nodiscard]] virtual ime_state ime_query() const = 0;

protected:
    ~ime_target() = default;
};

struct text_input_props {
    /// The field's layout as a flex item (its paint fields are not read).
    lx::ui::style layout{};
    /// The text; null: the node's own buffer.
    text_buffer* buffer = nullptr;
    const char* placeholder = "";
    /// The node's own buffer's text when it is first mounted.
    const char* initial = nullptr;
    std::uint32_t max_bytes = k_text_input_capacity;
    lx::runtime::overflow_action overflow = lx::runtime::overflow_action::reject;
    /// The text changed: called with its length in bytes.
    handler on_change{};
    /// Enter: called with the length.
    handler on_submit{};
    bool disabled = false;
    std::int16_t tab_index = 0;
    const char* name = nullptr;
};

class text_input_node final : public ui_node, public ime_target {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    text_input_node();
    ~text_input_node() override;

    bool apply(const text_input_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    bool on_focus_key(key_event event, focus_manager& focus) override;
    bool on_pointer(const pointer_event& event) override;
    void on_interaction_changed() override;
    void on_timer(std::uint32_t tag) override;
    [[nodiscard]] a11y::node accessibility_node() const override;
    /// The I-beam (an arrow while disabled).
    [[nodiscard]] cursor_style cursor() const override {
        return has_flag(node_flag::disabled) ? cursor_style::arrow : cursor_style::text;
    }

    void ime_apply(const ime_update& update) override;
    void ime_reset() override;
    [[nodiscard]] ime_state ime_query() const override;

    [[nodiscard]] std::string_view text() const;
    [[nodiscard]] std::uint32_t caret() const { return caret_; }
    [[nodiscard]] std::uint32_t anchor() const { return anchor_; }
    [[nodiscard]] bool has_selection() const { return caret_ != anchor_; }
    [[nodiscard]] std::uint32_t selection_begin() const { return std::min(caret_, anchor_); }
    [[nodiscard]] std::uint32_t selection_end() const { return std::max(caret_, anchor_); }
    [[nodiscard]] std::string_view preedit() const { return {preedit_, preedit_size_}; }
    [[nodiscard]] std::uint32_t capacity() const;
    /// Insertions refused or cut by the capacity.
    [[nodiscard]] std::uint64_t rejected() const { return rejected_; }
    /// The caret shows now (focused, in the blink's on phase).
    [[nodiscard]] bool caret_visible() const;
    /// The caret's rect in the node's layout space (its x from the layout, bidi-correct).
    [[nodiscard]] lx::rect2i caret_rect() const;
    /// The field's text area (inside the border and padding).
    [[nodiscard]] lx::rect2i text_area() const;
    /// The horizontal scroll of the text, px.
    [[nodiscard]] int scroll_x() const { return scroll_x_; }
    [[nodiscard]] const text_block_node& text_node() const { return *text_; }

    /// Programmatic editing (what the keys do).
    void set_selection(std::uint32_t anchor, std::uint32_t caret);
    /// Replaces the selection with `utf8`; false when the capacity refused it all.
    bool insert(std::string_view utf8);
    void select_all();

private:
    [[nodiscard]] char* data();
    [[nodiscard]] const char* data() const;
    [[nodiscard]] std::uint32_t size() const;
    void set_size(std::uint32_t n);
    /// Cuts the text to `capacity()` at a character boundary; true when it did.
    bool clamp_to_capacity();
    void erase(std::uint32_t begin, std::uint32_t end);
    void changed();
    void sync_text();
    void caret_moved(bool keep_blink = false);
    void move_to(std::uint32_t index, bool extend);
    [[nodiscard]] std::uint32_t step(std::uint32_t index, bool forward) const;
    [[nodiscard]] std::uint32_t word_step(std::uint32_t index, bool forward) const;
    [[nodiscard]] std::uint32_t index_at(lx::point2i local, bool* upstream = nullptr) const;
    [[nodiscard]] int text_x() const;
    [[nodiscard]] bool rtl() const;
    void keep_caret_visible();
    void restart_blink();
    void copy_selection() const;
    void resolve_paint();

    text_block_node* text_ = nullptr;
    text_buffer* buffer_ = nullptr;
    std::uint32_t seen_revision_ = 0;
    std::uint32_t own_size_ = 0;
    std::uint32_t max_bytes_ = k_text_input_capacity;
    lx::runtime::overflow_action overflow_ = lx::runtime::overflow_action::reject;
    std::uint32_t caret_ = 0;
    std::uint32_t anchor_ = 0;
    std::int32_t preedit_cursor_[2]{-1, -1};
    std::uint32_t preedit_size_ = 0;
    std::uint64_t rejected_ = 0;
    int scroll_x_ = 0;
    handler on_change_{};
    handler on_submit_{};
    std::int16_t tab_index_ = 0;
    bool caret_on_ = true;
    bool was_focused_ = false;
    bool selecting_ = false;
    bool applied_ = false;
    std::uint64_t generation_ = 0;
    lx::ui::style layout_{};
    lx::layout::flex_style flex_{};
    packed_quad_style box_[2]{}; ///< at rest, focused
    lx::text::text_attr preedit_attr_{};
    char placeholder_[64]{};
    char name_[32]{};
    char own_[k_text_input_capacity + 1]{};
    char preedit_[k_preedit_capacity + 1]{};
    char display_[k_text_input_capacity + k_preedit_capacity + 1]{};
};

/// A single-line text field.
class text_input_builder {
public:
    text_input_builder() = default;
    text_input_builder& buffer(text_buffer& b) { props_.buffer = &b; return *this; }
    text_input_builder& placeholder(const char* text) { props_.placeholder = text; return *this; }
    text_input_builder& initial(const char* text) { props_.initial = text; return *this; }
    text_input_builder& layout(const lx::ui::style& s) { props_.layout = s; return *this; }
    text_input_builder& max_bytes(std::uint32_t n) { props_.max_bytes = n; return *this; }
    text_input_builder& overflow(lx::runtime::overflow_action a) { props_.overflow = a; return *this; }
    text_input_builder& on_change(handler h) { props_.on_change = h; return *this; }
    text_input_builder& on_submit(handler h) { props_.on_submit = h; return *this; }
    text_input_builder& disabled(bool on = true) { props_.disabled = on; return *this; }
    text_input_builder& tab_index(int i) { props_.tab_index = static_cast<std::int16_t>(i); return *this; }
    text_input_builder& name(const char* a11y) { props_.name = a11y; return *this; }
    text_input_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    text_input_props props_{};
    element_key key_ = 0;
};

[[nodiscard]] text_input_builder text_input();

/// The focused node as an IME target (a text input), or null: what Phase 4 routes
/// text-input-v3 to.
[[nodiscard]] ime_target* focused_ime_target(const focus_manager& focus);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

/// The field's metrics (input_field.rs): border, padding, minimum size; the text's line.
constexpr int k_ti_border = 1;
constexpr int k_ti_pad_x = 8;  // px_2
constexpr int k_ti_pad_y = 6;  // py_1p5
constexpr int k_ti_min_h = 32; // min_h_8
constexpr int k_ti_min_w = 192;
constexpr float k_ti_line = 1.2f; // editor.rs: `line_height: relative(1.2)`
constexpr int k_ti_caret_w = 2; // CursorShape::Bar

/// The copied text clipboard::set_text points at (it keeps the pointer). UI affinity.
char g_ti_clipboard[lx::ui::k_text_input_capacity + 1]{};

[[nodiscard]] bool lx_ui_ti_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}

/// The longest prefix of `s` of at most `n` bytes that ends on a character boundary.
[[nodiscard]] std::size_t lx_ui_ti_cut(std::string_view s, std::size_t n) {
    if (n >= s.size())
        return s.size();
    while (n > 0 && lx_ui_ti_continuation(s[n]))
        --n;
    return n;
}

/// The code point starting at `i` (malformed: the byte).
[[nodiscard]] char32_t lx_ui_ti_decode(std::string_view s, std::size_t i) {
    const auto b = static_cast<unsigned char>(s[i]);
    if (b < 0x80)
        return b;
    int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    char32_t cp = b & (0x3Fu >> extra);
    for (int k = 1; k <= extra && i + k < s.size(); ++k)
        cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    return cp;
}

/// Word-jump classes: 0 space, 1 punctuation, 2 a word character.
[[nodiscard]] int lx_ui_ti_class(char32_t c) {
    if (c == ' ' || c == '\t' || c == 0xA0 || (c >= 0x2000 && c <= 0x200A) || c == 0x3000)
        return 0;
    if (c < 0x80 && ((c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') ||
                     (c >= '{' && c <= '~')))
        return 1;
    return 2;
}

[[nodiscard]] unsigned lx_ui_ti_encode(char32_t c, char (&out)[4]) {
    if (c < 0x80) {
        out[0] = static_cast<char>(c);
        return 1;
    }
    if (c < 0x800) {
        out[0] = static_cast<char>(0xC0 | (c >> 6));
        out[1] = static_cast<char>(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (c >> 12));
        out[1] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (c & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (c >> 18));
    out[1] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (c & 0x3F));
    return 4;
}

/// Layout px in the field's layout's 26.6 units (device px at the UI's device `scale`).
[[nodiscard]] std::int32_t lx_ui_ti_units(int px, float scale) {
    if (scale == 1.f)
        return px * 64;
    return static_cast<std::int32_t>(std::lround(static_cast<double>(px) * 64.0 * scale));
}

/// A 26.6 position of the field's layout (device px at the UI's device `scale`) in layout px.
[[nodiscard]] int lx_ui_ti_px(std::int32_t v26_6, float scale) {
    if (scale == 1.f)
        return (v26_6 + 32) >> 6;
    return static_cast<int>(std::lround(static_cast<double>(v26_6) / (64.0 * scale)));
}

[[nodiscard]] const char* lx_ui_ti_family() {
    const char* f = lx::ui::active_theme().fonts().ui_family;
    return f && f[0] ? f : lx::text::k_ui_family;
}

[[nodiscard]] float lx_ui_ti_text_px() { return lx::ui::label_size_px(lx::ui::label_size::normal, lx::ui::ui_rem_px()); }

[[nodiscard]] int lx_ui_ti_line_px() {
    return static_cast<int>(std::ceil(lx_ui_ti_text_px() * k_ti_line - 1e-3f));
}

} // namespace

bool lx::ui::text_buffer::assign(std::string_view text) {
    if (text.size() > capacity())
        return false;
    std::memcpy(storage.data(), text.data(), text.size());
    size = static_cast<std::uint32_t>(text.size());
    storage[size] = '\0';
    ++revision;
    return true;
}

lx::ui::element_type_id lx::ui::text_input_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::text_input_node::type() const { return static_type(); }
const char* lx::ui::text_input_node::type_name() const { return "text_input"; }

lx::ui::text_input_node::text_input_node() {
    // The text block is the node's own: the reconciler leaves the children alone.
    set_flag(node_flag::opaque_children, true);
    text_ = new text_block_node{};
    adopt(text_);
}

lx::ui::text_input_node::~text_input_node() = default;

char* lx::ui::text_input_node::data() { return buffer_ ? buffer_->storage.data() : own_; }
const char* lx::ui::text_input_node::data() const { return buffer_ ? buffer_->storage.data() : own_; }
std::uint32_t lx::ui::text_input_node::size() const { return buffer_ ? buffer_->size : own_size_; }

void lx::ui::text_input_node::set_size(std::uint32_t n) {
    if (buffer_) {
        buffer_->size = n;
        ++buffer_->revision;
        seen_revision_ = buffer_->revision;
        if (buffer_->storage.empty())
            return;
    } else {
        own_size_ = n;
    }
    data()[n] = '\0';
}

std::uint32_t lx::ui::text_input_node::capacity() const {
    const std::uint32_t storage = buffer_ ? buffer_->capacity() : k_text_input_capacity;
    // The display buffer holds `k_text_input_capacity` bytes of text (and the preedit).
    return std::min({storage, max_bytes_, k_text_input_capacity});
}

bool lx::ui::text_input_node::clamp_to_capacity() {
    // What the storage holds, then what the field may: cut at a character boundary.
    std::uint32_t n = size();
    if (buffer_)
        n = std::min(n, buffer_->capacity());
    const std::uint32_t cap = capacity();
    if (n <= cap && n == size())
        return false;
    const std::size_t cut = lx_ui_ti_cut({data(), n}, cap);
    set_size(static_cast<std::uint32_t>(cut));
    return true;
}

std::string_view lx::ui::text_input_node::text() const { return {data(), size()}; }

void lx::ui::text_input_node::resolve_paint() {
    const theme& t = active_theme();
    quad_style q{};
    q.background = t.color(tok::editor_background);
    for (unsigned i = 0; i < 4; ++i) {
        q.corner_radii[i] = radius_px(radius::md, ui_rem_px());
        q.border_widths[i] = static_cast<float>(k_ti_border);
    }
    q.border_color = t.color(tok::border_variant);
    box_[0] = pack_quad_style(q);
    q.border_color = t.color(tok::border_focused);
    box_[1] = pack_quad_style(q);
}

bool lx::ui::text_input_node::apply(const text_input_props& p) {
    const std::uint64_t generation = theme_generation();
    bool layout_changed = !applied_;
    if (!applied_ || generation != generation_ || !(p.layout == layout_)) {
        lx::layout::flex_style flex = resolve_layout(p.layout, {ui_density(), ui_rem_px()});
        // `min_w(192px)`, `min_h_8()` unless the layout says otherwise.
        if (flex.min_width.is_auto())
            flex.min_width = lx::layout::px(static_cast<float>(k_ti_min_w));
        layout_changed = layout_changed || !(flex == flex_);
        flex_ = flex;
        layout_ = p.layout;
    }
    const bool repaint = !applied_ || generation != generation_;
    if (repaint)
        resolve_paint();
    generation_ = generation;
    if (p.buffer != buffer_) {
        buffer_ = p.buffer;
        seen_revision_ = buffer_ ? buffer_->revision - 1u : 0u; // re-read it below
    }
    if (!applied_ && !buffer_ && p.initial) {
        const std::string_view init{p.initial};
        const std::size_t n = lx_ui_ti_cut(init, std::min<std::size_t>(k_text_input_capacity, p.max_bytes));
        std::memcpy(own_, init.data(), n);
        own_size_ = static_cast<std::uint32_t>(n);
        own_[n] = '\0';
        caret_ = anchor_ = own_size_;
    }
    max_bytes_ = p.max_bytes;
    overflow_ = p.overflow;
    on_change_ = p.on_change;
    on_submit_ = p.on_submit;
    tab_index_ = p.tab_index;
    const bool disabled_changed = p.disabled != has_flag(node_flag::disabled);
    set_flag(node_flag::interactive, !p.disabled);
    set_flag(node_flag::focusable, !p.disabled);
    set_flag(node_flag::disabled, p.disabled);
    std::size_t n = p.placeholder ? std::strlen(p.placeholder) : 0;
    n = lx_ui_ti_cut({p.placeholder ? p.placeholder : "", n}, sizeof(placeholder_) - 1);
    const bool new_placeholder = std::strncmp(placeholder_, p.placeholder ? p.placeholder : "", n) != 0 ||
                                 placeholder_[n] != '\0';
    if (n)
        std::memcpy(placeholder_, p.placeholder, n);
    placeholder_[n] = '\0';
    n = p.name ? std::strlen(p.name) : 0;
    n = std::min(n, sizeof(name_) - 1);
    if (n)
        std::memcpy(name_, p.name, n);
    name_[n] = '\0';
    bool reread = false;
    if (buffer_ && buffer_->revision != seen_revision_) {
        // The app changed the text: the caret goes to its end.
        seen_revision_ = buffer_->revision;
        caret_ = anchor_ = std::min(buffer_->size, capacity());
        reread = true;
    }
    // Never more text than the field holds (an app buffer assigned more, a smaller
    // `max_bytes`): cut at a character boundary, the buffer's revision bumped.
    if (clamp_to_capacity())
        reread = true;
    caret_ = std::min(caret_, size());
    anchor_ = std::min(anchor_, size());
    if (!applied_ || reread || new_placeholder || repaint || disabled_changed)
        sync_text();
    if ((repaint || disabled_changed) && !layout_changed)
        invalidate_paint(*this);
    applied_ = true;
    return layout_changed;
}

void lx::ui::text_input_node::sync_text() {
    // `apply` keeps the text within `capacity()`; the display is bounded here regardless.
    const std::uint32_t n = std::min(size(), k_text_input_capacity);
    const std::uint32_t at = std::min(caret_, n);
    const bool show_placeholder = n == 0 && preedit_size_ == 0;
    // The display: the text with the preedit at the caret.
    std::uint32_t d = 0;
    if (!show_placeholder) {
        std::memcpy(display_, data(), at);
        d = at;
        std::memcpy(display_ + d, preedit_, preedit_size_);
        d += preedit_size_;
        std::memcpy(display_ + d, data() + at, n - at);
        d += n - at;
    }
    display_[d] = '\0';
    text_block_props p{};
    p.text = show_placeholder ? placeholder_ : display_;
    p.font = {lx_ui_ti_family(), lx_ui_ti_text_px() * 0.75f};
    const bool disabled = has_flag(node_flag::disabled);
    p.color = active_theme().color(show_placeholder || disabled ? (disabled ? tok::text_disabled : tok::text_placeholder)
                                                                : tok::text);
    p.line_height = k_ti_line;
    p.wrap = lx::text::text_wrap::none;
    p.context = active_ui_resources().text;
    if (preedit_size_ && !show_placeholder) {
        preedit_attr_ = {};
        preedit_attr_.begin = at;
        preedit_attr_.end = at + preedit_size_;
        preedit_attr_.underline = lx::text::text_underline::single;
        p.attrs = {&preedit_attr_, 1};
    }
    text_->apply(p);
}

lx::size2i lx::ui::text_input_node::measure(lx::layout::constraints c, size_hint) const {
    constexpr int unbounded = lx::layout::constraints{}.max.width;
    const int h = std::max(k_ti_min_h, lx_ui_ti_line_px() + 2 * (k_ti_pad_y + k_ti_border));
    lx::size2i s{c.max.width < unbounded ? c.max.width : k_ti_min_w, h};
    s.width = std::clamp(s.width, c.min.width, std::max(c.min.width, c.max.width));
    s.height = std::clamp(s.height, c.min.height, std::max(c.min.height, c.max.height));
    return s;
}

lx::size2i lx::ui::text_input_node::measure_content(lx::layout::constraints c,
                                                    size_hint hint) const {
    return measure(c, hint);
}

const lx::layout::flex_style* lx::ui::text_input_node::flex_item_style() const { return &flex_; }

lx::rect2i lx::ui::text_input_node::text_area() const {
    const int ix = k_ti_border + k_ti_pad_x;
    const int iy = k_ti_border + k_ti_pad_y;
    return {bounds_.x + ix, bounds_.y + iy, std::max(bounds_.width - 2 * ix, 0),
            std::max(bounds_.height - 2 * iy, 0)};
}

void lx::ui::text_input_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    const lx::rect2i area = text_area();
    const lx::size2i s = text_->measure_cached({}, size_hint::preferred);
    const int line = lx_ui_ti_line_px();
    text_->place({area.x, area.y + (area.height - line) / 2, std::max(s.width, 1), line});
    keep_caret_visible();
}

bool lx::ui::text_input_node::rtl() const {
    return text_->layout().direction() == lx::text::text_direction::rtl;
}

int lx::ui::text_input_node::text_x() const {
    // Where the text block is drawn: a right-to-left paragraph narrower than the field sits
    // at its end, as a native right-to-left field shows it.
    const lx::rect2i area = text_area();
    const int w = text_->bounds().width;
    int x = area.x - scroll_x_;
    if (rtl() && w < area.width)
        x = area.x + area.width - w;
    return x;
}

lx::rect2i lx::ui::text_input_node::caret_rect() const {
    const lx::text::text_layout& l = text_->layout();
    std::uint32_t at = caret_;
    if (preedit_size_ && preedit_cursor_[0] >= 0)
        at = caret_ + static_cast<std::uint32_t>(preedit_cursor_[0]);
    else if (preedit_size_)
        at = caret_ + preedit_size_;
    const bool empty = size() == 0 && preedit_size_ == 0;
    const lx::text::caret_pos cp = empty ? lx::text::caret_pos{} : l.caret(at);
    const lx::rect2i tb = text_->bounds();
    int x = text_x() + lx_ui_ti_px(cp.x, text_->device_scale());
    if (empty)
        x = rtl() ? text_area().x + text_area().width - k_ti_caret_w : text_area().x;
    // The bar straddles the position, nudged inside the field at its edges.
    x -= k_ti_caret_w / 2;
    return {x, tb.y, k_ti_caret_w, tb.height};
}

bool lx::ui::text_input_node::caret_visible() const {
    return has_flag(node_flag::focused) && caret_on_ && !has_flag(node_flag::disabled) &&
           (preedit_size_ == 0 || preedit_cursor_[0] >= 0);
}

void lx::ui::text_input_node::keep_caret_visible() {
    const lx::rect2i area = text_area();
    const int w = text_->bounds().width;
    if (w <= area.width) {
        if (scroll_x_ != 0) {
            scroll_x_ = 0;
            invalidate_paint(*this);
        }
        return;
    }
    const lx::text::text_layout& l = text_->layout();
    std::uint32_t at = caret_ + (preedit_cursor_[0] >= 0 ? static_cast<std::uint32_t>(preedit_cursor_[0]) : preedit_size_);
    const int cx = lx_ui_ti_px(l.caret(at).x, text_->device_scale());
    int s = scroll_x_;
    if (cx - s < 0)
        s = cx;
    else if (cx - s > area.width - k_ti_caret_w)
        s = cx - area.width + k_ti_caret_w;
    s = std::clamp(s, 0, w - area.width + k_ti_caret_w);
    if (s != scroll_x_) {
        scroll_x_ = s;
        invalidate_paint(*this);
    }
}

void lx::ui::text_input_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    const bool focused = has_flag(node_flag::focused);
    out.quad(bounds_, box_[focused ? 1 : 0]);
    const lx::rect2i area = text_area();
    out.push_clip_local({area.x - k_ti_caret_w, bounds_.y + k_ti_border,
                         area.width + 2 * k_ti_caret_w, bounds_.height - 2 * k_ti_border});
    const int tx = text_x();
    const lx::rect2i tb = text_->bounds();
    const theme& th = active_theme();
    // The selection (or a preedit's highlighted range): one rect per run of clusters inside
    // it, in visual order — two or more where the range crosses a direction boundary.
    std::uint32_t a = selection_begin();
    std::uint32_t b = selection_end();
    if (preedit_size_) {
        a = b = 0;
        if (preedit_cursor_[0] >= 0 && preedit_cursor_[1] > preedit_cursor_[0]) {
            a = caret_ + static_cast<std::uint32_t>(preedit_cursor_[0]);
            b = caret_ + static_cast<std::uint32_t>(preedit_cursor_[1]);
        }
    }
    if (b > a && (size() > 0 || preedit_size_ > 0)) {
        const lx::text::text_layout& l = text_->layout();
        lx::color sel = th.color(tok::player0_selection);
        constexpr unsigned k_spans = 8;
        int x0s[k_spans]{};
        int x1s[k_spans]{};
        unsigned spans = 0;
        for (const lx::text::layout_cluster& c : l.clusters()) {
            if (c.begin < a || c.end > b || c.end <= c.begin)
                continue;
            const int p0 = lx_ui_ti_px(l.caret(c.begin).x, text_->device_scale());
            const int p1 = lx_ui_ti_px(l.caret(c.end, true).x, text_->device_scale());
            int lo = std::min(p0, p1);
            int hi = std::max(p0, p1);
            bool merged = false;
            for (unsigned s = 0; s < spans && !merged; ++s)
                if (lo <= x1s[s] && hi >= x0s[s]) {
                    x0s[s] = std::min(x0s[s], lo);
                    x1s[s] = std::max(x1s[s], hi);
                    merged = true;
                }
            if (!merged) {
                if (spans == k_spans) { // fold the overflow into the last span
                    x0s[spans - 1] = std::min(x0s[spans - 1], lo);
                    x1s[spans - 1] = std::max(x1s[spans - 1], hi);
                } else {
                    x0s[spans] = lo;
                    x1s[spans] = hi;
                    ++spans;
                }
            }
        }
        quad_style q{};
        q.background = sel;
        for (unsigned s = 0; s < spans; ++s)
            out.quad({tx + x0s[s], tb.y, x1s[s] - x0s[s], tb.height}, q);
    }
    out.push_origin({tx - tb.x, 0});
    text_->emit(out);
    out.pop();
    if (caret_visible()) {
        quad_style q{};
        q.background = th.color(tok::player0_cursor);
        out.quad(caret_rect(), q);
    }
    out.pop();
}

std::uint32_t lx::ui::text_input_node::step(std::uint32_t index, bool forward) const {
    const lx::text::text_layout& l = text_->layout();
    if (size() == 0)
        return 0;
    return forward ? std::min(l.next_caret(index), size()) : l.prev_caret(index);
}

std::uint32_t lx::ui::text_input_node::word_step(std::uint32_t index, bool forward) const {
    const std::string_view t = text();
    const lx::text::text_layout& l = text_->layout();
    const auto cls_at = [&](std::uint32_t i) { return lx_ui_ti_class(lx_ui_ti_decode(t, i)); };
    std::uint32_t i = index;
    if (forward) {
        while (i < t.size() && cls_at(i) == 0)
            i = std::min(l.next_caret(i), size());
        if (i < t.size()) {
            const int k = cls_at(i);
            while (i < t.size() && cls_at(i) == k)
                i = std::min(l.next_caret(i), size());
        }
        return i;
    }
    const auto before = [&](std::uint32_t at) { return l.prev_caret(at); };
    while (i > 0 && cls_at(before(i)) == 0)
        i = before(i);
    if (i > 0) {
        const int k = cls_at(before(i));
        while (i > 0 && cls_at(before(i)) == k)
            i = before(i);
    }
    return i;
}

void lx::ui::text_input_node::restart_blink() {
    caret_on_ = true;
    if (has_flag(node_flag::focused))
        (void)ui_timers().schedule(*this, k_timer_caret_blink,
                                   std::uint64_t{k_caret_blink_ms} * 1'000'000ull);
}

void lx::ui::text_input_node::caret_moved(bool keep_blink) {
    keep_caret_visible();
    if (!keep_blink)
        restart_blink();
    // The caret and the selection live in the field: repaint it (a few hundred px²).
    invalidate_paint(*this);
}

void lx::ui::text_input_node::move_to(std::uint32_t index, bool extend) {
    caret_ = std::min(index, size());
    if (!extend)
        anchor_ = caret_;
    caret_moved();
}

void lx::ui::text_input_node::set_selection(std::uint32_t anchor, std::uint32_t caret) {
    anchor_ = std::min(anchor, size());
    caret_ = std::min(caret, size());
    caret_moved();
}

void lx::ui::text_input_node::select_all() { set_selection(0, size()); }

void lx::ui::text_input_node::changed() {
    sync_text();
    restart_blink();
    // The text block marked the layout (its width); the arrange keeps the caret in view.
    invalidate_paint(*this);
    if (on_change_)
        on_change_(static_cast<int>(size()));
}

void lx::ui::text_input_node::erase(std::uint32_t begin, std::uint32_t end) {
    const std::uint32_t n = size();
    end = std::min(end, n);
    if (begin >= end)
        return;
    char* d = data();
    std::memmove(d + begin, d + end, n - end);
    set_size(n - (end - begin));
    caret_ = anchor_ = begin;
}

bool lx::ui::text_input_node::insert(std::string_view s) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    // One line: a pasted line break becomes a space, and stops there.
    std::size_t len = s.size();
    for (std::size_t i = 0; i < len; ++i)
        if (s[i] == '\n' || s[i] == '\r') {
            len = i;
            break;
        }
    s = s.substr(0, len);
    const std::uint32_t a = selection_begin();
    const std::uint32_t b = selection_end();
    const std::uint32_t n = size();
    // Saturating: the text never outgrows the capacity (`apply` cuts it), but a room must
    // not wrap if it did.
    const std::uint32_t used = n - (b - a);
    const std::uint32_t room = used < capacity() ? capacity() - used : 0u;
    std::size_t take = s.size();
    if (take > room) {
        ++rejected_;
        if (overflow_ == lx::runtime::overflow_action::reject || overflow_ == lx::runtime::overflow_action::drop_oldest)
            return false;
        take = lx_ui_ti_cut(s, room);
        if (overflow_ == lx::runtime::overflow_action::warn_and_drop && rejected_ == 1)
            log_ui.warn("text_input: an insertion was cut to the capacity");
        if (take == 0)
            return false;
    }
    if (take == 0 && a == b)
        return false;
    char* d = data();
    // Close the selection's gap and open the insertion's in one move.
    std::memmove(d + a + take, d + b, n - b);
    std::memcpy(d + a, s.data(), take);
    set_size(static_cast<std::uint32_t>(n - (b - a) + take));
    caret_ = anchor_ = static_cast<std::uint32_t>(a + take);
    changed();
    return true;
}

void lx::ui::text_input_node::copy_selection() const {
    if (!has_selection())
        return;
    const std::string_view t = text().substr(selection_begin(), selection_end() - selection_begin());
    const std::size_t n = std::min(t.size(), sizeof(g_ti_clipboard) - 1);
    std::memcpy(g_ti_clipboard, t.data(), n);
    g_ti_clipboard[n] = '\0';
    clipboard::global().set_text(g_ti_clipboard);
}

bool lx::ui::text_input_node::on_focus_key(key_event e, focus_manager&) {
    if (!e.pressed || !has_flag(node_flag::focused) || has_flag(node_flag::disabled))
        return false;
    const bool shift = (e.modifiers & k_mod_shift) != 0;
    const bool ctrl = (e.modifiers & k_mod_ctrl) != 0;
    if (preedit_size_)
        return false; // the input method composes: its keys are its own
    const bool r = rtl();
    switch (e.key) {
    case key::left:
    case key::right: {
        const bool forward = (e.key == key::right) != r;
        if (has_selection() && !shift && !ctrl) {
            move_to(forward ? selection_end() : selection_begin(), false);
            return true;
        }
        move_to(ctrl ? word_step(caret_, forward) : step(caret_, forward), shift);
        return true;
    }
    case key::home: move_to(0, shift); return true;
    case key::end: move_to(size(), shift); return true;
    case key::backspace:
        if (has_selection()) {
            erase(selection_begin(), selection_end());
        } else if (caret_ > 0) {
            erase(ctrl ? word_step(caret_, false) : step(caret_, false), caret_);
        } else {
            return true;
        }
        changed();
        return true;
    case key::delete_forward:
        if (has_selection()) {
            erase(selection_begin(), selection_end());
        } else if (caret_ < size()) {
            erase(caret_, ctrl ? word_step(caret_, true) : step(caret_, true));
        } else {
            return true;
        }
        changed();
        return true;
    case key::enter:
        if (on_submit_)
            on_submit_(static_cast<int>(size()));
        return true;
    case key::up:
    case key::down:
    case key::page_up:
    case key::page_down:
    case key::escape:
    case key::tab:
        return false;
    default:
        break;
    }
    const char32_t cp = e.codepoint;
    if (ctrl && !(e.modifiers & (k_mod_alt | k_mod_super))) {
        switch (cp) {
        case 'a': case 'A': select_all(); return true;
        case 'c': case 'C': copy_selection(); return true;
        case 'x': case 'X':
            if (has_selection()) {
                copy_selection();
                erase(selection_begin(), selection_end());
                changed();
            }
            return true;
        case 'v': case 'V': {
            const char* t = clipboard::global().text();
            if (t && t[0])
                (void)insert(t);
            return true;
        }
        default: return false;
        }
    }
    if (cp >= 0x20 && cp != 0x7F && !(e.modifiers & (k_mod_ctrl | k_mod_alt | k_mod_super))) {
        char utf8[4];
        const unsigned n = lx_ui_ti_encode(cp, utf8);
        (void)insert({utf8, n});
        return true;
    }
    return false;
}

std::uint32_t lx::ui::text_input_node::index_at(lx::point2i local, bool* upstream) const {
    if (size() == 0)
        return 0;
    const lx::rect2i tb = text_->bounds();
    const lx::text::text_hit h =
        text_->layout().hit_test(lx_ui_ti_units(local.x - text_x(), text_->device_scale()),
                                 lx_ui_ti_units(local.y - tb.y, text_->device_scale()));
    if (upstream)
        *upstream = h.upstream;
    return std::min(h.index, size());
}

bool lx::ui::text_input_node::on_pointer(const pointer_event& e) {
    if (has_flag(node_flag::disabled) || preedit_size_)
        return false;
    const lx::point2i p = local_point(*this, e.at);
    switch (e.kind) {
    case pointer_kind::press: {
        const std::uint32_t at = index_at(p);
        if (e.clicks >= 3) {
            set_selection(0, size());
        } else if (e.clicks == 2) {
            // The word (or the run of spaces) under the press.
            const std::uint32_t end = word_step(at, true);
            const std::uint32_t begin = word_step(end, false);
            set_selection(begin, end);
        } else {
            move_to(at, (e.modifiers & k_mod_shift) != 0);
        }
        selecting_ = e.clicks == 1;
        return true;
    }
    case pointer_kind::drag:
        if (!selecting_)
            return false;
        move_to(index_at(p), true);
        return true;
    case pointer_kind::release:
        selecting_ = false;
        return true;
    default:
        return false;
    }
}

void lx::ui::text_input_node::on_interaction_changed() {
    const bool focused = has_flag(node_flag::focused);
    if (focused != was_focused_) {
        was_focused_ = focused;
        if (focused) {
            restart_blink();
        } else {
            // Unfocused: the blink stops (no timer, no frames); the preedit goes.
            ui_timers().cancel(*this, k_timer_caret_blink);
            caret_on_ = true;
            if (preedit_size_) {
                preedit_size_ = 0;
                preedit_cursor_[0] = preedit_cursor_[1] = -1;
                sync_text();
            }
        }
    }
    ui_node::on_interaction_changed();
}

void lx::ui::text_input_node::on_timer(std::uint32_t tag) {
    if (tag != k_timer_caret_blink || !has_flag(node_flag::focused))
        return;
    caret_on_ = !caret_on_;
    detail::damage_node(*this, caret_rect());
    (void)ui_timers().schedule(*this, k_timer_caret_blink,
                               std::uint64_t{k_caret_blink_ms} * 1'000'000ull);
}

void lx::ui::text_input_node::ime_apply(const ime_update& u) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    const bool had_preedit = preedit_size_ != 0;
    preedit_size_ = 0;
    preedit_cursor_[0] = preedit_cursor_[1] = -1;
    bool text_changed = false;
    // 1. delete_surrounding_text: around the caret (a selection collapses to it first).
    if (u.delete_before || u.delete_after) {
        const std::string_view t = text();
        std::uint32_t b = caret_ > u.delete_before ? caret_ - u.delete_before : 0;
        std::uint32_t e = std::min<std::uint32_t>(caret_ + u.delete_after, size());
        while (b > 0 && lx_ui_ti_continuation(t[b]))
            --b;
        while (e < t.size() && lx_ui_ti_continuation(t[e]))
            ++e;
        anchor_ = caret_;
        erase(b, e);
        text_changed = true;
    }
    // 2. commit_string at the caret.
    if (!u.commit.empty())
        text_changed = insert(u.commit) || text_changed;
    // 3. preedit_string: shown at the caret, replacing a selection.
    if (!u.preedit.empty()) {
        if (has_selection()) {
            erase(selection_begin(), selection_end());
            text_changed = true;
        }
        const std::size_t n = lx_ui_ti_cut(u.preedit, k_preedit_capacity);
        std::memcpy(preedit_, u.preedit.data(), n);
        preedit_[n] = '\0';
        preedit_size_ = static_cast<std::uint32_t>(n);
        const auto clampc = [&](std::int32_t v) {
            return v < 0 ? -1 : static_cast<std::int32_t>(std::min<std::size_t>(static_cast<std::size_t>(v), n));
        };
        preedit_cursor_[0] = clampc(u.preedit_cursor_begin);
        preedit_cursor_[1] = clampc(u.preedit_cursor_end);
        if (preedit_cursor_[1] < preedit_cursor_[0])
            preedit_cursor_[1] = preedit_cursor_[0];
    }
    if (text_changed) {
        changed(); // syncs the display, preedit included
    } else if (had_preedit || preedit_size_) {
        sync_text();
        restart_blink();
        invalidate_paint(*this);
    }
}

void lx::ui::text_input_node::ime_reset() {
    if (!preedit_size_)
        return;
    preedit_size_ = 0;
    preedit_cursor_[0] = preedit_cursor_[1] = -1;
    sync_text();
    invalidate_paint(*this);
}

lx::ui::ime_state lx::ui::text_input_node::ime_query() const {
    ime_state s{};
    s.enabled = has_flag(node_flag::focused) && !has_flag(node_flag::disabled);
    s.surrounding = text();
    s.cursor = caret_;
    s.anchor = anchor_;
    s.cursor_rect = surface_rect(*this, caret_rect());
    return s;
}

lx::a11y::node lx::ui::text_input_node::accessibility_node() const {
    unsigned state = 0;
    if (has_flag(node_flag::focused))
        state |= static_cast<unsigned>(a11y::state_flag::focused);
    if (has_flag(node_flag::disabled))
        state |= static_cast<unsigned>(a11y::state_flag::disabled);
    return {a11y::role::text_field, name_, placeholder_, data(), state,
            const_cast<text_input_node*>(this)};
}

lx::ui::text_input_builder::operator child() const {
    return describe<text_input_node, text_input_props>(props_, key_);
}

lx::ui::text_input_builder lx::ui::text_input() { return text_input_builder{}; }

lx::ui::ime_target* lx::ui::focused_ime_target(const focus_manager& focus) {
    ui_node* n = focus.focused();
    if (!n || n->type() != text_input_node::static_type())
        return nullptr;
    return static_cast<text_input_node*>(n);
}
