#include <signal.h>
#include <stdio.h>

import std;
import lx.compositor;

namespace {
lx::compositor::compositor* g_compositor = nullptr;

// Stopping the loop lets teardown run, which is what restores the console mode on a TTY.
void on_signal(int) {
    if (g_compositor)
        g_compositor->request_stop();
}

// SIGHUP: re-read the configuration (`systemctl --user reload lumen-compositor`, which
// Type=notify-reload maps to SIGHUP and answers from RELOADING=1 / READY=1).
void on_reload(int) {
    if (g_compositor)
        g_compositor->request_reload();
}

// Debug aid: SIGUSR1 writes the next (fully repainted) frame as a PPM (see
// compositor::request_screenshot).
void on_screenshot(int) {
    if (g_compositor)
        g_compositor->request_screenshot();
}
} // namespace

int main(int argc, char* argv[]) {
    lx::compositor::config cfg{};
    static char output_scales[512] = {};
    // lumen-ctl and run-desktop.sh talk to the compositor here ($XDG_RUNTIME_DIR/<socket>.varlink).
    cfg.control_socket = true;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] != '-') {
            cfg.socket_name = arg;
        } else if (arg[1] == 'f' && arg[2] == '\0' && i + 1 < argc) {
            cfg.target_fps = std::atof(argv[++i]);
        } else if (std::strcmp(arg, "--decorations=server") == 0) {
            cfg.server_side_decorations = true;
        } else if (std::strcmp(arg, "--decorations=client") == 0) {
            // No xdg-decoration global: every client draws its own frame.
            cfg.server_side_decorations = false;
        } else if (std::strncmp(arg, "--decorations", 13) == 0) {
            std::fprintf(stderr, "lumen-compositor: --decorations=server|client\n");
            return 2;
        } else if (std::strcmp(arg, "--ping-timeout") == 0) {
            // xdg_wm_base.ping timeout in ms before a client is shown as not responding;
            // 0 disables pinging.
            char* end = nullptr;
            const long ms = i + 1 < argc ? std::strtol(argv[i + 1], &end, 10) : -1;
            if (ms < 0 || !end || *end != '\0') {
                std::fprintf(stderr, "lumen-compositor: --ping-timeout MS\n");
                return 2;
            }
            cfg.ping_timeout_ms = static_cast<unsigned>(ms);
            ++i;
        } else if (std::strcmp(arg, "--scale") == 0) {
            // Output scale, fractional allowed (1.25, 1.5, 2): rounded to n/120.
            char* end = nullptr;
            const double scale = i + 1 < argc ? std::strtod(argv[i + 1], &end) : 0.0;
            if (!(scale >= 0.25 && scale <= 8.0) || !end || *end != '\0') {
                std::fprintf(stderr, "lumen-compositor: --scale S (0.25 .. 8, e.g. 1.25)\n");
                return 2;
            }
            cfg.output_scale = scale;
            ++i;
        } else if (std::strcmp(arg, "--output-scale") == 0) {
            // One connector's scale over --scale: NAME=S (repeatable), e.g. Virtual-2=1.5.
            const char* spec = i + 1 < argc ? argv[i + 1] : nullptr;
            const char* eq = spec ? std::strchr(spec, '=') : nullptr;
            char* end = nullptr;
            const double scale = eq ? std::strtod(eq + 1, &end) : 0.0;
            if (!eq || eq == spec || !(scale >= 0.25 && scale <= 8.0) || !end || *end != '\0' ||
                std::strlen(output_scales) + std::strlen(spec) + 2 > sizeof(output_scales)) {
                std::fprintf(stderr, "lumen-compositor: --output-scale NAME=S (0.25 .. 8)\n");
                return 2;
            }
            if (output_scales[0])
                std::strcat(output_scales, ",");
            std::strcat(output_scales, spec);
            cfg.output_scales = output_scales;
            ++i;
        } else if (std::strcmp(arg, "--virtual-outputs") == 0) {
            // Without a KMS display: WxH[@S][,WxH[@S]...] virtual outputs (tests).
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lumen-compositor: --virtual-outputs WxH[@S][,...]\n");
                return 2;
            }
            cfg.virtual_outputs = argv[++i];
        } else if (std::strcmp(arg, "--session") == 0) {
            // Run as the session's display server under systemd (lumen-compositor.service,
            // started by lumen-session): export the session environment, then READY=1.
            cfg.session_mode = true;
        } else if (std::strcmp(arg, "--no-control") == 0) {
            // No control socket (lumen-ctl cannot reach this compositor).
            cfg.control_socket = false;
        } else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            std::printf(
                "usage: lumen-compositor [SOCKET] [-f FPS] [--session] [--no-control]\n"
                "                        [--decorations=server|client] [--ping-timeout MS]\n"
                "                        [--scale S] [--output-scale NAME=S]\n"
                "                        [--virtual-outputs WxH[@S][,...]] [--shell PATH]\n"
                "  SOCKET          Wayland socket name (default lumen-0)\n"
                "  --session       the session's display server under systemd\n"
                "                  (lumen-compositor.service): exports the session environment\n"
                "                  to the user manager, then sends READY=1\n"
                "  --no-control    no control socket ($XDG_RUNTIME_DIR/SOCKET.varlink, lumen-ctl)\n"
                "Signals: SIGTERM/SIGINT stop, SIGHUP reloads the configuration, SIGUSR1 writes\n"
                "a screenshot of the next frame.\n"
                "Configuration: $XDG_CONFIG_HOME/lumen/config.toml over\n"
                "$XDG_CONFIG_DIRS/lumen/config.toml, watched and applied live\n"
                "(LUMEN_CONFIG=PATH: that file only; LUMEN_CONFIG=0: the defaults);\n"
                "lumen-ctl config check validates it, lumen-ctl config defaults documents it.\n");
            return 0;
        } else if (std::strcmp(arg, "--shell") == 0) {
            // The one binary zlm_shell_v1 binds are accepted from (default: lumen-shell
            // next to this executable).
            if (i + 1 >= argc) {
                std::fprintf(stderr, "lumen-compositor: --shell PATH\n");
                return 2;
            }
            cfg.shell_binary_path = argv[++i];
        }
    }

    lx::compositor::compositor comp{cfg};
    g_compositor = &comp;

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    struct sigaction shot {};
    shot.sa_handler = on_screenshot;
    sigemptyset(&shot.sa_mask);
    shot.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &shot, nullptr);
    struct sigaction hup {};
    hup.sa_handler = on_reload;
    sigemptyset(&hup.sa_mask);
    hup.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &hup, nullptr);

    if (auto started = comp.start(); !started)
        return 1;

    const int code = comp.run();
    g_compositor = nullptr;
    return code;
}
