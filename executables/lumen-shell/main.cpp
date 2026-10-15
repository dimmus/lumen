#include <cstdio>
#include <cstdlib>
#include <cstring>

import lx.shell;

// lumen-shell [--display NAME]
//
// The desktop shell: the top panel and its task list. Started after lumen-compositor (the
// systemd unit orders it), it binds zlm_shell_v1 — which the compositor accepts only from
// the shell binary it was configured with — and exits 1 when the compositor refuses it or
// goes away, so the unit restarts it.
int main(int argc, char* argv[]) {
    lx::shell::shell_config cfg{};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            cfg.display = argv[++i];
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: lumen-shell [--display NAME]   (default: $WAYLAND_DISPLAY)\n");
            return 0;
        } else {
            std::fprintf(stderr, "lumen-shell: unknown argument '%s' (see --help)\n", argv[i]);
            return 2;
        }
    }
    lx::shell::shell_app shell{cfg};
    return shell.run();
}
