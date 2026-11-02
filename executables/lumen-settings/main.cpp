// lumen-settings — the Lumen desktop's settings application (ADR-013): a window on lx.app
// rendering the configuration surface from lx.config's schema (settings_ui.cppm). The
// session loads the layered configuration; edits splice the user layer (lx.config:edit),
// verify and rename it atomically; the Varlink polls (version, health, devices) feed the
// Session pane.
//
//   lumen-settings [--backend cpu|gl|vulkan] [--allow-software-gpu] [--socket NAME]
//                  [--theme one-dark|one-light|PATH] [--exit-after-first-present]

import std;
import lx.foundation;
import lx.app;
import lx.gfx;
import lx.runtime;
import lx.trace;
import lumen.settings_ui;

#include <cstdlib>

namespace {

constexpr lx::trace::category log_settings{"settings"};

struct args {
    lx::gfx::client_tier backend = lx::gfx::client_tier::none;
    bool allow_software = false;
    const char* socket = nullptr;
    const char* theme = nullptr;
    bool exit_after_first_present = false;
};

void usage() {
    std::println("usage: lumen-settings [--backend cpu|gl|vulkan] [--allow-software-gpu]\n"
                 "                      [--socket NAME] [--theme NAME|PATH]\n"
                 "                      [--exit-after-first-present]");
}

[[nodiscard]] bool parse(int argc, char** argv, args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--backend") {
            const char* v = value();
            out.backend = v ? lx::gfx::parse_client_tier(v) : lx::gfx::client_tier::none;
            if (out.backend == lx::gfx::client_tier::none) {
                log_settings.error("unknown backend '{}' (cpu, gl or vulkan)", v ? v : "");
                return false;
            }
        } else if (a == "--allow-software-gpu") {
            out.allow_software = true;
        } else if (a == "--socket") {
            out.socket = value();
        } else if (a == "--theme") {
            out.theme = value();
        } else if (a == "--exit-after-first-present") {
            out.exit_after_first_present = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            log_settings.error("unknown argument '{}' (--help lists them)", a);
            return false;
        }
    }
    return true;
}

void on_close(void* user, lx::app::window&) { static_cast<lx::application*>(user)->quit(0); }

} // namespace

int main(int argc, char* argv[]) {
    lx::application app{argc, argv};
    args a{};
    if (!parse(argc, argv, a))
        return 2;
    app.config().backend = a.backend;
    app.config().allow_software_gpu = a.allow_software;
    app.config().socket = a.socket;
    app.config().theme = a.theme;
    if (auto started = app.start(); !started) {
        log_settings.error(started.get_error());
        return 1;
    }
    app.set_close_handler(&on_close, &app);

    // The session the UI renders: the layered configuration (locate → read → parse); edits
    // land in it and in the user's config.toml (ADR-013 Phase C). Static storage, so a
    // captureless lambda reaches it.
    static settings::session the_session{true};
    settings::set_active_session(&the_session);

    lx::app::window_options o{};
    o.title = "Settings";
    o.app_id = "org.lumen.Settings";
    o.size = {960, 680};
    o.min_size = {780, 520};
    o.describe = &settings::describe;
    lx::app::window w;
    if (auto opened = w.open(app, o); !opened) {
        log_settings.error(opened.get_error());
        return 1;
    }
    the_session.root = &w.root();
    // The heads changed (a stage_* or a done event): one re-describe. `the_session` has
    // static storage, so a captureless lambda reaches it.
    settings::start_outputs(app.display().native(), [] { the_session.refresh(); });

    // The Varlink polls (ADR-013 Phase G): the control socket beside the display socket,
    // a poll every 2 s on a worker thread, results posted to the UI strand.
    {
        std::string socket_path;
        if (const char* s = std::getenv("LUMEN_SOCKET"); s && s[0]) {
            socket_path = s;
        } else {
            const char* dir = std::getenv("XDG_RUNTIME_DIR");
            const char* display = std::getenv("WAYLAND_DISPLAY");
            const std::string name = display && display[0] ? std::string(display) : "lumen-0";
            if (dir && dir[0])
                socket_path = std::string(dir) + "/" + name + ".varlink";
        }
        settings::session* ui = &the_session;
        settings::start_ipc(
            socket_path.c_str(), [ui](const settings::health_snapshot& h) {
                // The snapshot is not trivially copyable (strings): the caller-owned
                // pointer pattern, allocated here and freed on the UI strand.
                const auto* copy = new settings::health_snapshot(h);
                (void)lx::runtime::executor::global().post(
                    lx::runtime::affinity::ui,
                    [ui, copy] {
                        ui->set_health(*copy);
                        delete copy;
                    });
            });
    }

    if (a.exit_after_first_present) {
        settings::stop_ipc();
        const bool presented =
            app.run_until([&] { return w.timeline().first_presented_ns != 0 || !w.is_open(); },
                          10'000);
        const lx::app::window_timeline& wt = w.timeline();
        std::println("lumen-settings startup: tier={} presented={} buffer={}x{} scale={:.3f}",
                     lx::gfx::client_tier_name(w.tier()), w.stats().presented,
                     w.buffer_size().width, w.buffer_size().height, static_cast<double>(w.scale()));
        (void)wt;
        return presented ? 0 : 1;
    }
    const int code = app.run();
    settings::stop_ipc();
    return code;
}
