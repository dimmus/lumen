#include <csignal>
#include <cstdlib>

import lx.compositor;

namespace {
lx::compositor::compositor* g_compositor = nullptr;

// Stopping the loop lets teardown run, which is what restores the console mode on a TTY.
void on_signal(int) {
    if (g_compositor)
        g_compositor->request_stop();
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

    if (auto started = comp.start(); !started)
        return 1;

    const int code = comp.run();
    g_compositor = nullptr;
    return code;
}
