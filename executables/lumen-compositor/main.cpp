#include <cstdlib>

import lx.compositor;

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

    if (auto started = comp.start(); !started)
        return 1;

    return comp.run();
}
