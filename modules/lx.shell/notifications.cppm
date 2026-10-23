module;

// Notifications (design-session-and-wm N.2–N.4): the model of lumen-shell's
// org.freedesktop.Notifications server — pure (no D-Bus, no Wayland), unit-tested in
// tests/test_notifications.cpp — plus the body-markup subset and the image-data decoder.
//
// Spec 1.2 (specifications.freedesktop.org/notification-spec): ids are never 0 and not
// reused within a session (the counter survives a shell restart, S.7); `replaces_id` of an
// open notification replaces it in place (same id, same place, its timer restarted, no new
// popup); an unknown one makes a new notification. Expiry: -1 → the configured timeout by
// urgency (`timeout`, `timeout-low`, `critical-timeout`; 0 = never), 0 → never, > 0 → that
// many ms. Close reasons: 1 expired, 2 dismissed by the user (close button, a click without a
// default action, an action invoked, `notifications.dismiss`), 3 CloseNotification, 4 other.
//
// Where a notification goes:
//   popup stack   newest first; the newest `max-visible` are shown, the rest wait ("+n more")
//                 with their timers not running. Timers run only while shown, pause while
//                 the pointer is over the stack and while the session is locked.
//   history       expired notifications (closed — a `resident` one stays open), those that
//                 never popped up (do-not-disturb, more than `max-visible` arriving while
//                 locked), bounded by `history` (oldest out; still open ones then close with
//                 reason 1). `transient` ones never enter it.
// Do-not-disturb (manual, or automatic while the screen is captured): low and normal go
// straight to the history, open; critical ones still pop up (KDE's rule). While locked new
// ones queue; at unlock the newest `max-visible` of those pop up and older ones go to the
// history. A client sending more than 20 per second has the excess merged into its newest
// open notification (replaced), warned once.
//
// Shell thread only. Allocates per notification (strings, the decoded image), never per
// frame; bounded by the history size plus the open ones (at most k_max_open).

import lx.foundation;
import lx.text;

export module lx.shell:notifications;

import std;

export namespace lx::shell {

// ── Body markup (spec "Markup"): <b> <i> <u> <a href> <img src alt>, entities ──

/// The body as text with styled spans. Span tags: 0 plain; 1 + n: link n (`links[n]`).
struct rich_text {
    std::string text;
    std::vector<lx::text::text_span> spans;
    /// Hyperlinks (http, https, mailto only; any other scheme is shown as plain text).
    std::vector<std::string> links;
};

/// Parses the markup subset: `<b>`, `<i>`, `<u>`, `<a href="…">`, `<img src="…" alt="…"/>`
/// (its alt text), `<br/>` (a line break), the entities &amp; &lt; &gt; &quot; &apos; and
/// numeric ones. Anything else is shown as the text it is (an unknown tag included); a tag
/// left open is closed at the end, a stray closing tag dropped. Bounded: 8 KiB of input, 64
/// spans, 16 links (the rest plain).
void parse_body_markup(std::string_view markup, rich_text& out);
/// The markup's text without tags (a tooltip, a log line).
[[nodiscard]] std::string markup_to_plain(std::string_view markup);
/// Whether `url` may be opened from a notification: http, https or mailto.
[[nodiscard]] bool link_allowed(std::string_view url);

// ── Images ──

/// A square premultiplied ARGB8888 image (the source fitted inside, centred).
struct notify_image {
    int size = 0;
    std::vector<std::uint32_t> pixels;
};

/// Why `decode_image_data` refused.
enum class image_data_error : unsigned char {
    none,
    /// Width, height or rowstride ≤ 0, or larger than 4096.
    bad_size,
    /// Not 8 bits per sample, or channels ≠ 3 + has_alpha.
    bad_format,
    /// Fewer bytes than the rows need.
    short_data,
};
[[nodiscard]] const char* image_data_error_text(image_data_error e);
/// The `image-data` hint (iiibiiay: width, height, rowstride, has_alpha, bits_per_sample,
/// channels, data; RGB or RGBA, straight alpha) fitted into a square of at most `max_size`
/// pixels (box-filtered when larger: a 512×512 photo is not kept at full size).
[[nodiscard]] image_data_error decode_image_data(int width, int height, int rowstride,
                                                 bool has_alpha, int bits_per_sample,
                                                 int channels, std::span<const std::uint8_t> data,
                                                 int max_size, notify_image& out);
/// Fits premultiplied ARGB `src` (`w`×`h`) into a `size`² square, centred, transparent around.
void fit_square(const std::uint32_t* src, int w, int h, int size, std::vector<std::uint32_t>& out);

// ── The model ──

enum class notify_urgency : unsigned char { low = 0, normal = 1, critical = 2 };
enum class close_reason : unsigned { expired = 1, dismissed = 2, closed = 3, undefined = 4 };

struct notify_action {
    std::string key;
    std::string label;
};

/// One Notify call, its hints read.
struct notify_request {
    /// The caller's unique bus name ("" in tests).
    std::string sender;
    std::string app_name;
    std::string app_icon;
    std::string summary;
    std::string body;
    unsigned replaces_id = 0;
    std::vector<notify_action> actions;
    notify_urgency urgency = notify_urgency::normal;
    std::string category;
    std::string desktop_entry;
    /// image-path / image_path (a file:// URI, an absolute path or an icon name).
    std::string image_path;
    /// image-data / image_data / icon_data, decoded (size 0: none).
    notify_image image{};
    bool resident = false;
    bool transient = false;
    /// ms; -1 the server's default.
    int expire_timeout = -1;
};

/// The [notifications] settings the model uses.
struct notify_settings {
    unsigned timeout_ms = 5000;
    unsigned timeout_low_ms = 3000;
    /// 0: critical ones stay until dismissed.
    unsigned critical_timeout_ms = 0;
    unsigned max_visible = 3;
    unsigned history = 50;
    bool dnd_while_sharing = true;
    /// More than this many per second from one sender are merged.
    unsigned rate_limit = 20;
};

struct notification {
    unsigned id = 0;
    notify_request req{};
    rich_text body{};
    /// Changes with every replacement (the view repaints a card whose rev changed).
    unsigned rev = 0;
    long long arrived_ns = 0;
    /// The whole timeout (ns); 0 = never expires.
    long long timeout_ns = 0;
    /// Left of it while not running; the deadline while running.
    long long remaining_ns = 0;
    long long deadline_ns = 0;
    bool running = false;
    /// NotificationClosed has not been sent.
    bool open = true;
    bool in_popup = false;
    bool in_history = false;
    /// Arrived while locked (decided at unlock).
    bool queued_locked = false;
    /// An action is being invoked (the activation token is on its way): hidden, still open.
    bool invoking = false;
    /// Its sender left the bus: no action can be invoked any more.
    bool sender_gone = false;
};

/// What the model tells its owner (the D-Bus server: the signals).
struct notify_sink {
    void (*closed)(unsigned id, close_reason reason, void* user) = nullptr;
    /// A new id block began (persist "next id ≥ `next`", S.7).
    void (*reserve)(unsigned next, void* user) = nullptr;
    void* user = nullptr;
};

class notification_center {
public:
    static constexpr unsigned k_max_open = 256;
    /// Ids are persisted in blocks: one write per this many notifications.
    static constexpr unsigned k_id_block = 64;

    void set_sink(notify_sink s) { sink_ = s; }
    void configure(const notify_settings& s, long long now);
    [[nodiscard]] const notify_settings& settings() const { return settings_; }
    /// Ids continue from here (a restarted shell: the persisted block's end).
    void set_next_id(unsigned next);
    [[nodiscard]] unsigned next_id() const { return next_id_; }

    /// Notify: the id (never 0). `merged` tells the rate limit replaced another.
    unsigned notify(notify_request req, long long now, bool* merged = nullptr);
    /// CloseNotification: false when `id` is not open (the D-Bus error then).
    bool close(unsigned id, close_reason reason, long long now);
    /// The user dismissed it (close button, a click without default action): reason 2.
    bool dismiss(unsigned id, long long now) { return close(id, close_reason::dismissed, now); }
    /// The newest shown notification / every popup (notifications.dismiss[-all]).
    bool dismiss_newest(long long now);
    unsigned dismiss_all(long long now);
    /// Clears the history (open ones there close with reason 2).
    void clear_history(long long now);
    /// Removes one entry from the history (closing it with 2 when open).
    bool forget(unsigned id, long long now);

    /// An action was clicked: hidden at once, still open until `finish_invoke` (the D-Bus
    /// layer sends ActivationToken and ActionInvoked in between). False when it cannot be
    /// invoked (not open, its sender gone, no such action; "default" needs a default
    /// action).
    bool begin_invoke(unsigned id, std::string_view key, long long now);
    /// After ActionInvoked: closed with reason 2, unless resident (then kept, open, in the
    /// history).
    void finish_invoke(unsigned id, long long now);
    /// Whether `id` has the action `key` and can be invoked now.
    [[nodiscard]] bool can_invoke(unsigned id, std::string_view key) const;

    /// Expires what is due; true when anything changed.
    bool expire(long long now);
    /// The next deadline (0: none).
    [[nodiscard]] long long next_deadline() const;

    /// The pointer is over the popup stack: every shown timer pauses.
    void set_hover(bool on, long long now);
    void set_locked(bool locked, long long now);
    [[nodiscard]] bool locked() const { return locked_; }
    void set_dnd(bool on, long long now);
    /// The screen is being captured (zlm capture_state): do-not-disturb when configured.
    void set_capturing(bool on, long long now);
    [[nodiscard]] bool dnd() const { return dnd_ || (capturing_ && settings_.dnd_while_sharing); }
    [[nodiscard]] bool dnd_manual() const { return dnd_; }

    /// A sender left the bus: its notifications' actions are not invokable any more.
    void sender_gone(std::string_view sender);
    /// Whether any open notification of `sender` has actions (its bus name is worth
    /// watching).
    [[nodiscard]] bool sender_has_actions(std::string_view sender) const;

    // ── What is shown ──
    /// The shown popups, newest first (at most max-visible; none while locked).
    [[nodiscard]] unsigned shown(const notification** out, unsigned cap) const;
    /// Popups waiting beyond the shown ones.
    [[nodiscard]] unsigned waiting() const;
    /// The history, newest first.
    [[nodiscard]] unsigned history(const notification** out, unsigned cap) const;
    [[nodiscard]] const notification* find(unsigned id) const;
    /// Entries in the history the user has not looked at (the panel's count).
    [[nodiscard]] unsigned unread() const { return unread_; }
    void mark_read() {
        if (unread_) {
            unread_ = 0;
            ++rev_;
        }
    }
    /// Any shown popup is critical (the view goes to the overlay layer).
    [[nodiscard]] bool critical_shown() const;
    /// Changes whenever anything shown (popups, history, the count, do-not-disturb) changed.
    [[nodiscard]] unsigned rev() const { return rev_; }
    [[nodiscard]] unsigned open_count() const;

private:
    notification* get(unsigned id);
    void add_to_history(notification& n, long long now);
    void close_entry(notification& n, close_reason reason);
    void drop_unused();
    /// Starts the timers of the shown popups, pauses the others.
    void update_running(long long now);
    long long timeout_for(const notify_request& r) const;
    unsigned allocate_id();

    notify_settings settings_{};
    notify_sink sink_{};
    /// Oldest first.
    std::vector<std::unique_ptr<notification>> entries_;
    unsigned next_id_ = 1;
    unsigned reserved_until_ = 0;
    unsigned rev_ = 1;
    unsigned unread_ = 0;
    bool hover_ = false;
    bool locked_ = false;
    bool dnd_ = false;
    bool capturing_ = false;
    struct rate {
        std::string sender;
        long long window_ns = 0;
        unsigned count = 0;
        bool warned = false;
        unsigned long long used = 0;
    };
    rate rates_[8]{};
    unsigned long long rate_clock_ = 0;
};

/// The persisted state (S.7): `$XDG_RUNTIME_DIR/lumen/shell-state`, two lines
/// ("next-id N", "dnd 0|1"). Missing or unreadable: {1, false}.
struct shell_state {
    unsigned next_id = 1;
    bool dnd = false;
};
[[nodiscard]] shell_state load_shell_state(const char* path);
/// Written atomically (a temporary renamed over it); false on failure.
bool save_shell_state(const char* path, const shell_state& s);
/// The path: $LUMEN_SHELL_STATE (0: none), else $XDG_RUNTIME_DIR/lumen/shell-state ("" without
/// XDG_RUNTIME_DIR).
[[nodiscard]] std::string shell_state_path();

// ── The D-Bus server (notify_bus.cpp) ──

/// org.freedesktop.Notifications at /org/freedesktop/Notifications on a bus connection
/// (`:user_bus`'s, or a socketpair's in tests), driving a `notification_center`.
/// GetCapabilities: actions, body, body-markup, body-hyperlinks, icon-static, persistence
/// (no body-images: an <img> shows its alt text; no sound). GetServerInformation: Lumen,
/// Lumen, the version, 1.2.
class notification_server {
public:
    notification_server() = default;
    ~notification_server();
    notification_server(const notification_server&) = delete;
    notification_server& operator=(const notification_server&) = delete;

    /// Exports the object on `bus` (sd_bus*). The name is the owner's to request.
    [[nodiscard]] bool attach(void* bus, notification_center& center);
    void detach();
    [[nodiscard]] bool attached() const { return slot_ != nullptr; }

    /// After the activation token for an invoked action: ActivationToken (when `token`) then
    /// ActionInvoked, then the model's finish (closed with 2 unless resident).
    void action_invoked(unsigned id, const char* key, const char* token, long long now);
    /// The model's sink: NotificationClosed.
    void emit_closed(unsigned id, close_reason reason);
    /// The clock the server stamps calls with (tests: a fixed one).
    void set_clock(long long (*now)()) { now_ = now; }
    /// A Notify, CloseNotification or a sender change happened since the last call.
    [[nodiscard]] bool take_changed() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }
    /// Senders watched for disconnection (open notifications with actions): at most 32.
    [[nodiscard]] unsigned watched_senders() const;

    struct events;

private:
    friend struct events;
    void watch_sender(const std::string& sender);
    void unwatch_idle_senders();

    void* bus_ = nullptr;  // sd_bus*
    void* slot_ = nullptr; // sd_bus_slot* of the vtable
    notification_center* center_ = nullptr;
    long long (*now_)() = nullptr;
    bool changed_ = false;
    struct sender_watch {
        notification_server* owner = nullptr;
        std::string name;
        void* slot = nullptr; // sd_bus_slot*: NameOwnerChanged arg0 = name
    };
    std::vector<std::unique_ptr<sender_watch>> senders_;
};

} // namespace lx::shell
