module;

export module lx.ui:list;

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
import :interact;
import :invalidate;
import :node;
import :reconcile;
import :reducer;
import :scroll;
import :style;
import :theme;

// ── Lists: keyboard-navigable, virtualised rows (ADR-010 §5.6) ───────────────────────────
//
// `list(count, row, context)` is a viewport over `count` rows of one fixed height (Zed's
// `ListItem` with its `ListItemSpacing`: below). The rows are described by the app's `row`
// function, which gets a `list_row` (the index, whether it is selected, handlers that select
// or activate it) and returns a child, usually a `list_item`. The list holds the scroll offset
// and the selection, takes one tab stop and the keys (up / down, home / end, page up / page
// down move the selection and scroll it into view; enter and space activate it), and draws its
// focus ring on its own box: the selection (`ghost_element.selected` on the row) and the focus
// ring are separate states (§5.5).
//
// Rows live in chunks of `k_list_chunk_rows` (a `list_chunk_node` stacks its rows at the row
// height, no flex solve). Up to `k_list_virtual_threshold` rows every chunk exists. Past it the
// list is virtualised: only the chunks covering the visible rows ± `k_list_margin_rows` exist,
// in a fixed pool of chunk slots — chunk k always occupies slot k mod the pool size, and the
// pool always holds consecutive chunks — so scrolling never creates or destroys a node: a
// chunk whose rows left the window is re-described for the rows entering it (its rows'
// props re-applied in place). The list is a reducer host: a scroll that moves the window, a
// selection change or a re-describe of the tree around it describes the visible rows again
// (`request_rebuild`; the reconcile reuses every row), on the same frame (`ui_root` rebuilds
// what its layout asked for and lays out again).
//
// Rows are laid out in content coordinates (row i at the list's top + i × height) and drawn
// under the scroll offset, like a scroll area's content: a scroll damages the viewport and
// lays nothing out; a hover moving between rows repaints the two rows and nothing else.
//
// Heights: crates/ui/src/components/list/list_item.rs (zed a5c4ca8) gives a row no height of
// its own — `Dense` (the default) is its content's, a `Label` at `TextSize::Default` on GPUI's
// φ line: ⌈14 × 1.618⌉ = 23 px; `ExtraDense` takes `py_neg_px()` (21 px), `Sparse` `py_1()`
// (31 px). The row: `h_flex().w_full()` over `h_flex().gap_1().px(Base06)` with the content
// `gap(Base06)`; hover `ghost_element.hover`, pressed `ghost_element.active`, selected
// `ghost_element.selected`; `inset` adds `px(Base04)` outside and rounds the inner box `sm`.

export namespace lx::ui {

/// `ListItemSpacing`.
enum class list_item_spacing : std::uint8_t { dense, extra_dense, sparse };

/// A row's height at the rem: the label line (`TextSize::Default` × φ, rounded up) and the
/// spacing's padding.
[[nodiscard]] int list_row_height_px(list_item_spacing spacing, float rem_px = 16.f);

inline constexpr unsigned k_list_virtual_threshold = 100;
inline constexpr unsigned k_list_chunk_rows = 16;
inline constexpr unsigned k_list_margin_rows = 8;
/// Chunk slots of a virtualised list (512 rows: a viewport of more than about 450 rows
/// shows only the first 512 — `overflow_action::drop_newest`).
inline constexpr unsigned k_list_max_chunks = 32;

/// What a row is described with.
struct list_row {
    unsigned index = 0;
    bool selected = false;
    /// Selects the row (a click): give it to the item's `on_click`.
    handler select{};
    /// Selects and activates it (`list_props::on_activate`).
    handler activate{};
};

using list_row_fn = child (*)(void* context, const list_row& row);

struct list_props {
    /// The viewport as a flex item (its paint fields are not read).
    lx::ui::style layout{};
    unsigned count = 0;
    list_item_spacing spacing = list_item_spacing::dense;
    /// px; 0: `list_row_height_px(spacing)`.
    int row_height = 0;
    list_row_fn row = nullptr;
    void* context = nullptr;
    /// The selection: takes effect when it changes (−1 none). The list moves it itself.
    int selected = -1;
    /// The selection moved (by key or click): called with the index.
    handler on_select{};
    /// Enter / space on the selection, or a row's `activate`: called with the index.
    handler on_activate{};
    const char* name = nullptr;
};

struct list_chunk_props {
    unsigned first = 0;
    unsigned count = 0;
    int row_height = 0;
    constexpr bool operator==(const list_chunk_props&) const = default;
};

/// `count` rows from `first`, stacked at `row_height`, full width.
class list_chunk_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;
    bool apply(const list_chunk_props& props);
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    [[nodiscard]] const list_chunk_props& props() const { return props_; }

private:
    list_chunk_props props_{};
};

class list_node final : public reducer_host, public scroll_target {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    list_node();
    ~list_node() override;

    bool apply(const list_props& props);
    void rebuild(build_context& ctx, reconciler& rec) override;

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    [[nodiscard]] lx::size2i measure_content(lx::layout::constraints c,
                                             size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;
    [[nodiscard]] const lx::layout::flex_style* flex_item_style() const override;
    [[nodiscard]] bool child_viewport(const ui_node& child, lx::point2i& offset,
                                      lx::rect2i& clip) const override;
    [[nodiscard]] scroll_target* as_scroll_target() override { return this; }
    bool on_pointer(const pointer_event& event) override;
    bool on_focus_key(key_event event, focus_manager& focus) override;
    [[nodiscard]] a11y::node accessibility_node() const override;

    [[nodiscard]] scroll_metrics scroll_metrics_along(unsigned axis) const override;
    void scroll_to(unsigned axis, float offset) override;

    /// Moves the selection to `index` (clamped; −1 none), scrolls it into view and tells
    /// `on_select` when it moved.
    void select(int index);
    /// `select`, then `on_activate(index)`.
    void activate_row(unsigned index);
    [[nodiscard]] int selected() const { return selected_; }
    [[nodiscard]] unsigned count() const { return count_; }
    [[nodiscard]] int row_height() const { return row_h_; }
    [[nodiscard]] int offset() const { return static_cast<int>(std::lround(off_)); }
    /// Rows that exist as nodes now (the chunks' rows).
    [[nodiscard]] unsigned live_rows() const;
    /// The first row and the row count the chunks hold.
    [[nodiscard]] unsigned first_live_row() const { return built_first_ * k_list_chunk_rows; }
    [[nodiscard]] bool virtualised() const { return count_ > k_list_virtual_threshold; }
    /// The node of row `index`, null when it does not exist.
    [[nodiscard]] ui_node* row_node(unsigned index) const;
    /// Rows a page key moves by: those that fit the viewport.
    [[nodiscard]] unsigned page_rows() const;

private:
    struct window {
        unsigned first = 0; ///< first chunk
        unsigned slots = 0; ///< chunk slots (consecutive chunks)
    };
    [[nodiscard]] window wanted_window() const;
    void reveal(int index);
    void clamp_offset();

    lx::ui::style layout_{};
    lx::layout::flex_style flex_{};
    std::uint64_t generation_ = 0;
    unsigned count_ = 0;
    int row_h_ = 23;
    list_row_fn row_ = nullptr;
    void* context_ = nullptr;
    handler on_select_{};
    handler on_activate_{};
    int selected_ = -1;
    int prop_selected_ = -2;
    float off_ = 0.f;
    unsigned built_first_ = 0;
    unsigned built_slots_ = 0;
    bool built_ = false;
    bool applied_ = false;
    char name_[32]{};
};

class list_builder {
public:
    list_builder(unsigned count, list_row_fn row, void* context)
        : count_{count}, row_{row}, context_{context} {}
    list_builder& layout(const lx::ui::style& s) { layout_ = s; return *this; }
    list_builder& spacing(list_item_spacing s) { spacing_ = s; return *this; }
    list_builder& row_height(int px) { row_height_ = px; return *this; }
    list_builder& selected(int index) { selected_ = index; return *this; }
    list_builder& on_select(handler h) { on_select_ = h; return *this; }
    list_builder& on_activate(handler h) { on_activate_ = h; return *this; }
    list_builder& name(const char* a11y) { name_ = a11y; return *this; }
    list_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    unsigned count_ = 0;
    list_row_fn row_ = nullptr;
    void* context_ = nullptr;
    lx::ui::style layout_{};
    list_item_spacing spacing_ = list_item_spacing::dense;
    int row_height_ = 0;
    int selected_ = -1;
    handler on_select_{};
    handler on_activate_{};
    const char* name_ = nullptr;
    element_key key_ = 0;
};

/// A list of `count` rows described by `row(context, list_row)`.
[[nodiscard]] list_builder list(unsigned count, list_row_fn row, void* context = nullptr);

/// Zed's `ListItem`.
class list_item_builder {
public:
    explicit list_item_builder(child content) : content_{content} {}
    list_item_builder& start_slot(child c) { start_ = c; return *this; }
    list_item_builder& end_slot(child c) { end_ = c; return *this; }
    list_item_builder& selected(bool on = true) { selected_ = on; return *this; }
    list_item_builder& disabled(bool on = true) { disabled_ = on; return *this; }
    list_item_builder& spacing(list_item_spacing s) { spacing_ = s; return *this; }
    /// Indent drawn outside the item, rounded inner box (menus).
    list_item_builder& inset(bool on = true) { inset_ = on; return *this; }
    list_item_builder& on_click(handler h) { on_click_ = h; return *this; }
    /// A list's row: its selection and `select` as the click.
    list_item_builder& row(const list_row& r) {
        selected_ = r.selected;
        on_click_ = r.select;
        return *this;
    }
    /// Takes the focus itself (a standalone item; a list's rows leave it to the list).
    list_item_builder& focusable(bool on = true) { focusable_ = on; return *this; }
    list_item_builder& name(const char* a11y) { name_ = a11y; return *this; }
    list_item_builder& key(element_key k) { key_ = k; return *this; }
    operator child() const; // NOLINT(google-explicit-constructor)

private:
    child content_ = nullptr;
    child start_ = nullptr;
    child end_ = nullptr;
    bool selected_ = false;
    bool disabled_ = false;
    bool inset_ = false;
    bool focusable_ = false;
    list_item_spacing spacing_ = list_item_spacing::dense;
    handler on_click_{};
    const char* name_ = nullptr;
    element_key key_ = 0;
};

[[nodiscard]] list_item_builder list_item(child content);
/// An item showing `text` as a label.
[[nodiscard]] list_item_builder list_item(const char* text);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

int lx::ui::list_row_height_px(list_item_spacing spacing, float rem_px) {
    const float l = active_theme().fonts().ui_line_height;
    const float line = label_size_px(label_size::normal, rem_px) * (l > 0.f ? l : k_phi);
    const int base = static_cast<int>(std::ceil(line - 1e-3f));
    switch (spacing) {
    case list_item_spacing::extra_dense: return base - 2; // `py_neg_px()`
    case list_item_spacing::sparse: return base + 8;      // `py_1()`
    case list_item_spacing::dense:
    default: return base;
    }
}

// ── list_chunk_node ──

lx::ui::element_type_id lx::ui::list_chunk_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::list_chunk_node::type() const { return static_type(); }
const char* lx::ui::list_chunk_node::type_name() const { return "list_chunk"; }

bool lx::ui::list_chunk_node::apply(const list_chunk_props& p) {
    const bool changed = !(p == props_);
    props_ = p;
    return changed;
}

lx::size2i lx::ui::list_chunk_node::measure(lx::layout::constraints c, size_hint) const {
    return {c.max.width < lx::layout::constraints{}.max.width ? c.max.width : 0,
            static_cast<int>(props_.count) * props_.row_height};
}

void lx::ui::list_chunk_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    const int h = props_.row_height;
    // A row's percentages resolve against the list's width and the row's height.
    const percent_base_scope base{static_cast<float>(bounds.width), static_cast<float>(h)};
    for (unsigned i = 0; i < child_count(); ++i)
        if (ui_node* r = child_at(i))
            r->place({bounds.x, bounds.y + static_cast<int>(i) * h, bounds.width, h});
}

// ── list_node ──

lx::ui::element_type_id lx::ui::list_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::list_node::type() const { return static_type(); }
const char* lx::ui::list_node::type_name() const { return "list"; }

lx::ui::list_node::list_node() {
    detail::viewport_created();
    set_flag(node_flag::focusable, true);
}

lx::ui::list_node::~list_node() { detail::viewport_destroyed(); }

bool lx::ui::list_node::apply(const list_props& p) {
    const std::uint64_t generation = theme_generation();
    bool layout_changed = !applied_;
    if (!applied_ || generation != generation_ || !(p.layout == layout_)) {
        lx::layout::flex_style flex = resolve_layout(p.layout, {ui_density(), ui_rem_px()});
        flex.overflow_y = lx::layout::overflow::scroll;
        layout_changed = layout_changed || !(flex == flex_);
        flex_ = flex;
        layout_ = p.layout;
    }
    const int h = p.row_height > 0 ? p.row_height : list_row_height_px(p.spacing, ui_rem_px());
    if (h != row_h_ || p.count != count_ || generation != generation_)
        layout_changed = true;
    generation_ = generation;
    row_h_ = h;
    count_ = p.count;
    row_ = p.row;
    context_ = p.context;
    on_select_ = p.on_select;
    on_activate_ = p.on_activate;
    if (p.selected != prop_selected_) {
        prop_selected_ = p.selected;
        selected_ = std::clamp(p.selected, -1, static_cast<int>(count_) - 1);
    }
    if (selected_ >= static_cast<int>(count_))
        selected_ = static_cast<int>(count_) - 1;
    std::size_t n = p.name ? std::strlen(p.name) : 0;
    n = std::min(n, sizeof(name_) - 1);
    if (n)
        std::memcpy(name_, p.name, n);
    name_[n] = '\0';
    applied_ = true;
    // The rows may describe differently now (the app's data): describe the visible ones.
    request_rebuild();
    return layout_changed;
}

lx::ui::list_node::window lx::ui::list_node::wanted_window() const {
    const unsigned total = (count_ + k_list_chunk_rows - 1) / k_list_chunk_rows;
    if (total == 0)
        return {0, 0};
    if (!virtualised())
        return {0, total};
    const int h = std::max(row_h_, 1);
    // Before the first layout the viewport is unknown: guess two chunks.
    const int vp = bounds_.height > 0 ? bounds_.height : h * static_cast<int>(k_list_chunk_rows) * 2;
    const int off = offset();
    const unsigned top = static_cast<unsigned>(off / h);
    const unsigned bottom = static_cast<unsigned>((off + vp + h - 1) / h);
    const unsigned first = top > k_list_margin_rows ? top - k_list_margin_rows : 0;
    const unsigned last = std::min(count_, bottom + k_list_margin_rows);
    const unsigned c0 = first / k_list_chunk_rows;
    const unsigned c1 = (last + k_list_chunk_rows - 1) / k_list_chunk_rows;
    const unsigned need = c1 > c0 ? c1 - c0 : 1;
    // The pool only grows (a slot count change re-slots every chunk once).
    const unsigned slots = std::min({std::max(need, built_slots_), total, k_list_max_chunks});
    const unsigned start = std::min(c0, total - slots);
    return {start, slots};
}

void lx::ui::list_node::rebuild(build_context& ctx, reconciler& rec) {
    build_scope scope{ctx};
    const window w = wanted_window();
    const unsigned slots = w.slots;
    const element* kids[k_list_max_chunks + 1]{};
    for (unsigned k = w.first; k < w.first + slots; ++k) {
        const unsigned first = k * k_list_chunk_rows;
        const unsigned n = std::min(k_list_chunk_rows, count_ - first);
        child rows[k_list_chunk_rows]{};
        for (unsigned j = 0; j < n; ++j) {
            const unsigned index = first + j;
            list_row r{};
            r.index = index;
            r.selected = static_cast<int>(index) == selected_;
            list_node* self = this;
            r.select = handler{[self, index](int) { self->select(static_cast<int>(index)); }};
            r.activate = handler{[self, index](int) { self->activate_row(index); }};
            rows[j] = row_ ? row_(context_, r) : nullptr;
        }
        kids[k % slots] = with_children(
            describe<list_chunk_node, list_chunk_props>({first, n, row_h_}), {rows, n});
    }
    kids[slots] = describe<scrollbar_node, scrollbar_props>({1});
    if (ctx.overflowed())
        return; // keep what is there rather than reconcile a truncated description
    ++rebuilds_;
    const unsigned old = child_count();
    reserve_children(slots + 1);
    bool changed = old != slots + 1;
    for (unsigned s = 0; s <= slots; ++s) {
        ui_node* prev = s < old ? child_at(s) : nullptr;
        const bool reused = prev && kids[s] && prev->type() == kids[s]->type &&
                            prev->key() == kids[s]->key;
        ui_node* settled = rec.apply(prev, kids[s]);
        changed = changed || !reused;
        place_child(s, settled);
    }
    if (old > slots + 1)
        truncate_children(slots + 1);
    else
        set_child_count(slots + 1);
    built_first_ = w.first;
    built_slots_ = slots;
    built_ = true;
    if (changed)
        mark_layout_dirty();
}

lx::size2i lx::ui::list_node::measure(lx::layout::constraints c, size_hint) const {
    constexpr int unbounded = lx::layout::constraints{}.max.width;
    lx::size2i s{c.max.width < unbounded ? c.max.width : 0, static_cast<int>(count_) * row_h_};
    s.width = std::clamp(s.width, c.min.width, std::max(c.min.width, c.max.width));
    s.height = std::clamp(s.height, c.min.height, std::max(c.min.height, c.max.height));
    return s;
}

lx::size2i lx::ui::list_node::measure_content(lx::layout::constraints c, size_hint hint) const {
    return measure(c, hint);
}

void lx::ui::list_node::clamp_offset() {
    const float max = std::max(static_cast<float>(static_cast<int>(count_) * row_h_ - bounds_.height), 0.f);
    off_ = std::clamp(off_, 0.f, max);
}

void lx::ui::list_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;
    clamp_offset();
    for (unsigned i = 0; i < child_count(); ++i) {
        ui_node* c = child_at(i);
        if (!c)
            continue;
        if (c->type() == list_chunk_node::static_type()) {
            const list_chunk_props& p = static_cast<list_chunk_node*>(c)->props();
            c->place({bounds.x, bounds.y + static_cast<int>(p.first) * row_h_, bounds.width,
                      static_cast<int>(p.count) * row_h_});
        } else if (c->type() == scrollbar_node::static_type()) {
            auto* bar = static_cast<scrollbar_node*>(c);
            bar->update(bounds, false);
            bar->place(bar->geometry().strip);
        }
    }
    // The viewport changed which rows show, or the rows were never described (a first build
    // the frame arena could not hold): describe them (ui_root lays out again).
    const window w = wanted_window();
    if (!built_ || w.first != built_first_ || w.slots != built_slots_)
        request_rebuild();
}

void lx::ui::list_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    out.push_clip_local(bounds_);
    out.push_origin({0, -offset()});
    const ui_node* bar = nullptr;
    for (unsigned i = 0; i < child_count(); ++i) {
        const ui_node* c = child_at(i);
        if (!c)
            continue;
        if (c->type() == scrollbar_node::static_type()) {
            bar = c;
            continue;
        }
        c->emit(out);
    }
    out.pop();
    out.pop();
    if (bar)
        bar->emit(out);
    if (has_flag(node_flag::focus_ring)) {
        static constexpr float k_square[4] = {0.f, 0.f, 0.f, 0.f};
        draw_focus_ring(out, bounds_, k_square);
    }
}

const lx::layout::flex_style* lx::ui::list_node::flex_item_style() const { return &flex_; }

bool lx::ui::list_node::child_viewport(const ui_node& child, lx::point2i& offset,
                                       lx::rect2i& clip) const {
    if (child.type() == scrollbar_node::static_type())
        return false;
    offset = {0, -this->offset()};
    clip = bounds_;
    return true;
}

lx::ui::scroll_metrics lx::ui::list_node::scroll_metrics_along(unsigned axis) const {
    scroll_metrics m{};
    if (!axis) {
        m.viewport = m.content = static_cast<float>(bounds_.width);
        return m;
    }
    m.offset = off_;
    m.viewport = static_cast<float>(bounds_.height);
    m.content = std::max(static_cast<float>(static_cast<int>(count_) * row_h_), m.viewport);
    return m;
}

void lx::ui::list_node::scroll_to(unsigned axis, float offset) {
    if (!axis)
        return;
    const int before = this->offset();
    off_ = offset;
    clamp_offset();
    if (this->offset() == before)
        return;
    invalidate_paint(*this);
    const window w = wanted_window();
    if (built_ && (w.first != built_first_ || w.slots != built_slots_))
        request_rebuild();
}

bool lx::ui::list_node::on_pointer(const pointer_event& e) {
    if (e.kind != pointer_kind::wheel || e.dy == 0.f)
        return false;
    const int before = offset();
    scroll_to(1, off_ + e.dy);
    return offset() != before;
}

unsigned lx::ui::list_node::page_rows() const {
    const int h = std::max(row_h_, 1);
    return static_cast<unsigned>(std::max(bounds_.height / h, 1));
}

void lx::ui::list_node::reveal(int index) {
    if (index < 0)
        return;
    const float y0 = static_cast<float>(index * row_h_);
    const float y1 = y0 + static_cast<float>(row_h_);
    const float vp = static_cast<float>(bounds_.height);
    if (y0 < off_)
        scroll_to(1, y0);
    else if (y1 > off_ + vp)
        scroll_to(1, y1 - vp);
}

void lx::ui::list_node::select(int index) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (count_ == 0)
        return;
    index = std::clamp(index, -1, static_cast<int>(count_) - 1);
    if (index == selected_) {
        reveal(index);
        return;
    }
    selected_ = index;
    reveal(index);
    // The two rows whose selection changed are described again (a paint change each).
    request_rebuild();
    if (on_select_)
        on_select_(index);
}

void lx::ui::list_node::activate_row(unsigned index) {
    select(static_cast<int>(index));
    if (on_activate_)
        on_activate_(static_cast<int>(index));
}

bool lx::ui::list_node::on_focus_key(key_event e, focus_manager&) {
    if (!e.pressed || count_ == 0 || !has_flag(node_flag::focused))
        return false;
    const int last = static_cast<int>(count_) - 1;
    const int page = static_cast<int>(page_rows());
    switch (e.key) {
    case key::up: select(selected_ < 0 ? last : std::max(selected_ - 1, 0)); return true;
    case key::down: select(selected_ < 0 ? 0 : std::min(selected_ + 1, last)); return true;
    case key::home: select(0); return true;
    case key::end: select(last); return true;
    case key::page_up: select(std::max((selected_ < 0 ? 0 : selected_) - page, 0)); return true;
    case key::page_down: select(std::min((selected_ < 0 ? 0 : selected_) + page, last)); return true;
    case key::enter:
    case key::space:
        if (selected_ < 0)
            return false;
        if (on_activate_)
            on_activate_(selected_);
        return true;
    default: return false;
    }
}

unsigned lx::ui::list_node::live_rows() const {
    unsigned n = 0;
    for (unsigned i = 0; i < child_count(); ++i)
        if (const ui_node* c = child_at(i); c && c->type() == list_chunk_node::static_type())
            n += c->child_count();
    return n;
}

lx::ui::ui_node* lx::ui::list_node::row_node(unsigned index) const {
    for (unsigned i = 0; i < child_count(); ++i) {
        const ui_node* c = child_at(i);
        if (!c || c->type() != list_chunk_node::static_type())
            continue;
        const list_chunk_props& p = static_cast<const list_chunk_node*>(c)->props();
        if (index >= p.first && index < p.first + p.count)
            return c->child_at(index - p.first);
    }
    return nullptr;
}

lx::a11y::node lx::ui::list_node::accessibility_node() const {
    unsigned state = 0;
    if (has_flag(node_flag::focused))
        state |= static_cast<unsigned>(a11y::state_flag::focused);
    return {a11y::role::list, name_, "", "", state, const_cast<list_node*>(this)};
}

lx::ui::list_builder::operator child() const {
    list_props p{};
    p.layout = layout_;
    p.count = count_;
    p.spacing = spacing_;
    p.row_height = row_height_;
    p.row = row_;
    p.context = context_;
    p.selected = selected_;
    p.on_select = on_select_;
    p.on_activate = on_activate_;
    p.name = name_;
    return describe<list_node, list_props>(p, key_);
}

lx::ui::list_builder lx::ui::list(unsigned count, list_row_fn row, void* context) {
    return list_builder{count, row, context};
}

// ── list_item ──

lx::ui::list_item_builder::operator child() const {
    const int h = list_row_height_px(spacing_, ui_rem_px());
    control_paint paint{};
    paint.rest = {selected_ ? lx::ui::paint(tok::ghost_element_selected)
                            : lx::ui::paint(tok::ghost_element_background),
                  k_transparent};
    paint.hover = {lx::ui::paint(tok::ghost_element_hover), k_transparent};
    paint.active = {lx::ui::paint(tok::ghost_element_active), k_transparent};
    if (inset_)
        for (radius& r : paint.corners)
            r = radius::sm;

    // The content: `h_flex().flex_grow_1().gap(Base06).overflow_hidden()` over the start
    // slot and the children; the end slot `h_flex().flex_shrink_1().overflow_hidden()`.
    child main_kids[2]{};
    unsigned n = 0;
    if (start_)
        main_kids[n++] = start_;
    if (content_)
        main_kids[n++] = content_;
    control_props main{};
    main.layout = style{}.flex_row().items_center().flex_grow(1.f).flex_shrink(1.f)
                      .min_w(lx::layout::px(0.f)).gap(sp::b06).overflow_hidden();
    child row_kids[2] = {control(main, {main_kids, n}), nullptr};
    unsigned m = 1;
    if (end_) {
        control_props end{};
        end.layout = style{}.flex_row().items_center().flex_shrink(1.f).overflow_hidden();
        row_kids[m++] = control(end, {&end_, 1u});
    }

    control_props c{};
    // `h_flex().w_full()` over `h_flex().w_full().gap_1().px(Base06)`, one box here; inset:
    // `px(Base04)` outside (a margin) and the box rounded `sm`.
    c.layout = style{}.flex_row().items_center().w_full().h(lx::layout::px(static_cast<float>(h)))
                   .gap(rems_from_px(4.f)).px(sp::b06);
    if (end_)
        c.layout.justify_between();
    if (inset_)
        c.layout.mx(sp::b04).flex_shrink(1.f).w(lx::layout::auto_length).flex_grow(1.f);
    c.paint = paint;
    c.on_activate = on_click_;
    c.role = lx::a11y::role::list_item;
    c.name = name_;
    c.a11y_state = selected_ ? static_cast<unsigned>(lx::a11y::state_flag::selected) : 0u;
    c.interactive = true;
    c.focusable = focusable_;
    c.disabled = disabled_;
    c.selected = selected_;
    c.focus_ring = focusable_;
    return control(c, {row_kids, m}, key_);
}

lx::ui::list_item_builder lx::ui::list_item(child content) { return list_item_builder{content}; }

lx::ui::list_item_builder lx::ui::list_item(const char* text) {
    return list_item_builder{label(text).truncate()}.name(text);
}
