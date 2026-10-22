module;

// Display hotplug (ADR 008 Part B): the udev monitor's fd on the UI event loop, the connector
// probe on the worker, and the handoff of each finished probe back to the UI thread.
//
// Event-driven, never polled: the netlink socket is readable only when a drm uevent arrives
// (the kernel's BPF filter drops everything else), so an idle desktop pays nothing. Only
// `change` with HOTPLUG=1 for our card starts a probe. A probe is a forced connector probe
// on the master fd — the kernel re-reads modes and EDID only for the master — which can
// take tens of milliseconds of DDC on real monitors, so it runs on the worker (Mutter
// probes in its KMS thread) into a worker-owned scan. Bursts coalesce: one probe in flight,
// plus one more for whatever arrived meanwhile (its hints merged). While the session is
// paused nothing is probed (not master: the kernel would answer from its cache); the hints
// are kept and the resume probes everything.

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.drm;

export module lx.compositor:hotplug;

import std;

import :output;

export namespace lx::compositor {

/// Where a finished probe goes (the compositor). UI affinity.
struct hotplug_host {
    void* user = nullptr;
    /// A probe finished. `scan` is the worker's buffer, valid for the call only. `forced`:
    /// asked for by `request_full` (a resume), not by a uevent.
    void (*scan_ready)(void* user, const lx::drm::connector_scan& scan, bool forced) = nullptr;
    /// The card itself was removed (a `remove` uevent of its devnum).
    void (*card_removed)(void* user) = nullptr;
};

/// The hotplug controller (ADR 008 H1/H2). UI affinity, except the probe task it posts to
/// the worker strand.
class hotplug_controller {
public:
    hotplug_controller() = default;
    hotplug_controller(const hotplug_controller&) = delete;
    hotplug_controller& operator=(const hotplug_controller&) = delete;

    /// Opens the udev monitor for the card `card` (its dev_t) and waits on it in `loop`.
    /// Probes run on the worker strand when `worker` (else inline, on the UI thread). Fails
    /// without libudev or when udev refuses (logged once by the caller); `request_full`
    /// still works then (the resume probe).
    [[nodiscard]] lx::result<void> start(lx::drm::kms_device& kms, unsigned long long card,
                                         lx::runtime::event_loop& loop, bool worker,
                                         hotplug_host host);
    /// Prepares probing without a monitor (the resume probe still runs).
    void attach(lx::drm::kms_device& kms, lx::runtime::event_loop& loop, bool worker,
                hotplug_host host);
    void stop();
    [[nodiscard]] bool monitoring() const { return source_ != nullptr; }

    /// While paused (VT away) uevents are owed, not probed: only the master's probe is a
    /// forced one. The resume calls `request_full`.
    void set_paused(bool paused) { paused_ = paused; }
    /// Probes every connector now (or as soon as the probe in flight is done). `forced` is
    /// handed to `scan_ready`.
    void request_full(bool forced);
    /// A probe is in flight.
    [[nodiscard]] bool probing() const { return in_flight_; }
    /// Diagnostics.
    [[nodiscard]] unsigned probes() const { return probes_; }
    [[nodiscard]] unsigned batches() const { return batches_; }
    /// Uevents for the card owed to a resume (the session was away when they came).
    [[nodiscard]] bool owed() const { return owed_.hotplug; }

private:
    void on_readable();
    void request(const lx::drm::hotplug_batch& batch, bool forced);
    void launch();
    /// Worker (or inline): the probe itself.
    void run_probe();
    /// UI: the probe finished.
    void finish();

    lx::drm::kms_device* kms_ = nullptr;
    lx::runtime::event_loop* loop_ = nullptr;
    bool worker_ = false;
    hotplug_host host_{};
    lx::drm::uevent_monitor monitor_{};
    std::unique_ptr<lx::runtime::fd_source> source_{};
    /// The worker's scan: written only by the probe in flight, read by UI after it.
    std::unique_ptr<lx::drm::connector_scan> scan_{};
    bool paused_ = false;
    bool in_flight_ = false;
    /// What the probe in flight covers, and what arrived since (the next probe's).
    lx::drm::hotplug_batch current_{};
    bool current_forced_ = false;
    lx::drm::hotplug_batch again_{};
    bool again_forced_ = false;
    /// Owed to the resume.
    lx::drm::hotplug_batch owed_{};
    /// Written by the probe, read by UI after it.
    unsigned hint_ = 0;
    bool probe_ok_ = false;
    unsigned probe_us_ = 0;
    lx::error probe_error_{};
    unsigned probes_ = 0;
    unsigned batches_ = 0;
    bool logged_failure_ = false;
};

} // namespace lx::compositor

namespace {
constexpr const char* k_category = "compositor.hotplug";

[[nodiscard]] bool debug_on() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}
} // namespace

void lx::compositor::hotplug_controller::attach(lx::drm::kms_device& kms,
                                                lx::runtime::event_loop& loop, bool worker,
                                                hotplug_host host) {
    kms_ = &kms;
    loop_ = &loop;
    worker_ = worker;
    host_ = host;
    if (!scan_) {
        // Once, at start (≈150 KB): the base a hinted probe copies the other connectors from.
        scan_ = std::make_unique<lx::drm::connector_scan>(kms.last_scan());
    }
}

lx::result<void> lx::compositor::hotplug_controller::start(lx::drm::kms_device& kms,
                                                           unsigned long long card,
                                                           lx::runtime::event_loop& loop,
                                                           bool worker, hotplug_host host) {
    attach(kms, loop, worker, host);
    auto opened = lx::drm::uevent_monitor::open(card);
    if (!opened)
        return opened.get_error();
    monitor_ = std::move(opened).value();
    hotplug_controller* self = this;
    source_ = std::make_unique<lx::runtime::fd_source>(
        monitor_.fd(), lx::runtime::callback{[self] { self->on_readable(); }});
    loop.add_source(source_.get());
    return {};
}

void lx::compositor::hotplug_controller::stop() {
    if (source_ && loop_)
        loop_->remove_source(source_.get());
    source_.reset();
    monitor_ = lx::drm::uevent_monitor{};
}

void lx::compositor::hotplug_controller::on_readable() {
    const lx::drm::hotplug_batch b = monitor_.drain();
    if (b.card_removed) {
        if (host_.card_removed)
            host_.card_removed(host_.user);
        return;
    }
    if (!b.hotplug) {
        // `udevadm trigger`, a render node, another card: nothing to probe.
        return;
    }
    ++batches_;
    if (debug_on()) {
        lx::trace::logger::global().logf(
            lx::trace::level::debug, k_category, "hotplug: uevent connectors={}{}",
            b.all_connectors ? "all" : "hinted", paused_ ? " (session away: owed)" : "");
    }
    if (paused_) {
        lx::drm::merge_hotplug(owed_, b);
        return;
    }
    request(b, false);
}

void lx::compositor::hotplug_controller::request_full(bool forced) {
    lx::drm::hotplug_batch b{};
    b.hotplug = true;
    b.all_connectors = true;
    owed_ = {};
    request(b, forced);
}

void lx::compositor::hotplug_controller::request(const lx::drm::hotplug_batch& b, bool forced) {
    if (!kms_)
        return;
    if (in_flight_) {
        lx::drm::merge_hotplug(again_, b);
        again_forced_ = again_forced_ || forced;
        return;
    }
    current_ = b;
    current_forced_ = forced;
    launch();
}

void lx::compositor::hotplug_controller::launch() {
    in_flight_ = true;
    ++probes_;
    // One hinted connector is probed alone; several, or none named, means all of them.
    hint_ = !current_.all_connectors && current_.connector_count == 1 ? current_.connectors[0] : 0;
    hotplug_controller* self = this;
    if (worker_ && lx::runtime::executor::global().post(lx::runtime::affinity::worker,
                                                        [self] { self->run_probe(); }))
        return;
    run_probe();
}

void lx::compositor::hotplug_controller::run_probe() {
    const auto t0 = std::chrono::steady_clock::now();
    // Records nothing in the device: other threads read its `last_scan`.
    auto r = kms_->probe_connectors(hint_, *scan_, false);
    probe_ok_ = static_cast<bool>(r);
    if (!r)
        probe_error_ = r.get_error();
    probe_us_ = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
    hotplug_controller* self = this;
    // The worker is the UI loop's one other-thread producer (`event_loop::post`).
    if (lx::runtime::current_affinity() == lx::runtime::affinity::worker && loop_ &&
        loop_->post([self] { self->finish(); }))
        return;
    finish();
}

void lx::compositor::hotplug_controller::finish() {
    in_flight_ = false;
    if (!probe_ok_) {
        if (!logged_failure_) {
            logged_failure_ = true;
            lx::trace::logger::global().log_error(probe_error_, k_category);
        }
    } else {
        if (debug_on()) {
            lx::trace::logger::global().logf(lx::trace::level::debug, k_category,
                                             "hotplug: probe connectors={} hint={} took={} us",
                                             scan_->count, hint_, probe_us_);
        }
        if (host_.scan_ready)
            host_.scan_ready(host_.user, *scan_, current_forced_);
    }
    if (again_.hotplug) {
        current_ = again_;
        current_forced_ = again_forced_;
        again_ = {};
        again_forced_ = false;
        launch();
    }
}
