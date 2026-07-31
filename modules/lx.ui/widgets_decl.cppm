module;

#include <cstring>

import lx.foundation;
import lx.layout;
import lx.scene;

export module lx.ui:widgets_decl;

import :element;
import :node;
import :events;

export namespace lx::ui {

// ── box ───────────────────────────────────────────────────────────────────────

struct box_props {
    lx::color fill{};
    bool row = false;
    int gap = 0;
};

class box_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const element_type_id id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "box"; }

    void apply(const box_props& props) { props_ = props; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;

private:
    box_props props_{};
};

[[nodiscard]] child box(lx::color fill, child content);
[[nodiscard]] child box(child content);

// ── text ────────────────────────────────────────────────────────────────────

struct text_props {
    const char* label = "";
    lx::color color{};
};

class text_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const element_type_id id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "text"; }

    void apply(const text_props& props) { props_ = props; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void emit(draw_scope& out) const override;

private:
    text_props props_{};
};

[[nodiscard]] child text(const char* label, lx::color color = lx::color::rgb(1.f, 1.f, 1.f));

// ── button ──────────────────────────────────────────────────────────────────

struct button_props {
    const char* label = "";
    lx::color fill{};
    lx::color text_color{};
    void (*on_press)() = nullptr;
};

class button_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const element_type_id id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "button"; }

    void apply(const button_props& props) { props_ = props; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void emit(draw_scope& out) const override;
    bool on_mouse(mouse_event event) override;

private:
    button_props props_{};
};

[[nodiscard]] child button(const char* label, void (*on_press)(), lx::color fill = {});

} // namespace lx::ui


lx::size2i lx::ui::box_node::measure(lx::layout::constraints c, size_hint hint) const {
    return ui_node::measure(c, hint);
}

void lx::ui::box_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_dirty_ = false;

    int cursor_x = bounds.x;
    int cursor_y = bounds.y;
    int max_cross = 0;
    for (unsigned i = 0; i < child_count_; ++i) {
        if (!children_[i])
            continue;
        const auto child_size = children_[i]->measure({{0, 0}, {bounds.width, bounds.height}},
                                                      size_hint::preferred);
        if (props_.row) {
            children_[i]->arrange({cursor_x, bounds.y, child_size.width, child_size.height});
            cursor_x += child_size.width + props_.gap;
            if (child_size.height > max_cross)
                max_cross = child_size.height;
        } else {
            children_[i]->arrange({bounds.x, cursor_y, child_size.width, child_size.height});
            cursor_y += child_size.height + props_.gap;
            if (child_size.width > max_cross)
                max_cross = child_size.width;
        }
    }
    (void)max_cross;
}

void lx::ui::box_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    if (props_.fill.a > 0.f)
        out.fill(bounds_, props_.fill);
    ui_node::emit(out);
}

lx::ui::child lx::ui::box(lx::color fill, child content) {
    return with_children(describe<box_node, box_props>({fill}), &content, content ? 1u : 0u);
}

lx::ui::child lx::ui::box(child content) {
    return box(lx::color{}, content);
}

lx::size2i lx::ui::text_node::measure(lx::layout::constraints c, size_hint) const {
    const int width = props_.label ? static_cast<int>(std::strlen(props_.label)) * 8 : 0;
    lx::size2i out{width, 20};
    if (out.width < c.min.width)
        out.width = c.min.width;
    if (out.height < c.min.height)
        out.height = c.min.height;
    if (out.width > c.max.width)
        out.width = c.max.width;
    if (out.height > c.max.height)
        out.height = c.max.height;
    return out;
}

void lx::ui::text_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    // Placeholder: solid bar until lx.text shaping is wired into draw_scope.
    out.fill(bounds_, props_.color);
}

lx::ui::child lx::ui::text(const char* label, lx::color color) {
    return describe<text_node, text_props>({label, color});
}

lx::size2i lx::ui::button_node::measure(lx::layout::constraints c, size_hint hint) const {
    lx::size2i out{120, 32};
    if (out.width < c.min.width)
        out.width = c.min.width;
    if (out.height < c.min.height)
        out.height = c.min.height;
    if (out.width > c.max.width)
        out.width = c.max.width;
    if (out.height > c.max.height)
        out.height = c.max.height;
    return out;
}

void lx::ui::button_node::emit(draw_scope& out) const {
    if (!visible_)
        return;
    out.fill(bounds_, props_.fill.a > 0.f ? props_.fill : lx::color::rgb(0.2f, 0.2f, 0.25f));
    lx::rect2i label_bounds = bounds_;
    label_bounds.x += 8;
    label_bounds.width -= 16;
    out.fill(label_bounds, props_.text_color.a > 0.f ? props_.text_color
                                                     : lx::color::rgb(1.f, 1.f, 1.f));
}

bool lx::ui::button_node::on_mouse(mouse_event event) {
    if (!visible_ || !bounds_.contains(event.position))
        return false;
    if (event.pressed && props_.on_press)
        props_.on_press();
    return true;
}

lx::ui::child lx::ui::button(const char* label, void (*on_press)(), lx::color fill) {
    return describe<button_node, button_props>({label, fill, lx::color::rgb(1.f, 1.f, 1.f), on_press});
}
