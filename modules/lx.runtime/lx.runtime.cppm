module;

#include <atomic>
#include <chrono>
#include <thread>

#if !defined(_WIN32)
#include <poll.h>
#endif

import lx.foundation;
import lx.sync;
import lx.trace;

export module lx.runtime;

export import :executor;
export import :memory;

export namespace lx::runtime {

using clock_time = long long;

// ── Event sources ───────────────────────────────────────────────────────────

class event_source {
public:
    virtual ~event_source() = default;
    [[nodiscard]] virtual bool prepare(clock_time& timeout) = 0;
    [[nodiscard]] virtual bool check() = 0;
    [[nodiscard]] virtual bool dispatch() = 0;
};

class timer_source : public event_source {
public:
    explicit timer_source(clock_time interval_ns, callback cb, bool repeat = true);
    void set_interval(clock_time interval_ns);
    void set_enabled(bool enabled);

    /// Overrides the next firing with an absolute `steady_clock` time, replacing the
    /// interval for that one tick. Valid from inside the timer's own callback: a frame
    /// clock learns when the next frame is due only while handling the current one, and a
    /// deadline chosen there must not be overwritten by the plain interval on return.
    void schedule_at(clock_time when_ns);

    bool prepare(clock_time& timeout) override;
    bool check() override;
    bool dispatch() override;

private:
    clock_time interval_ns_ = 0;
    callback cb_{};
    bool repeat_ = true;
    bool enabled_ = true;
    bool rescheduled_ = false;
    long long next_fire_ns_ = 0;
};

class idle_source : public event_source {
public:
    explicit idle_source(callback cb);
    bool prepare(clock_time& timeout) override;
    bool check() override;
    bool dispatch() override;

private:
    callback cb_{};
    bool pending_ = true;
};

/// Poll an FD each loop iteration (Wayland display, DRM, sd-bus, …).
class fd_source : public event_source {
public:
    fd_source(int fd, callback cb, clock_time poll_interval_ns = 1'000'000);
    bool prepare(clock_time& timeout) override;
    bool check() override;
    bool dispatch() override;
    void set_fd(int fd);

private:
    int fd_ = -1;
    callback cb_{};
    clock_time poll_interval_ns_ = 1'000'000;
    bool ready_ = false;
};

// ── Event loop (UI / wayland thread) ────────────────────────────────────────

class event_loop {
public:
    event_loop();
    ~event_loop();

    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    [[nodiscard]] int run();
    void quit(int code = 0);

    /// Thread-safe: enqueue from worker/render threads via SPSC queue.
    [[nodiscard]] bool post(callback cb);
    void add_source(event_source* source);
    void remove_source(event_source* source);

    [[nodiscard]] unsigned source_count() const;
    [[nodiscard]] unsigned source_capacity() const;

    [[nodiscard]] clock_time now() const;
    [[nodiscard]] class strand& strand();

private:
    void drain_inbound();
    void drain_registered_strands();

    // Atomic so quit() stays safe from a signal handler or another thread.
    std::atomic<int> exit_code_{0};
    std::atomic<bool> running_{false};
    class strand ui_strand_{affinity::ui};
    lx::sync::spsc_queue<callback, 4096> inbound_{};
};

// ── Typed event bus (UI thread only) ────────────────────────────────────────

template<typename Event>
class subscription {
public:
    subscription() = default;
    void cancel();
    [[nodiscard]] bool active() const;

private:
    friend class event_bus;
    unsigned id_ = 0;
};

class event_bus {
public:
    template<typename Event, typename Handler>
    subscription<Event> subscribe(Handler handler) {
        assert_affinity(affinity::ui);
        (void)handler;
        return {};
    }

    template<typename Event>
    void publish(const Event& event) {
        assert_affinity(affinity::ui);
        (void)event;
    }

    void unsubscribe(unsigned subscription_id);
};

} // namespace lx::runtime

module :private;

static long long lx_runtime_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static constexpr unsigned k_max_event_sources = 32;

struct lx_runtime_loop_state {
    lx::runtime::event_source* sources[k_max_event_sources]{};
    unsigned source_count = 0;
};

static lx_runtime_loop_state& loop_state() {
    static lx_runtime_loop_state s;
    return s;
}

lx::runtime::event_loop::event_loop() = default;
lx::runtime::event_loop::~event_loop() = default;

int lx::runtime::event_loop::run() {
    running_ = true;
    while (running_) {
        drain_inbound();
        ui_strand_.drain();

        clock_time min_sleep_ns = 16'666'667;
        for (unsigned i = 0; i < loop_state().source_count; ++i) {
            auto* source = loop_state().sources[i];
            if (!source) continue;
            clock_time timeout = min_sleep_ns;
            (void)source->prepare(timeout);
            if (timeout >= 0 && timeout < min_sleep_ns)
                min_sleep_ns = timeout;
        }

        for (unsigned i = 0; i < loop_state().source_count; ++i) {
            auto* source = loop_state().sources[i];
            if (!source) continue;
            if (source->check())
                (void)source->dispatch();
        }

        if (min_sleep_ns > 0)
            std::this_thread::sleep_for(std::chrono::nanoseconds(min_sleep_ns));
    }
    return exit_code_;
}

void lx::runtime::event_loop::quit(int code) {
    exit_code_ = code;
    running_ = false;
}

bool lx::runtime::event_loop::post(callback cb) {
    if (!cb) return false;
    return inbound_.try_push(cb);
}

void lx::runtime::event_loop::drain_inbound() {
    callback fn{};
    while (inbound_.try_pop(fn)) {
        if (fn) fn();
    }
}

void lx::runtime::event_loop::add_source(event_source* source) {
    auto& state = loop_state();
    if (state.source_count < k_max_event_sources)
        state.sources[state.source_count++] = source;
}

void lx::runtime::event_loop::remove_source(event_source* source) {
    auto& state = loop_state();
    for (unsigned i = 0; i < state.source_count; ++i) {
        if (state.sources[i] == source) {
            state.sources[i] = state.sources[--state.source_count];
            return;
        }
    }
}

unsigned lx::runtime::event_loop::source_count() const {
    return loop_state().source_count;
}

unsigned lx::runtime::event_loop::source_capacity() const {
    return k_max_event_sources;
}

lx::runtime::clock_time lx::runtime::event_loop::now() const {
    return lx_runtime_now_ns();
}

lx::runtime::strand& lx::runtime::event_loop::strand() { return ui_strand_; }

lx::runtime::timer_source::timer_source(clock_time interval_ns, callback cb, bool repeat)
    : interval_ns_{interval_ns}, cb_{cb}, repeat_{repeat} {
    next_fire_ns_ = lx_runtime_now_ns();
}

void lx::runtime::timer_source::set_interval(clock_time interval_ns) { interval_ns_ = interval_ns; }
void lx::runtime::timer_source::set_enabled(bool enabled) { enabled_ = enabled; }

void lx::runtime::timer_source::schedule_at(clock_time when_ns) {
    next_fire_ns_ = static_cast<long long>(when_ns);
    rescheduled_ = true;
    enabled_ = true;
}

bool lx::runtime::timer_source::prepare(clock_time& timeout) {
    if (!enabled_) {
        timeout = -1;
        return false;
    }
    const auto now = lx_runtime_now_ns();
    const auto remaining = next_fire_ns_ - now;
    timeout = remaining > 0 ? remaining : 0;
    return true;
}

bool lx::runtime::timer_source::check() {
    if (!enabled_) return false;
    return lx_runtime_now_ns() >= next_fire_ns_;
}

bool lx::runtime::timer_source::dispatch() {
    rescheduled_ = false;
    if (cb_) cb_();
    if (rescheduled_) {
        rescheduled_ = false;
        return true;
    }
    const auto now = lx_runtime_now_ns();
    if (repeat_)
        next_fire_ns_ = now + interval_ns_;
    else
        enabled_ = false;
    return true;
}

lx::runtime::idle_source::idle_source(callback cb) : cb_{cb} {}
bool lx::runtime::idle_source::prepare(clock_time& timeout) {
    timeout = 0;
    return pending_;
}
bool lx::runtime::idle_source::check() { return pending_; }
bool lx::runtime::idle_source::dispatch() {
    pending_ = false;
    if (cb_) cb_();
    return true;
}

lx::runtime::fd_source::fd_source(int fd, callback cb, clock_time poll_interval_ns)
    : fd_{fd}, cb_{cb}, poll_interval_ns_{poll_interval_ns} {}

void lx::runtime::fd_source::set_fd(int fd) { fd_ = fd; }

bool lx::runtime::fd_source::prepare(clock_time& timeout) {
    ready_ = false;
    if (fd_ < 0) {
        timeout = -1;
        return false;
    }
#if !defined(_WIN32)
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
        ready_ = true;
        timeout = 0;
        return true;
    }
#endif
    // Not readable yet — wake periodically so the callback can still pump (Wayland).
    timeout = poll_interval_ns_;
    ready_ = true;
    return true;
}

bool lx::runtime::fd_source::check() { return ready_; }

bool lx::runtime::fd_source::dispatch() {
    ready_ = false;
    if (cb_)
        cb_();
    return true;
}

void lx::runtime::event_bus::unsubscribe(unsigned) {}

template<typename Event>
void lx::runtime::subscription<Event>::cancel() { id_ = 0; }

template<typename Event>
bool lx::runtime::subscription<Event>::active() const { return id_ != 0; }
