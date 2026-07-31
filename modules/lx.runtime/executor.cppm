module;

#include <atomic>
#include <cstdlib>
#include <thread>

import lx.foundation;
import lx.sync;
import lx.trace;

export module lx.runtime:executor;

export namespace lx::runtime {

using callback = void (*)();

/// Logical thread roles in a Lumen process.
enum class affinity : unsigned {
    ui = 0,       ///< widgets, scene mutation, input routing
    render = 1,   ///< draw list consume, Vulkan record/submit
    worker = 2,   ///< layout assist, I/O, decode, text shaping cache
    wayland = 3,  ///< compositor protocol dispatch (may merge with ui on embedded)
    any = 255,
};

[[nodiscard]] affinity current_affinity();
void set_current_affinity(affinity a);
[[nodiscard]] const char* affinity_name(affinity a);

/// Serializes callbacks onto one logical thread (typically UI).
class strand {
public:
    strand();
    explicit strand(affinity bound);

    void post(callback fn);
    [[nodiscard]] bool try_post(callback fn);
    void drain();

    [[nodiscard]] affinity bound_affinity() const;
    [[nodiscard]] unsigned pending() const;

private:
    affinity bound_ = affinity::ui;
    lx::sync::spsc_queue<callback, 4096> queue_{};
    std::atomic<unsigned> pending_{0};
};

/// Routes work to the strand registered for each affinity.
class executor {
public:
    static executor& global();

    void register_strand(affinity a, strand* strand);
    void unregister_strand(affinity a);

    /// Thread-safe: may be called from any thread.
    [[nodiscard]] bool post(affinity a, callback fn);
    void drain(affinity a);

    [[nodiscard]] strand& ui_strand();
    [[nodiscard]] strand& render_strand();
    [[nodiscard]] strand& worker_strand();

private:
    executor();
    strand ui_{affinity::ui};
    strand render_{affinity::render};
    strand worker_{affinity::worker};
    strand* strands_[4]{};
};

/// Assert current thread matches expected affinity (debug builds).
void assert_affinity(affinity expected);

} // namespace lx::runtime


thread_local lx::runtime::affinity lx_runtime_current_affinity =
    lx::runtime::affinity::any;

lx::runtime::affinity lx::runtime::current_affinity() {
    return lx_runtime_current_affinity;
}

void lx::runtime::set_current_affinity(affinity a) {
    lx_runtime_current_affinity = a;
}

const char* lx::runtime::affinity_name(affinity a) {
    switch (a) {
    case affinity::ui: return "ui";
    case affinity::render: return "render";
    case affinity::worker: return "worker";
    case affinity::wayland: return "wayland";
    default: return "any";
    }
}

lx::runtime::strand::strand() = default;
lx::runtime::strand::strand(affinity bound) : bound_{bound} {}

void lx::runtime::strand::post(callback fn) {
    (void)try_post(fn);
}

bool lx::runtime::strand::try_post(callback fn) {
    if (!fn) return false;
    if (!queue_.try_push(fn)) return false;
    pending_.store(pending_.load() + 1, std::memory_order_release);
    return true;
}

void lx::runtime::strand::drain() {
    lx_runtime_current_affinity = bound_;
    callback fn{};
    while (queue_.try_pop(fn)) {
        if (fn) fn();
        pending_.store(pending_.load() > 0 ? pending_.load() - 1 : 0,
                       std::memory_order_release);
    }
}

lx::runtime::affinity lx::runtime::strand::bound_affinity() const { return bound_; }
unsigned lx::runtime::strand::pending() const { return pending_.load(); }

lx::runtime::executor::executor() {
    strands_[static_cast<unsigned>(affinity::ui)] = &ui_;
    strands_[static_cast<unsigned>(affinity::render)] = &render_;
    strands_[static_cast<unsigned>(affinity::worker)] = &worker_;
}

lx::runtime::executor& lx::runtime::executor::global() {
    static executor instance;
    return instance;
}

void lx::runtime::executor::register_strand(affinity a, strand* s) {
    if (static_cast<unsigned>(a) < 4)
        strands_[static_cast<unsigned>(a)] = s;
}

void lx::runtime::executor::unregister_strand(affinity a) {
    if (static_cast<unsigned>(a) < 4)
        strands_[static_cast<unsigned>(a)] = nullptr;
}

bool lx::runtime::executor::post(affinity a, callback fn) {
    if (static_cast<unsigned>(a) < 4 && strands_[static_cast<unsigned>(a)])
        return strands_[static_cast<unsigned>(a)]->try_post(fn);
    if (a == affinity::ui) return ui_.try_post(fn);
    if (a == affinity::render) return render_.try_post(fn);
    if (a == affinity::worker) return worker_.try_post(fn);
    return false;
}

void lx::runtime::executor::drain(affinity a) {
    if (static_cast<unsigned>(a) < 4 && strands_[static_cast<unsigned>(a)])
        strands_[static_cast<unsigned>(a)]->drain();
}

lx::runtime::strand& lx::runtime::executor::ui_strand() { return ui_; }
lx::runtime::strand& lx::runtime::executor::render_strand() { return render_; }
lx::runtime::strand& lx::runtime::executor::worker_strand() { return worker_; }

void lx::runtime::assert_affinity(affinity expected) {
#if !defined(NDEBUG)
    if (current_affinity() != expected && expected != affinity::any) {
        // Avoid heap/IO on the hot path beyond a single stderr write.
        const char* want = affinity_name(expected);
        const char* have = affinity_name(current_affinity());
        lx::trace::logger::global().log(
            lx::trace::level::error, "runtime",
            "affinity violation");
        (void)want;
        (void)have;
        std::abort();
    }
#else
    (void)expected;
#endif
}
