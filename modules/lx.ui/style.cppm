module;

import lx.foundation;

export module lx.ui:style;

export namespace lx::ui {

enum class state { normal, hovered, pressed, focused, disabled };

struct style_property {
    const char* key = "";
    lx::color value{};
    float number = 0.f;
};

class theme {
public:
    [[nodiscard]] static lx::result<theme> load(const char* compiled_path);
    [[nodiscard]] style_property resolve(const char* widget_type, state st,
                                         const char* property) const;
};

class style_ref {
public:
    style_ref() = default;
    explicit style_ref(const theme* t, const char* widget_type);

    style_ref& set(const char* property, lx::color value);
    style_ref& set(const char* property, float value);
    [[nodiscard]] style_property get(state st, const char* property) const;

private:
    const theme* theme_ = nullptr;
    const char* widget_type_ = "widget";
};

} // namespace lx::ui


lx::result<lx::ui::theme> lx::ui::theme::load(const char* compiled_path) {
    (void)compiled_path;
    return lx::not_implemented("lx::ui::theme::load");
}
lx::ui::style_property lx::ui::theme::resolve(const char*, state, const char* key) const {
    return {key, {}, 0.f};
}
lx::ui::style_ref::style_ref(const theme* t, const char* type) : theme_{t}, widget_type_{type} {}
lx::ui::style_ref& lx::ui::style_ref::set(const char*, lx::color) { return *this; }
lx::ui::style_ref& lx::ui::style_ref::set(const char*, float) { return *this; }
lx::ui::style_property lx::ui::style_ref::get(state st, const char* property) const {
    if (theme_) return theme_->resolve(widget_type_, st, property);
    return {property, {}, 0.f};
}
