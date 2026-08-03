module;

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

import lx.foundation;
import lx.sync;
import lx.trace;

export module lx.runtime:executor;

export namespace lx::runtime {

/// Allocation-free type-erased callable — the unit of cross-thread work.
///
/// A bare `void (*)()` cannot say *what* to work on, so anything posted to a worker had to
/// smuggle its subject through a global. `task` stores the callable's state inline: any
/// trivially-copyable callable up to `k_context_bytes` (a captured `this` plus a few words,
/// or a function pointer plus an opaque context) is copied into the task by value.
///
/// The inline payload keeps `task` trivially copyable, so it remains a valid `spsc_queue`
/// element and posting never touches the heap. Callables that do not fit — anything owning
/// a `std::function`, a container, or a non-trivial destructor — are rejected at compile
/// time rather than silently heap-allocating on the hot path; pass a pointer to
/// caller-owned state instead.
class task {
public:
    static constexpr unsigned k_context_align = 16;
    static constexpr unsigned k_context_bytes = 48;

    task() = default;

    /// Function plus an opaque context pointer — the C-style callback shape.
    task(void (*fn)(void*), void* context) {
        if (!fn)
            return;
        pair p{fn, context};
        std::memcpy(context_, &p, sizeof(p));
        invoke_ = [](const void* ctx) {
            pair stored{};
            std::memcpy(&stored, ctx, sizeof(stored));
            stored.fn(stored.context);
        };
    }

    /// Any trivially-copyable callable that fits inline — including a plain `void (*)()`
    /// and captureless or small-capture lambdas.
    template<typename F, typename D = std::decay_t<F>>
        requires(!std::is_same_v<D, task> && std::is_invocable_v<const D&> &&
                 std::is_trivially_copyable_v<D> && std::is_trivially_destructible_v<D> &&
                 sizeof(D) <= k_context_bytes && alignof(D) <= k_context_align)
    task(F&& fn) { // NOLINT(google-explicit-constructor) — callable-to-task is the point
        ::new (static_cast<void*>(context_)) D(std::forward<F>(fn));
        invoke_ = [](const void* ctx) { (*static_cast<const D*>(ctx))(); };
    }

    void operator()() const {
        if (invoke_)
            invoke_(context_);
    }

    [[nodiscard]] explicit operator bool() const { return invoke_ != nullptr; }

private:
    struct pair {
        void (*fn)(void*) = nullptr;
        void* context = nullptr;
    };

    using invoker = void (*)(const void* context);

    invoker invoke_ = nullptr;
    alignas(k_context_align) unsigned char context_[k_context_bytes]{};
};

static_assert(std::is_trivially_copyable_v<task>, "task must stay queue-storable");
static_assert(sizeof(task) == 64, "task should occupy one cache line");

/// Historical spelling. `task` is the type; this alias keeps existing signatures readable.
using callback = task;

/// Queue depth per strand. Each slot is one `task` (64 B), so this is the per-strand
/// memory cost: 1024 slots = 64 KiB.
inline constexpr unsigned k_strand_queue_depth = 1024;

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

/// Serializes tasks onto one logical thread (typically UI).
///
/// Backed by an SPSC queue: **one** producer thread per strand and exactly one consumer
/// (the thread that calls `drain()`). Posting to the same strand from two threads is a
/// data race — route through the strand that owns the work instead.
class strand {
public:
    strand();
    explicit strand(affinity bound);

    void post(task fn);
    [[nodiscard]] bool try_post(task fn);
    /// Runs queued tasks with `current_affinity()` set to this strand's affinity for the
    /// duration, then restores the caller's affinity. Restoring matters because a thread
    /// that drains a strand it does not own would otherwise stay mislabeled, and
    /// `assert_affinity` aborts on a mismatch in debug builds.
    void drain();

    [[nodiscard]] affinity bound_affinity() const;
    [[nodiscard]] unsigned pending() const;

private:
    affinity bound_ = affinity::ui;
    lx::sync::spsc_queue<task, k_strand_queue_depth> queue_{};
    std::atomic<unsigned> pending_{0};
};

/// Routes work to the strand registered for each affinity.
class executor {
public:
    static executor& global();

    void register_strand(affinity a, strand* strand);
    void unregister_strand(affinity a);

    /// Posts to the target strand. Safe from a thread other than the consumer, but only
    /// one producer per target strand — see `strand`.
    [[nodiscard]] bool post(affinity a, task fn);
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

void lx::runtime::strand::post(task fn) {
    (void)try_post(fn);
}

bool lx::runtime::strand::try_post(task fn) {
    if (!fn) return false;
    if (!queue_.try_push(fn)) return false;
    // Producer and consumer touch this from different threads: load-then-store would drop
    // counts under any concurrency at all.
    pending_.fetch_add(1, std::memory_order_release);
    return true;
}

void lx::runtime::strand::drain() {
    const affinity previous = lx_runtime_current_affinity;
    lx_runtime_current_affinity = bound_;

    task fn{};
    while (queue_.try_pop(fn)) {
        if (fn) fn();
        pending_.fetch_sub(1, std::memory_order_release);
    }

    lx_runtime_current_affinity = previous;
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

bool lx::runtime::executor::post(affinity a, task fn) {
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
