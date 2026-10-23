module;

#if !defined(_WIN32)
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <time.h>

import lx.foundation;
import lx.sync;
import lx.trace;

export module lx.runtime;

import std;

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
    /// A file descriptor the loop should wait on (epoll), or -1. Readiness is reported
    /// through `on_readable`, so a source never has to poll for it.
    [[nodiscard]] virtual int poll_fd() const { return -1; }
    virtual void on_readable() {}
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
    [[nodiscard]] bool enabled() const { return enabled_; }
    /// The absolute `steady_clock` time of the next firing.
    [[nodiscard]] clock_time next_fire() const { return next_fire_ns_; }

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

/// Dispatches when its FD is readable (Wayland display, DRM, libinput, sd-bus, udev):
/// the loop waits on it with epoll, so an idle FD costs nothing. A nonzero
/// `poll_interval_ns` additionally dispatches at that period whether or not the FD is
/// readable — only for an FD whose readiness cannot be trusted.
class fd_source : public event_source {
public:
    fd_source(int fd, callback cb, clock_time poll_interval_ns = 0);
    bool prepare(clock_time& timeout) override;
    bool check() override;
    bool dispatch() override;
    void set_fd(int fd);
    [[nodiscard]] int poll_fd() const override { return fd_; }
    void on_readable() override { ready_ = true; }

private:
    int fd_ = -1;
    callback cb_{};
    clock_time poll_interval_ns_ = 0;
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

    /// Enqueue from one other thread — SPSC, so a single producer only. Wakes the loop.
    [[nodiscard]] bool post(task cb);
    void add_source(event_source* source);
    void remove_source(event_source* source);
    /// Wakes a sleeping loop. Thread-safe and async-signal-safe (one eventfd write).
    void wake();
    /// Runs after every iteration that dispatched something — the place to flush what the
    /// iteration produced (Wayland client queues), once.
    void set_after_dispatch(callback cb);
    /// Times the loop has woken from waiting (diagnostics).
    [[nodiscard]] unsigned long long wakeups() const { return wakeups_; }

    [[nodiscard]] unsigned source_count() const;
    [[nodiscard]] unsigned source_capacity() const;

    [[nodiscard]] clock_time now() const;
    [[nodiscard]] class strand& strand();

private:
    void drain_inbound();
    void drain_registered_strands();

    void arm_timer(clock_time deadline_ns);

    // Atomic so quit() stays safe from a signal handler or another thread.
    std::atomic<int> exit_code_{0};
    std::atomic<bool> running_{false};
    // A quit() that no run() has consumed yet: one that lands before run() starts (a
    // signal during start-up) must still stop it, and run() would otherwise reset it.
    std::atomic<bool> quit_pending_{false};
    int epoll_fd_ = -1;
    int wake_fd_ = -1;
    int timer_fd_ = -1;
    clock_time armed_ns_ = -1;
    callback after_{};
    unsigned long long wakeups_ = 0;
    class strand ui_strand_{affinity::ui};
    lx::sync::spsc_queue<task, k_strand_queue_depth> inbound_{};
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

namespace {
#if !defined(_WIN32)
// epoll user data for the loop's own descriptors; sources use their own address.
char g_wake_token;
char g_timer_token;
#endif
void wake_trampoline(void* loop) { static_cast<lx::runtime::event_loop*>(loop)->wake(); }
} // namespace

lx::runtime::event_loop::event_loop() {
#if !defined(_WIN32)
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (epoll_fd_ >= 0) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = &g_wake_token;
        if (wake_fd_ >= 0)
            (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
        ev.data.ptr = &g_timer_token;
        if (timer_fd_ >= 0)
            (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);
    }
#endif
    // Work posted for the UI thread wakes this loop instead of waiting for a poll.
    ui_strand_.set_waker(&wake_trampoline, this);
    executor::global().register_strand(affinity::ui, &ui_strand_);
}

lx::runtime::event_loop::~event_loop() {
    executor::global().unregister_strand(affinity::ui);
    ui_strand_.set_waker(nullptr, nullptr);
#if !defined(_WIN32)
    for (int* fd : {&timer_fd_, &wake_fd_, &epoll_fd_}) {
        if (*fd >= 0)
            ::close(*fd);
        *fd = -1;
    }
#endif
}

void lx::runtime::event_loop::wake() {
#if !defined(_WIN32)
    if (wake_fd_ >= 0) {
        const unsigned long long one = 1;
        (void)!::write(wake_fd_, &one, sizeof(one));
    }
#endif
}

void lx::runtime::event_loop::set_after_dispatch(callback cb) { after_ = cb; }

void lx::runtime::event_loop::arm_timer(clock_time deadline_ns) {
#if !defined(_WIN32)
    if (timer_fd_ < 0 || deadline_ns == armed_ns_)
        return;
    itimerspec spec{};
    if (deadline_ns > 0) {
        spec.it_value.tv_sec = static_cast<time_t>(deadline_ns / 1'000'000'000);
        spec.it_value.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000);
    }
    // Absolute on CLOCK_MONOTONIC (what steady_clock reads on Linux): a deadline, so
    // nothing the loop does in between can make the timer late by its own duration.
    (void)::timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &spec, nullptr);
    armed_ns_ = deadline_ns;
#else
    (void)deadline_ns;
#endif
}

int lx::runtime::event_loop::run() {
    running_ = true;
    // After setting running_: a quit() racing with this either sees running_ already set
    // (and clears it) or leaves quit_pending_ for this exchange to find.
    if (quit_pending_.exchange(false))
        running_ = false;
#if !defined(_WIN32)
    if (epoll_fd_ >= 0 && wake_fd_ >= 0 && timer_fd_ >= 0) {
        // Blocks until a source has work: an fd is readable, a timer is due, or another
        // thread posted. Nothing runs on a schedule of its own — an idle desktop sleeps.
        while (running_) {
            drain_inbound();
            ui_strand_.drain();

            clock_time timeout = -1; // relative, ns; -1 = until something happens
            const clock_time now = lx_runtime_now_ns();
            for (unsigned i = 0; i < loop_state().source_count; ++i) {
                auto* source = loop_state().sources[i];
                if (!source)
                    continue;
                clock_time t = -1;
                (void)source->prepare(t);
                if (t >= 0 && (timeout < 0 || t < timeout))
                    timeout = t;
            }
            if (!running_)
                break;
            if (timeout > 0)
                arm_timer(now + timeout);
            else if (timeout < 0 && armed_ns_ > 0)
                arm_timer(0); // a deadline that went away (a stopped timer) must not wake us
            epoll_event events[16];
            const int n = ::epoll_wait(epoll_fd_, events, 16, timeout == 0 ? 0 : -1);
            ++wakeups_;
            for (int i = 0; i < n; ++i) {
                void* tag = events[i].data.ptr;
                if (tag == &g_wake_token || tag == &g_timer_token) {
                    unsigned long long drained = 0;
                    (void)!::read(tag == &g_wake_token ? wake_fd_ : timer_fd_, &drained,
                                  sizeof(drained));
                    if (tag == &g_timer_token)
                        armed_ns_ = -1; // fired: the next deadline must re-arm it
                    continue;
                }
                static_cast<event_source*>(tag)->on_readable();
            }
            if (!running_)
                break;
            drain_inbound();
            ui_strand_.drain();
            for (unsigned i = 0; i < loop_state().source_count; ++i) {
                auto* source = loop_state().sources[i];
                if (source && source->check())
                    (void)source->dispatch();
            }
            if (after_)
                after_();
        }
        quit_pending_ = false; // consumed by this run
        return exit_code_;
    }
#endif
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
    quit_pending_ = false;
    return exit_code_;
}

void lx::runtime::event_loop::quit(int code) {
    exit_code_ = code;
    quit_pending_ = true;
    running_ = false;
    wake(); // a blocked epoll_wait would otherwise not notice
}

bool lx::runtime::event_loop::post(task cb) {
    if (!cb) return false;
    if (!inbound_.try_push(cb))
        return false;
    wake();
    return true;
}

void lx::runtime::event_loop::drain_inbound() {
    task fn{};
    while (inbound_.try_pop(fn)) {
        if (fn) fn();
    }
}

void lx::runtime::event_loop::add_source(event_source* source) {
    auto& state = loop_state();
    if (!source || state.source_count >= k_max_event_sources)
        return;
    state.sources[state.source_count++] = source;
#if !defined(_WIN32)
    if (const int fd = source->poll_fd(); fd >= 0 && epoll_fd_ >= 0) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = source;
        (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }
#endif
}

void lx::runtime::event_loop::remove_source(event_source* source) {
    auto& state = loop_state();
    for (unsigned i = 0; i < state.source_count; ++i) {
        if (state.sources[i] == source) {
#if !defined(_WIN32)
            if (const int fd = source->poll_fd(); fd >= 0 && epoll_fd_ >= 0)
                (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif
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
    if (repeat_ && interval_ns_ > 0) {
        // From the deadline, not from now: re-arming at `now + interval` let every tick's
        // lateness and the callback's own duration push all later ticks back — a 60 Hz
        // clock measured 57 Hz. Periods already missed are skipped, not replayed.
        next_fire_ns_ += interval_ns_;
        if (next_fire_ns_ <= now)
            next_fire_ns_ += ((now - next_fire_ns_) / interval_ns_ + 1) * interval_ns_;
    } else {
        enabled_ = false;
    }
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
    if (fd_ < 0) {
        timeout = -1;
        return false;
    }
    if (poll_interval_ns_ <= 0) {
        // Readiness comes from the loop's epoll (`on_readable`); nothing to schedule.
        timeout = -1;
        return ready_;
    }
    ready_ = false;
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
