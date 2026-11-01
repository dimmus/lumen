module;

// lx.shell:view_ui — what the shell's canvas views share as they move from the shm canvas
// onto lx.ui (ADR-010 Phase 5 P5-7): a control whose boxes are the shell's own literal
// colours rather than theme tokens — the switcher, the launcher and the notification stack
// keep the exact palette the canvas drew, which the smoke suites pin pixel for pixel —
// with control_node's layout, hover, press and pointer routing underneath. Also the
// 0xAARRGGBB → lx::color spelling the views' constants always used.

import lx.foundation;
import lx.ui;

export module lx.shell:view_ui;

import std;

export namespace lx::shell {

/// A control's boxes as literal colours (rest / hover / pressed) and the pointer events
/// forwarded to the view's own state machine (as panel_cell_props forwards the panel's).
/// An all-zero quad_style (the default) draws nothing in that state.
struct shell_box_props {
    lx::ui::control_props control{};
    lx::ui::quad_style rest{};
    lx::ui::quad_style hover{};
    lx::ui::quad_style active{};
    /// Hover changed on the box (`hovered`: on or off; a press counts as hover).
    void (*notify)(void* user, bool hovered) = nullptr;
    /// A pointer event on the box.
    void (*event)(void* user, const lx::ui::pointer_event& e) = nullptr;
    void* user = nullptr;
};

class shell_box_node final : public lx::ui::control_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "shell_box"; }

    bool apply(const shell_box_props& p) {
        props_ = p;
        for (unsigned s = 0; s < 3; ++s) {
            const lx::ui::quad_style& q = s == 0 ? p.rest : s == 1 ? p.hover : p.active;
            packs_[s] = lx::ui::pack_quad_style(q);
            drawn_[s] =
                q.background.a > 0.f || q.background_end || q.border_color.a > 0.f;
        }
        // The paint is ours (control_props::paint stays empty: nothing of it is drawn), so
        // a change of these quads alone must still damage — the callers re-describe with a
        // repaint of the whole view, as the panel does.
        return lx::ui::control_node::apply(p.control);
    }

    void on_interaction_changed() override {
        lx::ui::control_node::on_interaction_changed();
        if (props_.notify)
            props_.notify(props_.user, has_flag(lx::ui::node_flag::hovered) ||
                                           has_flag(lx::ui::node_flag::pressed));
    }

    bool on_pointer(const lx::ui::pointer_event& e) override {
        if (props_.event)
            props_.event(props_.user, e);
        return true;
    }

    void emit(lx::ui::draw_scope& out) const override {
        if (!visible())
            return;
        const unsigned s = paint_state();
        if (drawn_[s])
            out.quad(bounds(), packs_[s]);
        lx::ui::ui_node::emit(out);
    }

private:
    shell_box_props props_{};
    lx::ui::packed_quad_style packs_[3]{};
    bool drawn_[3]{};
};

/// A literal-colour box over `children` (see `shell_box_node`).
[[nodiscard]] lx::ui::child shell_box(const shell_box_props& props,
                                      std::span<const lx::ui::child> children,
                                      lx::ui::element_key key = 0) {
    return lx::ui::with_children(
        lx::ui::describe<shell_box_node, shell_box_props>(props, key), children);
}

/// A colour straight from an 0xAARRGGBB constant, as the canvas views spelled them.
[[nodiscard]] constexpr lx::color shell_color(std::uint32_t argb) {
    return lx::color::rgb(static_cast<float>((argb >> 16) & 0xFF) / 255.f,
                          static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                          static_cast<float>(argb & 0xFF) / 255.f,
                          static_cast<float>((argb >> 24) & 0xFF) / 255.f);
}

} // namespace lx::shell
