// Render-thread cost of a compositor frame with and without server-side decorations
// (ADR-010 §4.11): a 1920x1080 view with a solid background and six overlapping 480x320
// windows, composited by the CPU and GL backends exactly as `composite_output` calls them
// (`frame_painter::paint_cpu` / `paint_gl` on a published snapshot). Decorated, each window's
// fills are quad primitives and its title ("Window N - Lumen Terminal: ~/src/lumen", the
// compositor's title font) and close glyph are mono sprites from the compositor's text atlas
// (`chrome_atlas`, laid out once before the run), and the frame goes through `:ui_cpu` /
// `:ui_gl`; undecorated, it is the blit path.
//
// Two damage cases: the whole frame (a repaint) and one window's content (a client
// committing every frame — the steady case). GL reports the render thread's time to submit
// (what it spends) and the time until the frame's fence signals (submit + GPU). The CPU rows
// also give the UI thread's `commit_frame` (the decorated scene has a primitive stream:
// every draw takes an order from the overlap grid and the refs are sorted and batched).
//
// Two more cases (Phase 5): a hover change of one close button (its circle drawn, then not,
// on alternate frames, damage the button only) and the render thread's per-frame
// draw_command → blit_command conversion (`frame_painter::surfaces`) timed alone at 16, 64
// and 256 surface draws, beside the full composite of the same frame.
//
//   bench_ui_composite [--frames N] [--warmup N] [--backend cpu|gl|all]

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.compositor;
import lx.text;

namespace {

constexpr int k_w = 1920;
constexpr int k_h = 1080;
constexpr int k_win_w = 480;
constexpr int k_win_h = 320;
constexpr unsigned k_windows = 6;
constexpr unsigned k_texture_base = 0x100u;

/// The compositor's text atlas and the titles' text, shared by every scene (the atlas is UI
/// state that outlives frames, as in the compositor).
struct titles {
    lx::compositor::chrome_atlas atlas{};
    lx::compositor::chrome_text text{atlas};
};

titles& chrome() {
    static titles t{};
    return t;
}

struct scene_setup {
    lx::scene::scene_graph scene{};
    std::vector<std::unique_ptr<lx::compositor::title_text>> titles;
    lx::scene::surface_node background{lx::surface_id{1}};
    std::vector<std::unique_ptr<lx::scene::surface_node>> windows;
    std::vector<std::unique_ptr<lx::compositor::frame_node>> frames;
    unsigned frame_index = 0;

    explicit scene_setup(bool decorated) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::scene::output_view_config v{};
        v.logical = {0, 0, k_w, k_h};
        v.projection = {{0, 0}, 1.0, {k_w, k_h}};
        scene.set_output_view(0, v);
        background.set_bounds({0, 0, k_w, k_h});
        background.set_solid(true, lx::color::rgb(0.1f, 0.12f, 0.15f, 1.f));
        (void)scene.root().add(&background);
        for (unsigned i = 0; i < k_windows; ++i) {
            const lx::rect2i g{120 + static_cast<int>(i) * 220, 140 + static_cast<int>(i % 3) * 180,
                               k_win_w, k_win_h};
            if (decorated) {
                auto f = std::make_unique<lx::compositor::frame_node>();
                f->geometry = g;
                f->extents = lx::compositor::extents_for(lx::compositor::decoration_mode::server, false);
                f->shown = true;
                f->active = i + 1 == k_windows;
                auto t = std::make_unique<lx::compositor::title_text>();
                char title[64];
                std::snprintf(title, sizeof title, "Window %u - Lumen Terminal: ~/src/lumen", i);
                (void)chrome().text.layout_title(t->runs[0], title, false,
                                                 lx::compositor::title_text_room(k_win_w), 1.0, true);
                (void)chrome().text.close(1.0);
                f->title = t.get();
                f->text = &chrome().text;
                titles.push_back(std::move(t));
                (void)scene.root().add(f.get());
                frames.push_back(std::move(f));
            }
            auto w = std::make_unique<lx::scene::surface_node>(lx::surface_id{10 + i});
            lx::gfx::imported_image img{};
            img.width = k_win_w;
            img.height = k_win_h;
            img.image_id = k_texture_base + i;
            w->attach(img);
            w->set_bounds({0, 0, k_win_w, k_win_h});
            w->set_transform({g.x, g.y});
            w->set_opaque(true);
            (void)scene.root().add(w.get());
            windows.push_back(std::move(w));
        }
    }

    const lx::scene::immutable_frame_snapshot& publish(bool full) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        if (full)
            scene.note_full_damage_output(0);
        else
            scene.note_damage(window_rect(2));
        (void)scene.commit_frame(0, ++frame_index);
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        return scene.acquire_render_snapshot(0);
    }
    /// A hover change: window 2's close button hovered on odd frames, normal on even ones,
    /// the button's rect damaged (what `decoration_manager::set_button` does).
    const lx::scene::immutable_frame_snapshot& publish_hover() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::compositor::frame_node& f = *frames[2];
        f.button = f.button == lx::compositor::deco_button::normal ? lx::compositor::deco_button::hover
                                                                    : lx::compositor::deco_button::normal;
        scene.note_damage(lx::compositor::close_rect(f.geometry));
        (void)scene.commit_frame(0, ++frame_index);
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        return scene.acquire_render_snapshot(0);
    }
    void release(const lx::scene::immutable_frame_snapshot& s) {
        scene.release_render_frame(0, s);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    }
    [[nodiscard]] static lx::rect2i window_rect(unsigned i) {
        return {120 + static_cast<int>(i) * 220, 140 + static_cast<int>(i % 3) * 180, k_win_w, k_win_h};
    }
};

/// `n` surface draws over a solid background: 96x64 tiles of the windows' textures, laid out
/// in a grid with overlap, as the per-frame conversion sees a busy desktop's draw list.
struct tiles_setup {
    lx::scene::scene_graph scene{};
    lx::scene::surface_node background{lx::surface_id{1}};
    std::vector<std::unique_ptr<lx::scene::surface_node>> tiles;
    /// A container holds 128 children: the tiles go 64 to a group.
    std::vector<std::unique_ptr<lx::scene::container_node>> groups;
    unsigned frame_index = 0;

    explicit tiles_setup(unsigned n) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::scene::output_view_config v{};
        v.logical = {0, 0, k_w, k_h};
        v.projection = {{0, 0}, 1.0, {k_w, k_h}};
        scene.set_output_view(0, v);
        background.set_bounds({0, 0, k_w, k_h});
        background.set_solid(true, lx::color::rgb(0.1f, 0.12f, 0.15f, 1.f));
        (void)scene.root().add(&background);
        for (unsigned i = 1; i < n; ++i) {
            auto t = std::make_unique<lx::scene::surface_node>(lx::surface_id{100 + i});
            lx::gfx::imported_image img{};
            img.width = k_win_w;
            img.height = k_win_h;
            img.image_id = k_texture_base + i % k_windows;
            t->attach(img);
            t->set_bounds({0, 0, 96, 64});
            t->set_transform({static_cast<int>((i * 73u) % static_cast<unsigned>(k_w - 96)),
                              static_cast<int>((i * 41u) % static_cast<unsigned>(k_h - 64))});
            if ((i - 1) % 64 == 0) {
                groups.push_back(std::make_unique<lx::scene::container_node>());
                (void)scene.root().add(groups.back().get());
            }
            (void)groups.back()->add(t.get());
            tiles.push_back(std::move(t));
        }
    }
    const lx::scene::immutable_frame_snapshot& publish() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        scene.note_full_damage_output(0);
        (void)scene.commit_frame(0, ++frame_index);
        lx::runtime::set_current_affinity(lx::runtime::affinity::render);
        return scene.acquire_render_snapshot(0);
    }
    void release(const lx::scene::immutable_frame_snapshot& s) {
        scene.release_render_frame(0, s);
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    }
};

std::vector<std::uint32_t> window_pixels(unsigned i) {
    std::vector<std::uint32_t> px(static_cast<std::size_t>(k_win_w * k_win_h));
    for (int y = 0; y < k_win_h; ++y)
        for (int x = 0; x < k_win_w; ++x)
            px[static_cast<std::size_t>(y * k_win_w + x)] =
                0xFF000000u | (static_cast<std::uint32_t>((x + i * 40) & 0xFF) << 16) |
                (static_cast<std::uint32_t>(y & 0xFF) << 8) | (0x40u + i * 20u);
    return px;
}

struct stats {
    double p50 = 0, p95 = 0;
};

stats summarize(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    if (v.empty())
        return {};
    return {v[v.size() / 2], v[std::min(v.size() - 1, (v.size() * 95) / 100)]};
}

void row(const char* backend, const char* scene, const char* damage, stats submit, stats total,
         bool has_total, unsigned long long stream_frames) {
    if (has_total)
        std::printf("%-4s %-12s %-7s render %8.1f us p50 %8.1f us p95 | +gpu %8.1f us p50 %8.1f us p95%s\n",
                    backend, scene, damage, submit.p50, submit.p95, total.p50, total.p95,
                    stream_frames ? "  (stream)" : "");
    else
        std::printf("%-4s %-12s %-7s render %8.1f us p50 %8.1f us p95%s\n", backend, scene, damage,
                    submit.p50, submit.p95, stream_frames ? "  (stream)" : "");
}

double us_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    unsigned frames = 200;
    unsigned warmup = 20;
    std::string backend = "all";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            frames = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--warmup" && i + 1 < argc)
            warmup = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (a == "--backend" && i + 1 < argc)
            backend = argv[++i];
        else {
            std::printf("usage: bench_ui_composite [--frames N] [--warmup N] [--backend cpu|gl|all]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    std::vector<std::vector<std::uint32_t>> textures;
    for (unsigned i = 0; i < k_windows; ++i)
        textures.push_back(window_pixels(i));
    std::printf("bench_ui_composite: %dx%d, %u windows of %dx%d, %u frames (+%u warm-up)\n", k_w, k_h,
                k_windows, k_win_w, k_win_h, frames, warmup);

    if (backend == "cpu" || backend == "all") {
        lx::gfx::cpu_compositor comp;
        for (unsigned i = 0; i < k_windows; ++i) {
            lx::gfx::pixel_surface s{};
            s.pixels = reinterpret_cast<unsigned char*>(textures[i].data());
            s.width = k_win_w;
            s.height = k_win_h;
            s.stride = k_win_w * 4u;
            s.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
            (void)comp.register_texture(k_texture_base + i, s);
        }
        lx::gfx::ui_cpu_renderer ui;
        std::vector<std::uint32_t> target(static_cast<std::size_t>(k_w * k_h));
        lx::gfx::pixel_surface t{};
        t.pixels = reinterpret_cast<unsigned char*>(target.data());
        t.width = k_w;
        t.height = k_h;
        t.stride = k_w * 4u;
        t.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
        for (const bool decorated : {false, true})
            for (const bool full : {true, false}) {
                scene_setup s{decorated};
                lx::compositor::frame_painter painter;
                std::vector<double> times, builds;
                for (unsigned f = 0; f < frames + warmup; ++f) {
                    const auto tb = std::chrono::steady_clock::now();
                    const auto& snap = s.publish(full);
                    const double build = us_since(tb);
                    const auto t0 = std::chrono::steady_clock::now();
                    (void)painter.paint_cpu(comp, &ui, t, snap, snap.damage_rects(), &chrome().atlas);
                    const double us = us_since(t0);
                    if (f >= warmup) {
                        times.push_back(us);
                        builds.push_back(build);
                    }
                    s.release(snap);
                }
                const stats st = summarize(times);
                row("cpu", decorated ? "decorated" : "undecorated", full ? "full" : "window", st, {},
                    false, painter.stream_frames());
                const stats bt = summarize(builds);
                std::printf("     (UI thread: commit_frame — emit, visibility pass, order, batch: "
                            "%.1f us p50 %.1f us p95)\n",
                            bt.p50, bt.p95);
            }
        {
            scene_setup s{true};
            lx::compositor::frame_painter painter;
            {
                const auto& snap = s.publish(true);
                (void)painter.paint_cpu(comp, &ui, t, snap, snap.damage_rects(), &chrome().atlas);
                s.release(snap);
            }
            std::vector<double> times;
            for (unsigned f = 0; f < frames + warmup; ++f) {
                const auto& snap = s.publish_hover();
                const auto t0 = std::chrono::steady_clock::now();
                (void)painter.paint_cpu(comp, &ui, t, snap, snap.damage_rects(), &chrome().atlas);
                const double us = us_since(t0);
                if (f >= warmup)
                    times.push_back(us);
                s.release(snap);
            }
            row("cpu", "decorated", "hover", summarize(times), {}, false, painter.stream_frames());
        }
        for (const unsigned n : {16u, 64u, 256u}) {
            tiles_setup s{n};
            lx::compositor::frame_painter painter;
            std::vector<double> conv, total;
            for (unsigned f = 0; f < frames + warmup; ++f) {
                const auto& snap = s.publish();
                const auto t0 = std::chrono::steady_clock::now();
                const auto blits = painter.surfaces(snap);
                const double ns = 1000.0 * us_since(t0);
                const auto t1 = std::chrono::steady_clock::now();
                (void)painter.paint_cpu(comp, &ui, t, snap, snap.damage_rects(), &chrome().atlas);
                const double us = us_since(t1);
                if (f >= warmup && blits.size() == n) {
                    conv.push_back(ns);
                    total.push_back(us);
                }
                s.release(snap);
            }
            const stats c = summarize(conv);
            const stats tt = summarize(total);
            std::printf("cpu  surfaces()  %3u draws  %8.0f ns p50 %8.0f ns p95 | composite %8.1f us p50 "
                        "(%.2f %%)\n",
                        n, c.p50, c.p95, tt.p50, tt.p50 > 0 ? c.p50 / (10.0 * tt.p50) : 0.0);
        }
    }

    if (backend == "gl" || backend == "all") {
        const int fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            std::printf("gl: no render node, skipped\n");
            return 0;
        }
        auto device = lx::gfx::egl_device::create(fd);
        if (!device || device->is_software_renderer()) {
            std::printf("gl: %s, skipped\n", device ? "software renderer" : device.get_error().message);
            ::close(fd);
            return 0;
        }
        std::printf("gl: %s, GLES %d.%d\n", device->renderer(), device->gles_major(), device->gles_minor());
        {
            lx::gfx::gl_compositor comp;
            if (!comp.initialize(*device)) {
                std::printf("gl: compositor init failed\n");
                return 1;
            }
            const auto rgba = [](const std::vector<std::uint32_t>& w) {
                std::vector<unsigned char> out(w.size() * 4);
                for (std::size_t i = 0; i < w.size(); ++i) {
                    out[i * 4 + 0] = static_cast<unsigned char>(w[i] >> 16);
                    out[i * 4 + 1] = static_cast<unsigned char>(w[i] >> 8);
                    out[i * 4 + 2] = static_cast<unsigned char>(w[i]);
                    out[i * 4 + 3] = static_cast<unsigned char>(w[i] >> 24);
                }
                return out;
            };
            for (unsigned i = 0; i < k_windows; ++i)
                (void)comp.upload_rgba(k_texture_base + i, k_win_w, k_win_h, rgba(textures[i]).data());
            lx::gfx::ui_gl_renderer ui;
            const bool ui_ready = static_cast<bool>(ui.initialize());
            if (!ui_ready)
                std::printf("gl: no GLES 3.1 primitive renderer — decorated rows are the lowered path\n");
            auto target = lx::gfx::ui_gl_target::create(k_w, k_h);
            if (!target) {
                std::printf("gl: target failed\n");
                return 1;
            }
            for (const bool decorated : {false, true})
                for (const bool full : {true, false}) {
                    scene_setup s{decorated};
                    lx::compositor::frame_painter painter;
                    std::vector<double> submit, total;
                    for (unsigned f = 0; f < frames + warmup; ++f) {
                        const auto& snap = s.publish(full);
                        const auto t0 = std::chrono::steady_clock::now();
                        auto p = painter.paint_gl(comp, ui_ready ? &ui : nullptr, target->framebuffer(),
                                                  k_w, k_h, snap, snap.damage_rects(), &chrome().atlas);
                        const double us = us_since(t0);
                        if (p && p->composite.out_fence_fd >= 0) {
                            pollfd pfd{p->composite.out_fence_fd, POLLIN, 0};
                            (void)::poll(&pfd, 1, 1000);
                            ::close(p->composite.out_fence_fd);
                        }
                        const double all = us_since(t0);
                        if (f >= warmup) {
                            submit.push_back(us);
                            total.push_back(all);
                        }
                        s.release(snap);
                    }
                    row("gl", decorated ? "decorated" : "undecorated", full ? "full" : "window",
                        summarize(submit), summarize(total), true, painter.stream_frames());
                }
            const auto fenced = [&](lx::compositor::frame_painter& painter,
                                    const lx::scene::immutable_frame_snapshot& snap, double& submit_us) {
                const auto t0 = std::chrono::steady_clock::now();
                auto p = painter.paint_gl(comp, ui_ready ? &ui : nullptr, target->framebuffer(), k_w, k_h,
                                          snap, snap.damage_rects(), &chrome().atlas);
                submit_us = us_since(t0);
                if (p && p->composite.out_fence_fd >= 0) {
                    pollfd pfd{p->composite.out_fence_fd, POLLIN, 0};
                    (void)::poll(&pfd, 1, 1000);
                    ::close(p->composite.out_fence_fd);
                }
                return us_since(t0);
            };
            {
                scene_setup s{true};
                lx::compositor::frame_painter painter;
                {
                    double ignored = 0;
                    const auto& snap = s.publish(true);
                    (void)fenced(painter, snap, ignored);
                    s.release(snap);
                }
                std::vector<double> submit, total;
                for (unsigned f = 0; f < frames + warmup; ++f) {
                    const auto& snap = s.publish_hover();
                    double us = 0;
                    const double all = fenced(painter, snap, us);
                    if (f >= warmup) {
                        submit.push_back(us);
                        total.push_back(all);
                    }
                    s.release(snap);
                }
                row("gl", "decorated", "hover", summarize(submit), summarize(total), true,
                    painter.stream_frames());
            }
            for (const unsigned n : {16u, 64u, 256u}) {
                tiles_setup s{n};
                lx::compositor::frame_painter painter;
                std::vector<double> conv, total;
                for (unsigned f = 0; f < frames + warmup; ++f) {
                    const auto& snap = s.publish();
                    const auto t0 = std::chrono::steady_clock::now();
                    const auto blits = painter.surfaces(snap);
                    const double ns = 1000.0 * us_since(t0);
                    double us = 0;
                    (void)fenced(painter, snap, us);
                    if (f >= warmup && blits.size() == n) {
                        conv.push_back(ns);
                        total.push_back(us);
                    }
                    s.release(snap);
                }
                const stats c = summarize(conv);
                const stats tt = summarize(total);
                std::printf("gl   surfaces()  %3u draws  %8.0f ns p50 %8.0f ns p95 | composite %8.1f us p50 "
                            "(%.2f %%)\n",
                            n, c.p50, c.p95, tt.p50, tt.p50 > 0 ? c.p50 / (10.0 * tt.p50) : 0.0);
            }
        }
        // The device (and its fd) live until exit: the context must outlive the GL objects
        // above, and nothing follows.
    }
    return 0;
}
