// lumen-hello — a window on lx.app (ADR-010 Phase 4): ≈640 × 480 logical, app_id
// org.lumen.hello, title "Hello Lumen", built from the §5.6 components (hello_ui.hpp) and
// drawn with the default pixel tier.
//
//   lumen-hello [--backend cpu|gl|vulkan] [--socket NAME] [--theme one-dark|one-light|PATH]
//               [--exit-after-first-present] [--frames N]
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

#include <stdio.h>

import std;
import lx.foundation;
import lx.app;
import lx.gfx;
import lx.ui;

#include "hello_ui.hpp"

namespace {

struct args {
    lx::gfx::client_tier backend = lx::gfx::client_tier::none;
    const char* socket = nullptr;
    const char* theme = nullptr;
    bool exit_after_first_present = false;
    unsigned frames = 0;
};

void usage() {
    std::fprintf(stderr,
                 "usage: lumen-hello [--backend cpu|gl|vulkan] [--socket NAME] [--theme NAME|PATH]\n"
                 "                   [--exit-after-first-present] [--frames N]\n");
}

[[nodiscard]] bool parse(int argc, char** argv, args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--backend") {
            const char* v = value();
            out.backend = v ? lx::gfx::parse_client_tier(v) : lx::gfx::client_tier::none;
            if (out.backend == lx::gfx::client_tier::none) {
                std::fprintf(stderr, "lumen-hello: unknown backend '%s'\n", v ? v : "");
                return false;
            }
        } else if (a == "--socket") {
            out.socket = value();
        } else if (a == "--theme") {
            out.theme = value();
        } else if (a == "--exit-after-first-present") {
            out.exit_after_first_present = true;
        } else if (a == "--frames") {
            const char* v = value();
            out.frames = v ? static_cast<unsigned>(std::strtoul(v, nullptr, 10)) : 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            usage();
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
    app.config().socket = a.socket;
    app.config().theme = a.theme;
    if (auto started = app.start(); !started) {
        std::fprintf(stderr, "lumen-hello: %s\n", lx::format_error(started.get_error()));
        return 1;
    }
    app.set_close_handler(&on_close, &app);

    lx::app::window_options o{};
    o.title = "Hello Lumen";
    o.app_id = "org.lumen.hello";
    o.size = {640, 480};
    o.min_size = {360, 320};
    o.describe = &hello::describe;
    lx::app::window w;
    if (auto opened = w.open(app, o); !opened) {
        std::fprintf(stderr, "lumen-hello: %s\n", lx::format_error(opened.get_error()));
        return 1;
    }
    w.set_continuous(a.frames > 0);

    if (a.exit_after_first_present) {
        const bool presented =
            app.run_until([&] { return w.timeline().first_presented_ns != 0 || !w.is_open(); }, 10'000);
        const lx::app::app_timeline& t = app.timeline();
        const lx::app::window_timeline& wt = w.timeline();
        std::printf("lumen-hello startup: tier=%s exec=%lld connect=%lld configure=%lld commit=%lld "
                    "presented=%lld exec_to_connect_us=%lld exec_to_configure_us=%lld "
                    "exec_to_commit_us=%lld exec_to_present_us=%lld exec_src=%s\n",
                    lx::gfx::client_tier_name(w.tier()), us(t.exec_ns), us(t.connect_ns),
                    us(wt.first_configure_ns), us(wt.first_commit_ns), us(wt.first_presented_ns),
                    since(t.exec_ns, t.connect_ns), since(t.exec_ns, wt.first_configure_ns),
                    since(t.exec_ns, wt.first_commit_ns), since(t.exec_ns, wt.first_presented_ns),
                    t.exec_from_env ? "env" : "main");
        std::fflush(stdout);
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
        std::printf("lumen-hello frames: tier=%s presented=%llu frame_us_p50=%lld frame_us_p95=%lld "
                    "buffer=%dx%d scale=%.3f\n",
                    lx::gfx::client_tier_name(w.tier()),
                    static_cast<unsigned long long>(w.stats().presented), pct(0.5), pct(0.95),
                    w.buffer_size().width, w.buffer_size().height, static_cast<double>(w.scale()));
        return done ? 0 : 1;
    }
    return app.run();
}
