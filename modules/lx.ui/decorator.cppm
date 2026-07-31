module;

#include <concepts>

import lx.foundation;
import lx.layout;

export module lx.ui:decorator;

import :element;
import :node;

export namespace lx::ui {

/// Anything that wraps one descriptor in another. Decorators are values, not shared
/// builder objects, so two `child`s can be decorated concurrently without interference
/// — the failure mode a `static` fluent builder has.
template<typename F>
concept decorator = requires(F f, child c) {
    { f(c) } -> std::same_as<child>;
};

/// Postfix composition: `content | insets(8) | align(center) | grow(1)`.
/// Reads outside-in like the resulting tree.
template<decorator F>
[[nodiscard]] child operator|(child c, F f) {
    return c ? f(c) : nullptr;
}

// ── Layout wrapper nodes ────────────────────────────────────────────────────────
// Each is a single-child proxy that adjusts geometry. They exist as retained nodes so
// reconciliation can reuse them, and they are the reason modifiers compose freely.

struct insets_props {
    lx::inset padding{};
};

class insets_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    void apply(const insets_props& props);

    [[nodiscard]] lx::size2i measure(layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;

private:
    insets_props props_{};
};

struct align_props {
    layout::align horizontal = layout::align::center;
    layout::align vertical = layout::align::center;
};

class align_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    void apply(const align_props& props);

    [[nodiscard]] lx::size2i measure(layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;

private:
    align_props props_{};
};

struct sizing_props {
    lx::size2i min{};
    lx::size2i max{99999, 99999};
};

class sizing_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    void apply(const sizing_props& props);

    [[nodiscard]] lx::size2i measure(layout::constraints c, size_hint hint) const override;

private:
    sizing_props props_{};
};

struct grow_props {
    unsigned factor = 1;
};

/// Marks a child as consuming leftover main-axis space. Flex containers read
/// `grow_factor()` off their children rather than tracking it themselves.
class grow_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    void apply(const grow_props& props);
    [[nodiscard]] unsigned grow_factor() const;

private:
    grow_props props_{};
};

struct opacity_props {
    float value = 1.f;
};

class opacity_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    void apply(const opacity_props& props);
    void emit(draw_scope& out) const override;

private:
    opacity_props props_{};
};

struct clip_props {
    bool enabled = true;
};

class clip_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    void apply(const clip_props& props);
    void emit(draw_scope& out) const override;

private:
    clip_props props_{};
};

// ── Curried modifiers ───────────────────────────────────────────────────────────
// Each has a direct form for explicit nesting and a curried form for the pipe.

[[nodiscard]] child insets(lx::inset padding, child content);
[[nodiscard]] auto insets(lx::inset padding) {
    return [padding](child content) { return lx::ui::insets(padding, content); };
}
[[nodiscard]] auto insets(int uniform) {
    const lx::inset padding{uniform, uniform, uniform, uniform};
    return [padding](child content) { return lx::ui::insets(padding, content); };
}

[[nodiscard]] child align(layout::align horizontal, layout::align vertical, child content);
[[nodiscard]] auto align(layout::align horizontal, layout::align vertical) {
    return [horizontal, vertical](child content) {
        return lx::ui::align(horizontal, vertical, content);
    };
}
[[nodiscard]] auto center() {
    return [](child content) {
        return lx::ui::align(layout::align::center, layout::align::center, content);
    };
}

[[nodiscard]] child sizing(lx::size2i min, lx::size2i max, child content);
[[nodiscard]] auto sizing(lx::size2i min, lx::size2i max) {
    return [min, max](child content) { return lx::ui::sizing(min, max, content); };
}
[[nodiscard]] auto pin_size(lx::size2i exact) {
    return [exact](child content) { return lx::ui::sizing(exact, exact, content); };
}
[[nodiscard]] auto min_size(lx::size2i min) {
    return [min](child content) { return lx::ui::sizing(min, {99999, 99999}, content); };
}

[[nodiscard]] child grow(unsigned factor, child content);
[[nodiscard]] auto grow(unsigned factor = 1) {
    return [factor](child content) { return lx::ui::grow(factor, content); };
}

[[nodiscard]] child opacity(float value, child content);
[[nodiscard]] auto opacity(float value) {
    return [value](child content) { return lx::ui::opacity(value, content); };
}

[[nodiscard]] child clip(child content);
[[nodiscard]] auto clipped() {
    return [](child content) { return lx::ui::clip(content); };
}

/// Attach a reconciliation identity. Needed on list rows so reordering keeps state.
[[nodiscard]] auto keyed(element_key key) {
    return [key](child content) -> child {
        if (content)
            const_cast<element*>(content)->key = key;
        return content;
    };
}

} // namespace lx::ui


namespace {

[[nodiscard]] lx::ui::child lx_ui_wrap(lx::ui::child described, lx::ui::child content) {
    if (!described)
        return nullptr;
    const lx::ui::child items[1] = {content};
    return lx::ui::with_children(described, items, 1);
}

[[nodiscard]] int lx_ui_align_offset(lx::layout::align how, int available, int used) {
    const int slack = available - used;
    if (slack <= 0)
        return 0;
    switch (how) {
    case lx::layout::align::center:
        return slack / 2;
    case lx::layout::align::end:
        return slack;
    case lx::layout::align::start:
    case lx::layout::align::stretch:
    case lx::layout::align::space_between:
    case lx::layout::align::space_around:
    default:
        return 0;
    }
}

} // namespace

// ── insets ──────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::insets_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::insets_node::type() const { return static_type(); }
const char* lx::ui::insets_node::type_name() const { return "insets"; }
void lx::ui::insets_node::apply(const insets_props& props) { props_ = props; }

lx::size2i lx::ui::insets_node::measure(layout::constraints c, size_hint hint) const {
    const int extra_w = props_.padding.left + props_.padding.right;
    const int extra_h = props_.padding.top + props_.padding.bottom;

    layout::constraints inner = c;
    inner.max.width = c.max.width > extra_w ? c.max.width - extra_w : 0;
    inner.max.height = c.max.height > extra_h ? c.max.height - extra_h : 0;
    inner.min.width = c.min.width > extra_w ? c.min.width - extra_w : 0;
    inner.min.height = c.min.height > extra_h ? c.min.height - extra_h : 0;

    const ui_node* content = child_at(0);
    const lx::size2i inner_size = content ? content->measure(inner, hint) : lx::size2i{};
    return {inner_size.width + extra_w, inner_size.height + extra_h};
}

void lx::ui::insets_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    clear_layout_dirty();
    if (ui_node* content = child_at(0)) {
        content->arrange({bounds.x + props_.padding.left, bounds.y + props_.padding.top,
                          bounds.width - props_.padding.left - props_.padding.right,
                          bounds.height - props_.padding.top - props_.padding.bottom});
    }
}

// ── align ───────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::align_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::align_node::type() const { return static_type(); }
const char* lx::ui::align_node::type_name() const { return "align"; }
void lx::ui::align_node::apply(const align_props& props) { props_ = props; }

lx::size2i lx::ui::align_node::measure(layout::constraints c, size_hint hint) const {
    // Alignment consumes whatever it is given; the child keeps its own preference.
    if (hint == size_hint::min) {
        const ui_node* content = child_at(0);
        return content ? content->measure(c, hint) : c.min;
    }
    return c.max;
}

void lx::ui::align_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    clear_layout_dirty();
    ui_node* content = child_at(0);
    if (!content)
        return;

    layout::constraints c{};
    c.max = bounds.size();
    const lx::size2i wanted = content->measure(c, size_hint::preferred);

    const int width = props_.horizontal == layout::align::stretch ? bounds.width : wanted.width;
    const int height = props_.vertical == layout::align::stretch ? bounds.height : wanted.height;

    content->arrange({bounds.x + lx_ui_align_offset(props_.horizontal, bounds.width, width),
                      bounds.y + lx_ui_align_offset(props_.vertical, bounds.height, height),
                      width, height});
}

// ── sizing ──────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::sizing_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::sizing_node::type() const { return static_type(); }
const char* lx::ui::sizing_node::type_name() const { return "sizing"; }
void lx::ui::sizing_node::apply(const sizing_props& props) { props_ = props; }

lx::size2i lx::ui::sizing_node::measure(layout::constraints c, size_hint hint) const {
    layout::constraints clamped = c;
    if (props_.min.width > clamped.min.width) clamped.min.width = props_.min.width;
    if (props_.min.height > clamped.min.height) clamped.min.height = props_.min.height;
    if (props_.max.width < clamped.max.width) clamped.max.width = props_.max.width;
    if (props_.max.height < clamped.max.height) clamped.max.height = props_.max.height;
    if (clamped.max.width < clamped.min.width) clamped.max.width = clamped.min.width;
    if (clamped.max.height < clamped.min.height) clamped.max.height = clamped.min.height;
    return ui_node::measure(clamped, hint);
}

// ── grow ────────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::grow_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::grow_node::type() const { return static_type(); }
const char* lx::ui::grow_node::type_name() const { return "grow"; }
void lx::ui::grow_node::apply(const grow_props& props) { props_ = props; }
unsigned lx::ui::grow_node::grow_factor() const { return props_.factor; }

// ── opacity ─────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::opacity_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::opacity_node::type() const { return static_type(); }
const char* lx::ui::opacity_node::type_name() const { return "opacity"; }
void lx::ui::opacity_node::apply(const opacity_props& props) { props_ = props; }

void lx::ui::opacity_node::emit(draw_scope& out) const {
    if (!visible())
        return;
    out.push_opacity(props_.value);
    ui_node::emit(out);
    out.pop();
}

// ── clip ────────────────────────────────────────────────────────────────────────

lx::ui::element_type_id lx::ui::clip_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::clip_node::type() const { return static_type(); }
const char* lx::ui::clip_node::type_name() const { return "clip"; }
void lx::ui::clip_node::apply(const clip_props& props) { props_ = props; }

void lx::ui::clip_node::emit(draw_scope& out) const {
    if (!visible())
        return;
    if (!props_.enabled) {
        ui_node::emit(out);
        return;
    }
    out.push_clip(bounds());
    ui_node::emit(out);
    out.pop();
}

// ── modifier entry points ───────────────────────────────────────────────────────

lx::ui::child lx::ui::insets(lx::inset padding, child content) {
    return lx_ui_wrap(describe<insets_node, insets_props>({padding}), content);
}

lx::ui::child lx::ui::align(layout::align horizontal, layout::align vertical, child content) {
    return lx_ui_wrap(describe<align_node, align_props>({horizontal, vertical}), content);
}

lx::ui::child lx::ui::sizing(lx::size2i min, lx::size2i max, child content) {
    return lx_ui_wrap(describe<sizing_node, sizing_props>({min, max}), content);
}

lx::ui::child lx::ui::grow(unsigned factor, child content) {
    return lx_ui_wrap(describe<grow_node, grow_props>({factor}), content);
}

lx::ui::child lx::ui::opacity(float value, child content) {
    return lx_ui_wrap(describe<opacity_node, opacity_props>({value}), content);
}

lx::ui::child lx::ui::clip(child content) {
    return lx_ui_wrap(describe<clip_node, clip_props>({true}), content);
}
