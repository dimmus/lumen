module;

// xdg-desktop-portal-lumen (design-app-compat-capture Part E): the portal backend a Lumen
// session uses for org.freedesktop.impl.portal.Screenshot (version 3: Screenshot with the
// Screen, Window and Area targets, PickColor), and the source chooser
// xdg-desktop-portal-wlr runs for ScreenCast (`--choose`, dmenu protocol).
//
// D-Bus (sd-bus, user bus): bus name org.freedesktop.impl.portal.desktop.lumen, object
// /org/freedesktop/portal/desktop. Each call gets the frontend's Request object at its
// `handle` path (org.freedesktop.impl.portal.Request.Close cancels it, answering response
// 2). One request at a time — a second while one is interactive is answered 2 at once.
//
//   Screenshot, non-interactive (Screen or no target): every output captured, composed in
//     layout space at the largest scale, saved privately to $XDG_RUNTIME_DIR/lumen-portal/.
//   Screenshot, interactive or Area: the overlay over the frozen frame (drag an area, click
//     a screen, Enter for all, Escape → response 1); saved to $XDG_PICTURES_DIR/Screenshots.
//   Screenshot, Window: the window list; the chosen window is captured on its own (covered
//     or not), at its own scale.
//   PickColor: the overlay; the colour under the click, (r, g, b) in [0, 1] sRGB.
//
// Permission stays the frontend's (it asks the Access dialog for a non-interactive
// screenshot of an app without the permission); a locked session has no capture at all
// (the compositor stops the sessions) and the request answers 2.
//
// Single-threaded: one poll over the bus, a signalfd and — only while a request runs — its
// Wayland connection. Idle, the process sleeps in poll with no timeout.

export module lx.portal:service;

import std;

export namespace lx::portal {

inline constexpr const char* k_bus_name = "org.freedesktop.impl.portal.desktop.lumen";
inline constexpr const char* k_object_path = "/org/freedesktop/portal/desktop";
inline constexpr unsigned k_screenshot_version = 3;
/// Screen | Window | Area (no Active Window: nothing outside the shell knows which one is).
inline constexpr unsigned k_screenshot_targets = 1u | 2u | 4u;

struct service_options {
    /// Take the bus name over from a running instance (`-r`).
    bool replace = false;
};

/// Runs the backend until SIGINT/SIGTERM or the bus name is lost; the exit status.
[[nodiscard]] int run_service(const service_options& options);

/// `--choose`: shows `input` (xdpw's dmenu lines) and sets `chosen` to the chosen line.
/// 0 when one was chosen, 1 when declined, 2 when the chooser could not be shown.
[[nodiscard]] int run_chooser(std::string_view input, std::string& chosen);

} // namespace lx::portal
