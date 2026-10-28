module;

#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

import lx.foundation;
import lx.sync;
import lx.trace;

export module lx.runtime;

import std;

export import :executor;
export import :memory;
export import :psi;
export import :sched;

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
    /// Wait for `poll_fd` to signal an exceptional condition (EPOLLPRI), not to be readable.
    /// For files that are always readable and report events that way: PSI triggers.
    [[nodiscard]] virtual bool poll_priority() const { return false; }
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
    // Only without epoll_pwait2 (Linux < 5.11), which takes the timeout itself.
    int timer_fd_ = -1;
    bool pwait2_ = false;
    clock_time armed_ns_ = -1;
    callback after_{};
    unsigned long long wakeups_ = 0;
    class strand ui_strand_{affinity::ui};
    lx::sync::spsc_queue<task, k_strand_queue_depth> inbound_{};
};

// ── Kernel memory pressure (UI thread) ──────────────────────────────────────

/// Feeds the kernel's memory pressure (PSI) into a `memory_budget_coordinator`: a "some"
/// trigger makes it moderate, a "full" trigger critical, and the level decays once the
/// triggers stay quiet (`psi_pressure_state`). Runs on the loop's thread — the UI affinity,
/// where the coordinator's handler runs. Lumen's own pool usage still counts: the
/// coordinator keeps the higher of the two.
class psi_monitor {
public:
    psi_monitor() = default;
    ~psi_monitor();

    psi_monitor(const psi_monitor&) = delete;
    psi_monitor& operator=(const psi_monitor&) = delete;

    /// Opens the pressure file, arms both triggers and adds them to `loop`. Logs the outcome
    /// once. Fails where the kernel has no PSI or refuses the triggers (no file, no write
    /// access, a kernel before 6.5 that takes no trigger from a user); the caller then runs
    /// on pool usage alone.
    [[nodiscard]] lx::result<void> start(event_loop& loop, memory_budget_coordinator& memory,
                                         psi_config config = {});
    void stop();

    [[nodiscard]] bool active() const { return loop_ != nullptr; }
    /// The pressure file the triggers are on, "" while not active.
    [[nodiscard]] std::string_view path() const { return path_.data(); }
    [[nodiscard]] memory_pressure level() const { return state_.level(); }

private:
    class trigger_source final : public event_source {
    public:
        bool prepare(clock_time& timeout) override;
        bool check() override { return ready_; }
        bool dispatch() override;
        [[nodiscard]] int poll_fd() const override { return fd_.get(); }
        [[nodiscard]] bool poll_priority() const override { return true; }
        void on_readable() override { ready_ = true; }

        psi_monitor* owner_ = nullptr;
        memory_pressure level_ = memory_pressure::moderate;
        lx::unique_fd fd_{};
        long long last_ns_ = 0;
        unsigned strikes_ = 0;
        bool ready_ = false;
    };
    class decay_source final : public event_source {
    public:
        bool prepare(clock_time& timeout) override;
        bool check() override;
        bool dispatch() override;

        psi_monitor* owner_ = nullptr;
    };

    void on_trigger(trigger_source& source);
    void on_quiet();

    event_loop* loop_ = nullptr;
    memory_budget_coordinator* memory_ = nullptr;
    psi_config config_{};
    psi_pressure_state state_{};
    trigger_source moderate_{};
    trigger_source critical_{};
    decay_source decay_{};
    std::array<char, 256> path_{};
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

// The loop's clock for the phase of an iteration under way — read once before the sources
// prepare and once after the wait, rather than by every timer source in turn (a dozen
// vDSO reads per phase). 0 outside the loop: a source then reads the clock itself.
static thread_local long long g_phase_now = 0;

static long long lx_runtime_phase_now() {
    return g_phase_now != 0 ? g_phase_now : lx_runtime_now_ns();
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
// epoll user data for the loop's own descriptors; sources use their own address.
char g_wake_token;
char g_timer_token;
void wake_trampoline(void* loop) { static_cast<lx::runtime::event_loop*>(loop)->wake(); }
} // namespace

lx::runtime::event_loop::event_loop() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epoll_fd_ >= 0) {
        // epoll_pwait2 takes a nanosecond timeout: the next deadline needs no timerfd to
        // arm before each wait and read after each fire — two syscalls per timer wake.
        epoll_event probe{};
        const timespec zero{};
        pwait2_ = ::epoll_pwait2(epoll_fd_, &probe, 1, &zero, nullptr) >= 0; // ENOSYS < 5.11
    }
    if (!pwait2_)
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
    // Work posted for the UI thread wakes this loop instead of waiting for a poll.
    ui_strand_.set_waker(&wake_trampoline, this);
    executor::global().register_strand(affinity::ui, &ui_strand_);
}

lx::runtime::event_loop::~event_loop() {
    executor::global().unregister_strand(affinity::ui);
    ui_strand_.set_waker(nullptr, nullptr);
    for (int* fd : {&timer_fd_, &wake_fd_, &epoll_fd_}) {
        if (*fd >= 0)
            ::close(*fd);
        *fd = -1;
    }
}

void lx::runtime::event_loop::wake() {
    if (wake_fd_ >= 0) {
        const unsigned long long one = 1;
        (void)!::write(wake_fd_, &one, sizeof(one));
    }
}

void lx::runtime::event_loop::set_after_dispatch(callback cb) { after_ = cb; }

void lx::runtime::event_loop::arm_timer(clock_time deadline_ns) {
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
}

int lx::runtime::event_loop::run() {
    running_ = true;
    // After setting running_: a quit() racing with this either sees running_ already set
    // (and clears it) or leaves quit_pending_ for this exchange to find.
    if (quit_pending_.exchange(false))
        running_ = false;
    if (epoll_fd_ >= 0 && wake_fd_ >= 0 && (pwait2_ || timer_fd_ >= 0)) {
        // A timeout's expiry is subject to the thread's timer slack (50 µs by default; an
        // absolute timerfd had none). The frame clock is a deadline, so keep it tight.
        if (pwait2_)
            (void)::prctl(PR_SET_TIMERSLACK, 1000UL, 0UL, 0UL, 0UL);
        // Blocks until a source has work: an fd is readable, a timer is due, or another
        // thread posted. Nothing runs on a schedule of its own — an idle desktop sleeps.
        while (running_) {
            drain_inbound();
            ui_strand_.drain();

            clock_time timeout = -1; // relative, ns; -1 = until something happens
            const clock_time now = lx_runtime_now_ns();
            g_phase_now = now;
            for (unsigned i = 0; i < loop_state().source_count; ++i) {
                auto* source = loop_state().sources[i];
                if (!source)
                    continue;
                clock_time t = -1;
                (void)source->prepare(t);
                if (t >= 0 && (timeout < 0 || t < timeout))
                    timeout = t;
            }
            g_phase_now = 0;
            if (!running_)
                break;
            epoll_event events[16];
            int n = 0;
            if (pwait2_) {
                timespec ts{};
                const timespec* wait = nullptr;
                if (timeout >= 0) {
                    // Relative to now, not to the read before the sources prepared.
                    clock_time left = now + timeout - lx_runtime_now_ns();
                    if (left < 0)
                        left = 0;
                    ts.tv_sec = static_cast<time_t>(left / 1'000'000'000);
                    ts.tv_nsec = static_cast<long>(left % 1'000'000'000);
                    wait = &ts;
                }
                n = ::epoll_pwait2(epoll_fd_, events, 16, wait, nullptr);
            } else {
                if (timeout > 0)
                    arm_timer(now + timeout);
                else if (timeout < 0 && armed_ns_ > 0)
                    arm_timer(0); // a deadline that went away must not wake us
                n = ::epoll_wait(epoll_fd_, events, 16, timeout == 0 ? 0 : -1);
            }
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
            // One read for every check. A timer that comes due while another source is
            // dispatching is seen by the next iteration, which then does not wait.
            g_phase_now = lx_runtime_now_ns();
            for (unsigned i = 0; i < loop_state().source_count; ++i) {
                auto* source = loop_state().sources[i];
                if (source && source->check())
                    (void)source->dispatch();
            }
            g_phase_now = 0;
            if (after_)
                after_();
        }
        quit_pending_ = false; // consumed by this run
        return exit_code_;
    }
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
    if (const int fd = source->poll_fd(); fd >= 0 && epoll_fd_ >= 0) {
        epoll_event ev{};
        ev.events = source->poll_priority() ? EPOLLPRI : EPOLLIN;
        ev.data.ptr = source;
        (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
    }
}

void lx::runtime::event_loop::remove_source(event_source* source) {
    auto& state = loop_state();
    for (unsigned i = 0; i < state.source_count; ++i) {
        if (state.sources[i] == source) {
            if (const int fd = source->poll_fd(); fd >= 0 && epoll_fd_ >= 0)
                (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
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
    const auto now = lx_runtime_phase_now();
    const auto remaining = next_fire_ns_ - now;
    timeout = remaining > 0 ? remaining : 0;
    return true;
}

bool lx::runtime::timer_source::check() {
    if (!enabled_) return false;
    return lx_runtime_phase_now() >= next_fire_ns_;
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
    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
        ready_ = true;
        timeout = 0;
        return true;
    }
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

// ── psi_monitor ─────────────────────────────────────────────────────────────

namespace {

constexpr lx::trace::category log_psi{"runtime.psi"};

const char* pressure_name(lx::runtime::memory_pressure level) {
    switch (level) {
    case lx::runtime::memory_pressure::moderate: return "moderate";
    case lx::runtime::memory_pressure::critical: return "critical";
    default: return "normal";
    }
}

/// Opens `path` once per trigger (a PSI file holds one trigger per open file: a second
/// write is EBUSY) and writes the trigger. 0, or the errno of the first failure.
int open_psi_trigger(const char* path, std::span<const char> trigger, lx::unique_fd& out) {
    lx::unique_fd fd{::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC)};
    if (fd.get() < 0)
        return errno;
    if (::write(fd.get(), trigger.data(), trigger.size()) < 0)
        return errno;
    out = std::move(fd);
    return 0;
}

} // namespace

lx::runtime::psi_monitor::~psi_monitor() { stop(); }

lx::result<void> lx::runtime::psi_monitor::start(event_loop& loop,
                                                 memory_budget_coordinator& memory,
                                                 psi_config config) {
    stop();
    std::array<char, 48> some{};
    std::array<char, 48> full{};
    const std::size_t some_len =
        format_psi_trigger(some, false, config.moderate_some_us, config.window_us);
    const std::size_t full_len =
        format_psi_trigger(full, true, config.critical_full_us, config.window_us);
    if (some_len == 0 || full_len == 0)
        return lx::invalid_argument("psi_monitor: stall longer than its window");

    // Which pressure, in order:
    //  1. $MEMORY_PRESSURE_WATCH — systemd's protocol (MemoryPressureWatch=, on by default
    //     for units with memory accounting): the file it names, or "/dev/null" for off. It
    //     names the unit's own memory.pressure. Its MEMORY_PRESSURE_WRITE trigger is not
    //     used: Lumen needs two levels, so two triggers of its own.
    //  2. The compositor's own cgroup. PSI is hierarchical: a stall of any compositor thread
    //     shows there whatever caused it — a system short of memory, or a MemoryHigh= on
    //     the service, session.slice or the user's slice — and stalls that freeing its
    //     caches cannot relieve do not. /proc/pressure/memory counts every cgroup stalling
    //     against its own limit (a build in a MemoryHigh= unit, a container): reacting to
    //     that would drop imports and re-import them every window, for nobody's benefit.
    //     As a user service (deploy/systemd: session.slice in the user manager's delegated
    //     tree) the file is the user's to write.
    //  3. /proc/pressure/memory, where the cgroup's file is not ours to write (a login
    //     shell's session-N.scope is root's) or there is no cgroup v2: a coarser signal is
    //     better than none.
    std::array<std::array<char, 256>, 3> candidates{};
    unsigned count = 0;
    if (const char* watch = std::getenv("MEMORY_PRESSURE_WATCH")) {
        if (watch[0] == '\0' || std::string_view{watch} == "/dev/null") {
            log_psi.info("kernel memory pressure off (MEMORY_PRESSURE_WATCH={}): pool usage "
                         "only", watch);
            return lx::make_error(lx::io_err::not_found, "psi_monitor: turned off");
        }
        auto r = std::format_to_n(candidates[count].data(), candidates[count].size() - 1, "{}",
                                  watch);
        *r.out = '\0';
        ++count;
    }
    {
        std::array<char, 1024> text{};
        lx::unique_fd fd{::open("/proc/self/cgroup", O_RDONLY | O_CLOEXEC)};
        const long n = fd.get() >= 0 ? ::read(fd.get(), text.data(), text.size() - 1) : -1;
        const std::string_view cgroup =
            n > 0 ? cgroup_v2_path(std::string_view{text.data(), static_cast<std::size_t>(n)})
                  : std::string_view{};
        if (cgroup.size() > 1) { // "/" is the root: the same counters as /proc/pressure
            auto r = std::format_to_n(candidates[count].data(), candidates[count].size() - 1,
                                      "/sys/fs/cgroup{}/memory.pressure", cgroup);
            *r.out = '\0';
            ++count;
        }
    }
    std::ranges::copy(std::string_view{"/proc/pressure/memory"}, candidates[count].data());
    ++count;

    int first_err = 0;
    const char* first_path = "";
    for (unsigned i = 0; i < count; ++i) {
        const char* path = candidates[i].data();
        int err = open_psi_trigger(path, std::span{some.data(), some_len}, moderate_.fd_);
        if (err == 0)
            err = open_psi_trigger(path, std::span{full.data(), full_len}, critical_.fd_);
        if (err == 0) {
            std::ranges::copy(candidates[i], path_.begin());
            break;
        }
        moderate_.fd_.reset();
        critical_.fd_.reset();
        // ENOENT: no PSI (CONFIG_PSI=n) or no such cgroup file; EACCES: not ours to write;
        // EPERM/EINVAL: a kernel that takes no trigger from a user, or not this window;
        // EOPNOTSUPP: booted with psi=0.
        log_psi.debug("{}: {}", path, std::strerror(err));
        if (first_err == 0) {
            first_err = err;
            first_path = path;
        }
    }
    if (moderate_.fd_.get() < 0) {
        log_psi.info("kernel memory pressure unavailable ({}: {}): pool usage only", first_path,
                     std::strerror(first_err));
        return lx::make_error(first_err == ENOENT ? lx::io_err::not_found
                                                  : lx::io_err::permission_denied,
                              "psi_monitor: no pressure file takes a trigger");
    }

    loop_ = &loop;
    memory_ = &memory;
    config_ = config;
    state_ = psi_pressure_state{config};
    moderate_.owner_ = this;
    moderate_.level_ = memory_pressure::moderate;
    critical_.owner_ = this;
    critical_.level_ = memory_pressure::critical;
    decay_.owner_ = this;
    loop.add_source(&moderate_);
    loop.add_source(&critical_);
    loop.add_source(&decay_);
    log_psi.info("kernel memory pressure: {} (some {} ms → moderate, full {} ms → critical, "
                 "per {} s)",
                 path(), config.moderate_some_us / 1000, config.critical_full_us / 1000,
                 config.window_us / 1'000'000);
    return {};
}

void lx::runtime::psi_monitor::stop() {
    if (loop_) {
        loop_->remove_source(&decay_);
        loop_->remove_source(&critical_);
        loop_->remove_source(&moderate_);
    }
    if (memory_ && state_.level() != memory_pressure::normal)
        memory_->on_external_pressure(memory_pressure::normal);
    loop_ = nullptr;
    memory_ = nullptr;
    state_ = psi_pressure_state{config_};
    for (trigger_source* t : {&moderate_, &critical_}) {
        t->fd_.reset();
        t->last_ns_ = 0;
        t->strikes_ = 0;
        t->ready_ = false;
    }
    path_[0] = '\0';
}

void lx::runtime::psi_monitor::on_trigger(trigger_source& source) {
    const memory_pressure prev = state_.level();
    const memory_pressure level = state_.on_trigger(source.level_, lx_runtime_phase_now());
    if (level == prev)
        return;
    log_psi.info("kernel memory pressure {}", pressure_name(level));
    memory_->on_external_pressure(level);
}

void lx::runtime::psi_monitor::on_quiet() {
    // Either trigger's file reads the whole pressure file (from offset 0, whatever the
    // trigger did). A failed read decays to normal: averages that cannot be read keep
    // nothing up.
    std::array<char, 256> text{};
    const long n = ::pread(moderate_.fd_.get(), text.data(), text.size() - 1, 0);
    const psi_averages avg =
        n > 0 ? parse_psi_averages(std::string_view{text.data(), static_cast<std::size_t>(n)})
              : psi_averages{};
    const memory_pressure prev = state_.level();
    const memory_pressure level = state_.on_quiet(lx_runtime_phase_now(), avg);
    if (level == prev)
        return;
    log_psi.info("kernel memory pressure {} (some avg10 {}.{:02} %, full avg10 {}.{:02} %)",
                 pressure_name(level), avg.some_avg10 / 100, avg.some_avg10 % 100,
                 avg.full_avg10 / 100, avg.full_avg10 % 100);
    memory_->on_external_pressure(level);
}

bool lx::runtime::psi_monitor::trigger_source::prepare(clock_time& timeout) {
    timeout = -1; // readiness comes from the loop's epoll
    return ready_;
}

bool lx::runtime::psi_monitor::trigger_source::dispatch() {
    ready_ = false;
    // The kernel reports a trigger at most once per window. One that keeps reporting much
    // sooner is broken — a trigger whose cgroup went away polls as EPOLLERR for good — and
    // would spin the UI thread: give PSI up rather than that.
    const long long now = lx_runtime_phase_now();
    const long long window_ns = static_cast<long long>(owner_->config_.window_us) * 1000;
    strikes_ = last_ns_ != 0 && now - last_ns_ < window_ns / 2 ? strikes_ + 1 : 0;
    last_ns_ = now;
    if (strikes_ >= 4) {
        log_psi.warn("{}: trigger reports faster than its window; kernel memory pressure off",
                     owner_->path());
        owner_->stop();
        return true;
    }
    owner_->on_trigger(*this);
    return true;
}

bool lx::runtime::psi_monitor::decay_source::prepare(clock_time& timeout) {
    const long long deadline = owner_->state_.deadline();
    if (deadline < 0) {
        timeout = -1;
        return false;
    }
    const long long left = deadline - lx_runtime_phase_now();
    timeout = left > 0 ? left : 0;
    return left <= 0;
}

bool lx::runtime::psi_monitor::decay_source::check() {
    const long long deadline = owner_->state_.deadline();
    return deadline >= 0 && lx_runtime_phase_now() >= deadline;
}

bool lx::runtime::psi_monitor::decay_source::dispatch() {
    owner_->on_quiet();
    return true;
}
