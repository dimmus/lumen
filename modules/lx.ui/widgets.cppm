module;

import lx.foundation;
import lx.scene;
import lx.layout;
import lx.text;
import lx.runtime;
import lx.gfx;
import lx.wayland.client;
import lx.a11y;

export module lx.ui:widgets;

import :events;
import :style;

export namespace lx::ui {

class widget {
public:
    virtual ~widget() = default;

    [[nodiscard]] virtual const char* type_name() const;
    [[nodiscard]] virtual lx::size2i preferred_size(layout::constraints c) const;

    virtual void layout(lx::rect2i bounds);
    virtual void paint(lx::gfx::render_pass& pass);
    virtual bool on_mouse(mouse_event event);
    virtual bool on_key(key_event event);

    void invalidate();
    void set_visible(bool visible);
    void set_enabled(bool enabled);
    void set_style(style_ref style);

    [[nodiscard]] lx::rect2i bounds() const;
    [[nodiscard]] lx::scene::scene_node* scene_node() const;
    [[nodiscard]] a11y::node accessibility_node() const;

protected:
    lx::rect2i bounds_{};
    bool visible_ = true;
    bool enabled_ = true;
    style_ref style_{};
    a11y::role a11y_role_ = a11y::role::unknown;
    const char* a11y_name_ = "";
};

class label : public widget {
public:
    void set_text(const char* text);
    [[nodiscard]] const char* text() const;
    lx::size2i preferred_size(layout::constraints c) const override;
    void paint(lx::gfx::render_pass& pass) override;

private:
    const char* text_ = "";
    text::text_layout layout_{};
};

class button : public widget {
public:
    button();
    void set_text(const char* text);
    void set_on_click(lx::runtime::task handler);
    lx::size2i preferred_size(layout::constraints c) const override;
    bool on_mouse(mouse_event event) override;
    void paint(lx::gfx::render_pass& pass) override;

private:
    const char* text_ = "";
    lx::runtime::task on_click_{};
    state visual_state_ = state::normal;
};

class entry : public widget {
public:
    entry();
    void set_text(const char* text);
    void set_placeholder(const char* text);
    void set_on_change(lx::runtime::task handler);
    lx::size2i preferred_size(layout::constraints c) const override;
    bool on_key(key_event event) override;
    void insert_text(const char* utf8);
    void select_all();

private:
    const char* text_ = "";
    const char* placeholder_ = "";
    lx::runtime::task on_change_{};
    unsigned caret_ = 0;
};

class checkbox : public widget {
public:
    checkbox();
    void set_checked(bool checked);
    [[nodiscard]] bool checked() const;
    void set_label(const char* text);
    bool on_mouse(mouse_event event) override;

private:
    bool checked_ = false;
    const char* label_ = "";
};

class panel : public widget {
public:
    void set_content(widget* child);
    lx::size2i preferred_size(layout::constraints c) const override;
    void layout(lx::rect2i bounds) override;

private:
    widget* content_ = nullptr;
};

class image : public widget {
public:
    void set_texture(lx::texture_id texture);
    lx::size2i preferred_size(layout::constraints c) const override;
    void paint(lx::gfx::render_pass& pass) override;

private:
    lx::texture_id texture_{};
    lx::size2i intrinsic_{64, 64};
};

class scroll_area : public widget {
public:
    void set_content(widget* child);
    void set_scroll_offset(lx::point2i offset);
    lx::size2i preferred_size(layout::constraints c) const override;

private:
    widget* content_ = nullptr;
    lx::point2i scroll_{};
};

class window : public widget {
public:
    [[nodiscard]] static lx::result<window> create(const char* title, lx::size2i size);

    void set_title(const char* title);
    void set_content(widget* root);
    void show();
    void close();
    void set_resizable(bool resizable);

    [[nodiscard]] lx::wayland::surface* native_surface();

private:
    const char* title_ = "";
    widget* content_ = nullptr;
    bool resizable_ = true;
    lx::wayland::surface native_{};
};

} // namespace lx::ui

const char* lx::ui::widget::type_name() const { return "widget"; }
lx::size2i lx::ui::widget::preferred_size(layout::constraints) const { return {100, 32}; }
void lx::ui::widget::layout(lx::rect2i b) { bounds_ = b; }
void lx::ui::widget::paint(lx::gfx::render_pass&) {}
bool lx::ui::widget::on_mouse(mouse_event) { return false; }
bool lx::ui::widget::on_key(key_event) { return false; }
void lx::ui::widget::invalidate() {}
void lx::ui::widget::set_visible(bool v) { visible_ = v; }
void lx::ui::widget::set_enabled(bool e) { enabled_ = e; }
void lx::ui::widget::set_style(style_ref s) { style_ = s; }
lx::rect2i lx::ui::widget::bounds() const { return bounds_; }
lx::scene::scene_node* lx::ui::widget::scene_node() const { return nullptr; }
lx::a11y::node lx::ui::widget::accessibility_node() const {
    return {a11y_role_, a11y_name_, "", "", 0, const_cast<widget*>(this)};
}

void lx::ui::label::set_text(const char* t) {
    text_ = t;
    layout_.set_text(t);
}
const char* lx::ui::label::text() const { return text_; }
lx::size2i lx::ui::label::preferred_size(layout::constraints c) const {
    const auto sz = layout_.bounding_size();
    if (sz.width > 0) return sz;
    return widget::preferred_size(c);
}
void lx::ui::label::paint(lx::gfx::render_pass&) {}

lx::ui::button::button() { a11y_role_ = a11y::role::button; }
void lx::ui::button::set_text(const char* t) {
    text_ = t;
    a11y_name_ = t;
}
void lx::ui::button::set_on_click(lx::runtime::task cb) { on_click_ = cb; }
lx::size2i lx::ui::button::preferred_size(layout::constraints c) const {
    return widget::preferred_size(c);
}
bool lx::ui::button::on_mouse(mouse_event e) {
    if (e.pressed && on_click_) on_click_();
    return true;
}
void lx::ui::button::paint(lx::gfx::render_pass&) {}

lx::ui::entry::entry() { a11y_role_ = a11y::role::text_field; }
void lx::ui::entry::set_text(const char* t) { text_ = t; }
void lx::ui::entry::set_placeholder(const char* t) { placeholder_ = t; }
void lx::ui::entry::set_on_change(lx::runtime::task cb) { on_change_ = cb; }
lx::size2i lx::ui::entry::preferred_size(layout::constraints c) const {
    return widget::preferred_size(c);
}
bool lx::ui::entry::on_key(key_event e) {
    if (!e.pressed) return true;
    if (e.key == key::backspace && caret_ > 0) --caret_;
    return true;
}
void lx::ui::entry::insert_text(const char* utf8) {
    (void)utf8;
    if (on_change_) on_change_();
}
void lx::ui::entry::select_all() { caret_ = 0; }

lx::ui::checkbox::checkbox() { a11y_role_ = a11y::role::button; }
void lx::ui::checkbox::set_checked(bool c) { checked_ = c; }
bool lx::ui::checkbox::checked() const { return checked_; }
void lx::ui::checkbox::set_label(const char* t) {
    label_ = t;
    a11y_name_ = t;
}
bool lx::ui::checkbox::on_mouse(mouse_event e) {
    if (e.pressed) checked_ = !checked_;
    return true;
}

void lx::ui::panel::set_content(widget* w) { content_ = w; }
lx::size2i lx::ui::panel::preferred_size(layout::constraints c) const {
    if (content_) return content_->preferred_size(c);
    return widget::preferred_size(c);
}
void lx::ui::panel::layout(lx::rect2i b) {
    widget::layout(b);
    if (content_) content_->layout(b);
}

void lx::ui::image::set_texture(lx::texture_id t) { texture_ = t; }
lx::size2i lx::ui::image::preferred_size(layout::constraints) const { return intrinsic_; }
void lx::ui::image::paint(lx::gfx::render_pass&) {}

void lx::ui::scroll_area::set_content(widget* w) { content_ = w; }
void lx::ui::scroll_area::set_scroll_offset(lx::point2i o) { scroll_ = o; }
lx::size2i lx::ui::scroll_area::preferred_size(layout::constraints c) const {
    return widget::preferred_size(c);
}

lx::result<lx::ui::window> lx::ui::window::create(const char* title, lx::size2i size) {
    window w{};
    w.set_title(title);
    w.layout({0, 0, size.width, size.height});
    return w;
}

void lx::ui::window::set_title(const char* t) { title_ = t; }
void lx::ui::window::set_content(widget* w) { content_ = w; }
void lx::ui::window::show() {}
void lx::ui::window::close() {}
void lx::ui::window::set_resizable(bool r) { resizable_ = r; }
lx::wayland::surface* lx::ui::window::native_surface() { return &native_; }
