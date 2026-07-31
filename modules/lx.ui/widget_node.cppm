module;

import lx.foundation;
import lx.scene;
import lx.gfx;

export module lx.ui:widget_node;

import :widgets;

export namespace lx::ui {

class widget_node : public lx::scene::scene_node {
public:
    explicit widget_node(widget& w);

    lx::scene::node_kind kind() const override { return lx::scene::node_kind::widget; }
    void sync_bounds();
    void emit_draws(lx::scene::draw_list& out) const override;

    [[nodiscard]] widget& ui_widget() const;

private:
    widget* widget_ = nullptr;
};

} // namespace lx::ui


lx::ui::widget_node::widget_node(widget& w) : widget_{&w} {}

void lx::ui::widget_node::sync_bounds() {
    if (!widget_) return;
    mark_dirty(widget_->bounds());
}

void lx::ui::widget_node::emit_draws(lx::scene::draw_list& out) const {
    if (!widget_ || !is_visible()) return;
    lx::gfx::render_pass pass{};
    const_cast<widget*>(widget_)->paint(pass);
    lx::scene::draw_command cmd{};
    cmd.dst = bounds();
    cmd.src = {0, 0, bounds().width, bounds().height};
    cmd.opacity = opacity();
    cmd.sort_key = lx::draw_sort_key::make(1, out.size());
    out.push(cmd);
}

lx::ui::widget& lx::ui::widget_node::ui_widget() const { return *widget_; }
