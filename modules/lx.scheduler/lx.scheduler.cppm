module;

import lx.foundation;
import lx.runtime;

export module lx.scheduler;

export import :presentation;
export import :budget;

export namespace lx::scheduler {

struct frame_tick {
    lx::runtime::clock_time timestamp_ns = 0;
    double delta_seconds = 0.0;
    unsigned frame_index = 0;
};

/// Alias to runtime affinity — scheduler routes frame phases to logical threads.
using thread_affinity = lx::runtime::affinity;

class frame_scheduler {
public:
    frame_scheduler();
    ~frame_scheduler();

    void on_vsync(frame_tick tick);

    /// Thread-safe: schedule callback on target affinity strand.
    [[nodiscard]] bool post_to(thread_affinity affinity, lx::runtime::callback fn);

    void drain(thread_affinity affinity);

    [[nodiscard]] frame_tick last_tick() const;
    [[nodiscard]] double target_fps() const;
    void set_target_fps(double fps);

    /// RAII: sets current thread affinity for the scope duration.
    class affinity_scope {
    public:
        explicit affinity_scope(thread_affinity affinity);
        ~affinity_scope();

    private:
        lx::runtime::affinity previous_{lx::runtime::affinity::any};
    };

private:
    frame_tick last_{};
    double target_fps_ = 60.0;
};

} // namespace lx::scheduler

module :private;

lx::scheduler::frame_scheduler::frame_scheduler() = default;
lx::scheduler::frame_scheduler::~frame_scheduler() = default;

void lx::scheduler::frame_scheduler::on_vsync(frame_tick tick) {
    last_ = tick;
}

bool lx::scheduler::frame_scheduler::post_to(thread_affinity affinity,
                                             lx::runtime::callback fn) {
    return lx::runtime::executor::global().post(affinity, fn);
}

void lx::scheduler::frame_scheduler::drain(thread_affinity affinity) {
    lx::runtime::executor::global().drain(affinity);
}

lx::scheduler::frame_tick lx::scheduler::frame_scheduler::last_tick() const {
    return last_;
}

double lx::scheduler::frame_scheduler::target_fps() const { return target_fps_; }

void lx::scheduler::frame_scheduler::set_target_fps(double fps) { target_fps_ = fps; }

lx::scheduler::frame_scheduler::affinity_scope::affinity_scope(thread_affinity affinity)
    : previous_{lx::runtime::current_affinity()} {
    lx::runtime::set_current_affinity(static_cast<lx::runtime::affinity>(affinity));
}

lx::scheduler::frame_scheduler::affinity_scope::~affinity_scope() {
    lx::runtime::set_current_affinity(previous_);
}
