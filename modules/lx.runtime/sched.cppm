module;

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

// The flags glibc's <sched.h> names since 2.41; the kernel took them earlier.
#ifndef SCHED_FLAG_RESET_ON_FORK
#define SCHED_FLAG_RESET_ON_FORK 0x01 // Linux 3.14
#endif
#ifndef SCHED_FLAG_KEEP_POLICY
#define SCHED_FLAG_KEEP_POLICY 0x08 // Linux 5.3
#endif
#ifndef SCHED_FLAG_KEEP_PARAMS
#define SCHED_FLAG_KEEP_PARAMS 0x10 // Linux 5.3
#endif
#ifndef SCHED_FLAG_UTIL_CLAMP_MIN
#define SCHED_FLAG_UTIL_CLAMP_MIN 0x20 // Linux 5.3
#endif
#ifndef SCHED_RESET_ON_FORK
#define SCHED_RESET_ON_FORK 0x40000000
#endif

export module lx.runtime:sched;

import std;

// CPU scheduling for latency-critical threads (threading.md, "Thread scheduling"). A thread
// asks for one of three tiers, best first, and keeps the first the kernel grants:
//   realtime       SCHED_RR at a low priority — with the process's own privilege
//                  (CAP_SYS_NICE, RLIMIT_RTPRIO), else through a broker (rtkit)
//   high_priority  a negative nice — the same two ways
//   latency        what any process may set on itself: a shorter EEVDF slice
//                  (sched_runtime, Linux 6.12) and a uclamp.min floor (Linux 5.3)
// The decision is `apply_thread_sched`, pure over injected calls; `kernel_sched_ops` are the
// real ones. The broker's D-Bus side lives in lx.session.

export namespace lx::runtime {

/// What a thread runs, which decides how far up it may go.
enum class thread_role : unsigned char {
    /// Bounded work per frame (composite, present): real-time when allowed.
    render,
    /// Work whose length nothing bounds (client requests, I/O, config): never real-time —
    /// a flood would become a thread that starves the machine.
    dispatch,
};

enum class sched_tier : unsigned char { none, latency, high_priority, realtime };
enum class sched_via : unsigned char { none, self, broker };

struct sched_request {
    thread_role role = thread_role::dispatch;
    /// The lowest real-time priority: above every SCHED_OTHER task, which is the point,
    /// and below everything that chose a priority (audio at rtkit's 20, IRQ threads at 50).
    int rt_priority = 1;
    int nice = -10;
    /// EEVDF slice: a shorter one is an earlier virtual deadline, so a woken thread runs
    /// sooner (and may preempt, 6.12+). The kernel default is 0.75 ms × (1 + log2 CPUs).
    unsigned slice_us = 500;
    /// uclamp.min (of 1024), render only: schedutil sees at least this utilisation while
    /// the thread runs, so frame work starts at a mid clock instead of waiting for PELT to
    /// ramp up from idle; it does not pin the maximum.
    unsigned util_min = 512;
    /// RLIMIT_RTTIME hard limit set before real-time is obtained (rtkit's default maximum);
    /// the soft limit, half of it, drops every real-time thread back to SCHED_OTHER.
    unsigned long long rttime_us = 200'000;
};

struct sched_outcome {
    sched_tier tier = sched_tier::none;
    sched_via via = sched_via::none;
    /// realtime: the RT priority; high_priority: the nice level; else 0.
    int priority = 0;
    /// The slice the kernel reports back, 0 = its default (before 6.12 it ignores it).
    unsigned slice_us = 0;
    /// The uclamp.min the kernel reports back.
    unsigned util_min = 0;
    /// realtime: RLIMIT_RTTIME's hard limit.
    unsigned long long rttime_us = 0;
    /// errno of each refusal on the way down (0: granted or not asked).
    int realtime_error = 0;
    int nice_error = 0;
    int broker_error = 0;
    int hints_error = 0;
};

/// The kernel calls; each returns 0 or an errno. `tid` is a thread of this process.
struct sched_ops {
    void* ctx = nullptr;
    /// SCHED_RESET_ON_FORK: children and new threads start at SCHED_OTHER, nice 0, default
    /// slice and clamps. Must succeed before anything above `latency` is asked for.
    int (*reset_on_fork)(void* ctx, int tid) = nullptr;
    /// RLIMIT_RTTIME at most `hard_us` (soft = half) and the SIGXCPU guard installed.
    int (*limit_rttime)(void* ctx, unsigned long long hard_us) = nullptr;
    int (*set_realtime)(void* ctx, int tid, int priority) = nullptr;
    /// A thread now real-time: the SIGXCPU guard demotes it.
    void (*guard_realtime)(void* ctx, int tid) = nullptr;
    int (*set_nice)(void* ctx, int tid, int nice) = nullptr;
    /// Slice and uclamp.min (0: leave the clamp alone), keeping policy and nice; writes back
    /// what the kernel then reports.
    int (*set_fair_hints)(void* ctx, int tid, unsigned& slice_us, unsigned& util_min) = nullptr;
};

struct sched_broker_limits {
    int max_rt_priority = 0;
    int min_nice = 0;
    unsigned long long rttime_max_us = 0;
};

/// A service that raises threads for processes that may not themselves (rtkit). All null:
/// none. Each call returns 0 or an errno.
struct sched_broker {
    void* ctx = nullptr;
    int (*limits)(void* ctx, sched_broker_limits& out) = nullptr;
    int (*make_realtime)(void* ctx, int tid, int priority) = nullptr;
    int (*make_high_priority)(void* ctx, int tid, int nice) = nullptr;
};

/// Raises `tid` as far as `req.role` allows and the kernel or the broker grants, best tier
/// first. A thread that gets real-time gets no fair-class hints (a uclamp.min would lower
/// the full clock RT tasks get by default).
[[nodiscard]] sched_outcome apply_thread_sched(const sched_request& req, int tid,
                                               const sched_ops& ops, const sched_broker& broker);

/// sched_setattr(2), setpriority(2), setrlimit(2) and a SIGXCPU handler.
[[nodiscard]] sched_ops kernel_sched_ops();
[[nodiscard]] int current_tid();

[[nodiscard]] constexpr const char* sched_tier_name(sched_tier t) {
    switch (t) {
    case sched_tier::realtime: return "real-time";
    case sched_tier::high_priority: return "high priority";
    case sched_tier::latency: return "latency hints";
    case sched_tier::none: break;
    }
    return "default";
}

} // namespace lx::runtime


lx::runtime::sched_outcome lx::runtime::apply_thread_sched(const sched_request& req, int tid,
                                                           const sched_ops& ops,
                                                           const sched_broker& broker) {
    sched_outcome out{};
    // Children and threads created later must not inherit any of it.
    const bool may_raise = ops.reset_on_fork && ops.reset_on_fork(ops.ctx, tid) == 0;

    sched_broker_limits limits{};
    int broker_state = -1; // -1 not asked, 0 there, else its errno
    auto broker_up = [&]() {
        if (broker_state < 0)
            broker_state = !broker.limits ? ENOENT : broker.limits(broker.ctx, limits);
        if (broker_state != 0)
            out.broker_error = broker_state;
        return broker_state == 0;
    };

    if (may_raise && req.role == thread_role::render) {
        // RLIMIT_RTTIME first, always: real-time is never held without it (rtkit refuses
        // one that is missing or above its maximum).
        int err = ops.limit_rttime(ops.ctx, req.rttime_us);
        if (err == 0) {
            err = ops.set_realtime(ops.ctx, tid, req.rt_priority);
            if (err == 0) {
                out.tier = sched_tier::realtime;
                out.via = sched_via::self;
                out.priority = req.rt_priority;
                out.rttime_us = req.rttime_us;
            }
        }
        out.realtime_error = err;
        if (out.tier == sched_tier::none && broker_up() && broker.make_realtime &&
            limits.max_rt_priority > 0 && limits.rttime_max_us > 0) {
            const int prio = std::min(req.rt_priority, limits.max_rt_priority);
            const unsigned long long rttime = std::min(req.rttime_us, limits.rttime_max_us);
            int berr = ops.limit_rttime(ops.ctx, rttime);
            if (berr == 0)
                berr = broker.make_realtime(broker.ctx, tid, prio);
            if (berr == 0) {
                out.tier = sched_tier::realtime;
                out.via = sched_via::broker;
                out.priority = prio;
                out.rttime_us = rttime;
            } else {
                out.broker_error = berr;
            }
        }
        if (out.tier == sched_tier::realtime) {
            ops.guard_realtime(ops.ctx, tid);
            return out;
        }
    }

    if (may_raise && req.nice < 0) {
        int err = ops.set_nice(ops.ctx, tid, req.nice);
        if (err == 0) {
            out.tier = sched_tier::high_priority;
            out.via = sched_via::self;
            out.priority = req.nice;
        } else {
            out.nice_error = err;
            if (broker_up() && broker.make_high_priority) {
                const int nice = std::max(req.nice, limits.min_nice);
                if (nice < 0) {
                    const int berr = broker.make_high_priority(broker.ctx, tid, nice);
                    if (berr == 0) {
                        out.tier = sched_tier::high_priority;
                        out.via = sched_via::broker;
                        out.priority = nice;
                    } else {
                        out.broker_error = berr;
                    }
                }
            }
        }
    }

    unsigned slice = req.slice_us;
    unsigned util = req.role == thread_role::render ? req.util_min : 0;
    if (ops.set_fair_hints && (slice || util)) {
        out.hints_error = ops.set_fair_hints(ops.ctx, tid, slice, util);
        if (out.hints_error == 0) {
            out.slice_us = slice;
            out.util_min = util;
            if (out.tier == sched_tier::none && (slice || util))
                out.tier = sched_tier::latency;
        }
    }
    return out;
}

namespace {

// struct sched_attr, SCHED_ATTR_SIZE_VER1 (the util clamps, 5.3). Our own name: glibc 2.41+
// declares the same struct, older ones do not.
struct kernel_sched_attr {
    std::uint32_t size;
    std::uint32_t sched_policy;
    std::uint64_t sched_flags;
    std::int32_t sched_nice;
    std::uint32_t sched_priority;
    std::uint64_t sched_runtime;
    std::uint64_t sched_deadline;
    std::uint64_t sched_period;
    std::uint32_t sched_util_min;
    std::uint32_t sched_util_max;
};
static_assert(sizeof(kernel_sched_attr) == 56);

int sched_set(int tid, kernel_sched_attr& a) {
    a.size = sizeof(a);
    return ::syscall(SYS_sched_setattr, tid, &a, 0u) == 0 ? 0 : errno;
}

int sched_get(int tid, kernel_sched_attr& a) {
    a = {};
    return ::syscall(SYS_sched_getattr, tid, &a, static_cast<unsigned>(sizeof(a)), 0u) == 0
               ? 0
               : errno;
}

// Real-time threads the SIGXCPU guard demotes. Written before RT is held, read in the
// handler: lock-free atomics only.
constexpr unsigned k_guarded = 4;
std::atomic<int> g_guarded[k_guarded]{};

void on_sigxcpu(int) {
    // RLIMIT_RTTIME's soft limit: a real-time thread ran that long without blocking. The
    // signal goes to the process, not the thread, so every guarded thread drops to
    // SCHED_OTHER — before the hard limit's SIGKILL. Async-signal-safe: syscalls only.
    const int saved = errno;
    bool any = false;
    for (auto& slot : g_guarded) {
        const int tid = slot.exchange(0, std::memory_order_relaxed);
        if (tid == 0)
            continue;
        kernel_sched_attr a{};
        a.size = sizeof(a);
        a.sched_policy = SCHED_OTHER;
        a.sched_flags = SCHED_FLAG_RESET_ON_FORK; // clearing it would need CAP_SYS_NICE
        (void)::syscall(SYS_sched_setattr, tid, &a, 0u);
        any = true;
    }
    if (any) {
        static constexpr char msg[] = "lumen: a real-time thread ran past RLIMIT_RTTIME's soft "
                                      "limit without blocking; real-time scheduling dropped\n";
        (void)!::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    errno = saved;
}

int k_reset_on_fork(void*, int tid) {
    kernel_sched_attr a{};
    a.sched_flags = SCHED_FLAG_RESET_ON_FORK | SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS;
    const int err = sched_set(tid, a);
    if (err != EINVAL)
        return err;
    // Before 5.3 (no KEEP_*): sched_setscheduler with the policy it has.
    const int policy = ::sched_getscheduler(tid);
    sched_param p{};
    if (policy < 0 || ::sched_getparam(tid, &p) != 0)
        return errno;
    return ::sched_setscheduler(tid, policy | SCHED_RESET_ON_FORK, &p) == 0 ? 0 : errno;
}

int k_limit_rttime(void*, unsigned long long hard_us) {
    rlimit rl{};
    if (::getrlimit(RLIMIT_RTTIME, &rl) != 0)
        return errno;
    // Never raise a limit someone set lower.
    const rlim_t hard = rl.rlim_max == RLIM_INFINITY ? static_cast<rlim_t>(hard_us)
                                                     : std::min(rl.rlim_max,
                                                                static_cast<rlim_t>(hard_us));
    rl.rlim_max = hard;
    rl.rlim_cur = std::min(rl.rlim_cur, hard / 2);
    if (rl.rlim_cur == 0)
        return EINVAL; // no room for the soft limit's warning before the SIGKILL
    if (::setrlimit(RLIMIT_RTTIME, &rl) != 0)
        return errno;
    static std::atomic<bool> installed{false};
    if (!installed.exchange(true)) {
        struct sigaction sa{};
        sa.sa_handler = &on_sigxcpu;
        sa.sa_flags = SA_RESTART;
        ::sigemptyset(&sa.sa_mask);
        if (::sigaction(SIGXCPU, &sa, nullptr) != 0) {
            installed = false;
            return errno;
        }
    }
    return 0;
}

int k_set_realtime(void*, int tid, int priority) {
    kernel_sched_attr a{};
    a.sched_policy = SCHED_RR;
    a.sched_priority = static_cast<std::uint32_t>(priority);
    a.sched_flags = SCHED_FLAG_RESET_ON_FORK;
    return sched_set(tid, a);
}

void k_guard_realtime(void*, int tid) {
    for (auto& slot : g_guarded) {
        int empty = 0;
        if (slot.load(std::memory_order_relaxed) == tid ||
            slot.compare_exchange_strong(empty, tid, std::memory_order_relaxed))
            return;
    }
}

int k_set_nice(void*, int tid, int nice) {
    return ::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0 ? 0 : errno;
}

int k_set_fair_hints(void*, int tid, unsigned& slice_us, unsigned& util_min) {
    kernel_sched_attr a{};
    if (const int err = sched_get(tid, a); err != 0)
        return err;
    if (a.sched_policy != SCHED_OTHER && a.sched_policy != SCHED_BATCH)
        return EINVAL;
    // Policy and nice as they are (sched_setattr sets both); deadline fields must be 0.
    a.sched_flags = SCHED_FLAG_RESET_ON_FORK;
    a.sched_runtime = static_cast<std::uint64_t>(slice_us) * 1000u;
    a.sched_deadline = 0;
    a.sched_period = 0;
    if (util_min) {
        a.sched_flags |= SCHED_FLAG_UTIL_CLAMP_MIN;
        a.sched_util_min = util_min;
    }
    int err = sched_set(tid, a);
    if (err != 0 && util_min) {
        // EOPNOTSUPP without CONFIG_UCLAMP_TASK, EINVAL/E2BIG before 5.3; the slice may
        // still be taken.
        a.sched_flags = SCHED_FLAG_RESET_ON_FORK;
        a.sched_util_min = 0;
        util_min = 0;
        err = sched_set(tid, a);
    }
    if (err != 0)
        return err;
    kernel_sched_attr back{};
    if (sched_get(tid, back) != 0)
        return 0;
    // Before 6.12 sched_runtime is ignored for SCHED_OTHER and reads back 0.
    if (back.sched_runtime != static_cast<std::uint64_t>(slice_us) * 1000u)
        slice_us = 0;
    util_min = util_min ? back.sched_util_min : 0;
    return 0;
}

} // namespace

lx::runtime::sched_ops lx::runtime::kernel_sched_ops() {
    sched_ops ops{};
    ops.reset_on_fork = &k_reset_on_fork;
    ops.limit_rttime = &k_limit_rttime;
    ops.set_realtime = &k_set_realtime;
    ops.guard_realtime = &k_guard_realtime;
    ops.set_nice = &k_set_nice;
    ops.set_fair_hints = &k_set_fair_hints;
    return ops;
}

int lx::runtime::current_tid() { return static_cast<int>(::gettid()); }
