module;

import lx.foundation;

export module lx.scheduler:budget;

export namespace lx::scheduler {

/// Hot-path CPU budgets — compositor traces warn when exceeded (P1 enforcement).
struct hot_path_budget {
    unsigned max_tick_ui_us = 2000;       ///< UI thread per-frame budget
    unsigned max_tick_render_us = 4000;   ///< Render thread per-frame budget
    unsigned max_draw_commands = 4096;    ///< Matches draw_list capacity
    unsigned max_import_cache_entries = 512;
    unsigned max_frame_latency_ms = 32;   ///< Target end-to-end latency ceiling
};

class budget_tracker {
public:
    explicit budget_tracker(hot_path_budget budget = {});

    void begin_frame(unsigned frame_index);
    void end_tick_ui(unsigned elapsed_us);
    void end_tick_render(unsigned elapsed_us);

    [[nodiscard]] bool tick_ui_over_budget() const;
    [[nodiscard]] bool tick_render_over_budget() const;
    [[nodiscard]] const hot_path_budget& budget() const;
    [[nodiscard]] unsigned last_tick_ui_us() const;
    [[nodiscard]] unsigned last_tick_render_us() const;

private:
    hot_path_budget budget_{};
    unsigned frame_index_ = 0;
    unsigned last_ui_us_ = 0;
    unsigned last_render_us_ = 0;
    bool ui_over_ = false;
    bool render_over_ = false;
};

} // namespace lx::scheduler


lx::scheduler::budget_tracker::budget_tracker(hot_path_budget budget) : budget_{budget} {}

void lx::scheduler::budget_tracker::begin_frame(unsigned frame_index) {
    frame_index_ = frame_index;
    ui_over_ = false;
    render_over_ = false;
}

void lx::scheduler::budget_tracker::end_tick_ui(unsigned elapsed_us) {
    last_ui_us_ = elapsed_us;
    ui_over_ = elapsed_us > budget_.max_tick_ui_us;
}

void lx::scheduler::budget_tracker::end_tick_render(unsigned elapsed_us) {
    last_render_us_ = elapsed_us;
    render_over_ = elapsed_us > budget_.max_tick_render_us;
}

bool lx::scheduler::budget_tracker::tick_ui_over_budget() const { return ui_over_; }
bool lx::scheduler::budget_tracker::tick_render_over_budget() const { return render_over_; }
const lx::scheduler::hot_path_budget& lx::scheduler::budget_tracker::budget() const {
    return budget_;
}
unsigned lx::scheduler::budget_tracker::last_tick_ui_us() const { return last_ui_us_; }
unsigned lx::scheduler::budget_tracker::last_tick_render_us() const { return last_render_us_; }
