module;

import lx.foundation;
import lx.ui;
import lx.layout;
import lx.runtime;

export module lx.ui.builder;

export namespace lx::ui {

// ── Fluent builders ─────────────────────────────────────────────────────────

class window_builder {
public:
    window_builder& title(const char* t);
    window_builder& size(lx::size2i s);
    window_builder& resizable(bool r);
    window_builder& decorated(bool d);
    [[nodiscard]] lx::result<window> build();

private:
    const char* title_ = "Lumen";
    lx::size2i size_{800, 600};
    bool resizable_ = true;
    bool decorated_ = true;
};

class container_builder;

template<typename WidgetT>
class widget_builder;

class flex_builder {
public:
    explicit flex_builder(container_builder& parent);

    flex_builder& direction(layout::axis axis);
    flex_builder& spacing(int px);
    flex_builder& main_align(layout::align a);
    flex_builder& cross_align(layout::align a);

    /// Per-call builder — not shared static state (AGENTS.md anti-pattern).
    template<typename WidgetT>
    [[nodiscard]] widget_builder<WidgetT> add() {
        return widget_builder<WidgetT>{};
    }

    container_builder& end();

private:
    container_builder* parent_ = nullptr;
    layout::flex_node layout_{};
};

class container_builder {
public:
    flex_builder vertical();
    flex_builder horizontal();
    container_builder& padding(lx::inset p);
    container_builder& add_child(lx::ui::widget& w);
};

template<typename WidgetT>
class widget_builder {
public:
    widget_builder& id(const char* widget_id);
    widget_builder& text(const char* t);
    widget_builder& enabled(bool e);
    widget_builder& style(style_ref s);
    widget_builder& on_click(lx::runtime::task handler);
    widget_builder& on_change(lx::runtime::task handler);
    void mount(window& parent);
    [[nodiscard]] WidgetT& get();

private:
    WidgetT widget_{};
    window* mounted_window_ = nullptr;
};

// ── Convenience entry points ─────────────────────────────────────────────────

[[nodiscard]] window_builder make_window();
[[nodiscard]] container_builder make_column();
[[nodiscard]] container_builder make_row();

} // namespace lx::ui

module :private;

lx::ui::window_builder& lx::ui::window_builder::title(const char* t) {
    title_ = t;
    return *this;
}
lx::ui::window_builder& lx::ui::window_builder::size(lx::size2i s) {
    size_ = s;
    return *this;
}
lx::ui::window_builder& lx::ui::window_builder::resizable(bool r) {
    resizable_ = r;
    return *this;
}
lx::ui::window_builder& lx::ui::window_builder::decorated(bool d) {
    decorated_ = d;
    return *this;
}
lx::result<lx::ui::window> lx::ui::window_builder::build() {
    return lx::ui::window::create(title_, size_);
}

lx::ui::flex_builder::flex_builder(container_builder& parent) : parent_{&parent} {}
lx::ui::flex_builder& lx::ui::flex_builder::direction(layout::axis a) {
    layout_.direction = a;
    return *this;
}
lx::ui::flex_builder& lx::ui::flex_builder::spacing(int px) {
    layout_.spacing = px;
    return *this;
}
lx::ui::flex_builder& lx::ui::flex_builder::main_align(layout::align a) {
    layout_.main_align = a;
    return *this;
}
lx::ui::flex_builder& lx::ui::flex_builder::cross_align(layout::align a) {
    layout_.cross_align = a;
    return *this;
}
lx::ui::container_builder& lx::ui::flex_builder::end() { return *parent_; }

lx::ui::flex_builder lx::ui::container_builder::vertical() {
    flex_builder fb{*this};
    fb.direction(layout::axis::vertical);
    return fb;
}
lx::ui::flex_builder lx::ui::container_builder::horizontal() {
    flex_builder fb{*this};
    fb.direction(layout::axis::horizontal);
    return fb;
}
lx::ui::container_builder& lx::ui::container_builder::padding(lx::inset) { return *this; }
lx::ui::container_builder& lx::ui::container_builder::add_child(lx::ui::widget&) {
    return *this;
}

template<typename WidgetT>
lx::ui::widget_builder<WidgetT>& lx::ui::widget_builder<WidgetT>::id(const char*) {
    return *this;
}
template<typename WidgetT>
lx::ui::widget_builder<WidgetT>& lx::ui::widget_builder<WidgetT>::text(const char* t) {
    if constexpr (requires(WidgetT w) { w.set_text(""); })
        widget_.set_text(t);
    return *this;
}
template<typename WidgetT>
lx::ui::widget_builder<WidgetT>& lx::ui::widget_builder<WidgetT>::enabled(bool e) {
    widget_.set_enabled(e);
    return *this;
}
template<typename WidgetT>
lx::ui::widget_builder<WidgetT>& lx::ui::widget_builder<WidgetT>::style(style_ref s) {
    widget_.set_style(s);
    return *this;
}
template<typename WidgetT>
lx::ui::widget_builder<WidgetT>&
lx::ui::widget_builder<WidgetT>::on_click(lx::runtime::task cb) {
    if constexpr (requires(WidgetT w) { w.set_on_click(nullptr); })
        widget_.set_on_click(cb);
    return *this;
}
template<typename WidgetT>
lx::ui::widget_builder<WidgetT>&
lx::ui::widget_builder<WidgetT>::on_change(lx::runtime::task cb) {
    if constexpr (requires(WidgetT w) { w.set_on_change(nullptr); })
        widget_.set_on_change(cb);
    return *this;
}
template<typename WidgetT>
void lx::ui::widget_builder<WidgetT>::mount(window& parent) {
    mounted_window_ = &parent;
}
template<typename WidgetT>
WidgetT& lx::ui::widget_builder<WidgetT>::get() {
    return widget_;
}

lx::ui::window_builder lx::ui::make_window() { return {}; }
lx::ui::container_builder lx::ui::make_column() { return {}; }
lx::ui::container_builder lx::ui::make_row() { return {}; }
