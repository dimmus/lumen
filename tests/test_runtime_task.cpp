#include "lumen_test.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#include <unistd.h>

import lx.runtime;

namespace {

int g_plain_calls = 0;
void plain_fn() { ++g_plain_calls; }

struct subject {
    int value = 0;
    void bump() { ++value; }
};

void bump_subject(void* ctx) { static_cast<subject*>(ctx)->bump(); }

} // namespace

LUMEN_TEST(task_default_is_empty_and_safe_to_invoke) {
    lx::runtime::task empty{};
    LUMEN_CHECK(!static_cast<bool>(empty));
    empty(); // must be a no-op, not a null call
}

LUMEN_TEST(task_carries_a_context_pointer) {
    subject s{};
    lx::runtime::task t{&bump_subject, &s};
    LUMEN_CHECK(static_cast<bool>(t));
    t();
    t();
    LUMEN_CHECK(s.value == 2);
}

LUMEN_TEST(task_stores_captured_state_inline) {
    subject s{};
    int captured = 7;
    // The whole point of the fix: a callable that knows *what* to work on.
    lx::runtime::task t{[&s, captured] { s.value += captured; }};
    t();
    LUMEN_CHECK(s.value == 7);

    // Copying a task copies its payload — it is a queue element, not a reference.
    lx::runtime::task copy = t;
    copy();
    LUMEN_CHECK(s.value == 14);
}

LUMEN_TEST(task_accepts_a_plain_function_pointer) {
    g_plain_calls = 0;
    lx::runtime::task t{&plain_fn};
    t();
    LUMEN_CHECK(g_plain_calls == 1);
}

LUMEN_TEST(strand_runs_posted_work_with_its_context) {
    lx::runtime::strand s{lx::runtime::affinity::worker};
    subject subj{};

    LUMEN_CHECK(s.try_post(lx::runtime::task{[&subj] { subj.bump(); }}));
    LUMEN_CHECK(s.try_post(lx::runtime::task{&bump_subject, &subj}));
    LUMEN_CHECK(s.pending() == 2);

    s.drain();
    LUMEN_CHECK(subj.value == 2);
    LUMEN_CHECK(s.pending() == 0);
}

LUMEN_TEST(strand_rejects_empty_task) {
    lx::runtime::strand s{lx::runtime::affinity::worker};
    LUMEN_CHECK(!s.try_post(lx::runtime::task{}));
    LUMEN_CHECK(s.pending() == 0);
}

// drain() used to set the thread's affinity and leave it set, so a thread that drained a
// strand it does not own stayed mislabeled — and assert_affinity aborts on a mismatch.
LUMEN_TEST(strand_drain_restores_caller_affinity) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);

    lx::runtime::strand worker{lx::runtime::affinity::worker};
    lx::runtime::affinity seen_inside = lx::runtime::affinity::any;
    LUMEN_CHECK(worker.try_post(
        lx::runtime::task{[&seen_inside] { seen_inside = lx::runtime::current_affinity(); }}));

    worker.drain();

    LUMEN_CHECK(seen_inside == lx::runtime::affinity::worker);
    LUMEN_CHECK(lx::runtime::current_affinity() == lx::runtime::affinity::ui);
}

// pending_ was a load-then-store pair touched by both producer and consumer, so counts
// were lost under concurrency. Producer and consumer must agree at quiescence.
LUMEN_TEST(strand_pending_count_survives_concurrent_producer) {
    constexpr unsigned k_posts = 20000;

    lx::runtime::strand s{lx::runtime::affinity::worker};
    std::atomic<unsigned> accepted{0};
    std::atomic<unsigned> executed{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        for (unsigned i = 0; i < k_posts;) {
            if (s.try_post(lx::runtime::task{[&executed] {
                    executed.fetch_add(1, std::memory_order_relaxed);
                }})) {
                accepted.fetch_add(1, std::memory_order_relaxed);
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    lx::runtime::set_current_affinity(lx::runtime::affinity::worker);
    while (!producer_done.load(std::memory_order_acquire) || s.pending() > 0)
        s.drain();
    producer.join();
    s.drain();

    LUMEN_CHECK(accepted.load() == k_posts);
    LUMEN_CHECK(executed.load() == k_posts);
    LUMEN_CHECK(s.pending() == 0);
}

LUMEN_TEST(executor_routes_by_affinity) {
    auto& ex = lx::runtime::executor::global();
    subject ui_subject{};
    subject worker_subject{};

    LUMEN_CHECK(ex.post(lx::runtime::affinity::ui,
                        lx::runtime::task{[&ui_subject] { ui_subject.bump(); }}));
    LUMEN_CHECK(ex.post(lx::runtime::affinity::worker,
                        lx::runtime::task{&bump_subject, &worker_subject}));

    ex.drain(lx::runtime::affinity::ui);
    LUMEN_CHECK(ui_subject.value == 1);
    LUMEN_CHECK(worker_subject.value == 0); // still queued on the worker strand

    ex.drain(lx::runtime::affinity::worker);
    LUMEN_CHECK(worker_subject.value == 1);
}

// ── event_loop: waits on its sources instead of polling them ──────────────────────────

namespace {
long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

// Re-arming at `now + interval` after the callback let each tick's lateness and duration
// push every later one back: a 60 Hz clock ran at 57.
LUMEN_TEST(timer_repeats_from_its_deadline_not_from_its_dispatch) {
    lx::runtime::event_loop loop;
    int fired = 0;
    const long long start = now_ms();
    lx::runtime::timer_source timer{10'000'000, lx::runtime::task{[&] {
                                        ++fired;
                                        std::this_thread::sleep_for(std::chrono::milliseconds(3));
                                        if (fired == 21)
                                            loop.quit(0);
                                    }},
                                    true};
    loop.add_source(&timer);
    (void)loop.run();
    loop.remove_source(&timer);
    const long long elapsed = now_ms() - start;
    // 20 periods after the first firing: 200 ms on schedule, 260+ ms if each drifted.
    LUMEN_CHECK(elapsed >= 195);
    LUMEN_CHECK(elapsed < 240);
}

LUMEN_TEST(loop_sleeps_until_another_thread_posts) {
    lx::runtime::event_loop loop;
    std::thread poster{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        (void)loop.post(lx::runtime::task{[&] { loop.quit(7); }});
    }};
    const int code = loop.run();
    poster.join();
    LUMEN_CHECK(code == 7);
    // Woken by the post, not on a period: a polling loop would have woken several times.
    LUMEN_CHECK(loop.wakeups() <= 2);
}

LUMEN_TEST(loop_wakes_for_work_posted_to_the_ui_strand) {
    lx::runtime::event_loop loop;
    std::thread poster{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        (void)lx::runtime::executor::global().post(lx::runtime::affinity::ui,
                                                   lx::runtime::task{[&] { loop.quit(3); }});
    }};
    const int code = loop.run();
    poster.join();
    LUMEN_CHECK(code == 3);
    LUMEN_CHECK(loop.wakeups() <= 2);
}

LUMEN_TEST(fd_source_dispatches_when_readable_and_only_then) {
    int fds[2];
    LUMEN_CHECK(::pipe(fds) == 0);
    lx::runtime::event_loop loop;
    int dispatched = 0;
    lx::runtime::fd_source source{fds[0], lx::runtime::task{[&] {
                                      ++dispatched;
                                      char c = 0;
                                      (void)!::read(fds[0], &c, 1);
                                      loop.quit(0);
                                  }}};
    loop.add_source(&source);
    std::thread writer{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        (void)!::write(fds[1], "x", 1);
    }};
    (void)loop.run();
    writer.join();
    loop.remove_source(&source);
    ::close(fds[0]);
    ::close(fds[1]);
    LUMEN_CHECK(dispatched == 1);
    LUMEN_CHECK(loop.wakeups() <= 2);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
