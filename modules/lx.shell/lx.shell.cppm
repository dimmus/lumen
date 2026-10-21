module;

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(LUMEN_SHELL_WAYLAND)
#include <poll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <wayland-client.h>
#include "cursor-shape-v1-client-protocol.h"
#include "zlm_shell_v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#if defined(LUMEN_PROTOCOL_XDG_ACTIVATION_V1)
#include "xdg-activation-v1-client-protocol.h"
#define LUMEN_SHELL_ACTIVATION 1
#endif
#if defined(LUMEN_PROTOCOL_VIEWPORTER) && defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1)
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#define LUMEN_SHELL_FRACTIONAL 1
#endif
#if defined(LUMEN_PROTOCOL_EXT_IDLE_NOTIFY_V1) && \
    defined(LUMEN_PROTOCOL_WLR_OUTPUT_POWER_MANAGEMENT_UNSTABLE_V1)
#include "ext-idle-notify-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#define LUMEN_SHELL_IDLE 1
#endif
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#define namespace name_space
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#pragma clang diagnostic pop
#endif

import lx.foundation;
import lx.shell.policy;
import lx.trace;

export module lx.shell;

export import :task_model;
export import :bridge;
export import :shm_canvas;
export import :panel;
export import :desktop_entry;
export import :app_catalog;
export import :app_launch;
export import :launcher;
export import :idle_policy;

export namespace lx::shell {

struct shell_config {
    /// Wayland display (socket) name; null = $WAYLAND_DISPLAY, as libwayland decides.
    const char* display = nullptr;
    /// When to power the outputs off and when to lock (`--idle-policy`, `--blank-after`,
    /// `--blank-locked-after`, `--lock-command`, `--[no-]lock-on-blank`): see `:idle_policy`.
    idle_policy_config idle{};
};

/// lumen-shell: a privileged Wayland client of lumen-compositor (architecture §2.2, §5).
///
/// One thread. It binds zlm_shell_v1 (the compositor accepts that only from its configured
/// shell binary), keeps the window list from zlm_policy_bridge_v1, and shows it as a top
/// panel on wlr-layer-shell — one per output, each listing the windows on its output (ADR
/// 008 M13; waybar's default, Plasma's "only tasks from the current screen"). It also launches applications: the panel's Apps button and the
/// Super tap (a zlm_shell_v1 v4 key grab) toggle the launcher, Super+Return starts a
/// terminal, and every launch is its own systemd user service (app_launch). It is also the
/// default idle policy: after `--blank-after` seconds without input (idle inhibitors obeyed;
/// ext_idle_notifier_v1) it powers the outputs off (zwlr_output_power_v1) — input wakes them
/// inside the compositor — and, with a `--lock-command`, the lock policy: Super+L, logind's
/// Lock and before-sleep (zlm_shell_v1 v5 lock_requested) and the blank timeout start the
/// locker, a locker that dies is started again, and the launcher closes when the session
/// locks (the compositor hides the shell and keeps input from it meanwhile). The loop sleeps
/// in ppoll on the display, a signalfd, the catalog's inotify fd, one timerfd (key repeat,
/// notice expiry, catalog debounce, activation-token timeout — only armed while one of them
/// is due) and, once a launch opened it, the user bus. Exit status: 0 on SIGINT/SIGTERM, 1
/// when the compositor refuses or goes away (the systemd unit restarts it).
class shell_app {
public:
    explicit shell_app(shell_config config = {});
    ~shell_app();
    shell_app(const shell_app&) = delete;
    shell_app& operator=(const shell_app&) = delete;

    [[nodiscard]] int run();

    [[nodiscard]] policy_registry& policies();
    [[nodiscard]] policy_bridge& bridge() { return bridge_; }

    /// Registry, seat and activation bookkeeping (defined with the listeners).
    struct events;

private:
    friend struct events;
    enum timer_slot : unsigned {
        t_repeat,
        t_notice,
        t_debounce,
        t_token,
        /// A locker that died is started again (at most once per 5 s).
        t_relaunch,
        /// The blank that waits for a lock the blank timeout started: at most 5 s.
        t_lock_blank,
        t_count
    };
    enum class pointer_target : unsigned char { none, panel, launcher };
    /// One bound wl_output: its xdg-output rect, its panel and its power control. Created on
    /// `global`, destroyed on `global_remove` (panel first) — never per frame.
    struct output_record {
        shell_app* app = nullptr;
        void* output = nullptr; // wl_output*
        unsigned name = 0;      // registry name
        void* xdg = nullptr;    // zxdg_output_v1*
        char connector[32]{};
        /// Logical rect (xdg-output; else the mode over the scale at the origin).
        lx::rect2i logical{};
        bool have_xdg = false;
        int mode_w = 0;
        int mode_h = 0;
        int scale = 1;
        /// The first wl_output.done arrived: the state above is complete.
        bool done = false;
        std::unique_ptr<panel> bar{};
        void* power = nullptr; // zwlr_output_power_v1*
        /// zwlr_output_power_v1.mode: off.
        bool off = false;
    };
    struct pending_token {
        void* token = nullptr; // xdg_activation_token_v1*
        std::unique_ptr<prepared_launch> launch;
        long long deadline_ns = 0;
    };
    static constexpr unsigned k_max_tokens = 8;

    [[nodiscard]] int fail(const char* what);
    void teardown();

    // ── Launching ──
    void ensure_catalog();
    void toggle_launcher();
    void open_launcher();
    void close_launcher();
    void launch_entry(unsigned entry, unsigned serial);
    void launch_terminal(unsigned serial);
    void begin_launch(std::unique_ptr<prepared_launch> p, unsigned serial, void* surface);
    void start_launch(const prepared_launch& p, const char* token);
    void token_done(pending_token& t, const char* token);
    void expire_tokens(long long now);
    void report_failure(const char* id, const char* name, launch_failure f, const char* detail,
                        int error_number, const char* unit);
    void show_notice(const char* text);
    void on_outcome(const launch_outcome& o);

    // ── Timers ──
    void set_timer(timer_slot slot, long long deadline_ns);
    void arm_timer();
    void on_timer();
    [[nodiscard]] static long long now_ns();

    // ── Keyboard ──
    void on_key(unsigned serial, unsigned code, bool pressed);

    // ── Outputs ──
    /// The output's first done (or the setup, for outputs bound before it): its panel and
    /// its power control.
    void output_ready(output_record& o);
    /// The output's global went: its panel and objects go.
    void remove_output(unsigned name);
    void destroy_output(output_record& o);
    [[nodiscard]] output_record* output_of_panel_surface(void* surface);
    /// Where the launcher opens (M13): the active window's output, else the one whose panel
    /// was last pointed at, else the first.
    [[nodiscard]] output_record* launcher_output();
    /// Every panel's task filter after the outputs' rects changed.
    void outputs_moved();
    [[nodiscard]] bool any_panel_drawn() const;
    void set_apps_all(bool open);
    void set_notice_all(const char* text);

    // ── Idle policy ──
    /// After the binds: the idle notification and the outputs' power controls, as the
    /// policy asks; logs why not otherwise.
    void setup_idle();
    void teardown_idle();
    /// The power control of one output (for one bound after the setup too).
    void add_power(output_record& o);
    /// Powers every output off (zwlr_output_power_v1), logging `why`.
    void blank_outputs(const char* why);

    // ── Lock policy ──
    /// zlm_shell_v1 v5 events.
    void on_lock_state(unsigned state);
    void on_lock_requested(unsigned reason);
    /// Starts `--lock-command` (its own systemd unit, as every launch). `why` is logged.
    void launch_locker(const char* why);
    /// The locked-screen idle notification exists exactly while the policy wants one.
    void update_locked_notification();

    shell_config config_{};
    policy_bridge bridge_{};
    std::vector<std::unique_ptr<output_record>> outputs_{};
    /// The panels exist (after the setup): an output bound later gets its panel at its done.
    bool panels_enabled_ = false;
    /// The panel the pointer is on, and the output whose panel was pointed at last.
    panel* pointer_panel_ = nullptr;
    unsigned last_panel_output_ = 0;
    app_catalog catalog_{};
    launcher launcher_{};
    app_launcher apps_{};
    xkb_keyboard xkb_{};
    void* display_ = nullptr;      // wl_display*
    void* registry_ = nullptr;     // wl_registry*
    void* compositor_ = nullptr;   // wl_compositor*
    void* shm_ = nullptr;          // wl_shm*
    void* seat_ = nullptr;         // wl_seat*
    void* layer_shell_ = nullptr;  // zwlr_layer_shell_v1*
    void* shell_ = nullptr;        // zlm_shell_v1*
    void* cursor_shape_ = nullptr; // wp_cursor_shape_manager_v1*
    void* xdg_output_manager_ = nullptr; // zxdg_output_manager_v1*
    void* activation_ = nullptr;   // xdg_activation_v1*
    void* viewporter_ = nullptr;   // wp_viewporter*
    void* fractional_ = nullptr;   // wp_fractional_scale_manager_v1*
    void* idle_notifier_ = nullptr;      // ext_idle_notifier_v1*
    void* idle_notification_ = nullptr;  // ext_idle_notification_v1*
    void* power_manager_ = nullptr;      // zwlr_output_power_manager_v1*
    /// Blanking is set up (the power controls are made per output, also for later ones).
    bool power_enabled_ = false;
    void* locked_notification_ = nullptr; // ext_idle_notification_v1* (while locked)
    idle_policy idle_{};
    void* pointer_ = nullptr;      // wl_pointer*
    void* keyboard_ = nullptr;     // wl_keyboard*
    void* cursor_device_ = nullptr; // wp_cursor_shape_device_v1*
    unsigned compositor_version_ = 0;
    unsigned seat_name_ = 0;
    unsigned seat_version_ = 0;
    int timer_fd_ = -1;
    long long deadline_[t_count]{};
    pointer_target pointer_target_ = pointer_target::none;
    bool axis_discrete_seen_ = false;
    double axis_accum_ = 0.0;
    // Key repeat (wl_keyboard.repeat_info; rate 0 = none).
    int repeat_rate_ = 25;
    int repeat_delay_ = 600;
    unsigned repeat_code_ = 0;
    unsigned repeat_serial_ = 0;
    unsigned last_key_serial_ = 0;
    pending_token tokens_[k_max_tokens]{};
    /// (desktop ID, reason) pairs already warned about in this catalog generation.
    std::vector<std::pair<std::string, launch_failure>> warned_{};
    unsigned warned_gen_ = 0;
    bool catalog_scanned_after_frame_ = false;
};

} // namespace lx::shell

module :private;

namespace {

constexpr const char* k_shell_category = "shell";
[[maybe_unused]] constexpr const char* k_apps_category = "shell.apps";

[[maybe_unused]] void shell_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_shell_category, line);
}

[[maybe_unused]] void apps_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_apps_category, line);
}

[[maybe_unused]] [[nodiscard]] bool shell_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

} // namespace

lx::shell::shell_app::shell_app(shell_config config) : config_{config}, idle_{config.idle} {}
lx::shell::shell_app::~shell_app() { teardown(); }
lx::shell::policy_registry& lx::shell::shell_app::policies() {
    return policy_registry::instance();
}

long long lx::shell::shell_app::now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

#if defined(LUMEN_SHELL_WAYLAND)

namespace {

[[nodiscard]] unsigned min_version(unsigned have, unsigned want) {
    return have < want ? have : want;
}

constexpr unsigned k_super_l = 0xffeb;
constexpr unsigned k_super_r = 0xffec;
constexpr unsigned k_return = 0xff0d;
constexpr unsigned k_key_l = 0x006c;
/// How long a blank waits for the lock the blank timeout started.
constexpr long long k_lock_blank_wait_ns = 5'000'000'000LL;
/// How long a launch waits for its activation token before going without one.
constexpr long long k_token_timeout_ns = 100'000'000LL;
/// A failure notice on the panel stays this long (or until clicked).
constexpr long long k_notice_ns = 6'000'000'000LL;
/// inotify bursts (a package install) settle into one rescan.
constexpr long long k_debounce_ns = 250'000'000LL;

} // namespace

struct lx::shell::shell_app::events {
    static shell_app& self(void* data) { return *static_cast<shell_app*>(data); }

    // ── wl_registry ──
    static void global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                       uint32_t version);
    static void global_remove(void* data, wl_registry*, uint32_t name) {
        self(data).remove_output(name);
    }

    // ── wl_output (v4): one record each; the panel at its first done ──
    static output_record& out(void* data) { return *static_cast<output_record*>(data); }
    static void output_geometry(void* data, wl_output*, int32_t x, int32_t y, int32_t, int32_t,
                                int32_t, const char*, const char*, int32_t) {
        auto& o = out(data);
        if (!o.have_xdg) {
            o.logical.x = x;
            o.logical.y = y;
        }
    }
    static void output_mode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
        if (flags & WL_OUTPUT_MODE_CURRENT) {
            out(data).mode_w = w;
            out(data).mode_h = h;
        }
    }
    static void output_done(void* data, wl_output*) {
        auto& o = out(data);
        if (!o.have_xdg && o.mode_w > 0) {
            // No xdg-output: the mode over the integer scale (logical for integer scales).
            const int sc = o.scale > 0 ? o.scale : 1;
            o.logical.width = o.mode_w / sc;
            o.logical.height = o.mode_h / sc;
        }
        const bool first = !o.done;
        o.done = true;
        if (first)
            o.app->output_ready(o);
        else
            o.app->outputs_moved();
    }
    static void output_scale(void* data, wl_output*, int32_t factor) { out(data).scale = factor; }
    static void output_name(void* data, wl_output*, const char* name) {
        std::snprintf(out(data).connector, sizeof(out(data).connector), "%s", name ? name : "");
    }
    static void output_description(void*, wl_output*, const char*) {}

    // ── zxdg_output_v1 (v3: closed by wl_output.done) ──
    static void xdg_position(void* data, zxdg_output_v1*, int32_t x, int32_t y) {
        auto& o = out(data);
        o.have_xdg = true;
        o.logical.x = x;
        o.logical.y = y;
    }
    static void xdg_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) {
        auto& o = out(data);
        o.have_xdg = true;
        o.logical.width = w;
        o.logical.height = h;
    }
    static void xdg_done(void*, zxdg_output_v1*) {}
    static void xdg_name(void*, zxdg_output_v1*, const char*) {}
    static void xdg_description(void*, zxdg_output_v1*, const char*) {}

    // ── wl_seat ──
    static void seat_caps(void* data, wl_seat* seat, uint32_t caps);
    static void seat_name(void*, wl_seat*, const char*) {}

    // ── wl_pointer: routed by surface to the panel or the launcher ──
    static void ptr_enter(void* data, wl_pointer*, uint32_t serial, wl_surface* s, wl_fixed_t x,
                          wl_fixed_t y) {
        auto& a = self(data);
        if (a.cursor_device_)
            wp_cursor_shape_device_v1_set_shape(
                static_cast<wp_cursor_shape_device_v1*>(a.cursor_device_), serial,
                WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
        const double px = wl_fixed_to_double(x), py = wl_fixed_to_double(y);
        if (output_record* o = s ? a.output_of_panel_surface(s) : nullptr) {
            a.pointer_target_ = pointer_target::panel;
            a.pointer_panel_ = o->bar.get();
            a.last_panel_output_ = o->name;
            a.pointer_panel_->pointer_enter(px, py);
        } else if (s && s == a.launcher_.surface()) {
            a.pointer_target_ = pointer_target::launcher;
            a.launcher_.pointer_enter(px, py);
        } else {
            a.pointer_target_ = pointer_target::none;
        }
    }
    static void ptr_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
        auto& a = self(data);
        if (a.pointer_target_ == pointer_target::panel && a.pointer_panel_)
            a.pointer_panel_->pointer_leave();
        else if (a.pointer_target_ == pointer_target::launcher)
            a.launcher_.pointer_leave();
        a.pointer_panel_ = nullptr;
        a.pointer_target_ = pointer_target::none;
    }
    static void ptr_motion(void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
        auto& a = self(data);
        const double px = wl_fixed_to_double(x), py = wl_fixed_to_double(y);
        if (a.pointer_target_ == pointer_target::panel && a.pointer_panel_)
            a.pointer_panel_->pointer_motion(px, py);
        else if (a.pointer_target_ == pointer_target::launcher)
            a.launcher_.pointer_motion(px, py);
    }
    static void ptr_button(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t button,
                           uint32_t state) {
        auto& a = self(data);
        const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
        if (a.pointer_target_ == pointer_target::panel && a.pointer_panel_)
            a.pointer_panel_->pointer_button(button, down);
        else if (a.pointer_target_ == pointer_target::launcher)
            a.launcher_.pointer_button(serial, button, down);
    }
    static void ptr_axis(void* data, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
        auto& a = self(data);
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || a.axis_discrete_seen_ ||
            a.pointer_target_ != pointer_target::launcher)
            return;
        // Continuous scrolling (touchpads): one row per row height of motion.
        a.axis_accum_ += wl_fixed_to_double(value);
        const int rows = static_cast<int>(a.axis_accum_ / (k_launcher_row_h * 3));
        if (rows) {
            a.axis_accum_ -= rows * (k_launcher_row_h * 3);
            a.launcher_.pointer_scroll_steps(rows);
        }
    }
    static void ptr_frame(void* data, wl_pointer*) { self(data).axis_discrete_seen_ = false; }
    static void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
    static void ptr_axis_stop(void* data, wl_pointer*, uint32_t, uint32_t) {
        self(data).axis_accum_ = 0.0;
    }
    static void ptr_axis_discrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete) {
        auto& a = self(data);
        if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
            return;
        a.axis_discrete_seen_ = true;
        if (a.pointer_target_ == pointer_target::launcher)
            a.launcher_.pointer_scroll_steps(discrete);
    }

    // ── wl_keyboard: only the launcher ever has keyboard focus ──
    static void kb_keymap(void* data, wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
        auto& a = self(data);
        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            close(fd);
            return;
        }
        if (!a.xkb_.set_keymap(fd, size))
            shell_log(lx::trace::level::warn, "keymap not usable: the launcher takes no typing");
    }
    static void kb_enter(void* data, wl_keyboard*, uint32_t, wl_surface* s, wl_array*) {
        auto& a = self(data);
        if (s && s == a.launcher_.surface())
            a.launcher_.keyboard_enter();
    }
    static void kb_leave(void* data, wl_keyboard*, uint32_t, wl_surface* s) {
        auto& a = self(data);
        a.set_timer(t_repeat, 0);
        a.repeat_code_ = 0;
        if (s && s == a.launcher_.surface()) {
            a.launcher_.keyboard_leave();
            if (!a.launcher_.is_open())
                a.set_apps_all(false);
        }
    }
    static void kb_key(void* data, wl_keyboard*, uint32_t serial, uint32_t, uint32_t key,
                       uint32_t state) {
        self(data).on_key(serial, key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
    }
    static void kb_modifiers(void* data, wl_keyboard*, uint32_t, uint32_t dep, uint32_t lat,
                             uint32_t lock, uint32_t group) {
        self(data).xkb_.update_mods(dep, lat, lock, group);
    }
    static void kb_repeat(void* data, wl_keyboard*, int32_t rate, int32_t delay) {
        self(data).repeat_rate_ = rate;
        self(data).repeat_delay_ = delay;
    }

#if defined(LUMEN_SHELL_ACTIVATION)
    // ── xdg_activation_token_v1 ──
    static void token_done(void* data, xdg_activation_token_v1* tok, const char* token);
#endif
#if defined(LUMEN_SHELL_IDLE)
    // ── ext_idle_notification_v1, zwlr_output_power_v1 ──
    static void idled(void* data, ext_idle_notification_v1*);
    static void resumed(void* data, ext_idle_notification_v1*);
    static void locked_idled(void* data, ext_idle_notification_v1*);
    static void locked_resumed(void*, ext_idle_notification_v1*) {}
    static void power_mode(void* data, zwlr_output_power_v1*, uint32_t mode);
    static void power_failed(void* data, zwlr_output_power_v1*);
#endif
};

namespace {

const wl_registry_listener k_registry_listener = {
    .global = lx::shell::shell_app::events::global,
    .global_remove = lx::shell::shell_app::events::global_remove,
};
const wl_output_listener k_output_listener = {
    .geometry = lx::shell::shell_app::events::output_geometry,
    .mode = lx::shell::shell_app::events::output_mode,
    .done = lx::shell::shell_app::events::output_done,
    .scale = lx::shell::shell_app::events::output_scale,
    .name = lx::shell::shell_app::events::output_name,
    .description = lx::shell::shell_app::events::output_description,
};
const zxdg_output_v1_listener k_xdg_output_listener = {
    .logical_position = lx::shell::shell_app::events::xdg_position,
    .logical_size = lx::shell::shell_app::events::xdg_size,
    .done = lx::shell::shell_app::events::xdg_done,
    .name = lx::shell::shell_app::events::xdg_name,
    .description = lx::shell::shell_app::events::xdg_description,
};
const wl_seat_listener k_seat_listener = {
    .capabilities = lx::shell::shell_app::events::seat_caps,
    .name = lx::shell::shell_app::events::seat_name,
};
// Bound at wl_seat v5 at most: every wl_pointer and wl_keyboard event up to v5 has a handler.
const wl_pointer_listener k_pointer_listener = {
    .enter = lx::shell::shell_app::events::ptr_enter,
    .leave = lx::shell::shell_app::events::ptr_leave,
    .motion = lx::shell::shell_app::events::ptr_motion,
    .button = lx::shell::shell_app::events::ptr_button,
    .axis = lx::shell::shell_app::events::ptr_axis,
    .frame = lx::shell::shell_app::events::ptr_frame,
    .axis_source = lx::shell::shell_app::events::ptr_axis_source,
    .axis_stop = lx::shell::shell_app::events::ptr_axis_stop,
    .axis_discrete = lx::shell::shell_app::events::ptr_axis_discrete,
};
const wl_keyboard_listener k_keyboard_listener = {
    .keymap = lx::shell::shell_app::events::kb_keymap,
    .enter = lx::shell::shell_app::events::kb_enter,
    .leave = lx::shell::shell_app::events::kb_leave,
    .key = lx::shell::shell_app::events::kb_key,
    .modifiers = lx::shell::shell_app::events::kb_modifiers,
    .repeat_info = lx::shell::shell_app::events::kb_repeat,
};
#if defined(LUMEN_SHELL_ACTIVATION)
const xdg_activation_token_v1_listener k_token_listener = {
    .done = lx::shell::shell_app::events::token_done,
};
#endif
#if defined(LUMEN_SHELL_IDLE)
const ext_idle_notification_v1_listener k_idle_listener = {
    .idled = lx::shell::shell_app::events::idled,
    .resumed = lx::shell::shell_app::events::resumed,
};
const ext_idle_notification_v1_listener k_locked_idle_listener = {
    .idled = lx::shell::shell_app::events::locked_idled,
    .resumed = lx::shell::shell_app::events::locked_resumed,
};
const zwlr_output_power_v1_listener k_power_listener = {
    .mode = lx::shell::shell_app::events::power_mode,
    .failed = lx::shell::shell_app::events::power_failed,
};
#endif

/// Why the connection ended, in words: the protocol error if there was one. Returns the
/// exit code: 0 when the shell itself asked to log out and the compositor simply went away,
/// which is how a logout ends — not an error.
int describe_connection_error(wl_display* d, const char* when, bool logout_requested = false) {
    const int err = wl_display_get_error(d);
    if (logout_requested && err != EPROTO) {
        shell_log(lx::trace::level::info, "session ended (logged out)");
        return 0;
    }
    char line[256];
    if (err == EPROTO) {
        const wl_interface* iface = nullptr;
        uint32_t id = 0;
        const uint32_t code = wl_display_get_protocol_error(d, &iface, &id);
        std::snprintf(line, sizeof(line), "%s: protocol error on %s#%u, code %u", when,
                      iface ? iface->name : "?", id, code);
    } else {
        std::snprintf(line, sizeof(line), "%s: connection lost (%s)", when, std::strerror(err));
    }
    shell_log(lx::trace::level::error, line);
    return 1;
}

void release_pointer(void* p) {
    if (!p)
        return;
    if (wl_pointer_get_version(static_cast<wl_pointer*>(p)) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(static_cast<wl_pointer*>(p));
    else
        wl_pointer_destroy(static_cast<wl_pointer*>(p));
}

void release_keyboard(void* k) {
    if (!k)
        return;
    if (wl_keyboard_get_version(static_cast<wl_keyboard*>(k)) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
        wl_keyboard_release(static_cast<wl_keyboard*>(k));
    else
        wl_keyboard_destroy(static_cast<wl_keyboard*>(k));
}

} // namespace

void lx::shell::shell_app::events::global(void* data, wl_registry* reg, uint32_t name,
                                          const char* iface, uint32_t version) {
    auto& a = self(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        a.compositor_version_ = version;
        a.compositor_ = wl_registry_bind(reg, name, &wl_compositor_interface,
                                         min_version(version, 4));
    } else if (std::strcmp(iface, wl_shm_interface.name) == 0) {
        a.shm_ = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !a.seat_) {
        // v5 at most: the pointer and keyboard listeners cover every event up to v5. The
        // listener goes on at once: capabilities follows the bind.
        a.seat_name_ = name;
        a.seat_version_ = version;
        a.seat_ = wl_registry_bind(reg, name, &wl_seat_interface, min_version(version, 5));
        wl_seat_add_listener(static_cast<wl_seat*>(a.seat_), &k_seat_listener, &a);
    } else if (std::strcmp(iface, wl_output_interface.name) == 0) {
        // Every output (v4: its name); a record each, its panel at its first done.
        auto rec = std::make_unique<output_record>();
        rec->app = &a;
        rec->name = name;
        rec->output = wl_registry_bind(reg, name, &wl_output_interface, min_version(version, 4));
        wl_output_add_listener(static_cast<wl_output*>(rec->output), &k_output_listener, rec.get());
        if (a.xdg_output_manager_) {
            rec->xdg = zxdg_output_manager_v1_get_xdg_output(
                static_cast<zxdg_output_manager_v1*>(a.xdg_output_manager_),
                static_cast<wl_output*>(rec->output));
            zxdg_output_v1_add_listener(static_cast<zxdg_output_v1*>(rec->xdg),
                                        &k_xdg_output_listener, rec.get());
        }
        a.outputs_.push_back(std::move(rec));
    } else if (std::strcmp(iface, zxdg_output_manager_v1_interface.name) == 0) {
        // v3: zxdg_output_v1 closed by wl_output.done. Outputs bound before it get theirs now.
        a.xdg_output_manager_ =
            wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, min_version(version, 3));
        for (auto& rec : a.outputs_) {
            if (rec->xdg)
                continue;
            rec->xdg = zxdg_output_manager_v1_get_xdg_output(
                static_cast<zxdg_output_manager_v1*>(a.xdg_output_manager_),
                static_cast<wl_output*>(rec->output));
            zxdg_output_v1_add_listener(static_cast<zxdg_output_v1*>(rec->xdg),
                                        &k_xdg_output_listener, rec.get());
        }
    } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        a.layer_shell_ = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                                          min_version(version, 4));
    } else if (std::strcmp(iface, zlm_shell_v1_interface.name) == 0) {
        // Version 2 brings toplevel_flags (not responding, demands attention); a v1
        // compositor simply never sends them.
        // v4: key grabs; v5: the session lock's state and lock requests. An older
        // compositor is bound at its own version, and the bridge reports what is absent.
        a.shell_ = wl_registry_bind(reg, name, &zlm_shell_v1_interface, min_version(version, 5));
        // capabilities follows the bind at once; listen before it is dispatched.
        (void)a.bridge_.observe(a.shell_);
    } else if (std::strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        a.cursor_shape_ = wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
#if defined(LUMEN_SHELL_ACTIVATION)
    } else if (std::strcmp(iface, xdg_activation_v1_interface.name) == 0) {
        a.activation_ = wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1);
#endif
#if defined(LUMEN_SHELL_FRACTIONAL)
    } else if (std::strcmp(iface, wp_viewporter_interface.name) == 0) {
        // With fractional scale: the panel and the launcher render at the output's pixel
        // density and show at their logical size (sharp at 1.25).
        a.viewporter_ = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (std::strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        a.fractional_ = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
#endif
#if defined(LUMEN_SHELL_IDLE)
    } else if (std::strcmp(iface, ext_idle_notifier_v1_interface.name) == 0) {
        // v1 is enough: get_idle_notification obeys idle inhibitors, which blanking must.
        a.idle_notifier_ = wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, 1);
    } else if (std::strcmp(iface, zwlr_output_power_manager_v1_interface.name) == 0) {
        a.power_manager_ = wl_registry_bind(reg, name, &zwlr_output_power_manager_v1_interface, 1);
#endif
    }
}

void lx::shell::shell_app::events::seat_caps(void* data, wl_seat* seat, uint32_t caps) {
    auto& a = self(data);
    const bool has_ptr = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_ptr && !a.pointer_) {
        auto* ptr = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(ptr, &k_pointer_listener, &a);
        a.pointer_ = ptr;
        if (a.cursor_shape_)
            a.cursor_device_ = wp_cursor_shape_manager_v1_get_pointer(
                static_cast<wp_cursor_shape_manager_v1*>(a.cursor_shape_), ptr);
    } else if (!has_ptr && a.pointer_) {
        if (a.cursor_device_)
            wp_cursor_shape_device_v1_destroy(
                static_cast<wp_cursor_shape_device_v1*>(a.cursor_device_));
        a.cursor_device_ = nullptr;
        release_pointer(a.pointer_);
        a.pointer_ = nullptr;
        a.pointer_target_ = pointer_target::none;
    }
    const bool has_kb = (caps & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (has_kb && !a.keyboard_) {
        auto* kb = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(kb, &k_keyboard_listener, &a);
        a.keyboard_ = kb;
    } else if (!has_kb && a.keyboard_) {
        release_keyboard(a.keyboard_);
        a.keyboard_ = nullptr;
        a.set_timer(t_repeat, 0);
    }
}

#if defined(LUMEN_SHELL_ACTIVATION)
void lx::shell::shell_app::events::token_done(void* data, xdg_activation_token_v1* tok,
                                              const char* token) {
    auto& a = self(data);
    for (auto& t : a.tokens_)
        if (t.token == tok) {
            a.token_done(t, token);
            return;
        }
    xdg_activation_token_v1_destroy(tok);
}
#endif

// ── Idle policy ──

#if defined(LUMEN_SHELL_IDLE)
void lx::shell::shell_app::events::idled(void* data, ext_idle_notification_v1*) {
    auto& a = self(data);
    switch (a.idle_.on_idled()) {
    case idle_action::none:
        return;
    case idle_action::blank: {
        char line[128];
        std::snprintf(line, sizeof(line), "idle for %u s: powering the outputs off",
                      a.idle_.config().blank_after_s);
        a.blank_outputs(line);
        return;
    }
    case idle_action::lock_then_blank:
        // GNOME's order: never dark while unlocked. The outputs go off once `locked` comes
        // (or after 5 s whatever happened, so a broken locker cannot keep them lit).
        a.launch_locker("idle");
        a.set_timer(t_lock_blank, now_ns() + k_lock_blank_wait_ns);
        return;
    }
}

void lx::shell::shell_app::events::locked_idled(void* data, ext_idle_notification_v1*) {
    auto& a = self(data);
    if (!a.idle_.on_locked_idled())
        return;
    char line[128];
    std::snprintf(line, sizeof(line), "locked and idle for %u s: powering the outputs off",
                  a.idle_.config().blank_locked_after_s);
    a.blank_outputs(line);
}

void lx::shell::shell_app::events::resumed(void* data, ext_idle_notification_v1*) {
    // Nothing to do: input already woke the outputs inside the compositor.
    self(data).idle_.on_resumed();
    if (shell_debug())
        lx::trace::logger::global().log(lx::trace::level::debug, "shell.idle", "resumed");
}

void lx::shell::shell_app::events::power_mode(void* data, zwlr_output_power_v1*, uint32_t mode) {
    auto& o = out(data);
    o.off = mode == ZWLR_OUTPUT_POWER_V1_MODE_OFF;
    if (shell_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "output power %s (%s)", o.off ? "off" : "on",
                      o.connector);
        lx::trace::logger::global().log(lx::trace::level::debug, "shell.idle", line);
    }
}

void lx::shell::shell_app::events::power_failed(void* data, zwlr_output_power_v1* p) {
    auto& o = out(data);
    char line[128];
    std::snprintf(line, sizeof(line),
                  "output power control failed (output %s went?) — not blanked any more",
                  o.connector);
    lx::trace::logger::global().log(lx::trace::level::warn, "shell.idle", line);
    zwlr_output_power_v1_destroy(p);
    if (o.power == p)
        o.power = nullptr;
}
#endif

void lx::shell::shell_app::blank_outputs(const char* why) {
    set_timer(t_lock_blank, 0);
#if defined(LUMEN_SHELL_IDLE)
    bool any = false;
    for (auto& o : outputs_)
        any = any || (o->power && !o->off);
    if (!any)
        return; // already dark (the other timeout, or the lock's blank, came first)
    lx::trace::logger::global().log(lx::trace::level::info, "shell.idle", why);
    for (auto& o : outputs_)
        if (o->power && !o->off)
            zwlr_output_power_v1_set_mode(static_cast<zwlr_output_power_v1*>(o->power),
                                          ZWLR_OUTPUT_POWER_V1_MODE_OFF);
#else
    (void)why;
#endif
}

void lx::shell::shell_app::update_locked_notification() {
#if defined(LUMEN_SHELL_IDLE)
    const unsigned timeout_ms =
        idle_.locked_timeout_ms(idle_notifier_ != nullptr, power_manager_ != nullptr && power_enabled_);
    if (timeout_ms != 0 && !locked_notification_ && seat_) {
        locked_notification_ = ext_idle_notifier_v1_get_idle_notification(
            static_cast<ext_idle_notifier_v1*>(idle_notifier_), timeout_ms,
            static_cast<wl_seat*>(seat_));
        ext_idle_notification_v1_add_listener(
            static_cast<ext_idle_notification_v1*>(locked_notification_), &k_locked_idle_listener,
            this);
    } else if (timeout_ms == 0 && locked_notification_) {
        ext_idle_notification_v1_destroy(
            static_cast<ext_idle_notification_v1*>(locked_notification_));
        locked_notification_ = nullptr;
    }
#endif
}

void lx::shell::shell_app::setup_idle() {
    if (!config_.idle.enabled) {
        lx::trace::logger::global().log(lx::trace::level::info, "shell.idle",
                                        "idle policy none: an external daemon decides (e.g. "
                                        "swayidle + wlopm)");
        return;
    }
#if defined(LUMEN_SHELL_IDLE)
    const unsigned timeout_ms = idle_.blank_timeout_ms(idle_notifier_ != nullptr,
                                                       power_manager_ != nullptr && !outputs_.empty());
    if (timeout_ms == 0) {
        char line[160];
        std::snprintf(line, sizeof(line), "no screen blanking:%s%s%s",
                      config_.idle.blank_after_s == 0 ? " --blank-after 0" : "",
                      !idle_notifier_ ? " no ext_idle_notifier_v1" : "",
                      !power_manager_ || outputs_.empty() ? " no zwlr_output_power_manager_v1" : "");
        lx::trace::logger::global().log(lx::trace::level::info, "shell.idle", line);
        return;
    }
    // One power control per output, for outputs that come later too (M13).
    power_enabled_ = true;
    for (auto& o : outputs_)
        add_power(*o);
    if (seat_) {
        idle_notification_ = ext_idle_notifier_v1_get_idle_notification(
            static_cast<ext_idle_notifier_v1*>(idle_notifier_), timeout_ms,
            static_cast<wl_seat*>(seat_));
        ext_idle_notification_v1_add_listener(
            static_cast<ext_idle_notification_v1*>(idle_notification_), &k_idle_listener, this);
    }
    char line[128];
    std::snprintf(line, sizeof(line), "blanking the outputs after %u s without input",
                  config_.idle.blank_after_s);
    lx::trace::logger::global().log(lx::trace::level::info, "shell.idle", line);
#else
    lx::trace::logger::global().log(lx::trace::level::info, "shell.idle",
                                    "no screen blanking: built without ext-idle-notify / "
                                    "wlr-output-power-management");
#endif
}

void lx::shell::shell_app::teardown_idle() {
#if defined(LUMEN_SHELL_IDLE)
    if (locked_notification_)
        ext_idle_notification_v1_destroy(
            static_cast<ext_idle_notification_v1*>(locked_notification_));
    locked_notification_ = nullptr;
    if (idle_notification_)
        ext_idle_notification_v1_destroy(static_cast<ext_idle_notification_v1*>(idle_notification_));
    if (idle_notifier_)
        ext_idle_notifier_v1_destroy(static_cast<ext_idle_notifier_v1*>(idle_notifier_));
    for (auto& o : outputs_) {
        if (o->power)
            zwlr_output_power_v1_destroy(static_cast<zwlr_output_power_v1*>(o->power));
        o->power = nullptr;
    }
    if (power_manager_)
        zwlr_output_power_manager_v1_destroy(
            static_cast<zwlr_output_power_manager_v1*>(power_manager_));
#endif
    power_enabled_ = false;
    idle_notification_ = idle_notifier_ = power_manager_ = nullptr;
}

void lx::shell::shell_app::add_power(output_record& o) {
#if defined(LUMEN_SHELL_IDLE)
    if (!power_enabled_ || !power_manager_ || o.power || !o.output)
        return;
    o.power = zwlr_output_power_manager_v1_get_output_power(
        static_cast<zwlr_output_power_manager_v1*>(power_manager_), static_cast<wl_output*>(o.output));
    zwlr_output_power_v1_add_listener(static_cast<zwlr_output_power_v1*>(o.power),
                                      &k_power_listener, &o);
#else
    (void)o;
#endif
}

// ── Outputs ──

lx::shell::shell_app::output_record* lx::shell::shell_app::output_of_panel_surface(void* surface) {
    for (auto& o : outputs_)
        if (o->bar && o->bar->surface() == surface)
            return o.get();
    return nullptr;
}

bool lx::shell::shell_app::any_panel_drawn() const {
    for (const auto& o : outputs_)
        if (o->bar && o->bar->first_frame_committed())
            return true;
    return false;
}

void lx::shell::shell_app::set_apps_all(bool open) {
    for (auto& o : outputs_)
        if (o->bar)
            o->bar->set_apps(true, open);
}

void lx::shell::shell_app::set_notice_all(const char* text) {
    for (auto& o : outputs_)
        if (o->bar)
            o->bar->set_notice(text);
}

void lx::shell::shell_app::output_ready(output_record& o) {
    add_power(o);
    if (!panels_enabled_ || o.bar)
        return;
    o.bar = std::make_unique<panel>();
    o.bar->set_apps(true, launcher_.is_open());
    o.bar->set_action_handler(
        [](panel_action action, void* user) {
            auto& a = *static_cast<shell_app*>(user);
            if (action == panel_action::apps)
                a.toggle_launcher();
            else if (action == panel_action::task && a.launcher_.is_open())
                a.close_launcher();
        },
        this);
    // This output's windows only: the one each overlaps most among the outputs.
    o.bar->set_filter({[](const task_entry& e, void* user) {
                           const auto& rec = *static_cast<const output_record*>(user);
                           const auto& all = rec.app->outputs_;
                           lx::rect2i rects[16];
                           unsigned n = 0;
                           unsigned self = 0;
                           for (const auto& other : all) {
                               if (n == 16 || !other->done)
                                   continue;
                               if (other.get() == &rec)
                                   self = n;
                               rects[n++] = other->logical;
                           }
                           const int at = task_output(e.geometry, rects, n);
                           // A window on no output is listed by the first panel, never lost.
                           return at < 0 ? self == 0 : static_cast<unsigned>(at) == self;
                       },
                       &o});
    if (auto created = o.bar->create({.compositor = compositor_,
                                      .shm = shm_,
                                      .layer_shell = layer_shell_,
                                      .viewporter = viewporter_,
                                      .fractional = fractional_},
                                     bridge_, o.output);
        !created) {
        lx::trace::logger::global().log_error(created.get_error(), k_shell_category);
        o.bar.reset();
        return;
    }
    char line[128];
    std::snprintf(line, sizeof(line), "panel requested on output %s (%dx%d+%d+%d)", o.connector,
                  o.logical.width, o.logical.height, o.logical.x, o.logical.y);
    shell_log(lx::trace::level::info, line);
    outputs_moved();
}

void lx::shell::shell_app::outputs_moved() {
    for (auto& o : outputs_)
        if (o->bar)
            o->bar->filter_changed();
}

void lx::shell::shell_app::destroy_output(output_record& o) {
    if (o.bar.get() == pointer_panel_) {
        pointer_panel_ = nullptr;
        if (pointer_target_ == pointer_target::panel)
            pointer_target_ = pointer_target::none;
    }
    if (o.bar)
        o.bar->destroy();
    o.bar.reset();
#if defined(LUMEN_SHELL_IDLE)
    if (o.power)
        zwlr_output_power_v1_destroy(static_cast<zwlr_output_power_v1*>(o.power));
#endif
    o.power = nullptr;
    if (o.xdg)
        zxdg_output_v1_destroy(static_cast<zxdg_output_v1*>(o.xdg));
    o.xdg = nullptr;
    if (o.output) {
        if (wl_output_get_version(static_cast<wl_output*>(o.output)) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(static_cast<wl_output*>(o.output));
        else
            wl_output_destroy(static_cast<wl_output*>(o.output));
    }
    o.output = nullptr;
}

void lx::shell::shell_app::remove_output(unsigned name) {
    for (auto it = outputs_.begin(); it != outputs_.end(); ++it) {
        if ((*it)->name != name)
            continue;
        char line[96];
        std::snprintf(line, sizeof(line), "output %s removed: its panel goes", (*it)->connector);
        shell_log(lx::trace::level::info, line);
        destroy_output(**it);
        outputs_.erase(it);
        outputs_moved();
        return;
    }
}

lx::shell::shell_app::output_record* lx::shell::shell_app::launcher_output() {
    output_record* first = nullptr;
    output_record* pointed = nullptr;
    lx::rect2i rects[16];
    output_record* recs[16];
    unsigned n = 0;
    for (auto& o : outputs_) {
        if (!o->done)
            continue;
        if (!first)
            first = o.get();
        if (o->name == last_panel_output_)
            pointed = o.get();
        if (n < 16) {
            recs[n] = o.get();
            rects[n++] = o->logical;
        }
    }
    // The active window's output (KRunner's "active screen").
    if (const task_entry* e = bridge_.model().get(bridge_.model().focused())) {
        const int at = task_output(e->geometry, rects, n);
        if (at >= 0)
            return recs[at];
    }
    return pointed ? pointed : first;
}

int lx::shell::shell_app::fail(const char* what) {
    shell_log(lx::trace::level::error, what);
    return 1;
}

// ── Timers ──

void lx::shell::shell_app::set_timer(timer_slot slot, long long deadline_ns) {
    deadline_[slot] = deadline_ns;
    arm_timer();
}

void lx::shell::shell_app::arm_timer() {
    if (timer_fd_ < 0)
        return;
    long long earliest = 0;
    for (long long d : deadline_)
        if (d && (!earliest || d < earliest))
            earliest = d;
    itimerspec its{};
    if (earliest) {
        its.it_value.tv_sec = earliest / 1000000000LL;
        its.it_value.tv_nsec = earliest % 1000000000LL;
        if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0)
            its.it_value.tv_nsec = 1;
    }
    (void)timerfd_settime(timer_fd_, TFD_TIMER_ABSTIME, &its, nullptr);
}

void lx::shell::shell_app::on_timer() {
    std::uint64_t expirations = 0;
    (void)!read(timer_fd_, &expirations, sizeof(expirations));
    const long long now = now_ns();
    if (deadline_[t_repeat] && deadline_[t_repeat] <= now) {
        deadline_[t_repeat] = 0;
        if (repeat_code_ && launcher_.is_open() && repeat_rate_ > 0) {
            key_press k{};
            if (xkb_.press(repeat_code_, k))
                launcher_.key(repeat_serial_, k, xkb_.ctrl(), xkb_.shift());
            deadline_[t_repeat] = now + 1000000000LL / repeat_rate_;
        }
    }
    if (deadline_[t_notice] && deadline_[t_notice] <= now) {
        deadline_[t_notice] = 0;
        set_notice_all(nullptr);
    }
    if (deadline_[t_debounce] && deadline_[t_debounce] <= now) {
        deadline_[t_debounce] = 0;
        if (launcher_.is_open() && catalog_.stale()) {
            catalog_.scan();
            launcher_.catalog_changed();
        }
    }
    if (deadline_[t_token] && deadline_[t_token] <= now) {
        deadline_[t_token] = 0;
        expire_tokens(now);
    }
    if (deadline_[t_relaunch] && deadline_[t_relaunch] <= now) {
        deadline_[t_relaunch] = 0;
        if (idle_.lock_state() == shell_lock::abandoned)
            launch_locker("relaunch after the locker exited");
    }
    if (deadline_[t_lock_blank] && deadline_[t_lock_blank] <= now) {
        deadline_[t_lock_blank] = 0;
        // The lock did not come in time: dark anyway (the compositor keeps it locked if it
        // comes later; nothing of the session shows while it is off).
        if (idle_.idle())
            blank_outputs("idle: no lock within 5 s — powering the outputs off anyway");
    }
    arm_timer();
}

// ── Lock policy ──

void lx::shell::shell_app::launch_locker(const char* why) {
    if (!idle_.can_lock())
        return;
    const char* home = std::getenv("HOME");
    desktop_entry e{};
    e.id = "locker"; // unit app-lumen-locker@<random>.service
    e.name = "the screen locker";
    e.exec = idle_.config().lock_command;
    auto p = std::make_unique<prepared_launch>();
    // Resolved through the catalog's PATH, as every launch.
    ensure_catalog();
    const launch_failure f = prepare_entry_launch(e, catalog_, home ? home : "/", *p);
    if (f != launch_failure::none) {
        report_failure("locker", "the screen locker", f, nullptr, 0, nullptr);
        return;
    }
    idle_.note_locker_started(now_ns());
    char line[PATH_MAX + 64];
    std::snprintf(line, sizeof(line), "lock: starting %s (%s)", p->program, why);
    lx::trace::logger::global().log(lx::trace::level::info, "shell.lock", line);
    // No activation token: a locker's surfaces take the keyboard as lock surfaces.
    start_launch(*p, nullptr);
}

void lx::shell::shell_app::on_lock_state(unsigned state) {
    const shell_lock s = state <= 3 ? static_cast<shell_lock>(state) : shell_lock::locked;
    const bool blank = idle_.on_lock_state(s);
    if (s != shell_lock::unlocked) {
        // Nothing of the shell is shown or reachable while locked (the compositor's doing);
        // what it had open is dropped, so the unlocked desktop comes back as it should.
        if (launcher_.is_open())
            close_launcher();
        pointer_target_ = pointer_target::none;
        repeat_code_ = 0;
        set_timer(t_repeat, 0);
    }
    if (s == shell_lock::abandoned) {
        const long long delay = idle_.relaunch_delay(now_ns());
        if (delay < 0) {
            if (idle_.can_lock())
                lx::trace::logger::global().log(
                    lx::trace::level::error, "shell.lock",
                    "the locker keeps exiting: not starting it again — the session stays locked "
                    "(loginctl unlock-session unlocks it)");
        } else if (delay == 0) {
            launch_locker("the locker exited");
        } else {
            set_timer(t_relaunch, now_ns() + delay);
        }
    } else {
        set_timer(t_relaunch, 0);
    }
    if (s == shell_lock::unlocked)
        set_timer(t_lock_blank, 0);
    if (blank)
        blank_outputs("locked: powering the outputs off");
    update_locked_notification();
}

void lx::shell::shell_app::on_lock_requested(unsigned reason) {
    if (!idle_.on_lock_requested()) {
        if (!idle_.can_lock())
            lx::trace::logger::global().log(lx::trace::level::info, "shell.lock",
                                            "lock requested, but no --lock-command is set");
        return;
    }
    launch_locker(reason == 1 ? "before sleep" : "logind Lock");
}

// ── Keyboard ──

void lx::shell::shell_app::on_key(unsigned serial, unsigned code, bool pressed) {
    if (!pressed) {
        if (code == repeat_code_) {
            repeat_code_ = 0;
            set_timer(t_repeat, 0);
        }
        return;
    }
    last_key_serial_ = serial;
    key_press k{};
    if (!launcher_.is_open() || !xkb_.press(code, k))
        return;
    launcher_.key(serial, k, xkb_.ctrl(), xkb_.shift());
    if (k.repeats && repeat_rate_ > 0 && launcher_.is_open()) {
        repeat_code_ = code;
        repeat_serial_ = serial;
        set_timer(t_repeat, now_ns() + static_cast<long long>(repeat_delay_) * 1000000LL);
    } else {
        repeat_code_ = 0;
        set_timer(t_repeat, 0);
    }
    if (!launcher_.is_open())
        set_apps_all(false);
}

// ── Launching ──

void lx::shell::shell_app::ensure_catalog() {
    if (catalog_.scanned() && !catalog_.stale())
        return;
    catalog_.scan();
    launcher_.catalog_changed();
    set_timer(t_debounce, 0);
}

void lx::shell::shell_app::open_launcher() {
    ensure_catalog();
    // On the active window's output, else the one whose panel was pointed at last (M13),
    // sized to its logical rect (xdg-output).
    output_record* o = launcher_output();
    const int w = o && o->logical.width > 0 ? o->logical.width : 1024;
    const int h = o && o->logical.height > 0 ? o->logical.height : 768;
    if (!launcher_.open(w, h, k_panel_height, o ? o->output : nullptr)) {
        shell_log(lx::trace::level::warn, "launcher: cannot open");
        return;
    }
    if (o && shell_debug()) {
        char line[96];
        std::snprintf(line, sizeof(line), "launcher on output %s", o->connector);
        shell_log(lx::trace::level::debug, line);
    }
    set_apps_all(true);
    // A panel notice belongs to the closed launcher; the launcher has its own status row.
    set_notice_all(nullptr);
    set_timer(t_notice, 0);
}

void lx::shell::shell_app::close_launcher() {
    launcher_.close();
    set_apps_all(false);
    repeat_code_ = 0;
    set_timer(t_repeat, 0);
    set_timer(t_debounce, 0);
}

void lx::shell::shell_app::toggle_launcher() {
    if (launcher_.is_open())
        close_launcher();
    else
        open_launcher();
}

void lx::shell::shell_app::show_notice(const char* text) {
    if (launcher_.is_open()) {
        launcher_.set_notice(text);
        return;
    }
    set_notice_all(text);
    set_timer(t_notice, now_ns() + k_notice_ns);
}

void lx::shell::shell_app::report_failure(const char* id, const char* name, launch_failure f,
                                          const char* detail, int error_number, const char* unit) {
    char reason[160];
    if (f == launch_failure::exec_error || f == launch_failure::spawn_error)
        std::snprintf(reason, sizeof(reason), "%s", std::strerror(error_number));
    else if (f == launch_failure::systemd_error)
        std::snprintf(reason, sizeof(reason), "systemd error %s", detail ? detail : "");
    else
        std::snprintf(reason, sizeof(reason), "%s", launch_failure_text(f));
    char text[320];
    std::snprintf(text, sizeof(text), "Could not start %s: %s", name && *name ? name : id, reason);
    show_notice(text);
    // Once per (desktop ID, reason) per catalog generation: a broken entry clicked ten
    // times is one line.
    if (warned_gen_ != catalog_.generation()) {
        warned_.clear();
        warned_gen_ = catalog_.generation();
    }
    for (const auto& w : warned_)
        if (w.first == id && w.second == f)
            return;
    warned_.emplace_back(id, f);
    char line[640];
    if (unit && *unit)
        std::snprintf(line, sizeof(line), "apps: %s (%s): %s (%s; journalctl --user -u '%s')",
                      text, id, reason, detail ? detail : "", unit);
    else
        std::snprintf(line, sizeof(line), "apps: %s (%s)", text, id);
    apps_log(lx::trace::level::warn, line);
}

void lx::shell::shell_app::on_outcome(const launch_outcome& o) {
    if (o.failure != launch_failure::none) {
        report_failure(o.id, o.name, o.failure, o.detail, o.error_number, o.unit);
        return;
    }
    char line[400];
    if (o.forked && o.unit[0])
        std::snprintf(line, sizeof(line), "apps: launched %s pid=%d in %s", o.id, o.pid, o.unit);
    else if (o.forked)
        std::snprintf(line, sizeof(line), "apps: launched %s pid=%d (double fork)", o.id, o.pid);
    else
        std::snprintf(line, sizeof(line), "apps: launched %s as %s", o.id, o.unit);
    apps_log(lx::trace::level::info, line);
}

void lx::shell::shell_app::launch_entry(unsigned entry, unsigned serial) {
    const desktop_entry* e = catalog_.entry(entry);
    if (!e)
        return;
    const char* home = std::getenv("HOME");
    auto p = std::make_unique<prepared_launch>();
    const launch_failure f = prepare_entry_launch(*e, catalog_, home ? home : "/", *p);
    if (f != launch_failure::none) {
        char id[128];
        std::snprintf(id, sizeof(id), "%.*s", static_cast<int>(e->id.size()), e->id.data());
        report_failure(id, e->name.data(), f, nullptr, 0, nullptr);
        return; // the launcher stays open, showing why
    }
    // The token request names the launcher's surface: it is sent before the surface goes.
    begin_launch(std::move(p), serial, launcher_.surface());
    close_launcher();
}

void lx::shell::shell_app::launch_terminal(unsigned serial) {
    const char* home = std::getenv("HOME");
    ensure_catalog();
    auto p = std::make_unique<prepared_launch>();
    const launch_failure f = prepare_terminal_launch(catalog_, home ? home : "/", *p);
    if (f != launch_failure::none) {
        report_failure("terminal", "a terminal", f, nullptr, 0, nullptr);
        return;
    }
    void* surface = launcher_.is_open() ? launcher_.surface()
                    : pointer_panel_       ? pointer_panel_->surface()
                                           : nullptr;
    for (auto& o : outputs_)
        if (!surface && o->bar)
            surface = o->bar->surface();
    begin_launch(std::move(p), serial, surface);
    if (launcher_.is_open())
        close_launcher();
}

void lx::shell::shell_app::begin_launch(std::unique_ptr<prepared_launch> p, unsigned serial,
                                        void* surface) {
#if defined(LUMEN_SHELL_ACTIVATION)
    if (activation_ && seat_) {
        for (auto& t : tokens_) {
            if (t.token)
                continue;
            // The triggering input's serial: a key press, a click or a grab activation —
            // each a press delivered to this client, so the compositor finds the token
            // grantable and the new window may take focus.
            auto* tok = xdg_activation_v1_get_activation_token(
                static_cast<xdg_activation_v1*>(activation_));
            xdg_activation_token_v1_add_listener(tok, &k_token_listener, this);
            xdg_activation_token_v1_set_serial(tok, serial, static_cast<wl_seat*>(seat_));
            xdg_activation_token_v1_set_app_id(tok, p->id);
            if (surface)
                xdg_activation_token_v1_set_surface(tok, static_cast<wl_surface*>(surface));
            xdg_activation_token_v1_commit(tok);
            t.token = tok;
            t.launch = std::move(p);
            t.deadline_ns = now_ns() + k_token_timeout_ns;
            long long earliest = 0;
            for (const auto& o : tokens_)
                if (o.token && (!earliest || o.deadline_ns < earliest))
                    earliest = o.deadline_ns;
            set_timer(t_token, earliest);
            return;
        }
    }
#else
    (void)serial;
    (void)surface;
#endif
    start_launch(*p, nullptr);
}

void lx::shell::shell_app::token_done(pending_token& t, const char* token) {
#if defined(LUMEN_SHELL_ACTIVATION)
    xdg_activation_token_v1_destroy(static_cast<xdg_activation_token_v1*>(t.token));
#endif
    t.token = nullptr;
    std::unique_ptr<prepared_launch> p = std::move(t.launch);
    t.deadline_ns = 0;
    if (p)
        start_launch(*p, token);
}

void lx::shell::shell_app::expire_tokens(long long now) {
    long long earliest = 0;
    for (auto& t : tokens_) {
        if (!t.token)
            continue;
        if (t.deadline_ns <= now) {
            if (shell_debug())
                apps_log(lx::trace::level::debug,
                         "apps: no activation token within 100 ms: launching without one");
            token_done(t, nullptr);
        } else if (!earliest || t.deadline_ns < earliest) {
            earliest = t.deadline_ns;
        }
    }
    deadline_[t_token] = earliest;
}

void lx::shell::shell_app::start_launch(const prepared_launch& p, const char* token) {
    const char* display = config_.display;
    if (!display || !*display)
        display = std::getenv("WAYLAND_DISPLAY");
    if (!display || !*display)
        display = "wayland-0";
    if (shell_debug()) {
        char line[PATH_MAX + 128];
        std::snprintf(line, sizeof(line), "apps: starting %s (%s) token=%s", p.id, p.program,
                      token ? "yes" : "no");
        apps_log(lx::trace::level::debug, line);
    }
    apps_.start(p, token ? token : "", display);
}

// ── Main loop ──

int lx::shell::shell_app::run() {
    // Signals arrive as readable data on an fd the loop already polls: the handler cannot
    // interrupt a libwayland call halfway, and shutdown is an ordinary loop exit.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0)
        return fail("sigprocmask failed");
    const int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (sfd < 0)
        return fail("signalfd failed");
    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    // zlm_shell_v1 v5: lock_state is sent right after the bind, before attach returns.
    bridge_.set_lock_handlers(
        [](unsigned state, void* user) { static_cast<shell_app*>(user)->on_lock_state(state); },
        [](unsigned reason, void* user) {
            static_cast<shell_app*>(user)->on_lock_requested(reason);
        },
        this);

    auto* d = wl_display_connect(config_.display);
    if (!d) {
        close(sfd);
        char line[160];
        std::snprintf(line, sizeof(line), "cannot connect to the Wayland display %s",
                      config_.display ? config_.display : "($WAYLAND_DISPLAY)");
        return fail(line);
    }
    display_ = d;
    auto* reg = wl_display_get_registry(d);
    registry_ = reg;
    wl_registry_add_listener(reg, &k_registry_listener, this);
    // First roundtrip: the globals, bound as announced. Second: the binds themselves —
    // a compositor that does not take this process for its shell answers zlm_shell_v1
    // with a protocol error here, before anything is drawn.
    if (wl_display_roundtrip(d) < 0 || wl_display_roundtrip(d) < 0) {
        describe_connection_error(d, "zlm_shell_v1 bind rejected (is this the compositor's "
                                     "configured shell binary? see its 'privileged global' "
                                     "warning)");
        close(sfd);
        return 1;
    }
    if (!compositor_ || compositor_version_ < 4 || !shm_ || !layer_shell_ || !shell_) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "missing globals:%s%s%s%s", !compositor_ || compositor_version_ < 4
                          ? " wl_compositor(v4+)" : "",
                      !shm_ ? " wl_shm" : "", !layer_shell_ ? " zwlr_layer_shell_v1" : "",
                      !shell_ ? " zlm_shell_v1" : "");
        close(sfd);
        return fail(line);
    }
    if (!seat_)
        shell_log(lx::trace::level::warn, "no wl_seat: the panel takes no input");
    if (!activation_)
        apps_log(lx::trace::level::info,
                 "apps: no xdg_activation_v1: launched windows get no activation token");

    if (auto attached = bridge_.attach(shell_); !attached) {
        lx::trace::logger::global().log_error(attached.get_error(), k_shell_category);
        close(sfd);
        return 1;
    }
    // One panel per output (M13): those known now get theirs here, later ones at their
    // first wl_output.done.
    panels_enabled_ = true;
    for (auto& o : outputs_)
        if (o->done)
            output_ready(*o);
    catalog_.configure(catalog_config::from_env());
    apps_.configure_from_env();
    apps_.set_outcome_handler(
        [](const launch_outcome& o, void* user) { static_cast<shell_app*>(user)->on_outcome(o); },
        this);
    launcher_.init({.compositor = compositor_,
                    .shm = shm_,
                    .layer_shell = layer_shell_,
                    .viewporter = viewporter_,
                    .fractional = fractional_},
                   catalog_,
                   [](unsigned entry, unsigned serial, void* user) {
                       static_cast<shell_app*>(user)->launch_entry(entry, serial);
                   },
                   [](void* user) {
                       auto& a = *static_cast<shell_app*>(user);
                       a.set_apps_all(false);
                       a.repeat_code_ = 0;
                       a.set_timer(t_repeat, 0);
                   },
                   this);
    // Shell keybindings (zlm_shell_v1 v4). The Super tap is observed — the key still reaches
    // the focused window — and fires only when Super was pressed and released alone.
    if (bridge_.can_grab_keys()) {
        const auto on_super = [](unsigned, unsigned, void* user) {
            static_cast<shell_app*>(user)->toggle_launcher();
        };
        const auto on_terminal = [](unsigned serial, unsigned, void* user) {
            static_cast<shell_app*>(user)->launch_terminal(serial);
        };
        (void)bridge_.grab_key(k_super_l, 0, policy_bridge::k_grab_tap, on_super, this);
        (void)bridge_.grab_key(k_super_r, 0, policy_bridge::k_grab_tap, on_super, this);
        (void)bridge_.grab_key(k_return, policy_bridge::k_mod_super, 0, on_terminal, this);
        // Super+L locks, when there is a locker to start (Lumen ships none).
        if (idle_.can_lock()) {
            const auto on_lock = [](unsigned, unsigned, void* user) {
                auto& a = *static_cast<shell_app*>(user);
                if (a.idle_.on_lock_requested())
                    a.launch_locker("Super+L");
            };
            (void)bridge_.grab_key(k_key_l, policy_bridge::k_mod_super, 0, on_lock, this);
        }
    } else {
        (void)bridge_.grab_key(0, 0, 0, nullptr, nullptr); // logs the absence once
        shell_log(lx::trace::level::info, "launcher only from the panel (no key grabs)");
    }
    setup_idle();
    if (!bridge_.has_lock_state()) {
        lx::trace::logger::global().log(lx::trace::level::info, "shell.lock",
                                        "compositor below zlm_shell_v1 v5: no session-lock "
                                        "integration");
    } else if (idle_.can_lock()) {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "lock command: %s (Super+L, logind Lock, before sleep%s)",
                      idle_.config().lock_command,
                      idle_.config().lock_on_blank && idle_.config().enabled &&
                              idle_.config().blank_after_s
                          ? ", before blanking"
                          : "");
        lx::trace::logger::global().log(lx::trace::level::info, "shell.lock", line);
    }
    update_locked_notification(); // started behind a lock (a restart while locked)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "connected to %s; panel requested",
                      config_.display ? config_.display : "$WAYLAND_DISPLAY");
        shell_log(lx::trace::level::info, line);
    }

    int code = 0;
    constexpr unsigned k_fixed = 4;
    pollfd fds[k_fixed + app_launcher::k_max_poll];
    for (;;) {
        // The canonical libwayland client loop: dispatch what is queued, flush, then
        // sleep until the socket or a signal has something.
        while (wl_display_prepare_read(d) != 0) {
            if (wl_display_dispatch_pending(d) < 0) {
                code = describe_connection_error(d, "dispatch failed", bridge_.logout_requested());
                break;
            }
        }
        if (code)
            break;
        {
            // The model's changes are taken once and handed to every panel. A snapshot in
            // progress keeps them until it is done.
            task_model& model = bridge_.model();
            const unsigned changes = model.in_snapshot() ? 0u : model.take_changes();
            for (auto& o : outputs_)
                if (o->bar)
                    o->bar->after_dispatch(changes);
        }
        launcher_.after_dispatch();
        for (auto& o : outputs_) {
            if (!o->bar || !o->bar->closed())
                continue;
            // The compositor closed it (its output went), or a buffer could not be made:
            // that output goes without a panel; the others stay.
            char line[128];
            std::snprintf(line, sizeof(line), "panel on output %s closed", o->connector);
            shell_log(lx::trace::level::warn, line);
            if (o->bar.get() == pointer_panel_) {
                pointer_panel_ = nullptr;
                pointer_target_ = pointer_target::none;
            }
            o->bar->destroy();
            o->bar.reset();
        }
        // The catalog is read once a panel is on screen: it never delays the panel.
        if (!catalog_scanned_after_frame_ && any_panel_drawn()) {
            catalog_scanned_after_frame_ = true;
            if (!catalog_.scanned())
                catalog_.scan();
        }
        fds[0] = {wl_display_get_fd(d), POLLIN, 0};
        if (wl_display_flush(d) < 0) {
            if (errno == EAGAIN) {
                fds[0].events |= POLLOUT;
            } else {
                wl_display_cancel_read(d);
                code = describe_connection_error(d, "flush failed", bridge_.logout_requested());
                break;
            }
        }
        fds[1] = {sfd, POLLIN, 0};
        fds[2] = {catalog_.watch_fd(), POLLIN, 0};
        fds[3] = {timer_fd_, POLLIN, 0};
        int timeout_ms = -1;
        const unsigned na = apps_.poll_fds(fds + k_fixed, app_launcher::k_max_poll, timeout_ms);
        timespec ts{};
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        }
        const int n = ppoll(fds, k_fixed + na, timeout_ms >= 0 ? &ts : nullptr, nullptr);
        if (n < 0 && errno != EINTR) {
            wl_display_cancel_read(d);
            code = fail("ppoll failed");
            break;
        }
        if (n > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            if (wl_display_read_events(d) < 0) {
                code = describe_connection_error(d, "compositor gone", bridge_.logout_requested());
                break;
            }
        } else {
            wl_display_cancel_read(d);
        }
        if (wl_display_dispatch_pending(d) < 0) {
            code = describe_connection_error(d, "dispatch failed", bridge_.logout_requested());
            break;
        }
        if (n > 0 && (fds[1].revents & POLLIN)) {
            signalfd_siginfo si{};
            if (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                char line[64];
                std::snprintf(line, sizeof(line), "signal %u: exiting", si.ssi_signo);
                shell_log(lx::trace::level::info, line);
                break;
            }
        }
        if (n > 0 && fds[2].fd >= 0 && (fds[2].revents & POLLIN)) {
            (void)catalog_.on_watch_readable();
            // Rescanned when the launcher next opens; while it is open, once the burst
            // settles.
            if (launcher_.is_open() && catalog_.stale())
                set_timer(t_debounce, now_ns() + k_debounce_ns);
        }
        if (n > 0 && fds[3].fd >= 0 && (fds[3].revents & POLLIN))
            on_timer();
        // The bus is processed every time it is open (its timeouts are due without fd
        // activity); pipes only with events.
        if (na > 0)
            apps_.dispatch(fds + k_fixed, na);
    }
    close(sfd);
    teardown();
    return code;
}

void lx::shell::shell_app::teardown() {
    auto* d = static_cast<wl_display*>(display_);
    if (!d)
        return;
    for (auto& t : tokens_) {
#if defined(LUMEN_SHELL_ACTIVATION)
        if (t.token)
            xdg_activation_token_v1_destroy(static_cast<xdg_activation_token_v1*>(t.token));
#endif
        t.token = nullptr;
        t.launch.reset();
    }
    launcher_.close();
    pointer_panel_ = nullptr;
    for (auto& o : outputs_)
        if (o->bar)
            o->bar->destroy();
    bridge_.detach();
    teardown_idle();
    if (cursor_device_)
        wp_cursor_shape_device_v1_destroy(static_cast<wp_cursor_shape_device_v1*>(cursor_device_));
    release_pointer(pointer_);
    release_keyboard(keyboard_);
#if defined(LUMEN_SHELL_ACTIVATION)
    if (activation_)
        xdg_activation_v1_destroy(static_cast<xdg_activation_v1*>(activation_));
#endif
#if defined(LUMEN_SHELL_FRACTIONAL)
    if (viewporter_)
        wp_viewporter_destroy(static_cast<wp_viewporter*>(viewporter_));
    if (fractional_)
        wp_fractional_scale_manager_v1_destroy(
            static_cast<wp_fractional_scale_manager_v1*>(fractional_));
#endif
    if (cursor_shape_)
        wp_cursor_shape_manager_v1_destroy(static_cast<wp_cursor_shape_manager_v1*>(cursor_shape_));
    if (shell_)
        zlm_shell_v1_destroy(static_cast<zlm_shell_v1*>(shell_));
    if (layer_shell_)
        zwlr_layer_shell_v1_destroy(static_cast<zwlr_layer_shell_v1*>(layer_shell_));
    for (auto& o : outputs_)
        destroy_output(*o);
    outputs_.clear();
    if (xdg_output_manager_)
        zxdg_output_manager_v1_destroy(static_cast<zxdg_output_manager_v1*>(xdg_output_manager_));
    if (seat_) {
        if (wl_seat_get_version(static_cast<wl_seat*>(seat_)) >= WL_SEAT_RELEASE_SINCE_VERSION)
            wl_seat_release(static_cast<wl_seat*>(seat_));
        else
            wl_seat_destroy(static_cast<wl_seat*>(seat_));
    }
    if (shm_)
        wl_shm_destroy(static_cast<wl_shm*>(shm_));
    if (compositor_)
        wl_compositor_destroy(static_cast<wl_compositor*>(compositor_));
    if (registry_)
        wl_registry_destroy(static_cast<wl_registry*>(registry_));
    (void)wl_display_flush(d);
    wl_display_disconnect(d);
    display_ = registry_ = compositor_ = shm_ = seat_ = layer_shell_ = shell_ = cursor_shape_ =
        xdg_output_manager_ = activation_ = pointer_ = keyboard_ = cursor_device_ = viewporter_ =
            fractional_ = nullptr;
    if (timer_fd_ >= 0)
        close(timer_fd_);
    timer_fd_ = -1;
}

#else // !LUMEN_SHELL_WAYLAND

struct lx::shell::shell_app::events {};

int lx::shell::shell_app::fail(const char* what) {
    shell_log(lx::trace::level::error, what);
    return 1;
}

int lx::shell::shell_app::run() {
    lx::trace::logger::global().log_error(lx::not_implemented("lx::shell::shell_app::run"),
                                          k_shell_category);
    return 1;
}

void lx::shell::shell_app::teardown() {}
void lx::shell::shell_app::setup_idle() {}
void lx::shell::shell_app::teardown_idle() {}
void lx::shell::shell_app::blank_outputs(const char*) {}
void lx::shell::shell_app::on_lock_state(unsigned) {}
void lx::shell::shell_app::on_lock_requested(unsigned) {}
void lx::shell::shell_app::launch_locker(const char*) {}
void lx::shell::shell_app::update_locked_notification() {}

#endif
