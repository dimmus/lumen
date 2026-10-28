// lumen-hello's window, built only from ADR-010 §5.6 components: a panel with a headline, a
// text field, a filled button that counts its clicks through a reducer (with a tooltip), a
// checkbox and a switch. Shared with tests/test_app_window.cpp, which drives this very tree
// through a compositor. Include after `import lx.ui;` and `import std;`.
#pragma once

namespace hello {

/// The form's state: one reducer holds it, so a click re-describes the form's subtree and
/// the reconcile repaints only what changed (the counter label).
struct form_state {
    int clicks = 0;
    bool checked = false;
    bool on = true;
};

enum class form_action : unsigned char { click, toggle_check, toggle_switch };

inline void reduce(form_state& s, const form_action& a) {
    switch (a) {
    case form_action::click: ++s.clicks; break;
    case form_action::toggle_check: s.checked = !s.checked; break;
    case form_action::toggle_switch: s.on = !s.on; break;
    }
}

using form_host = lx::ui::reducer_node<form_state, form_action>;

// Keys the test finds nodes by.
inline constexpr lx::ui::element_key k_field = 0x6865'0001;
inline constexpr lx::ui::element_key k_button = 0x6865'0002;
inline constexpr lx::ui::element_key k_counter = 0x6865'0003;
inline constexpr lx::ui::element_key k_checkbox = 0x6865'0004;
inline constexpr lx::ui::element_key k_switch = 0x6865'0005;
inline constexpr lx::ui::element_key k_form = 0x6865'0006;

inline lx::ui::child build_form(const form_state& s, form_host& host) {
    using namespace lx::ui;
    const child row[2] = {
        button("Increment")
            .style(button_style::filled)
            .size(button_size::large)
            .on_press(bind(host, form_action::click))
            .tooltip("Adds one to the counter")
            .key(k_button),
        label(frame_format("Clicked {} {}", s.clicks, s.clicks == 1 ? "time" : "times"))
            .color(tok::text_muted)
            .single_line()
            .key(k_counter),
    };
    const child toggles[2] = {
        checkbox(s.checked).label("Remember me").on_toggle(bind(host, form_action::toggle_check)).key(k_checkbox),
        toggle_switch(s.on).label("Notifications").on_toggle(bind(host, form_action::toggle_switch)).key(k_switch),
    };
    const child kids[3] = {
        div(style{}.flex_row().items_center().gap(sp::b12), row),
        div(style{}.flex_row().items_center().gap(sp::b16), toggles),
        label("Lumen draws this window with its own engine: one primitive stream, "
              "damage-scoped, on the CPU, GL or Vulkan.")
            .color(tok::text_muted)
            .size(label_size::small),
    };
    return div(style{}.flex_col().gap(sp::b16), kids);
}

inline lx::ui::child describe(lx::ui::build_context&) {
    using namespace lx::ui;
    const child body[3] = {
        headline("Hello Lumen").size(headline_size::large),
        text_input().placeholder("Type something").layout(style{}.w_full()).key(k_field),
        reducer<form_state, form_action>(form_state{}, &reduce, &build_form, k_form),
    };
    const child content[1] = {div(style{}.flex_col().gap(sp::b16).p(sp::b24).size_full(), body)};
    const child root[1] = {panel(content[0]).name("Hello")};
    return div(style{}.size_full().bg(tok::background), root);
}

} // namespace hello
