module;

// lumen-shell's one connection to the user bus (design-session-and-wm N.1).
//
// Opened once the first panel frame is on screen (it never delays the panel) and kept for
// the shell's life: the notification server (org.freedesktop.Notifications), the tray's
// StatusNotifierWatcher and host, and the application launcher's systemd calls all share
// it. Its fd is one more entry of the shell's ppoll set; the bus's own timeout (a call's
// reply deadline) lowers the poll timeout, and nothing else wakes it — no periodic matches,
// no polling: an idle desktop leaves the fd asleep.
//
// Well-known names are requested asynchronously with SD_BUS_NAME_QUEUE and without
// replacement: when another notification daemon (mako, dunst) or another watcher owns one,
// the shell waits in the bus's queue and gets it when that owner exits (NameAcquired).
//
// No sd-bus type crosses this interface (`void*`); the shell thread only.

import lx.foundation;

export module lx.shell:user_bus;

import std;

export namespace lx::shell {

class user_bus {
public:
    /// What a queued name request ended in, or how it changed later.
    enum class name_state : unsigned char { requested, owned, queued, lost, failed };
    using name_fn = void (*)(const char* name, name_state state, void* user);
    /// The connection broke: every slot of it is gone (observers drop theirs).
    using lost_fn = void (*)(void* user);

    static constexpr unsigned k_max_names = 6;
    static constexpr unsigned k_max_lost = 4;

    user_bus() = default;
    ~user_bus();
    user_bus(const user_bus&) = delete;
    user_bus& operator=(const user_bus&) = delete;

    /// Connects to $DBUS_SESSION_BUS_ADDRESS (or $XDG_RUNTIME_DIR/bus). False without one
    /// (logged once); the shell then runs without notifications and the tray.
    [[nodiscard]] bool open();
    /// An existing connection (tests: one end of a socketpair). Takes a reference.
    void adopt(void* sd_bus);
    void close();
    [[nodiscard]] bool is_open() const { return bus_ != nullptr; }
    /// sd_bus*, or null.
    [[nodiscard]] void* bus() const { return bus_; }
    /// Our unique name (":1.42"), or "".
    [[nodiscard]] const char* unique_name() const;

    /// For the poll set: the fd (-1 when closed) and the events it waits for.
    [[nodiscard]] int fd() const;
    [[nodiscard]] short events() const;
    /// Milliseconds until the bus has a deadline (a call's reply), or -1.
    [[nodiscard]] int timeout_ms() const;
    /// Processes everything queued (call after poll, whatever the fd said: a deadline may
    /// be due). A broken connection is closed and the lost handlers run.
    void process();

    /// Requests `name` (queued, no replacement); `fn` hears the outcome and every later
    /// change (owned on NameAcquired, lost on NameLost). False when closed or full.
    bool request_name(const char* name, name_fn fn, void* user);
    /// Releases a name requested here (fire and forget).
    void release_name(const char* name);
    /// Whether `name` is ours right now.
    [[nodiscard]] bool owns(const char* name) const;
    void add_lost_handler(lost_fn fn, void* user);

    struct events;

private:
    friend struct events;
    struct name_slot {
        char name[96]{};
        name_state state = name_state::requested;
        name_fn fn = nullptr;
        void* user = nullptr;
        void* call = nullptr; // sd_bus_slot* of RequestName
        bool used = false;
    };
    void lost();

    void* bus_ = nullptr;       // sd_bus*
    void* acquired_ = nullptr;  // sd_bus_slot*: NameAcquired
    void* name_lost_ = nullptr; // sd_bus_slot*: NameLost
    name_slot names_[k_max_names]{};
    struct lost_entry {
        lost_fn fn = nullptr;
        void* user = nullptr;
    };
    lost_entry lost_[k_max_lost]{};
    bool told_unavailable_ = false;
};

} // namespace lx::shell
