module;

import lx.foundation;

export module lx.ui:focus;

import :events;
import :widgets;

export namespace lx::ui {

struct focus_style {
    lx::color ring_color = lx::color::rgb(0.2f, 0.5f, 1.f, 1.f);
    int ring_width = 2;
    bool high_contrast = false;
    bool reduced_motion = false;
};

class focus_manager {
public:
    void set_root(widget* root);
    void set_style(focus_style style);
    [[nodiscard]] widget* focused() const;

    bool focus_next();
    bool focus_previous();
    bool handle_key(key_event event);

    void notify_compositor_magnifier(lx::rect2i focus_bounds);

private:
    widget* root_ = nullptr;
    widget* focused_ = nullptr;
    focus_style style_{};
};

} // namespace lx::ui


void lx::ui::focus_manager::set_root(widget* root) { root_ = root; }
void lx::ui::focus_manager::set_style(focus_style style) { style_ = style; }
lx::ui::widget* lx::ui::focus_manager::focused() const { return focused_; }

bool lx::ui::focus_manager::focus_next() {
    focused_ = root_;
    return focused_ != nullptr;
}

bool lx::ui::focus_manager::focus_previous() { return focus_next(); }

bool lx::ui::focus_manager::handle_key(key_event event) {
    if (event.key == key::tab && event.pressed) return focus_next();
    if (!focused_) return false;
    return focused_->on_key(event);
}

void lx::ui::focus_manager::notify_compositor_magnifier(lx::rect2i focus_bounds) {
    (void)focus_bounds;
    (void)style_;
}
