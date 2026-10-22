#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

import lx.portal;

// xdg-desktop-portal-lumen [-r|--replace] [-v|--verbose]
// xdg-desktop-portal-lumen --choose
//
// The Lumen portal backend (design-app-compat-capture Part E): Screenshot and PickColor for
// xdg-desktop-portal on a Lumen session (deploy/portal/lumen.portal, lumen-portals.conf),
// started by D-Bus activation through its systemd user unit, or by run-desktop.sh for a
// build-tree session. `--choose` is xdg-desktop-portal-wlr's ScreenCast source chooser
// (chooser_type=dmenu): the sources on stdin, the chosen line on stdout, exit 1 when the
// user declines. Logs go to stderr (LUMEN_LOG=debug for more).
int main(int argc, char* argv[]) {
    lx::portal::service_options options{};
    bool choose = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "-r") == 0 || std::strcmp(a, "--replace") == 0) {
            options.replace = true;
        } else if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--verbose") == 0) {
            setenv("LUMEN_LOG", "debug", 0);
        } else if (std::strcmp(a, "--choose") == 0) {
            choose = true;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::printf("usage: xdg-desktop-portal-lumen [-r|--replace] [-v|--verbose]\n"
                        "       xdg-desktop-portal-lumen --choose\n"
                        "  -r, --replace   take over from a running instance\n"
                        "  -v, --verbose   debug logging (LUMEN_LOG=debug)\n"
                        "  --choose        ScreenCast source chooser for xdg-desktop-portal-wlr:\n"
                        "                  sources on stdin (one per line), the choice on stdout\n");
            return 0;
        } else {
            std::fprintf(stderr, "xdg-desktop-portal-lumen: unknown argument '%s' (see --help)\n", a);
            return 2;
        }
    }
    if (choose) {
        std::string input;
        char buf[4096];
        ssize_t n = 0;
        while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0 && input.size() < (1u << 20))
            input.append(buf, static_cast<std::size_t>(n));
        std::string chosen;
        const int status = lx::portal::run_chooser(input, chosen);
        if (status == 0) {
            std::fputs(chosen.c_str(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        return status;
    }
    return lx::portal::run_service(options);
}
