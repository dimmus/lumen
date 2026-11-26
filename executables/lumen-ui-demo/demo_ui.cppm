export module lumen.ui_demo;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.trace;
import lx.ui;

export namespace ui_demo {

enum class page_id : unsigned char {
    buttons,
    toggles,
    text,
    media,
    inputs,
    values,
    layouts,
    collections,
    overlays,
    coverage,
};

struct page_info {
    page_id id;
    const char* group;
    const char* title;
    const char* keywords;
    lx::ui::icon icon;
};

[[nodiscard]] std::span<const page_info> catalog();

inline constexpr int k_compact_width = 700;
inline constexpr lx::ui::element_key k_demo_root = 0x5549'0001;
inline constexpr lx::ui::element_key k_search = 0x5549'0002;
inline constexpr lx::ui::element_key k_back = 0x5549'0003;
inline constexpr lx::ui::element_key k_primary_action = 0x5549'0004;
inline constexpr lx::ui::element_key k_disabled_action = 0x5549'0005;
inline constexpr lx::ui::element_key k_open_modal = 0x5549'0006;
inline constexpr lx::ui::element_key k_modal_confirm = 0x5549'0007;
inline constexpr lx::ui::element_key k_modal_cancel = 0x5549'0008;
inline constexpr lx::ui::element_key k_undo = 0x5549'0009;
inline constexpr lx::ui::element_key k_sidebar = 0x5549'000a;
inline constexpr lx::ui::element_key k_detail = 0x5549'000b;
inline constexpr lx::ui::element_key k_modal = 0x5549'000c;
inline constexpr lx::ui::element_key k_menu_trigger = 0x5549'000d;
inline constexpr lx::ui::element_key k_collection = 0x5549'000e;
inline constexpr lx::ui::element_key k_message = 0x5549'000f;
inline constexpr lx::ui::element_key k_submit = 0x5549'0010;
inline constexpr lx::ui::element_key k_paste = 0x5549'0011;
inline constexpr lx::ui::element_key k_input_feedback = 0x5549'0012;

[[nodiscard]] constexpr lx::ui::element_key nav_key(page_id page) {
    return 0x5549'0100 + static_cast<unsigned>(page);
}

[[nodiscard]] constexpr lx::ui::element_key page_content_key(page_id page) {
    return 0x5549'0200 + static_cast<unsigned>(page);
}

[[nodiscard]] constexpr lx::ui::element_key page_specimen_key(page_id page) {
    return 0x5549'0300 + static_cast<unsigned>(page);
}

// The toolkit edits the UI thread's buffers in place. Bind each span only after the
// reducer seeds its final state object (never to a copied initial state's storage).
struct edit_buffer {
    mutable std::array<char, 512> storage{};
    mutable lx::ui::text_buffer buffer{};

    [[nodiscard]] lx::ui::text_buffer& get(std::string_view initial = {}) const {
        if (buffer.storage.empty()) {
            buffer.storage = storage;
            if (!initial.empty())
                (void)buffer.assign(initial);
        }
        return buffer;
    }
};

enum class popup : unsigned char { none, select, dropdown, menu, popover, modal };
enum class notice : unsigned char { none, shown, archived };
enum class input_status : unsigned char { none, submitted, copied, pasted, empty, too_long };

struct demo_state {
    page_id page = page_id::buttons;
    bool compact = false;
    bool show_detail = true;
    int clicks = 0;
    bool pinned = false;
    bool saved = false;
    bool reminders = true;
    lx::ui::toggle_state check = lx::ui::toggle_state::indeterminate;
    int tab = 0;
    int volume = 6;
    int segment = 1;
    int selection = 0;
    int dropdown = 1;
    int copies = 2;
    int columns = 3;
    int swatch = 0;
    int row = -1;
    int activated_row = -1;
    bool dimmed = true;
    bool starred = false;
    bool archived = false;
    int modal_confirmations = 0;
    popup open = popup::none;
    notice toast = notice::none;
    int toast_generation = 0;
    input_status input_feedback = input_status::none;
    edit_buffer search{};
    edit_buffer entry{};
    edit_buffer step{};
    edit_buffer spin{};
    mutable lx::gfx::atlas_tile_id picture{};
};

enum class action_id : unsigned char {
    resize,
    navigate,
    back,
    search,
    click,
    reset,
    pin,
    save,
    check,
    reminders,
    tab,
    volume,
    segment,
    selection,
    dropdown,
    step,
    spin,
    swatch,
    row,
    activate_row,
    dim,
    edit_message,
    submit,
    copy_sample,
    paste,
    popup_toggle,
    popup_close,
    star,
    archive,
    undo,
    modal_confirm,
    notify,
    dismiss_notice,
};

struct demo_action {
    action_id id;
    int value = 0;
};

using demo_host = lx::ui::reducer_node<demo_state, demo_action>;

// lx.app provides a function-pointer describer. Bind this single demo window's root on
// its UI thread before the first tick, clearing the binding before destroying the root.
void bind_root(lx::ui::ui_root* root);
[[nodiscard]] lx::ui::child describe(lx::ui::build_context& context);

} // namespace ui_demo

namespace {

using namespace lx::ui;
using namespace ui_demo;
using lx::layout::px;
constexpr lx::trace::category log_demo{"ui_demo"};
constexpr const char* sample_text = "Made with Lumen";
constexpr const char* modes[] = {"List", "Grid", "Tiles"};
constexpr const char* sizes[] = {"Small", "Medium", "Large"};
constexpr const char* sorts[] = {"Name", "Date modified", "Size"};
constexpr const char* copy_keys[] = {"Ctrl-C"};
constexpr const char* paste_keys[] = {"Ctrl-V"};
constexpr const char* submit_keys[] = {"Enter"};
constexpr unsigned row_count = 10'000;

struct focus_request {
    element_key target = 0;
    element_key fallback = 0;
};

struct view_binding {
    ui_root* root = nullptr;
    std::uint64_t theme = 0;
    float scale = 0.f;
    focus_request next_focus{};
};

thread_local view_binding view{};

void focus_after_layout(element_key target, element_key fallback = 0) {
    if (view.root)
        view.next_focus = {target, fallback};
}

void scroll_into_view(ui_node& node) {
    // Map the target from the innermost viewport outward. Use the new offset at each
    // level; the outer viewport must reveal where the inner one now shows the node.
    auto rect = node.bounds();
    constexpr unsigned max_ancestors = 32;
    unsigned depth = 0;
    for (auto* current = &node;
         current && !current->has_flag(node_flag::layer) && depth < max_ancestors;
         current = current->parent(), ++depth) {
        auto* parent = current->parent();
        lx::point2i offset{};
        lx::rect2i clip_rect{};
        if (!parent || !parent->child_viewport(*current, offset, clip_rect))
            continue;
        if (auto* scroll = parent->as_scroll_target()) {
            const int start[] = {rect.x + offset.x, rect.y + offset.y};
            const int extent[] = {rect.width, rect.height};
            const int edge[] = {clip_rect.x, clip_rect.y};
            const int available[] = {clip_rect.width, clip_rect.height};
            for (unsigned axis = 0; axis < 2; ++axis) {
                if (available[axis] <= 0 || extent[axis] <= 0)
                    continue;
                const auto metric = scroll->scroll_metrics_along(axis);
                if (metric.max_offset() <= 0.f)
                    continue;
                // An oversized target can show only its leading edge.
                const int delta =
                    start[axis] < edge[axis] || extent[axis] > available[axis]
                        ? start[axis] - edge[axis]
                        : std::max(0, start[axis] + extent[axis] - edge[axis] - available[axis]);
                if (delta != 0)
                    scroll->scroll_to(axis, metric.offset + static_cast<float>(delta));
            }
            (void)parent->child_viewport(*current, offset, clip_rect);
        }
        rect.x += offset.x;
        rect.y += offset.y;
        // What moves with the next viewport is the part visible in this one.
        const int x = std::max(rect.x, clip_rect.x);
        const int y = std::max(rect.y, clip_rect.y);
        rect = {x, y, std::max(0, std::min(rect.x + rect.width, clip_rect.x + clip_rect.width) - x),
                std::max(0, std::min(rect.y + rect.height, clip_rect.y + clip_rect.height) - y)};
        if (rect.width == 0 || rect.height == 0)
            return;
    }
}

void reveal_keyboard_focus(void* context, lx::rect2i) {
    // The focus manager emits this notification even for a target currently outside
    // every viewport. Its native traversal and modality remain in charge.
    auto& focus = static_cast<ui_root*>(context)->focus();
    if (focus.focus_visible())
        scroll_into_view(*focus.focused());
}

[[nodiscard]] ui_node* find_key(ui_node& node, element_key key) {
    if (node.key() == key)
        return &node;
    if (!node.visible())
        return nullptr;
    for (unsigned i = 0; i < node.child_count(); ++i)
        if (auto* c = node.child_at(i))
            if (auto* found = find_key(*c, key))
                return found;
    return nullptr;
}

[[nodiscard]] bool within_key(const ui_node* node, element_key key) {
    for (auto* n = node; n; n = n->parent())
        if (n->key() == key)
            return true;
    return false;
}

// Locates destinations only after the reducer has reconciled the new pane and layout
// has placed it. No task, timer, retained node pointer or extra frame is needed.
class focus_layout final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const auto id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "demo_focus_layout"; }
    bool apply(bool pending) { return pending; }

    void arrange(lx::rect2i bounds) override {
        ui_node::arrange(bounds);
        if (!view.root || view.next_focus.target == 0)
            return;
        const auto request = std::exchange(view.next_focus, {});
        auto& focus = view.root->focus();
        for (const auto key : {request.target, request.fallback}) {
            if (key == 0)
                continue;
            if (auto* destination = find_key(*this, key);
                destination && focus.focus(*destination)) {
                if (!focus.focus_visible())
                    scroll_into_view(*destination);
                break;
            }
        }
    }
};

using page_fn = child (*)(const demo_state&, demo_host&);
struct registration {
    page_info info;
    const char* description;
    page_fn build;
};

child buttons_page(const demo_state&, demo_host&);
child toggles_page(const demo_state&, demo_host&);
child text_page(const demo_state&, demo_host&);
child media_page(const demo_state&, demo_host&);
child inputs_page(const demo_state&, demo_host&);
child values_page(const demo_state&, demo_host&);
child layouts_page(const demo_state&, demo_host&);
child collections_page(const demo_state&, demo_host&);
child overlays_page(const demo_state&, demo_host&);
child coverage_page(const demo_state&, demo_host&);

constexpr registration pages[] = {
    {{page_id::buttons, "Controls", "Buttons", "button action icon toggle filled outlined",
      icon::circle_dot},
     "Momentary actions, icon buttons and a persistent toggle.",
     &buttons_page},
    {{page_id::toggles, "Controls", "Toggles", "checkbox tri state indeterminate switch",
      icon::check},
     "Binary preferences and a checkbox with a mixed state.",
     &toggles_page},
    {{page_id::inputs, "Controls", "Text & Clipboard",
      "input entry selection copy paste ime drag drop", icon::pencil},
     "Editable text, selection and the system clipboard.",
     &inputs_page},
    {{page_id::values, "Controls", "Value Inputs",
      "slider segmented select drop down stepper spinner swatch color", icon::settings},
     "Ranges, choices, numeric fields and color swatches.",
     &values_page},
    {{page_id::text, "Presentation", "Typography", "label headline text block icon", icon::file},
     "Text hierarchy, wrapping and symbolic icons.",
     &text_page},
    {{page_id::media, "Presentation", "Images & Keycaps",
      "image avatar media photo keyboard shortcut key binding", icon::keyboard},
     "Atlas images, avatars and shortcut labels.",
     &media_page},
    {{page_id::layouts, "Presentation", "Layout & Chrome",
      "flex grid decorators clipping opacity tabs divider progress panel status toolbar",
      icon::monitor},
     "Compose surfaces, arrange elements and constrain their paint.",
     &layouts_page},
    {{page_id::collections, "Patterns", "Lists & Scrolling",
      "virtual list list_item scroll collection rows", icon::menu},
     "Fixed-height virtual rows inside an independently scrolling page.",
     &collections_page},
    {{page_id::overlays, "Patterns", "Overlays",
      "menu popup popover context modal dialog toast tooltip undo", icon::ellipsis},
     "Anchored surfaces, confirmation and reversible feedback.",
     &overlays_page},
    {{page_id::coverage, "Reference", "Toolkit Coverage",
      "native composed missing gap capabilities", icon::info},
     "What lx.ui provides, what apps compose and what is not a widget yet.",
     &coverage_page},
};

constexpr auto page_metadata = [] {
    std::array<page_info, std::size(pages)> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = pages[i].info;
    return result;
}();

[[nodiscard]] const registration& lookup(page_id id) {
    for (const auto& page : pages)
        if (page.info.id == id)
            return page;
    return pages[0];
}

[[nodiscard]] constexpr char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool contains_folded(std::string_view text, std::string_view needle) {
    return std::ranges::search(text, needle, {}, ascii_lower, ascii_lower).begin() != text.end();
}

[[nodiscard]] bool matches(const page_info& page, std::string_view query) {
    // Catalog labels are English. Every whitespace-delimited term must match the title,
    // section or component aliases; punctuation within a term is kept as typed.
    while (true) {
        const auto first = query.find_first_not_of(" \t\n\r");
        if (first == query.npos)
            return true;
        query.remove_prefix(first);
        const auto n = query.find_first_of(" \t\n\r");
        const auto term = query.substr(0, n);
        if (!contains_folded(page.title, term) && !contains_folded(page.group, term) &&
            !contains_folded(page.keywords, term))
            return false;
        if (n == query.npos)
            return true;
        query.remove_prefix(n);
    }
}

void change_number(int& value, text_buffer& buffer, int delta, int max) {
    const auto text = buffer.view();
    int parsed = 0;
    if (!text.empty()) {
        const auto end = text.data() + text.size();
        if (const auto result = std::from_chars(text.data(), end, parsed);
            result.ec == std::errc{} && result.ptr == end)
            value = std::clamp(parsed, 1, max);
    }
    value = std::clamp(value + delta, 1, max);
    char digits[12];
    const auto result = std::to_chars(digits, digits + sizeof digits, value);
    (void)buffer.assign({digits, result.ptr});
}

void show_toast(demo_state& s, notice kind) {
    s.toast = kind;
    s.toast_generation =
        s.toast_generation == std::numeric_limits<int>::max() ? 1 : s.toast_generation + 1;
}

void set_popup(demo_state& s, popup which) {
    if ((s.open == popup::modal) != (which == popup::modal))
        log_demo.debug("modal {}", which == popup::modal ? "opened" : "closed");
    s.open = which;
}

void reduce(demo_state& s, const demo_action& a) {
    switch (a.id) {
    case action_id::resize: {
        s.compact = a.value != 0;
        if (s.compact && s.open == popup::modal)
            s.show_detail = true;
        if (view.root) {
            const auto* focused = view.root->focus().focused();
            if (focused && s.compact && s.show_detail && within_key(focused, k_sidebar))
                focus_after_layout(k_back);
            else if (focused && s.compact && !s.show_detail && within_key(focused, k_detail))
                focus_after_layout(nav_key(s.page), k_search);
            else if (focused && !s.compact && focused->key() == k_back)
                focus_after_layout(nav_key(s.page), k_search);
        }
        log_demo.debug("layout mode={}", s.compact ? "compact" : "wide");
        break;
    }
    case action_id::navigate:
        s.page = static_cast<page_id>(a.value);
        s.show_detail = true;
        set_popup(s, popup::none);
        if (s.compact)
            focus_after_layout(k_back);
        log_demo.debug("page selected: {}", lookup(s.page).info.title);
        break;
    case action_id::back:
        s.show_detail = false;
        set_popup(s, popup::none);
        focus_after_layout(nav_key(s.page), k_search);
        break;
    case action_id::search:
        break; // native input already updated the buffer
    case action_id::click:
        if (s.clicks < std::numeric_limits<int>::max())
            ++s.clicks;
        break;
    case action_id::reset:
        s.clicks = 0;
        break;
    case action_id::pin:
        s.pinned = !s.pinned;
        break;
    case action_id::save:
        s.saved = !s.saved;
        break;
    case action_id::check:
        switch (s.check) {
        case toggle_state::unselected:
            s.check = toggle_state::indeterminate;
            break;
        case toggle_state::indeterminate:
            s.check = toggle_state::selected;
            break;
        case toggle_state::selected:
            s.check = toggle_state::unselected;
            break;
        }
        break;
    case action_id::reminders:
        s.reminders = !s.reminders;
        break;
    case action_id::tab:
        s.tab = a.value;
        break;
    case action_id::volume:
        s.volume = a.value;
        break;
    case action_id::segment:
        s.segment = a.value;
        break;
    case action_id::selection:
        s.selection = a.value;
        set_popup(s, popup::none);
        break;
    case action_id::dropdown:
        s.dropdown = a.value;
        set_popup(s, popup::none);
        break;
    case action_id::step:
        change_number(s.copies, s.step.get(), a.value, 9);
        break;
    case action_id::spin:
        change_number(s.columns, s.spin.get(), a.value, 12);
        break;
    case action_id::swatch:
        s.swatch = a.value;
        break;
    case action_id::row:
        s.row = a.value;
        break;
    case action_id::activate_row:
        s.activated_row = a.value;
        break;
    case action_id::dim:
        s.dimmed = !s.dimmed;
        break;
    case action_id::edit_message:
        s.input_feedback = input_status::none;
        break;
    case action_id::submit:
        s.input_feedback =
            s.entry.get().view().empty() ? input_status::empty : input_status::submitted;
        break;
    case action_id::copy_sample:
        clipboard::global().set_text(sample_text);
        s.input_feedback = input_status::copied;
        break;
    case action_id::paste: {
        const std::string_view clipboard_text = clipboard::global().text();
        // Like native single-line paste, stop at the first CR or LF.
        const auto text = clipboard_text.substr(0, clipboard_text.find_first_of("\r\n"));
        if (text.empty())
            s.input_feedback = input_status::empty;
        else
            s.input_feedback =
                s.entry.get().assign(text) ? input_status::pasted : input_status::too_long;
        break;
    }
    case action_id::popup_toggle: {
        const auto which = static_cast<popup>(a.value);
        set_popup(s, s.open == which ? popup::none : which);
        break;
    }
    case action_id::popup_close:
        if (s.open == static_cast<popup>(a.value))
            set_popup(s, popup::none);
        break;
    case action_id::star:
        s.starred = !s.starred;
        break;
    case action_id::archive:
        if (!s.archived) {
            s.archived = true;
            show_toast(s, notice::archived);
        }
        break;
    case action_id::undo:
        s.archived = false;
        s.toast = notice::none;
        focus_after_layout(k_open_modal);
        break;
    case action_id::modal_confirm:
        if (s.open == popup::modal) {
            if (s.modal_confirmations < std::numeric_limits<int>::max())
                ++s.modal_confirmations;
            reduce(s, {action_id::archive});
            set_popup(s, popup::none);
            focus_after_layout(k_undo);
        }
        break;
    case action_id::notify:
        show_toast(s, notice::shown);
        break;
    case action_id::dismiss_notice:
        if (s.toast_generation == a.value)
            s.toast = notice::none;
        break;
    }
}

[[nodiscard]] handler act(demo_host& host, action_id id, int value = 0) {
    return lx::ui::bind(host, demo_action{id, value});
}

[[nodiscard]] handler value_action(demo_host& host, action_id id) {
    return [&host, id](int v) { host.dispatch({id, v}); };
}

[[nodiscard]] handler toggle_popup(demo_host& host, popup which) {
    return act(host, action_id::popup_toggle, static_cast<int>(which));
}

[[nodiscard]] handler close_popup(demo_host& host, popup which) {
    return act(host, action_id::popup_close, static_cast<int>(which));
}

[[nodiscard]] child keyed(child content, element_key key) {
    if (content) {
        if (auto* el = build_context::current()->alloc<element>()) {
            *el = *content;
            el->key = key;
            return el;
        }
    }
    return nullptr;
}

[[nodiscard]] child caption(const char* text) {
    return label(text).size(label_size::small).color(tok::text_muted);
}

[[nodiscard]] child section(const char* title, std::initializer_list<child> items) {
    const child content[] = {
        label(title).bold().single_line(),
        div(style{}.flex_col().gap(sp::b12).py(sp::b12).border_t_1(tok::border_variant), items),
    };
    return div(style{}.flex_col().gap(sp::b08), content);
}

[[nodiscard]] child stack(std::initializer_list<child> items) {
    return div(style{}.flex_col().gap(sp::b20), items);
}

[[nodiscard]] child flow(std::initializer_list<child> items) {
    return div(style{}.flex_row().flex_wrap().items_center().gap(sp::b08), items);
}

[[nodiscard]] child buttons_page(const demo_state& s, demo_host& host) {
    const auto increment = act(host, action_id::click);
    return stack({
        section(
            "Action styles",
            {
                flow({button("Increment")
                          .style(button_style::filled)
                          .size(button_size::large)
                          .icon(icon::plus)
                          .on_press(increment)
                          .key(k_primary_action),
                      button("Reset")
                          .style(button_style::outlined)
                          .size(button_size::large)
                          .on_press(act(host, action_id::reset)),
                      label(frame_format("{} {}", s.clicks, s.clicks == 1 ? "press" : "presses"))
                          .single_line()
                          .color(tok::text_accent)}),
                divider(),
                flow({button("Tinted").tint(tint_color::success).on_press(increment),
                      button("Ghost").style(button_style::ghost).on_press(increment),
                      button("Disabled")
                          .style(button_style::outlined)
                          .disabled()
                          .on_press(increment)
                          .key(k_disabled_action)}),
            }),
        section("Icon & toggle buttons",
                {
                    flow({icon_button(icon::plus)
                              .style(button_style::outlined)
                              .name("Increment")
                              .tooltip("Increment the counter")
                              .on_press(increment),
                          icon_button(icon::refresh_cw)
                              .style(button_style::outlined)
                              .name("Reset counter")
                              .tooltip("Reset the counter")
                              .on_press(act(host, action_id::reset)),
                          toggle_button("Pin")
                              .icon(icon::check)
                              .selected(s.pinned)
                              .on_toggle(act(host, action_id::pin)),
                          caption(s.pinned ? "Pinned" : "Not pinned")}),
                }),
    });
}

[[nodiscard]] child toggles_page(const demo_state& s, demo_host& host) {
    const char* state = s.check == toggle_state::selected        ? "Checked"
                        : s.check == toggle_state::indeterminate ? "Mixed"
                                                                 : "Unchecked";
    return stack({
        section("Checkboxes",
                {
                    checkbox(s.saved).label("Save a copy").on_toggle(act(host, action_id::save)),
                    flow({checkbox(s.check)
                              .label("Select all")
                              .fill()
                              .on_toggle(act(host, action_id::check)),
                          caption(state)}),
                    checkbox(true).label("Always included").disabled(),
                }),
        section("Switches",
                {
                    flow({toggle_switch(s.reminders)
                              .label("Reminders")
                              .on_toggle(act(host, action_id::reminders)),
                          caption(s.reminders ? "On" : "Off")}),
                    toggle_switch(false).label("Background sync").disabled(),
                }),
    });
}

[[nodiscard]] child text_page(const demo_state&, demo_host&) {
    text_block_props paragraph{};
    paragraph.text = "A text block wraps to the measure of its container. "
                     "Resize the window to see this paragraph reflow.";
    paragraph.color = active_theme().color(tok::text_muted);
    paragraph.context = active_ui_resources().text;
    paragraph.line_height = 1.5f;
    return stack({
        section("Type scale",
                {
                    headline("A headline").size(headline_size::large),
                    label("A strong label").bold(),
                    label("Small secondary text").size(label_size::small).color(tok::text_muted),
                    text_block(paragraph),
                }),
        section("Symbolic icons",
                {
                    flow({icon_view(icon::info, 18.f, tok::icon_accent), label("Information"),
                          icon_view(icon::folder, 20.f, tok::icon_muted), label("Folder"),
                          icon_view(icon::circle_check, 20.f, tok::success), label("Success")}),
                }),
    });
}

// The atlas specimen is a small, opaque pixel-art landscape. Its palette is image content,
// not UI chrome; it does not change with the app theme.
constexpr unsigned image_w = 128, image_h = 80;
constexpr auto landscape = [] {
    std::array<std::uint8_t, image_w * image_h * 4> pixels{};
    constexpr std::array<std::uint8_t, 3> sky{41, 87, 129}, sun{252, 207, 115},
        far_hill{96, 140, 156}, near_hill{43, 101, 93}, ground{25, 68, 67};
    for (unsigned y = 0; y < image_h; ++y) {
        for (unsigned x = 0; x < image_w; ++x) {
            const int u = static_cast<int>(x / 2), v = static_cast<int>(y / 2);
            const int dx = u - 46, dy = v - 12;
            const int ridge = u < 21 ? 30 - u : 9 + (u - 21) / 2;
            const int front = u < 43 ? 34 - u / 5 : 26 + (u - 43) / 2;
            const auto& rgb = v > 36                   ? ground
                              : v >= front             ? near_hill
                              : v >= ridge             ? far_hill
                              : dx * dx + dy * dy < 35 ? sun
                                                       : sky;
            const auto i = (y * image_w + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                pixels[i + c] = rgb[c];
            pixels[i + 3] = 255;
        }
    }
    return pixels;
}();

[[nodiscard]] child media_page(const demo_state& s, demo_host&) {
    child picture = caption("Image atlas unavailable");
    child portrait = avatar("LM").size(40).border(tok::border).name("Lumen avatar");
    if (auto* cache = active_ui_resources().icons) {
        auto& atlas = cache->atlas();
        lx::gfx::atlas_tile pos{};
        if (!atlas.lookup(s.picture, pos)) {
            const auto tile = atlas.insert(lx::gfx::atlas_format::rgba8, image_w, image_h,
                                           landscape, image_w * 4);
            s.picture = tile ? *tile : lx::gfx::atlas_tile_id{};
        }
        if (s.picture.valid()) {
            picture = image(atlas, s.picture, {image_w, image_h});
            portrait =
                avatar(atlas, s.picture).size(40).border(tok::border).name("Landscape avatar");
        }
    }
    return stack({
        section("Atlas image", {flow({picture})}),
        section("Avatars", {flow({avatar("AL").size(40).name("Ada Lane"), portrait})}),
        section("Keycaps",
                {
                    flow({key_binding(copy_keys), label("Copy"), key_binding(paste_keys),
                          label("Paste")}),
                    key_binding_hint(submit_keys, tok::surface_background).prefix("Submit with"),
                }),
    });
}

[[nodiscard]] child inputs_page(const demo_state& s, demo_host& host) {
    const char* feedback = "";
    switch (s.input_feedback) {
    case input_status::none:
        feedback = "Ready";
        break;
    case input_status::submitted:
        feedback = frame_format("Submitted: {}", s.entry.get().view());
        break;
    case input_status::copied:
        feedback = "Sample copied";
        break;
    case input_status::pasted:
        feedback = "Clipboard pasted";
        break;
    case input_status::empty:
        feedback = "No text to use";
        break;
    case input_status::too_long:
        feedback = "Clipboard text exceeds this field’s capacity";
        break;
    }
    return stack({
        section("Editable field",
                {
                    text_input()
                        .buffer(s.entry.get("Hello, Lumen!"))
                        .placeholder("Enter a message")
                        .name("Message")
                        .layout(style{}.w_full())
                        .on_change([&host] {
                            if (host.state().input_feedback != input_status::none)
                                host.dispatch({action_id::edit_message});
                        })
                        .on_submit(act(host, action_id::submit))
                        .key(k_message),
                    flow({button("Submit")
                              .style(button_style::filled)
                              .on_press(act(host, action_id::submit))
                              .key(k_submit),
                          button("Copy sample")
                              .icon(icon::copy)
                              .style(button_style::outlined)
                              .on_press(act(host, action_id::copy_sample)),
                          button("Paste")
                              .style(button_style::outlined)
                              .on_press(act(host, action_id::paste))
                              .key(k_paste)}),
                    keyed(caption(feedback), k_input_feedback),
                }),
        section("Read-only example",
                {
                    text_input().initial("Editing disabled").disabled().name("Disabled text input"),
                }),
    });
}

[[nodiscard]] child values_page(const demo_state& s, demo_host& host) {
    const swatch palette[] = {{"Blue", active_theme().color(tok::terminal_ansi_blue)},
                              {"Rose", active_theme().color(tok::terminal_ansi_red)},
                              {"Green", active_theme().color(tok::terminal_ansi_green)},
                              {"Gold", active_theme().color(tok::terminal_ansi_yellow)}};
    return stack({
        section("Volume",
                {
                    slider(static_cast<float>(s.volume), 0.f, 10.f)
                        .steps(10)
                        .name("Volume")
                        .on_change(value_action(host, action_id::volume))
                        .layout(style{}.w_full()),
                    caption(frame_format("{}%", s.volume * 10)),
                }),
        section("Choices",
                {
                    flow({segmented(modes, s.segment)
                              .name("View mode")
                              .on_select(value_action(host, action_id::segment)),
                          caption(modes[s.segment])}),
                    flow({select(sizes, s.selection)
                              .name("Item size")
                              .open(s.open == popup::select)
                              .on_open(toggle_popup(host, popup::select))
                              .on_dismiss(close_popup(host, popup::select))
                              .on_select(value_action(host, action_id::selection)),
                          drop_down(sorts, s.dropdown)
                              .name("Sort order")
                              .open(s.open == popup::dropdown)
                              .on_open(toggle_popup(host, popup::dropdown))
                              .on_dismiss(close_popup(host, popup::dropdown))
                              .on_select(value_action(host, action_id::dropdown))}),
                }),
        section("Numbers",
                {
                    flow({label("Copies"), stepper(s.step.get("2"), s.copies)
                                               .range(1, 9)
                                               .name("Copies")
                                               .on_change(value_action(host, action_id::step))
                                               .on_submit(act(host, action_id::step))}),
                    flow({label("Columns"), spinner(s.spin.get("3"), s.columns)
                                                .range(1, 12)
                                                .name("Columns")
                                                .on_change(value_action(host, action_id::spin))
                                                .on_submit(act(host, action_id::spin))}),
                }),
        section("Accent choices",
                {
                    flow({swatch_row(palette, s.swatch)
                              .name("Accent color")
                              .on_select(value_action(host, action_id::swatch)),
                          caption(palette[s.swatch].label)}),
                }),
    });
}

[[nodiscard]] child tile(const char* text, tok bg) {
    const child content[] = {label(text).bold().size(label_size::small)};
    return div(
        style{}.flex_row().justify_center().items_center().min_h(px(40)).p(sp::b08).bg(bg).rounded(
            radius::sm),
        content);
}

[[nodiscard]] child layouts_page(const demo_state& s, demo_host& host) {
    const child tabs[] = {
        tab("Overview")
            .selected(s.tab == 0)
            .position(0, 2, static_cast<unsigned>(s.tab))
            .on_select(act(host, action_id::tab, 0)),
        tab("Activity")
            .selected(s.tab == 1)
            .position(1, 2, static_cast<unsigned>(s.tab))
            .on_select(act(host, action_id::tab, 1)),
    };
    const child toolbar[] = {icon_view(icon::monitor), label("Preview").bold()};
    const child left[] = {label(s.tab == 0 ? "Overview" : "Activity").size(label_size::small)};
    const child right[] = {label(s.tab == 0 ? "Ready" : "3 events").size(label_size::small)};
    const child progress[] = {
        caption(s.tab == 0 ? "2 of 5 tasks complete" : "3 of 5 tasks complete"),
        progress_bar(s.tab == 0 ? 2.f : 3.f, 5.f).name("Task progress"),
    };
    const child panel_body[] = {
        tab_bar(tabs),
        div(style{}.flex_col().gap(sp::b08).p(sp::b12), progress),
        status_bar(left, right),
    };
    const child chrome_row[] = {
        div(style{}.flex_row().gap(sp::b08).items_center().px(sp::b08), toolbar)};
    lx::layout::grid_style tracks{};
    tracks.columns = 2;
    tracks.column[0].fr = 1.f;
    tracks.column[1].fr = 1.f;
    tracks.column_gap = tracks.row_gap = px(8);
    const child cells[] = {
        placed({.column = 1, .row = 1, .column_span = 2}, tile("Span two", tok::element_selected)),
        tile("Column 1", tok::element_background),
        tile("Column 2", tok::element_background),
    };
    const child sample =
        tile("Overflow past the edge", tok::element_selected) | sizing({210, 40}, {210, 40});
    const child clipped =
        clip(sample | sizing({125, 40}, {125, 40})) | opacity(s.dimmed ? 0.45f : 1.f);
    return stack({
        section("Tabs, panel & status",
                {
                    chrome::row(32.f, false, chrome_row),
                    panel(div(style{}.flex_col(), panel_body)).name("Preview panel"),
                }),
        section("Flex & grid",
                {
                    flow({tile("Content width", tok::element_background),
                          tile("Wraps to next line", tok::element_background),
                          tile("Flex item", tok::element_background)}),
                    divider(),
                    grid(tracks, cells),
                }),
        section("Decorators",
                {
                    flow({clipped, toggle_switch(s.dimmed)
                                       .label("Dim preview")
                                       .on_toggle(act(host, action_id::dim))}),
                    caption("A fixed-width clip constrains the painted tile."),
                }),
    });
}

[[nodiscard]] child collection_row(void*, const list_row& row) {
    return list_item(frame_format("Sample {:05}", row.index + 1))
        .start_slot(icon_view(icon::file, 14.f, tok::icon_muted))
        .row(row)
        .inset();
}

[[nodiscard]] child collections_page(const demo_state& s, demo_host& host) {
    return stack({
        section("Virtualized collection",
                {
                    caption("10,000 rows · only the viewport and nearby rows are described"),
                    list(row_count, &collection_row)
                        .name("Sample collection")
                        .selected(s.row)
                        .layout(style{}.h(px(220)).w_full())
                        .row_height(32)
                        .on_select(value_action(host, action_id::row))
                        .on_activate(value_action(host, action_id::activate_row))
                        .key(k_collection),
                    caption(s.row < 0 ? "No row selected"
                                      : frame_format("Selected: Sample {:05}", s.row + 1)),
                    caption(s.activated_row < 0
                                ? "No row activated"
                                : frame_format("Opened: Sample {:05}", s.activated_row + 1)),
                }),
        section("Independent scroll area",
                {
                    scroll_area(style{}.h(px(100)).w_full(),
                                std::array<child, 6>{
                                    caption("01   North terrace"),
                                    caption("02   Courtyard"),
                                    caption("03   Workshop"),
                                    caption("04   Reading room"),
                                    caption("05   East gallery"),
                                    caption("06   Roof garden"),
                                },
                                scroll_axes::vertical, style{}.flex_col().p(sp::b08).gap(sp::b08)),
                }),
    });
}

[[nodiscard]] child overlays_page(const demo_state& s, demo_host& host) {
    const menu_entry actions[] = {
        menu_header("Sample item"),
        menu_item("Star", act(host, action_id::star)).toggle(s.starred),
        menu_separator(),
        menu_item("Archive", act(host, action_id::archive)).icon(icon::trash).disabled(s.archived),
    };
    const child pop_body[] = {
        label("Item options").bold(),
        toggle_switch(s.starred).label("Starred").on_toggle(act(host, action_id::star)),
    };
    const child target = button(s.archived ? "Archived item" : "Sample item")
                             .style(button_style::outlined)
                             .icon(icon::file)
                             .on_press(act(host, action_id::star));
    return stack({
        section("Anchored overlays",
                {
                    flow({icon_button(icon::info)
                              .name("Sample tooltip")
                              .style(button_style::outlined)
                              .tooltip("A native hover tooltip"),
                          popover_menu(button("Actions")
                                           .icon(icon::chevron_down, icon_position::end)
                                           .style(button_style::outlined)
                                           .on_press(toggle_popup(host, popup::menu))
                                           .key(k_menu_trigger),
                                       actions)
                              .open(s.open == popup::menu)
                              .on_dismiss(close_popup(host, popup::menu)),
                          popover(button("Item options")
                                      .style(button_style::outlined)
                                      .on_press(toggle_popup(host, popup::popover)),
                                  div(style{}
                                          .flex_col()
                                          .gap(sp::b08)
                                          .p(sp::b12)
                                          .bg(tok::elevated_surface_background)
                                          .rounded(radius::md)
                                          .border_1(tok::border_variant),
                                      pop_body))
                              .open(s.open == popup::popover)
                              .on_dismiss(close_popup(host, popup::popover))}),
                    flow({context_menu(target, actions), caption("Context menu"),
                          caption(s.starred ? "Starred" : "Not starred")}),
                }),
        section(
            "Confirmation & feedback",
            {
                flow({button("Archive item…")
                          .style(button_style::filled)
                          .icon(icon::trash)
                          .on_press(toggle_popup(host, popup::modal))
                          .disabled(s.archived)
                          .key(k_open_modal),
                      button("Show toast")
                          .style(button_style::outlined)
                          .on_press(act(host, action_id::notify))}),
                flow({caption(s.archived ? "Sample item archived" : "Sample item in collection"),
                      s.archived ? static_cast<child>(button("Undo")
                                                          .icon(icon::refresh_cw)
                                                          .on_press(act(host, action_id::undo))
                                                          .key(k_undo))
                                 : nullptr}),
            }),
    });
}

[[nodiscard]] child coverage_page(const demo_state&, demo_host&) {
    return stack({
        section(
            "Native",
            {
                caption("Text, icons, images, keycaps, action and value controls, scroll "
                        "areas, virtual lists and overlays."),
                caption(
                    "Clipboard and primary selection. lx.app also supports text and file URI "
                    "drags, drag icons and ask actions; these are not separate specimens here."),
            }),
        section("Composed",
                {
                    caption("Searchable sidebar and responsive split view: text input, selectable "
                            "rows, flex and scroll areas."),
                    caption("Undo: a toast reports the change, a button restores the demo item. "
                            "Form rows and page sections use flex and grid."),
                }),
        section("Missing",
                {
                    caption("No public multiline editor, tree view, date picker, bottom sheet, "
                            "indeterminate activity indicator or file chooser in lx.ui."),
                    caption("Image displays an atlas tile; decoding image files is not part of the "
                            "widget."),
                    caption("Desktop file dialogs and the shell calendar are separate platform "
                            "features, not widgets shown here."),
                }),
    });
}

[[nodiscard]] child sidebar(const demo_state& s, demo_host& host) {
    const child heading[] = {
        icon_view(icon::monitor, 20.f, tok::icon_accent),
        label("Lumen UI Demo").bold().single_line(),
    };
    const child search_box[] = {
        icon_view(icon::search, 16.f, tok::icon_muted),
        text_input()
            .buffer(s.search.get())
            .placeholder("Search examples")
            .name("Search examples")
            .layout(style{}.flex_grow().min_w(px(0)))
            .framed(false)
            .max_bytes(96)
            .on_change(act(host, action_id::search))
            .on_submit([&host] {
                for (const auto& page : page_metadata) {
                    if (matches(page, host.state().search.get().view())) {
                        host.dispatch({action_id::navigate, static_cast<int>(page.id)});
                        return;
                    }
                }
            })
            .key(k_search),
    };
    std::array<child, page_metadata.size() * 2> rows{};
    unsigned count = 0;
    std::string_view previous_group;
    for (const auto& page : page_metadata) {
        if (!matches(page, s.search.get().view()))
            continue;
        if (previous_group != page.group) {
            const child group[] = {
                label(page.group).bold().size(label_size::xsmall).color(tok::text_muted)};
            rows[count++] = div(style{}.flex_row().px(sp::b08).pt(sp::b08).pb(sp::b02), group);
            previous_group = page.group;
        }
        rows[count++] =
            list_item(page.title)
                .name(page.title)
                .focusable()
                .inset()
                .start_slot(icon_view(page.icon, 16.f,
                                      s.page == page.id ? tok::icon_accent : tok::icon_muted))
                .selected(s.page == page.id)
                .on_click(act(host, action_id::navigate, static_cast<int>(page.id)))
                .key(nav_key(page.id));
    }
    if (!count)
        rows[count++] = caption("No matching examples");
    const child contents[] = {
        div(style{}.flex_row().items_center().gap(sp::b08), heading),
        div(style{}
                .flex_row()
                .items_center()
                .gap(sp::b04)
                .pl(sp::b08)
                .pr(sp::b04)
                .rounded(radius::md)
                .bg(tok::background)
                .border_1(tok::border_variant),
            search_box),
        scroll_area(style{}.flex_grow().min_h(px(0)).w_full(), {rows.data(), count},
                    scroll_axes::vertical, style{}.flex_col().pr(sp::b04).gap(sp::b02)),
    };
    auto frame =
        style{}.flex_col().gap(sp::b12).p(sp::b12).bg(tok::panel_background).flex_shrink_0();
    if (s.compact)
        frame.w_full();
    else
        frame.w(px(224)).border_r_1(tok::border_variant);
    return keyed(div(frame, contents), k_sidebar);
}

[[nodiscard]] child detail(const demo_state& s, demo_host& host) {
    const auto& page = lookup(s.page);
    const child bar_items[] = {
        s.compact ? static_cast<child>(button("Back")
                                           .icon(icon::arrow_left)
                                           .size(button_size::medium)
                                           .on_press(act(host, action_id::back))
                                           .key(k_back))
                  : nullptr,
        label(page.info.group).bold().size(label_size::small).color(tok::text_muted).single_line(),
        icon_view(icon::chevron_right, 12.f, tok::icon_muted),
        label(page.info.title).size(label_size::small).single_line().truncate(),
    };
    const child heading[] = {
        headline(page.info.title).size(headline_size::large),
        caption(page.description),
    };
    const child content[] = {
        div(style{}.flex_col().gap(sp::b04), heading),
        keyed(page.build(s, host), page_specimen_key(s.page)),
    };
    const child centered[] = {
        div(style{}.flex_col().w_full().max_w(px(700)).gap(sp::b20).px(sp::b20).pt(sp::b20).pb(
                sp::b24),
            content),
    };
    const child shell[] = {
        div(style{}
                .flex_row()
                .items_center()
                .gap(sp::b08)
                .px(sp::b16)
                .h(px(48))
                .flex_shrink_0()
                .bg(tok::toolbar_background)
                .border_b_1(tok::border_variant),
            bar_items),
        scroll_area(style{}.flex_1().w_full().min_h(px(0)), centered, scroll_axes::vertical,
                    style{}.flex_col().items_center().w_full(), page_content_key(s.page)),
    };
    return keyed(
        div(style{}.flex_col().flex_grow().min_w(px(0)).min_h(px(0)).bg(tok::background), shell),
        k_detail);
}

[[nodiscard]] child build_demo(const demo_state& s, demo_host& host) {
    const child panes[] = {
        (!s.compact || !s.show_detail) ? sidebar(s, host) : nullptr,
        (!s.compact || s.show_detail) ? detail(s, host) : nullptr,
    };
    const child toast_item[] = {
        s.toast != notice::none
            ? static_cast<child>(
                  toast(s.toast == notice::archived ? "Item archived" : "Demo toast")
                      .status(s.toast == notice::archived ? status_kind::success
                                                          : status_kind::info)
                      .dismiss_button()
                      .duration_ms(4500)
                      .value(s.toast_generation)
                      .on_dismiss(value_action(host, action_id::dismiss_notice))
                      .key(0x554a'0000 + static_cast<element_key>(s.toast_generation)))
            : nullptr,
    };
    const bool modal_open = s.open == popup::modal;
    const child dialog =
        modal_open ? static_cast<child>(label("Move the sample item out of the collection?"))
                   : nullptr;
    auto confirmation = modal(dialog)
                            .title("Archive sample item?")
                            .open(modal_open)
                            .width(320.f)
                            .on_dismiss(close_popup(host, popup::modal))
                            .key(k_modal);
    if (modal_open) {
        confirmation.footer(button("Cancel")
                                .style(button_style::outlined)
                                .on_press(close_popup(host, popup::modal))
                                .key(k_modal_cancel),
                            button("Archive")
                                .style(button_style::filled)
                                .on_press(act(host, action_id::modal_confirm))
                                .key(k_modal_confirm));
    }
    const child children[] = {
        div(style{}.flex_row().flex_grow().min_h(px(0)).w_full(), panes),
        confirmation,
        s.toast != notice::none ? toast_stack(toast_item) : nullptr,
    };
    const child body[] = {div(style{}.flex_col().size_full().bg(tok::background), children)};
    return with_children(lx::ui::describe<focus_layout>(view.next_focus.target != 0), body);
}

} // namespace

std::span<const ui_demo::page_info> ui_demo::catalog() {
    return page_metadata;
}

void ui_demo::bind_root(lx::ui::ui_root* root) {
    if (view.root)
        view.root->focus().set_magnifier_hook(nullptr, nullptr);
    view = {.root = root};
    if (root)
        root->focus().set_magnifier_hook(&reveal_keyboard_focus, root);
}

lx::ui::child ui_demo::describe(lx::ui::build_context&) {
    demo_state initial{};
    if (auto* root = view.root) {
        const int width = root->bounds().width;
        initial.compact = width > 0 && width < k_compact_width;
        // Root already re-describes when theme, density or scale changes. Reducer
        // children are opaque to that pass, so explicitly refresh their descriptors.
        const auto theme = theme_generation();
        const float scale = root->scale();
        if (auto* node = root->tree(); node && node->type() == demo_host::static_type()) {
            auto& host = static_cast<demo_host&>(*node);
            if (host.state().compact != initial.compact)
                host.dispatch({action_id::resize, initial.compact ? 1 : 0});
            else if (theme != view.theme || scale != view.scale)
                host.request_rebuild();
        }
        view.theme = theme;
        view.scale = scale;
    }
    return lx::ui::reducer<demo_state, demo_action>(initial, &reduce, &build_demo, k_demo_root);
}
