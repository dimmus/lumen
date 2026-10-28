#include "lumen_test.hpp"

#include <errno.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

import std;
import lx.runtime;

// Thread scheduling tiers: which calls `apply_thread_sched` makes, in which order, and what
// it settles on, against injected kernel and broker answers. The last test runs the real
// calls for the dispatch role (never real-time) on a thread of its own.

using namespace lx::runtime;

namespace {

struct fake {
    int reset = 0;
    int rttime = 0;
    int realtime = EPERM;
    int nice = EPERM;
    int hints = 0;
    unsigned hint_slice_back = ~0u; // ~0: echo the request
    unsigned hint_util_back = ~0u;
    int broker_limits = ENOENT;
    sched_broker_limits limits{20, -15, 200'000};
    int broker_rt = 0;
    int broker_nice = 0;

    std::vector<std::string> calls;
    unsigned long long rttime_asked = 0;
    int guarded = 0;

    sched_ops ops() {
        sched_ops o{};
        o.ctx = this;
        o.reset_on_fork = [](void* c, int) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("reset");
            return f->reset;
        };
        o.limit_rttime = [](void* c, unsigned long long us) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("rttime " + std::to_string(us));
            f->rttime_asked = us;
            return f->rttime;
        };
        o.set_realtime = [](void* c, int, int prio) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("rr " + std::to_string(prio));
            return f->realtime;
        };
        o.guard_realtime = [](void* c, int tid) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("guard");
            f->guarded = tid;
        };
        o.set_nice = [](void* c, int, int n) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("nice " + std::to_string(n));
            return f->nice;
        };
        o.set_fair_hints = [](void* c, int, unsigned& slice, unsigned& util) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("hints " + std::to_string(slice) + " " + std::to_string(util));
            if (f->hint_slice_back != ~0u)
                slice = f->hint_slice_back;
            if (f->hint_util_back != ~0u)
                util = f->hint_util_back;
            return f->hints;
        };
        return o;
    }

    sched_broker broker() {
        sched_broker b{};
        b.ctx = this;
        b.limits = [](void* c, sched_broker_limits& out) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("broker limits");
            out = f->limits;
            return f->broker_limits;
        };
        b.make_realtime = [](void* c, int, int prio) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("broker rr " + std::to_string(prio));
            return f->broker_rt;
        };
        b.make_high_priority = [](void* c, int, int n) {
            auto* f = static_cast<fake*>(c);
            f->calls.push_back("broker nice " + std::to_string(n));
            return f->broker_nice;
        };
        return b;
    }
};

sched_request render() {
    sched_request r{};
    r.role = thread_role::render;
    return r;
}

bool calls_are(const fake& f, std::initializer_list<std::string_view> want) {
    if (f.calls.size() != want.size()) {
        for (const auto& c : f.calls)
            std::printf("      call: %s\n", c.c_str());
        return false;
    }
    return std::ranges::equal(f.calls, want);
}

} // namespace

LUMEN_TEST(render_gets_realtime_with_its_own_privilege) {
    fake f;
    f.realtime = 0;
    const sched_outcome o = apply_thread_sched(render(), 42, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::realtime && o.via == sched_via::self);
    LUMEN_CHECK(o.priority == 1 && o.rttime_us == 200'000);
    // RLIMIT_RTTIME before the policy, the guard after; no fair-class hints, no broker.
    LUMEN_CHECK(calls_are(f, {"reset", "rttime 200000", "rr 1", "guard"}));
    LUMEN_CHECK(f.guarded == 42);
}

LUMEN_TEST(render_asks_the_broker_within_its_limits) {
    fake f;
    f.broker_limits = 0;
    f.limits = {1, -15, 100'000}; // a stricter rtkit than its defaults
    sched_request req = render();
    req.rt_priority = 5;
    const sched_outcome o = apply_thread_sched(req, 7, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::realtime && o.via == sched_via::broker);
    LUMEN_CHECK(o.priority == 1 && o.rttime_us == 100'000);
    LUMEN_CHECK(o.realtime_error == EPERM);
    // The limit is lowered to rtkit's maximum before rtkit is asked.
    LUMEN_CHECK(calls_are(f, {"reset", "rttime 200000", "rr 5", "broker limits", "rttime 100000",
                              "broker rr 1", "guard"}));
}

LUMEN_TEST(no_realtime_without_rttime) {
    fake f;
    f.rttime = EPERM;
    f.realtime = 0;
    f.broker_limits = 0;
    const sched_outcome o = apply_thread_sched(render(), 7, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::high_priority);
    LUMEN_CHECK(std::ranges::find(f.calls, "rr 1") == f.calls.end());
    LUMEN_CHECK(std::ranges::find(f.calls, "broker rr 1") == f.calls.end());
    LUMEN_CHECK(std::ranges::find(f.calls, "guard") == f.calls.end());
    LUMEN_CHECK(o.realtime_error == EPERM);
}

LUMEN_TEST(render_falls_back_to_latency_hints) {
    // This host: no RLIMIT_RTPRIO, no CAP_SYS_NICE, no rtkit.
    fake f;
    const sched_outcome o = apply_thread_sched(render(), 7, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::latency && o.via == sched_via::none);
    LUMEN_CHECK(o.slice_us == 500 && o.util_min == 512);
    LUMEN_CHECK(o.realtime_error == EPERM && o.nice_error == EPERM && o.broker_error == ENOENT);
    // rtkit's absence is found once, not once per tier.
    LUMEN_CHECK(calls_are(f, {"reset", "rttime 200000", "rr 1", "broker limits", "nice -10",
                              "hints 500 512"}));
}

LUMEN_TEST(dispatch_never_goes_realtime) {
    fake f;
    f.realtime = 0;
    f.nice = 0;
    const sched_outcome o = apply_thread_sched(sched_request{}, 1, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::high_priority && o.via == sched_via::self);
    LUMEN_CHECK(o.priority == -10 && o.slice_us == 500 && o.util_min == 0);
    // No uclamp floor for dispatch: not all of its work is frame work.
    LUMEN_CHECK(calls_are(f, {"reset", "nice -10", "hints 500 0"}));
}

LUMEN_TEST(dispatch_nice_through_the_broker_is_clamped) {
    fake f;
    f.broker_limits = 0;
    f.limits.min_nice = -5;
    const sched_outcome o = apply_thread_sched(sched_request{}, 1, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::high_priority && o.via == sched_via::broker);
    LUMEN_CHECK(o.priority == -5);
    LUMEN_CHECK(calls_are(f, {"reset", "nice -10", "broker limits", "broker nice -5", "hints 500 0"}));
}

LUMEN_TEST(reset_on_fork_gates_everything_above_hints) {
    fake f;
    f.reset = EPERM;
    f.realtime = 0;
    f.nice = 0;
    const sched_outcome o = apply_thread_sched(render(), 7, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::latency);
    LUMEN_CHECK(calls_are(f, {"reset", "hints 500 512"}));
}

LUMEN_TEST(hints_report_what_the_kernel_took) {
    // Before 6.12 the slice is ignored and reads back 0; without CONFIG_UCLAMP_TASK the
    // clamp is dropped.
    fake f;
    f.hint_slice_back = 0;
    f.hint_util_back = 0;
    sched_outcome o = apply_thread_sched(render(), 7, f.ops(), f.broker());
    LUMEN_CHECK(o.tier == sched_tier::none && o.slice_us == 0 && o.util_min == 0);

    fake g;
    g.hints = EPERM;
    o = apply_thread_sched(render(), 7, g.ops(), g.broker());
    LUMEN_CHECK(o.tier == sched_tier::none && o.hints_error == EPERM);

    // No broker at all (built without sd-bus).
    fake h;
    o = apply_thread_sched(render(), 7, h.ops(), sched_broker{});
    LUMEN_CHECK(o.tier == sched_tier::latency && o.broker_error == ENOENT);
}

LUMEN_TEST(kernel_ops_on_a_dispatch_thread) {
    // Real syscalls, unprivileged or not: never real-time for dispatch, and whatever the
    // outcome claims for the slice is what sched_getattr reads back.
    sched_outcome o{};
    int policy = -1;
    std::array<std::uint64_t, 7> attr{}; // struct sched_attr, VER1: 56 bytes
    long got = -1;
    std::thread t([&] {
        o = apply_thread_sched(sched_request{}, current_tid(), kernel_sched_ops(),
                               sched_broker{});
        policy = ::sched_getscheduler(0);
        got = ::syscall(SYS_sched_getattr, 0, attr.data(), 56u, 0u);
    });
    t.join();
    LUMEN_CHECK(o.tier != sched_tier::realtime);
    LUMEN_CHECK((policy & ~0x40000000) == SCHED_OTHER);
    LUMEN_CHECK(o.util_min == 0);
    // sched_runtime is the fourth 8-byte field (after size+policy, flags, nice+priority).
    if (got == 0 && o.slice_us != 0)
        LUMEN_CHECK(attr[3] == static_cast<std::uint64_t>(o.slice_us) * 1000u);
    // SCHED_FLAG_RESET_ON_FORK, which the tier's reset step set.
    if (got == 0)
        LUMEN_CHECK((attr[1] & 0x01u) != 0);
    std::printf("      dispatch: %s, slice %u us, nice %d\n", sched_tier_name(o.tier), o.slice_us,
                o.priority);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
