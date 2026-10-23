module;

// The action namespace (design-session-and-wm C.3): one name per thing a key binding,
// `lumen-ctl action` or the control socket's RunAction can make happen, with who executes it
// (the compositor itself, or the shell over zlm_shell_v1 v7 `action`), what argument it
// takes and the rules a binding to it must follow. Execution lives with the owner; this
// table is what both processes and lumen-ctl agree on.
//
// `available` says whether this build executes the action (all of them since Part 5's
// notifications). A binding to an unavailable one is warned about and not installed: a key
// that does nothing must keep reaching applications.

export module lx.config:actions;

import std;
import :diagnostics;

export namespace lx::config {

enum class action_owner : unsigned char { none, compositor, shell };
enum class action_arg : unsigned char {
    none,
    /// A 1-based index (a workspace number).
    index,
};

enum class action_id : unsigned short {
    none,
    // shell
    shell_launcher,
    shell_terminal,
    shell_lock,
    switcher_next,
    switcher_previous,
    switcher_next_app,
    switcher_previous_app,
    notifications_toggle_dnd,
    notifications_dismiss,
    notifications_dismiss_all,
    // compositor: windows
    window_close,
    window_snap_left,
    window_snap_right,
    window_snap_up,
    window_snap_down,
    window_maximize_toggle,
    window_fullscreen_toggle,
    window_minimize,
    window_sticky_toggle,
    window_move_to_output_left,
    window_move_to_output_right,
    window_move_to_output_up,
    window_move_to_output_down,
    window_move_keyboard,
    window_resize_keyboard,
    window_move_interactive,
    window_resize_interactive,
    // compositor: workspaces
    workspace_switch,
    workspace_move_window,
    workspace_next,
    workspace_previous,
    workspace_move_window_next,
    workspace_move_window_previous,
    // compositor: keyboard, session
    keyboard_next_layout,
    keyboard_previous_layout,
    session_logout,
    session_reload_config,
    count_,
};

struct action_info {
    action_id id = action_id::none;
    const char* name = "";
    action_owner owner = action_owner::none;
    action_arg arg = action_arg::none;
    /// Bound only with a modifier; it holds the keyboard while that modifier is down
    /// (zlm v7 held action, design K.2).
    bool held = false;
    /// May run while the session is locked (`allow-when-locked`, RunAction while locked).
    bool lock_safe = false;
    /// Only from a pointer button binding (an interactive move or resize needs the press).
    bool pointer = false;
    /// This build executes it.
    bool available = false;
    const char* summary = "";
};

/// Every action, in `action_id` order.
[[nodiscard]] std::span<const action_info> actions();
[[nodiscard]] const action_info& action(action_id id);
/// By name (exact); null when there is none.
[[nodiscard]] const action_info* find_action(std::string_view name);
/// The nearest name within edit distance 2, for "did you mean"; null when none is near.
[[nodiscard]] const char* suggest_action(std::string_view name);

} // namespace lx::config

namespace {

using lx::config::action_arg;
using lx::config::action_id;
using lx::config::action_info;
using lx::config::action_owner;

constexpr action_owner k_shell = action_owner::shell;
constexpr action_owner k_comp = action_owner::compositor;

constexpr action_info k_actions[] = {
    {action_id::none, "none", action_owner::none, action_arg::none, false, true, false, true,
     "removes a binding"},
    {action_id::shell_launcher, "shell.launcher", k_shell, action_arg::none, false, false, false,
     true, "open or close the application launcher"},
    {action_id::shell_terminal, "shell.terminal", k_shell, action_arg::none, false, false, false,
     true, "start a terminal"},
    {action_id::shell_lock, "shell.lock", k_shell, action_arg::none, false, false, false, true,
     "lock the screen (idle.lock-command)"},
    {action_id::switcher_next, "switcher.next", k_shell, action_arg::none, true, false, false,
     true, "window switcher: next window"},
    {action_id::switcher_previous, "switcher.previous", k_shell, action_arg::none, true, false,
     false, true, "window switcher: previous window"},
    {action_id::switcher_next_app, "switcher.next-app", k_shell, action_arg::none, true, false,
     false, true, "window switcher: next window of the same application"},
    {action_id::switcher_previous_app, "switcher.previous-app", k_shell, action_arg::none, true,
     false, false, true, "window switcher: previous window of the same application"},
    {action_id::notifications_toggle_dnd, "notifications.toggle-dnd", k_shell, action_arg::none,
     false, false, false, true, "toggle do-not-disturb"},
    {action_id::notifications_dismiss, "notifications.dismiss", k_shell, action_arg::none, false,
     false, false, true, "dismiss the newest notification"},
    {action_id::notifications_dismiss_all, "notifications.dismiss-all", k_shell,
     action_arg::none, false, false, false, true, "dismiss every notification"},
    {action_id::window_close, "window.close", k_comp, action_arg::none, false, false, false, true,
     "ask the focused window to close"},
    {action_id::window_snap_left, "window.snap-left", k_comp, action_arg::none, false, false,
     false, true, "snap the focused window left (halves, quarters, the output to the left)"},
    {action_id::window_snap_right, "window.snap-right", k_comp, action_arg::none, false, false,
     false, true, "snap the focused window right (halves, quarters, the output to the right)"},
    {action_id::window_snap_up, "window.snap-up", k_comp, action_arg::none, false, false, false,
     true, "maximize, or snap to a top quarter"},
    {action_id::window_snap_down, "window.snap-down", k_comp, action_arg::none, false, false,
     false, true, "restore, or minimize"},
    {action_id::window_maximize_toggle, "window.maximize-toggle", k_comp, action_arg::none, false,
     false, false, true, "maximize or restore the focused window"},
    {action_id::window_fullscreen_toggle, "window.fullscreen-toggle", k_comp, action_arg::none,
     false, false, false, true, "fullscreen or restore the focused window"},
    {action_id::window_minimize, "window.minimize", k_comp, action_arg::none, false, false, false,
     true, "minimize the focused window"},
    {action_id::window_sticky_toggle, "window.sticky-toggle", k_comp, action_arg::none, false,
     false, false, true, "show the focused window on every workspace of its output"},
    {action_id::window_move_to_output_left, "window.move-to-output-left", k_comp,
     action_arg::none, false, false, false, true, "move the focused window to the output left"},
    {action_id::window_move_to_output_right, "window.move-to-output-right", k_comp,
     action_arg::none, false, false, false, true, "move the focused window to the output right"},
    {action_id::window_move_to_output_up, "window.move-to-output-up", k_comp, action_arg::none,
     false, false, false, true, "move the focused window to the output above"},
    {action_id::window_move_to_output_down, "window.move-to-output-down", k_comp,
     action_arg::none, false, false, false, true, "move the focused window to the output below"},
    {action_id::window_move_keyboard, "window.move-keyboard", k_comp, action_arg::none, false,
     false, false, true, "move the focused window with the arrow keys or the pointer"},
    {action_id::window_resize_keyboard, "window.resize-keyboard", k_comp, action_arg::none, false,
     false, false, true, "resize the focused window with the arrow keys or the pointer"},
    {action_id::window_move_interactive, "window.move-interactive", k_comp, action_arg::none,
     false, false, true, true, "move the window under the pointer (a button binding)"},
    {action_id::window_resize_interactive, "window.resize-interactive", k_comp, action_arg::none,
     false, false, true, true, "resize the window under the pointer (a button binding)"},
    {action_id::workspace_switch, "workspace.switch", k_comp, action_arg::index, false, false,
     false, true, "switch to workspace N (of the focused output)"},
    {action_id::workspace_move_window, "workspace.move-window", k_comp, action_arg::index, false,
     false, false, true, "move the focused window to workspace N"},
    {action_id::workspace_next, "workspace.next", k_comp, action_arg::none, false, false, false,
     true, "switch to the next workspace"},
    {action_id::workspace_previous, "workspace.previous", k_comp, action_arg::none, false, false,
     false, true, "switch to the previous workspace"},
    {action_id::workspace_move_window_next, "workspace.move-window-next", k_comp,
     action_arg::none, false, false, false, true, "move the focused window to the next workspace"},
    {action_id::workspace_move_window_previous, "workspace.move-window-previous", k_comp,
     action_arg::none, false, false, false, true,
     "move the focused window to the previous workspace"},
    {action_id::keyboard_next_layout, "keyboard.next-layout", k_comp, action_arg::none, false,
     false, false, true, "switch to the next keyboard layout"},
    {action_id::keyboard_previous_layout, "keyboard.previous-layout", k_comp, action_arg::none,
     false, false, false, true, "switch to the previous keyboard layout"},
    {action_id::session_logout, "session.logout", k_comp, action_arg::none, false, true, false,
     true, "log out"},
    {action_id::session_reload_config, "session.reload-config", k_comp, action_arg::none, false,
     true, false, true, "read the configuration again"},
};

static_assert(std::size(k_actions) == static_cast<std::size_t>(action_id::count_),
              "one action_info per action_id, in order");

} // namespace

std::span<const action_info> lx::config::actions() { return k_actions; }

const action_info& lx::config::action(action_id id) {
    const auto i = static_cast<std::size_t>(id);
    return i < std::size(k_actions) ? k_actions[i] : k_actions[0];
}

const action_info* lx::config::find_action(std::string_view name) {
    for (const action_info& a : k_actions)
        if (name == a.name)
            return &a;
    return nullptr;
}

const char* lx::config::suggest_action(std::string_view name) {
    const char* best = nullptr;
    unsigned best_d = 3;
    for (const action_info& a : k_actions) {
        const unsigned d = edit_distance(name, a.name, 2);
        if (d < best_d) {
            best_d = d;
            best = a.name;
        }
    }
    return best;
}
