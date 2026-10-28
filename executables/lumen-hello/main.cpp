// lumen-hello — a window on lx.app (ADR-010 Phase 4): ≈640 × 480 logical, app_id
// org.lumen.hello, title "Hello Lumen", built from the §5.6 components (lumen.hello.ui) and
// drawn with the default pixel tier.
//
//   lumen-hello [--backend cpu|gl|vulkan] [--allow-software-gpu] [--socket NAME]
//               [--theme one-dark|one-light|PATH] [--exit-after-first-present] [--frames N]
//
// --allow-software-gpu lets a forced GPU tier run on a software device (lavapipe, llvmpipe),
// which the automatic choice refuses: for parity and start-up measurements only (§8.3 S10's
// Vulkan rows on a host without a hardware Vulkan driver).
//
// --exit-after-first-present prints the start-up timeline as one line and exits once the
// first frame is presented (§8.3 S10 parses it; CLOCK_MONOTONIC µs; `exec` is LUMEN_EXEC_NS
// when the launcher set it, else the application's construction):
//
//   lumen-hello startup: tier=cpu exec=… connect=… configure=… commit=… presented=…
//       exec_to_connect_us=… exec_to_configure_us=… exec_to_commit_us=… exec_to_present_us=…
//       exec_src=env|main
//
// --frames N repaints the whole window every frame until N frames are presented, then prints
// the UI thread's frame time (callback or event → commit) and exits.
//
// Those two lines and --help are the program's output, on stdout; failures are logged through
// the `hello` category (stderr, LUMEN_LOG).

#include <sys/stat.h>

import std;
import lx.foundation;
import lx.app;
import lx.gfx;
import lx.trace;
import lumen.hello.ui;

namespace {

constexpr lx::trace::category log_hello{"hello"};

struct args {
    lx::gfx::client_tier backend = lx::gfx::client_tier::none;
    bool allow_software = false;
    const char* socket = nullptr;
    const char* theme = nullptr;
    bool exit_after_first_present = false;
    unsigned frames = 0;
    std::uint64_t render_node = 0;
};

void usage() {
    std::println("usage: lumen-hello [--backend cpu|gl|vulkan] [--allow-software-gpu] [--socket NAME]\n"
                 "                   [--theme NAME|PATH] [--exit-after-first-present] [--frames N]\n"
                 "                   [--render-node PATH]  (choose the tier on that node, not the\n"
                 "                   compositor's main device: a headless compositor names none)");
}

[[nodiscard]] bool parse(int argc, char** argv, args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--backend") {
            const char* v = value();
            out.backend = v ? lx::gfx::parse_client_tier(v) : lx::gfx::client_tier::none;
            if (out.backend == lx::gfx::client_tier::none) {
                log_hello.error("unknown backend '{}' (cpu, gl or vulkan)", v ? v : "");
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
        } else if (a == "--render-node") {
            const char* v = value();
            struct stat st{};
            if (!v || ::stat(v, &st) != 0 || !S_ISCHR(st.st_mode)) {
                log_hello.error("no device node '{}'", v ? v : "");
                return false;
            }
            out.render_node = static_cast<std::uint64_t>(st.st_rdev);
        } else if (a == "--frames") {
            const char* v = value();
            out.frames = v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            log_hello.error("unknown argument '{}' (--help lists them)", a);
            return false;
        }
    }
    return true;
}

[[nodiscard]] long long us(std::uint64_t ns) { return static_cast<long long>(ns / 1000); }

[[nodiscard]] long long since(std::uint64_t from, std::uint64_t to) {
    return to && from ? static_cast<long long>(to - from) / 1000 : -1;
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
    app.config().device_override = a.render_node;
    app.config().socket = a.socket;
    app.config().theme = a.theme;
    if (auto started = app.start(); !started) {
        log_hello.error(started.get_error());
        return 1;
    }
    app.set_close_handler(&on_close, &app);

    lx::app::window_options o{};
    o.title = "Hello Lumen";
    o.app_id = "org.lumen.hello";
    o.size = {640, 480};
    o.min_size = {360, 320};
    o.describe = &hello::describe;
    // --frames reports presented frames: feedback for each (else only the first frame's).
    o.presentation_feedback = a.frames > 0;
    lx::app::window w;
    if (auto opened = w.open(app, o); !opened) {
        log_hello.error(opened.get_error());
        return 1;
    }
    w.set_continuous(a.frames > 0);

    if (a.exit_after_first_present) {
        const bool presented =
            app.run_until([&] { return w.timeline().first_presented_ns != 0 || !w.is_open(); }, 10'000);
        const lx::app::app_timeline& t = app.timeline();
        const lx::app::window_timeline& wt = w.timeline();
        std::println("lumen-hello startup: tier={} exec={} connect={} configure={} commit={} "
                     "presented={} exec_to_connect_us={} exec_to_configure_us={} "
                     "exec_to_commit_us={} exec_to_present_us={} exec_src={}",
                     lx::gfx::client_tier_name(w.tier()), us(t.exec_ns), us(t.connect_ns),
                     us(wt.first_configure_ns), us(wt.first_commit_ns), us(wt.first_presented_ns),
                     since(t.exec_ns, t.connect_ns), since(t.exec_ns, wt.first_configure_ns),
                     since(t.exec_ns, wt.first_commit_ns), since(t.exec_ns, wt.first_presented_ns),
                     t.exec_from_env ? "env" : "main");
        std::fflush(nullptr);
        return presented ? 0 : 1;
    }
    if (a.frames > 0) {
        const bool done = app.run_until([&] { return w.stats().presented >= a.frames || !w.is_open(); },
                                        60'000);
        std::vector<long long> frame_us;
        for (unsigned i = 0; i < w.frame_count(); ++i) {
            const lx::app::frame_record& r = w.frame_at(i);
            if (r.start_ns && r.commit_ns >= r.start_ns)
                frame_us.push_back(static_cast<long long>(r.commit_ns - r.start_ns) / 1000);
        }
        std::sort(frame_us.begin(), frame_us.end());
        const auto pct = [&](double p) {
            return frame_us.empty() ? -1ll : frame_us[static_cast<std::size_t>(p * (frame_us.size() - 1))];
        };
        std::println("lumen-hello frames: tier={} presented={} frame_us_p50={} frame_us_p95={} "
                     "buffer={}x{} scale={:.3f}",
                     lx::gfx::client_tier_name(w.tier()), w.stats().presented, pct(0.5), pct(0.95),
                     w.buffer_size().width, w.buffer_size().height, static_cast<double>(w.scale()));
        return done ? 0 : 1;
    }
    return app.run();
}
