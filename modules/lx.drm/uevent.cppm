module;

// Display hotplug notifications (ADR 008 H1): a libudev monitor on the "udev" netlink
// group, filtered in the kernel (a BPF filter libudev installs) to subsystem "drm", devtype
// "drm_minor". The "udev" group delivers an event after udevd's rules ran, so tags and ACLs
// are current; wlroots (`backend/session/session.c`), KWin (`udev.cpp`), Mutter (GUdev) and
// niri (Smithay's `UdevBackend`) all listen there. libudev is already linked for libinput.
// sd-device's external-loop API (`sd_device_monitor_receive`) is too new to depend on, and
// the raw kernel group would mean parsing uevents ourselves for nothing.
//
// The monitor only reports *that* something changed; what changed is the probe's business
// (`kms_device::probe_connectors`). An idle monitor costs nothing: its socket is readable
// only when a drm uevent arrives.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(LUMEN_HAS_UDEV)
#include <libudev.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

import lx.foundation;

export module lx.drm:uevent;

export namespace lx::drm {

/// What a burst of uevents asks for, folded (`uevent_monitor::drain`).
struct hotplug_batch {
    /// At least one `change` with HOTPLUG=1 for the card.
    bool hotplug = false;
    /// One of them named no connector (CONNECTOR=), or more than `k_max_hints` were named:
    /// every connector has to be probed.
    bool all_connectors = false;
    static constexpr unsigned k_max_hints = 8;
    /// Connector ids named by CONNECTOR= (deduplicated), when not `all_connectors`.
    unsigned connectors[k_max_hints]{};
    unsigned connector_count = 0;
    /// LEASE=1 (a DRM lease changed; nothing of ours today).
    bool lease = false;
    /// The card itself went away (`remove` of its devnum).
    bool card_removed = false;
    /// Uevents read, ours or not (diagnostics).
    unsigned events = 0;

    [[nodiscard]] bool empty() const { return !hotplug && !card_removed && !lease; }
};

/// One uevent, judged against the card's device number (`card`, a dev_t). Pure: what
/// `drain` folds, exported for tests. Only `change` with HOTPLUG=1 of the card is a hotplug
/// (a `change` without it — `udevadm trigger` — is nothing); `remove` of the card is
/// `card_removed`; another device's events are nothing. `connector` and `property` are the
/// CONNECTOR= / PROPERTY= values (null when absent): a hint names one connector, without
/// one every connector is probed.
[[nodiscard]] hotplug_batch parse_uevent(const char* action, unsigned long long devnum,
                                         unsigned long long card, const char* hotplug,
                                         const char* connector, const char* property,
                                         const char* lease);
/// Folds `one` into `into`: flags OR'd, hints deduplicated, past `k_max_hints` (or after
/// a hint-less event) every connector.
void merge_hotplug(hotplug_batch& into, const hotplug_batch& one);

/// The monitor. UI affinity: its fd is a source on the UI event loop.
class uevent_monitor {
public:
    uevent_monitor() = default;
    ~uevent_monitor();
    uevent_monitor(const uevent_monitor&) = delete;
    uevent_monitor& operator=(const uevent_monitor&) = delete;
    uevent_monitor(uevent_monitor&& other) noexcept;
    uevent_monitor& operator=(uevent_monitor&& other) noexcept;

    /// Opens the monitor for the card whose dev_t is `card`. `not_implemented` without
    /// libudev in the build; `drm_err::monitor_failed` when udev refuses (no udevd, no
    /// netlink in a container).
    [[nodiscard]] static lx::result<uevent_monitor> open(unsigned long long card);
    /// The netlink socket (nonblocking), -1 when closed.
    [[nodiscard]] int fd() const;
    /// Receives every queued device (never blocks) and folds them into one batch.
    [[nodiscard]] hotplug_batch drain();
    [[nodiscard]] unsigned long long card() const { return card_; }

private:
    void release();
    void* udev_ = nullptr;
    void* monitor_ = nullptr;
    unsigned long long card_ = 0;
};

} // namespace lx::drm

namespace {

[[nodiscard]] bool is_one(const char* v) { return v && v[0] == '1' && v[1] == '\0'; }

void add_hint(lx::drm::hotplug_batch& b, unsigned id) {
    if (b.all_connectors)
        return;
    for (unsigned i = 0; i < b.connector_count; ++i)
        if (b.connectors[i] == id)
            return;
    if (b.connector_count >= lx::drm::hotplug_batch::k_max_hints) {
        b.all_connectors = true;
        b.connector_count = 0;
        return;
    }
    b.connectors[b.connector_count++] = id;
}

} // namespace

lx::drm::hotplug_batch lx::drm::parse_uevent(const char* action, unsigned long long devnum,
                                             unsigned long long card, const char* hotplug,
                                             const char* connector, const char* property,
                                             const char* lease) {
    hotplug_batch b{};
    b.events = 1;
    if (!action || devnum != card || card == 0)
        return b;
    if (std::strcmp(action, "remove") == 0) {
        b.card_removed = true;
        return b;
    }
    if (std::strcmp(action, "change") != 0)
        return b;
    if (is_one(lease))
        b.lease = true;
    if (!is_one(hotplug))
        return b;
    b.hotplug = true;
    // CONNECTOR= names the connector whose status or property changed (with PROPERTY= the
    // property: link-status, content protection). Without it the driver could not say —
    // vmwgfx's layout update, a GPU reset — and everything is probed.
    char* end = nullptr;
    const unsigned long id = connector && *connector ? std::strtoul(connector, &end, 10) : 0;
    if (id != 0 && end && *end == '\0')
        add_hint(b, static_cast<unsigned>(id));
    else
        b.all_connectors = true;
    (void)property;
    return b;
}

void lx::drm::merge_hotplug(hotplug_batch& into, const hotplug_batch& one) {
    into.events += one.events;
    into.lease = into.lease || one.lease;
    into.card_removed = into.card_removed || one.card_removed;
    if (!one.hotplug)
        return;
    into.hotplug = true;
    if (one.all_connectors) {
        into.all_connectors = true;
        into.connector_count = 0;
        return;
    }
    for (unsigned i = 0; i < one.connector_count; ++i)
        add_hint(into, one.connectors[i]);
}

lx::drm::uevent_monitor::~uevent_monitor() { release(); }

lx::drm::uevent_monitor::uevent_monitor(uevent_monitor&& other) noexcept {
    *this = static_cast<uevent_monitor&&>(other);
}

lx::drm::uevent_monitor& lx::drm::uevent_monitor::operator=(uevent_monitor&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    udev_ = other.udev_;
    monitor_ = other.monitor_;
    card_ = other.card_;
    other.udev_ = nullptr;
    other.monitor_ = nullptr;
    other.card_ = 0;
    return *this;
}

void lx::drm::uevent_monitor::release() {
#if defined(LUMEN_HAS_UDEV)
    if (monitor_)
        udev_monitor_unref(static_cast<udev_monitor*>(monitor_));
    if (udev_)
        udev_unref(static_cast<udev*>(udev_));
#endif
    monitor_ = nullptr;
    udev_ = nullptr;
}

lx::result<lx::drm::uevent_monitor> lx::drm::uevent_monitor::open(unsigned long long card) {
#if defined(LUMEN_HAS_UDEV)
    const auto fail = [](const char* why) {
        return lx::make_error(lx::error_domain::drm, static_cast<int>(lx::drm_err::monitor_failed),
                              why);
    };
    if (card == 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "uevent monitor: no card");
    uevent_monitor m{};
    m.card_ = card;
    m.udev_ = udev_new();
    if (!m.udev_)
        return fail("udev_new failed");
    auto* mon = udev_monitor_new_from_netlink(static_cast<udev*>(m.udev_), "udev");
    if (!mon)
        return fail("udev netlink monitor unavailable");
    m.monitor_ = mon;
    // In-kernel filter: the socket wakes for drm minors only, never for input, block or
    // network devices.
    if (udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", "drm_minor") < 0)
        return fail("udev monitor filter refused");
    if (udev_monitor_enable_receiving(mon) < 0)
        return fail("udev monitor cannot receive");
    if (udev_monitor_get_fd(mon) < 0)
        return fail("udev monitor has no fd");
    return m;
#else
    (void)card;
    return lx::not_implemented("lx::drm::uevent_monitor::open");
#endif
}

int lx::drm::uevent_monitor::fd() const {
#if defined(LUMEN_HAS_UDEV)
    return monitor_ ? udev_monitor_get_fd(static_cast<udev_monitor*>(monitor_)) : -1;
#else
    return -1;
#endif
}

lx::drm::hotplug_batch lx::drm::uevent_monitor::drain() {
    hotplug_batch batch{};
#if defined(LUMEN_HAS_UDEV)
    auto* mon = static_cast<udev_monitor*>(monitor_);
    if (!mon)
        return batch;
    // The socket is nonblocking (libudev opens it SOCK_NONBLOCK): this returns null once
    // the queue is empty. Bounded anyway, so a flood cannot hold the UI thread.
    for (unsigned n = 0; n < 256; ++n) {
        udev_device* dev = udev_monitor_receive_device(mon);
        if (!dev)
            break;
        const hotplug_batch one = parse_uevent(
            udev_device_get_action(dev), static_cast<unsigned long long>(udev_device_get_devnum(dev)),
            card_, udev_device_get_property_value(dev, "HOTPLUG"),
            udev_device_get_property_value(dev, "CONNECTOR"),
            udev_device_get_property_value(dev, "PROPERTY"),
            udev_device_get_property_value(dev, "LEASE"));
        merge_hotplug(batch, one);
        udev_device_unref(dev);
    }
#endif
    return batch;
}
