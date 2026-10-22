#include <stdio.h>

import std;
import lx.shell;

// lumen-shell [--display NAME] [--idle-policy shell|none] [--blank-after S]
//             [--blank-locked-after S] [--lock-command CMD] [--[no-]lock-on-blank]
//             [--background SPEC | --no-background]
//
// The desktop shell: the top panel and its task list, the launcher, and the default idle
// and lock policy (outputs off after --blank-after seconds without input, 600 by default; 0
// never; --idle-policy none leaves it to an external daemon; with --lock-command, Super+L,
// logind's Lock, before-sleep and the blank timeout start that locker). Started after
// lumen-compositor (the
// systemd unit orders it), it binds zlm_shell_v1 — which the compositor accepts only from
// the shell binary it was configured with — and exits 1 when the compositor refuses it or
// goes away, so the unit restarts it.
int main(int argc, char* argv[]) {
    lx::shell::shell_config cfg{};
    if (const char* env = std::getenv("LUMEN_BACKGROUND"); env && *env) {
        if (auto spec = lx::shell::parse_background(env))
            cfg.background = *spec;
        else
            std::fprintf(stderr, "lumen-shell: ignoring LUMEN_BACKGROUND '%s' (see --help)\n", env);
    }
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--display") == 0 && i + 1 < argc) {
            cfg.display = argv[++i];
        } else if ((std::strcmp(a, "--idle-policy") == 0 && i + 1 < argc &&
                    lx::shell::parse_idle_policy(argv[++i], cfg.idle.enabled)) ||
                   (std::strncmp(a, "--idle-policy=", 14) == 0 &&
                    lx::shell::parse_idle_policy(a + 14, cfg.idle.enabled))) {
        } else if ((std::strcmp(a, "--blank-after") == 0 && i + 1 < argc &&
                    lx::shell::parse_idle_seconds(argv[++i], cfg.idle.blank_after_s)) ||
                   (std::strncmp(a, "--blank-after=", 14) == 0 &&
                    lx::shell::parse_idle_seconds(a + 14, cfg.idle.blank_after_s))) {
        } else if ((std::strcmp(a, "--blank-locked-after") == 0 && i + 1 < argc &&
                    lx::shell::parse_idle_seconds(argv[++i], cfg.idle.blank_locked_after_s)) ||
                   (std::strncmp(a, "--blank-locked-after=", 21) == 0 &&
                    lx::shell::parse_idle_seconds(a + 21, cfg.idle.blank_locked_after_s))) {
        } else if (std::strcmp(a, "--lock-command") == 0 && i + 1 < argc) {
            cfg.idle.lock_command = argv[++i];
        } else if (std::strncmp(a, "--lock-command=", 15) == 0) {
            cfg.idle.lock_command = a + 15;
        } else if (std::strcmp(a, "--lock-on-blank") == 0) {
            cfg.idle.lock_on_blank = true;
        } else if (std::strcmp(a, "--no-lock-on-blank") == 0) {
            cfg.idle.lock_on_blank = false;
        } else if ((std::strcmp(a, "--background") == 0 && i + 1 < argc) ||
                   std::strncmp(a, "--background=", 13) == 0) {
            const char* v = a[12] == '=' ? a + 13 : argv[++i];
            auto spec = lx::shell::parse_background(v);
            if (!spec) {
                std::fprintf(stderr, "lumen-shell: bad --background '%s' (see --help)\n", v);
                return 2;
            }
            cfg.background = *spec;
            cfg.background_enabled = true;
        } else if (std::strcmp(a, "--no-background") == 0) {
            cfg.background_enabled = false;
        } else if (std::strcmp(a, "-h") == 0 || std::strcmp(a, "--help") == 0) {
            std::printf(
                "usage: lumen-shell [--display NAME] [--idle-policy shell|none] [--blank-after S]\n"
                "                   [--blank-locked-after S] [--lock-command CMD]\n"
                "                   [--lock-on-blank | --no-lock-on-blank]\n"
                "  --display NAME         Wayland display (default: $WAYLAND_DISPLAY)\n"
                "  --idle-policy MODE     shell: blank the outputs after --blank-after seconds\n"
                "                         without input (idle inhibitors obeyed); none: leave it\n"
                "                         to an external daemon such as swayidle + wlopm\n"
                "  --blank-after S        seconds (default 600; 0 = never)\n"
                "  --blank-locked-after S the same while the session is locked (default 60)\n"
                "  --lock-command CMD     the screen locker, an Exec line (e.g. 'swaylock -f');\n"
                "                         started by Super+L, loginctl lock-session, before\n"
                "                         sleep, and again if it exits while locked\n"
                "  --no-lock-on-blank     blank without locking first (default: lock, then blank)\n"
                "  --background SPEC      the desktop background (also LUMEN_BACKGROUND):\n"
                "                           lumen                      built-in (default)\n"
                "                           #RRGGBB                    a colour\n"
                "                           gradient:#RRGGBB:#RRGGBB[:DEG]  0 = left to right,\n"
                "                                                      90 = top to bottom\n"
                "                           image:PATH[:fill|fit|center|stretch|tile]\n"
                "                                                      PNG, JPEG, WebP or SVG\n"
                "  --no-background        none (leave it to another client, e.g. swaybg)\n");
            return 0;
        } else {
            std::fprintf(stderr, "lumen-shell: bad or unknown argument '%s' (see --help)\n", a);
            return 2;
        }
    }
    lx::shell::shell_app shell{cfg};
    return shell.run();
}
