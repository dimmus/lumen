module;

#include <time.h>

module lx.shell;

import std;
import lx.a11y;
import lx.foundation;
import lx.config;
import lx.layout;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
import :calendar_popup;
import :clock;

namespace {

constexpr lx::trace::category log_calendar{"shell.calendar"};

// A host can run a surface's first frame directly from its configure/frame source.
// Resolve the describer by the active root context, including those automatic frames.
// UI-affinity only; the host itself bounds its surface table to 32 entries.
std::array<lx::shell::calendar_popup*, 32> calendars{};

struct calendar_root_props {
    lx::ui::control_props control{};
    lx::shell::calendar_popup* self = nullptr;
};

class calendar_root_node final : public lx::ui::control_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const auto id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "calendar_root"; }
    bool apply(const calendar_root_props& p) {
        self_ = p.self;
        return control_node::apply(p.control);
    }
    bool on_focus_key(lx::ui::key_event e, lx::ui::focus_manager&) override {
        if (self_)
            self_->on_key(e);
        return true;
    }

private:
    lx::shell::calendar_popup* self_ = nullptr;
};

[[nodiscard]] lx::ui::style at(lx::rect2i r) {
    return lx::ui::style{}
        .absolute()
        .left(lx::layout::px(static_cast<float>(r.x)))
        .top(lx::layout::px(static_cast<float>(r.y)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

[[nodiscard]] lx::ui::child calendar_text(const char* text, lx::rect2i r, lx::ui::element_key key,
                                          lx::ui::tok color = lx::ui::tok::text) {
    const auto& theme = lx::ui::active_theme();
    lx::ui::text_block_props p{};
    p.text = text;
    p.font = {theme.fonts().ui_family, theme.fonts().ui_size_px * 0.75f, lx::text::weight::normal,
              lx::text::style::normal};
    p.color = theme.color(color);
    p.wrap = lx::text::text_wrap::none;
    p.ellipsize = true;
    p.context = lx::ui::active_ui_resources().text;
    return lx::ui::styled(at(r).flex_row().items_center().justify_center(),
                          lx::ui::text_block(p, key));
}

template <std::size_t N, typename... Args>
void bounded_text(char (&out)[N], std::format_string<Args...> fmt, Args&&... args) {
    const auto written = std::format_to_n(out, N - 1, fmt, std::forward<Args>(args)...);
    out[std::min<std::size_t>(written.size, N - 1)] = '\0';
}

} // namespace

lx::ui::child lx::shell::calendar_describe(lx::ui::build_context&) {
    for (calendar_popup* popup : calendars) {
        if (popup && popup->surface_.valid() && &popup->surface_.ui() == &lx::ui::active_context())
            return popup->build_ui();
    }
    return nullptr;
}

bool lx::shell::calendar_popup::open(lx::ui::host::layer_role& layer, lx::rect2i anchor,
                                     unsigned serial, const lx::config::clock_settings& settings,
                                     std::time_t now, settings_fn on_settings, void* user) {
    if (surface_.valid() && surface_.input().delivering()) {
        log_calendar.error(lx::invalid_argument("calendar popup opened during input delivery"));
        return false;
    }
    close();
    const auto today = clock_local_date(now);
    if (!ui_ || !ui_->started() || !layer.valid() || !serial || !ui_->display().default_seat() ||
        !calendar_model::valid_date(today)) {
        log_calendar.error(
            lx::invalid_argument("calendar popup requires a live parent, seat, serial and date"));
        return false;
    }
    auto slot = std::find(calendars.begin(), calendars.end(), nullptr);
    if (slot == calendars.end()) {
        log_calendar.error(lx::invalid_argument("calendar popup registry is full"));
        return false;
    }
    settings_ = settings;
    on_settings_ = on_settings;
    user_ = user;
    model_.first_day = settings.first_day;
    model_.reset(today);
    text_ = format_clock(now, settings);
    lx::ui::host::surface_options so{};
    so.describe = &calendar_describe;
    so.size = {k_calendar_width, k_calendar_height};
    so.opaque = false;
    so.frame_records = false;
    if (auto made = surface_.create(*ui_, so); !made) {
        log_calendar.error(made.get_error());
        return false;
    }
    // Install before show(): configure can produce the first frame without after_dispatch.
    *slot = this;
    lx::wayland::positioner_desc pos{};
    pos.size = {k_calendar_width, k_calendar_height};
    pos.anchor_rect = {anchor.x, anchor.y, std::max(1, anchor.width), std::max(1, anchor.height)};
    pos.anchor = 6;                        // bottom-left
    pos.gravity = 8;                       // bottom-right
    pos.constraint_adjustment = 1 | 2 | 8; // slide-x, slide-y, flip-y
    if (auto made = role_.create(surface_, layer, pos); !made) {
        log_calendar.error(made.get_error());
        close();
        return false;
    }
    role_.grab(serial);
    role_.set_done_handler(
        [](void* u) {
            // The Wayland popup listener must finish before its role is destroyed.
            static_cast<calendar_popup*>(u)->close_pending_ = true;
        },
        this);
    surface_.set_keyboard_focus_handler(
        [](bool entered, void* u) {
            auto* self = static_cast<calendar_popup*>(u);
            self->keyboard_entered_ = entered;
            if (entered)
                self->focus_root();
        },
        this);
    if (auto shown = surface_.show(); !shown) {
        log_calendar.error(shown.get_error());
        close();
        return false;
    }
    return true;
}

void lx::shell::calendar_popup::close() {
    if (surface_.valid() && surface_.input().delivering()) {
        close_pending_ = true;
        return;
    }
    // Unlink before destroying the root/context; no describer can see a stale surface.
    for (auto& popup : calendars)
        if (popup == this)
            popup = nullptr;
    role_.set_done_handler(nullptr, nullptr);
    role_.destroy();
    surface_.destroy();
    close_pending_ = keyboard_entered_ = false;
    on_settings_ = nullptr;
    user_ = nullptr;
}

lx::shell::calendar_popup::~calendar_popup() {
    close();
}

void lx::shell::calendar_popup::configure(const lx::config::clock_settings& settings,
                                          std::time_t now) {
    settings_ = settings;
    model_.first_day = settings.first_day;
    const auto today = clock_local_date(now);
    if (calendar_model::valid_date(today))
        model_.today = today;
    model_.rebuild();
    text_ = format_clock(now, settings);
    request_draw();
}

void lx::shell::calendar_popup::update_time(std::time_t now) {
    if (!is_open() || close_pending_)
        return;
    const auto date = clock_local_date(now);
    if (calendar_model::valid_date(date) && date != model_.today) {
        model_.today = date;
        model_.rebuild();
        request_draw();
    }
    const auto text = format_clock(now, settings_);
    if (std::strcmp(text.heading, text_.heading) != 0) {
        text_ = text;
        request_draw();
    }
}

void lx::shell::calendar_popup::request_draw() {
    if (!surface_.valid() || close_pending_)
        return;
    // set_describer requests a rebuild; paint damage alone keeps stale model descriptors.
    surface_.root().set_describer(&calendar_describe);
    surface_.root().add_repaint({0, 0, k_calendar_width, k_calendar_height});
}

void lx::shell::calendar_popup::focus_root() {
    if (!keyboard_entered_ || !surface_.valid())
        return;
    auto& root = surface_.root();
    if (!root.focus().focused())
        if (auto* tree = root.tree())
            (void)root.focus().focus(*tree, lx::ui::focus_cause::programmatic);
}

void lx::shell::calendar_popup::focus_selected_date() {
    if (!surface_.valid())
        return;
    auto& root = surface_.root();
    auto* tree = root.tree();
    if (!tree)
        return;
    // Date controls are direct root children, keyed by their bounded grid index.
    // Change focus during the input event so even a following Enter before the next
    // frame addresses the new model cell. Programmatic preserves pointer/key modality.
    for (unsigned cell = 0; cell < model_.cells.size(); ++cell) {
        if (!model_.cells[cell].selected)
            continue;
        const lx::ui::element_key key = 0xcb000u + cell * 4;
        for (unsigned child = 0; child < tree->child_count(); ++child) {
            auto* node = tree->child_at(child);
            if (node && node->key() == key) {
                (void)root.focus().focus(*node, lx::ui::focus_cause::programmatic);
                return;
            }
        }
        return;
    }
}

void lx::shell::calendar_popup::after_dispatch() {
    if (!surface_.valid())
        return;
    if (close_pending_) {
        close();
        return;
    }
    if (surface_.frame_due())
        surface_.run_frame();
    focus_root();
}

void lx::shell::calendar_popup::activate(unsigned id, unsigned serial) {
    if (!is_open() || close_pending_)
        return;
    const auto previous = model_.selected;
    if (id < model_.cells.size()) {
        model_.select(model_.cells[id].date);
    } else {
        switch (id) {
        case 100:
            model_.move_months(-12);
            break;
        case 101:
            model_.move_months(-1);
            break;
        case 102:
            model_.move_months(1);
            break;
        case 103:
            model_.move_months(12);
            break;
        case 104:
            model_.go_today();
            break;
        case 105:
            // The callback may request an activation token using this source wl_surface.
            // Keep it and its press serial alive through the callback and input unwind.
            close_pending_ = true;
            if (on_settings_)
                on_settings_(serial, user_);
            return;
        default:
            return;
        }
    }
    if (previous != model_.selected)
        request_draw();
    if (id < model_.cells.size())
        focus_selected_date();
}

void lx::shell::calendar_popup::on_key(const lx::ui::key_event& e) {
    if (!e.pressed || !is_open() || close_pending_)
        return;
    const auto previous = model_.selected;
    const unsigned weekday = std::chrono::weekday{std::chrono::sys_days{previous}}.c_encoding();
    const int offset = static_cast<int>(
        settings_.first_day == lx::config::clock_first_day::monday ? (weekday + 6) % 7 : weekday);
    switch (e.key) {
    case lx::ui::key::left:
        model_.move_days(-1);
        break;
    case lx::ui::key::right:
        model_.move_days(1);
        break;
    case lx::ui::key::up:
        model_.move_days(-7);
        break;
    case lx::ui::key::down:
        model_.move_days(7);
        break;
    case lx::ui::key::home:
        model_.move_days(-offset);
        break;
    case lx::ui::key::end:
        model_.move_days(6 - offset);
        break;
    case lx::ui::key::page_up:
        model_.move_months((e.modifiers & lx::ui::k_mod_ctrl) ? -12 : -1);
        break;
    case lx::ui::key::page_down:
        model_.move_months((e.modifiers & lx::ui::k_mod_ctrl) ? 12 : 1);
        break;
    case lx::ui::key::escape:
        close_pending_ = true;
        return;
    default:
        return;
    }
    if (previous != model_.selected) {
        request_draw();
        focus_selected_date();
    }
}

lx::ui::child lx::shell::calendar_popup::build_ui() {
    lx::ui::child children[64]{};
    unsigned n = 0;
    auto add = [&](lx::ui::child child) { children[n++] = child; };
    auto button = [&](unsigned id, const char* label, const char* name, lx::rect2i rect,
                      lx::ui::element_key key, const calendar_day* day = nullptr) {
        lx::ui::control_props c{};
        c.layout = at(rect).flex_row().items_center().justify_center();
        c.interactive = true;
        c.focusable = true;
        c.focus_ring = true;
        c.role = lx::a11y::role::button;
        c.name = name;
        c.disabled = day && !calendar_model::valid_date(day->date);
        c.selected = day && day->selected;
        if (c.selected)
            c.a11y_state = static_cast<unsigned>(lx::a11y::state_flag::selected);
        const auto border =
            lx::ui::paint(day && day->today ? lx::ui::tok::border_focused : lx::ui::tok::none);
        c.paint.rest = {
            lx::ui::paint(c.selected ? lx::ui::tok::element_selected : lx::ui::tok::none), border};
        c.paint.hover = {
            lx::ui::paint(c.selected ? lx::ui::tok::element_selected : lx::ui::tok::element_hover),
            border};
        c.paint.active = {lx::ui::paint(lx::ui::tok::element_active), border};
        for (auto& width : c.paint.border_widths)
            width = day && day->today ? 1 : 0;
        for (auto& corner : c.paint.corners)
            corner = lx::ui::radius::sm;
        c.value = static_cast<int>(id);
        c.on_activate = [this](int value) {
            activate(static_cast<unsigned>(value), ui_->last_press_serial());
        };
        const auto color = day && !day->in_month && !day->selected ? lx::ui::tok::text_muted
                           : day && day->today                     ? lx::ui::tok::text_accent
                                                                   : lx::ui::tok::text;
        const lx::ui::child label_child[1] = {
            calendar_text(label, {0, 0, rect.width, rect.height}, key + 1, color)};
        add(lx::ui::control(c, label_child, key));
    };

    std::tm tm{};
    tm.tm_year = int(model_.displayed.year()) - 1900;
    tm.tm_mon = int(unsigned(model_.displayed.month())) - 1;
    tm.tm_mday = 1;
    if (!std::strftime(title_, sizeof(title_), "%B %Y", &tm))
        bounded_text(title_, "{:04}-{:02}", int(model_.displayed.year()),
                     unsigned(model_.displayed.month()));
    button(100, "|<", "Previous year", k_calendar_prev_year, 0xca010u);
    button(101, "<", "Previous month", k_calendar_prev_month, 0xca020u);
    add(calendar_text(title_, {80, 8, 176, 32}, 0xca030u));
    button(102, ">", "Next month", k_calendar_next_month, 0xca040u);
    button(103, ">|", "Next year", k_calendar_next_year, 0xca050u);
    add(calendar_text(text_.heading, {12, 48, 312, 24}, 0xca060u));
    static constexpr const char* monday[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static constexpr const char* sunday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const auto* names =
        settings_.first_day == lx::config::clock_first_day::monday ? monday : sunday;
    const int grid_x = k_calendar_grid_x + (settings_.week_numbers ? k_calendar_week_gutter : 0);
    if (settings_.week_numbers)
        add(calendar_text("Wk",
                          {k_calendar_grid_x, k_calendar_grid_y - 24, k_calendar_week_gutter, 24},
                          0xca070u, lx::ui::tok::text_muted));
    for (unsigned col = 0; col < 7; ++col)
        add(calendar_text(
            names[col],
            {grid_x + int(col) * k_calendar_cell_w, k_calendar_grid_y - 24, k_calendar_cell_w, 24},
            0xca080u + col, lx::ui::tok::text_muted));
    for (unsigned row = 0; row < 6; ++row) {
        if (settings_.week_numbers) {
            // Label the ISO week of the Thursday in the displayed row, also for Sunday-first.
            bounded_text(weeks_[row], "{}", calendar_iso_week(model_.cells[row * 7 + 4].date));
            add(calendar_text(weeks_[row],
                              {k_calendar_grid_x, k_calendar_grid_y + int(row) * k_calendar_cell_h,
                               k_calendar_week_gutter, k_calendar_cell_h},
                              0xca090u + row, lx::ui::tok::text_muted));
        }
        for (unsigned col = 0; col < 7; ++col) {
            const unsigned i = row * 7 + col;
            const auto& day = model_.cells[i];
            bounded_text(days_[i], "{}", unsigned(day.date.day()));
            bounded_text(date_names_[i], "{:04}-{:02}-{:02}{}", int(day.date.year()),
                         unsigned(day.date.month()), unsigned(day.date.day()),
                         day.today ? " Today" : "");
            button(i, days_[i], date_names_[i],
                   {grid_x + int(col) * k_calendar_cell_w,
                    k_calendar_grid_y + int(row) * k_calendar_cell_h, k_calendar_cell_w,
                    k_calendar_cell_h},
                   0xcb000u + i * 4, &day);
        }
    }
    button(104, "Today", "Go to today", k_calendar_today, 0xcc000u);
    button(105, "Settings", "Date and time settings", k_calendar_settings, 0xcc010u);
    calendar_root_props root{};
    root.control.layout =
        lx::ui::style{}.w(lx::layout::percent(100.f)).h(lx::layout::percent(100.f));
    root.control.focusable = true;
    root.control.role = lx::a11y::role::group;
    root.control.name = "Calendar";
    root.control.paint =
        lx::ui::control_paint::fixed({lx::ui::paint(lx::ui::tok::elevated_surface_background),
                                      lx::ui::paint(lx::ui::tok::border)});
    for (auto& width : root.control.paint.border_widths)
        width = 1;
    for (auto& corner : root.control.paint.corners)
        corner = lx::ui::radius::lg;
    root.self = this;
    return lx::ui::with_children(
        lx::ui::describe<calendar_root_node, calendar_root_props>(root, 0xca000u), {children, n});
}
