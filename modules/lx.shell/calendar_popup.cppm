module;

#include <time.h>

import lx.foundation;
import lx.config;
import lx.ui;
import lx.ui.host;

export module lx.shell:calendar_popup;

import std;
import :clock;

export namespace lx::shell {

inline constexpr int k_calendar_width = 336;
inline constexpr int k_calendar_height = 376;
inline constexpr lx::rect2i k_calendar_prev_year{8, 8, 32, 32};
inline constexpr lx::rect2i k_calendar_prev_month{44, 8, 32, 32};
inline constexpr lx::rect2i k_calendar_next_month{260, 8, 32, 32};
inline constexpr lx::rect2i k_calendar_next_year{296, 8, 32, 32};
inline constexpr lx::rect2i k_calendar_today{16, 332, 144, 32};
inline constexpr lx::rect2i k_calendar_settings{232, 336, 88, 32};
inline constexpr int k_calendar_grid_x = 16;
inline constexpr int k_calendar_grid_y = 132;
inline constexpr int k_calendar_cell_w = 40;
inline constexpr int k_calendar_cell_h = 32;
inline constexpr int k_calendar_week_gutter = 24;

/// UI-affinity native popup. Input-triggered teardown is drained by after_dispatch().
class calendar_popup {
public:
    using settings_fn = void (*)(unsigned serial, void* user);

    calendar_popup() = default;
    ~calendar_popup();
    calendar_popup(const calendar_popup&) = delete;
    calendar_popup& operator=(const calendar_popup&) = delete;

    void init(lx::ui::host::context& ui) { ui_ = &ui; }
    [[nodiscard]] bool open(lx::ui::host::layer_role& layer, lx::rect2i anchor, unsigned serial,
                            const lx::config::clock_settings& settings, std::time_t now,
                            settings_fn on_settings, void* user);
    void close();
    [[nodiscard]] bool is_open() const { return surface_.valid(); }
    void configure(const lx::config::clock_settings& settings, std::time_t now);
    void update_time(std::time_t now);
    void after_dispatch();
    void activate(unsigned id, unsigned serial);
    void on_key(const lx::ui::key_event& event);

    [[nodiscard]] const calendar_model& model() const { return model_; }
    [[nodiscard]] lx::ui::host::surface* host_surface() { return &surface_; }
    [[nodiscard]] const lx::ui::host::popup_role& popup_parent() const { return role_; }

private:
    friend lx::ui::child calendar_describe(lx::ui::build_context&);
    [[nodiscard]] lx::ui::child build_ui();
    void request_draw();
    void focus_root();
    void focus_selected_date();

    lx::ui::host::context* ui_ = nullptr;
    lx::ui::host::surface surface_{};
    lx::ui::host::popup_role role_{};
    calendar_model model_{};
    lx::config::clock_settings settings_{};
    clock_text text_{};
    // Descriptors refer to these buffers until reconciliation copies their text.
    char title_[96]{};
    char days_[42][4]{};
    char weeks_[6][4]{};
    char date_names_[42][48]{};
    settings_fn on_settings_ = nullptr;
    void* user_ = nullptr;
    bool close_pending_ = false;
    bool keyboard_entered_ = false;
};

[[nodiscard]] lx::ui::child calendar_describe(lx::ui::build_context& ctx);

} // namespace lx::shell
