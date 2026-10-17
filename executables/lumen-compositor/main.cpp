#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

import lx.compositor;

namespace {
lx::compositor::compositor* g_compositor = nullptr;

// Stopping the loop lets teardown run, which is what restores the console mode on a TTY.
void on_signal(int) {
    if (g_compositor)
        g_compositor->request_stop();
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

    if (auto started = comp.start(); !started)
        return 1;

    const int code = comp.run();
    g_compositor = nullptr;
    return code;
}
