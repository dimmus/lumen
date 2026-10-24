// lumen-ui-bench — the UI engine's benchmark harness (ADR-010 §8), Phases 0–1.
//
// The scenarios S1–S3 of §8.3 run against lx.ui's declarative core the way an application
// uses it: descriptors reconciled onto retained nodes by `ui_root::tick`, nodes emitting
// ADR-010 primitives through `draw_scope`, the overlap grid and the `(order, kind, page)`
// sort of `draw_list::batch_for_render`. Every number is attributed to a stage (§8.1) with
// CLOCK_MONOTONIC read around the call; the lx.trace spans on the same stages feed Perfetto
// and are not what the table is made of.
//
// Backends: `none` measures the stages through `encode` (the sorted instances gathered into
// a ring — the bytes a backend uploads) and submits nothing; `cpu` then rasterises each
// produced frame with the reference renderer (`gfx::ui_cpu_renderer`) into an XRGB8888
// buffer of the scenario's size, inside the frame's damage — the ledger's rects, or the
// whole view when the frame carries none (an animation tick, the first frame) — and times
// that as the `raster` stage. `gl` and `vulkan` come with their Phase 1 stages, behind the
// same flag; nothing is renamed. `gl` draws each produced frame with `gfx::ui_gl_renderer`
// (GLES 3.1 on the render node, /dev/dri/renderD128) into an RGBA8 framebuffer object of the
// scenario's size, inside the same damage, and times two stages: `submit`, the CPU cost of
// `ui_gl_renderer::draw` (atlas upload, the gather of every batch's instances into the mapped
// ring slot, recording, glFlush — the bench's own `encode` into a host ring still runs, so
// G3 stays comparable across backends), and `gpu`, the frame's GPU time from
// GL_EXT_disjoint_timer_query, or, without one, the wall time from the flush to the frame's
// fence ("fence-wall"; the JSON's `gpu.timing` says which). A software GL_RENDERER
// (llvmpipe) is refused: its numbers are not GPU numbers. `vulkan` draws it with
// `gfx::ui_vk_renderer` into an R8G8B8A8_UNORM image on the device `device_selector` picks,
// with the same `submit` (CPU: upload, gather, record, vkQueueSubmit) and `gpu` (timestamps,
// or fence-wall) stages. A CPU device (lavapipe) is not refused — it is the only Vulkan some
// hosts have, and the path is worth exercising — but its rows say so: the JSON's `driver` is
// "vulkan-software" and `gpu.software` true, and scripts/ui-bench.sh never compares them nor
// makes them a baseline.
//
//   lumen-ui-bench [--scenario S1|S2|S3|all] [--backend none|cpu|gl|vulkan] [--frames N] [--warmup N]
//                  [--period-us US] [--json PATH] [--pin CPU|none] [--scale F] [--git SHA]
//                  [--preset NAME] [--gl-ring persistent|mapped] [--gl-fetch storage|attributes]
//
// Frames come from a frame clock (`--period-us`): a scenario whose work is a few
// microseconds per frame is otherwise over in a millisecond, one sample of whatever the
// host was doing at that instant rather than a distribution. The default is 1 ms — the
// samples spread over seconds and the core stays warm, which on a shared 2-vCPU host is
// what makes p95 repeat run after run; 16667 is a display's 60 Hz cadence and gives the
// cold number (every frame after a 16 ms sleep), 0 runs frames back to back. Deterministic:
// the scenes are seeded (xorshift64) and read no clock. The exit status is 0 whatever the
// numbers say — CTest checks that it ran; the `gates` block of the JSON and
// scripts/ui-bench.sh carry the verdicts.

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.runtime;
import lx.layout;
import lx.gfx;
import lx.scene;
import lx.ui;
import lx.trace;

// ── Heap accounting (G5) ────────────────────────────────────────────────────────
//
// Every `new` in the process goes through these — a global replacement, one relaxed atomic
// each — so a steady frame's allocation count is exact, not sampled. `mallinfo2` is read
// beside it: it also sees `malloc` calls that never went through `new`.

namespace {

std::atomic<std::uint64_t> g_new_calls{0};
std::atomic<std::uint64_t> g_new_bytes{0};

void* counted_alloc(std::size_t n) noexcept {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    g_new_bytes.fetch_add(n, std::memory_order_relaxed);
    return std::malloc(n > 0 ? n : 1);
}

void* counted_aligned_alloc(std::size_t n, std::align_val_t align) noexcept {
    g_new_calls.fetch_add(1, std::memory_order_relaxed);
    g_new_bytes.fetch_add(n, std::memory_order_relaxed);
    const auto a = static_cast<std::size_t>(align);
    const std::size_t rounded = (n + a - 1) / a * a;
    return std::aligned_alloc(a, rounded > 0 ? rounded : a);
}

} // namespace

void* operator new(std::size_t n) {
    if (void* p = counted_alloc(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    if (void* p = counted_alloc(n))
        return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (void* p = counted_aligned_alloc(n, a))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) {
    if (void* p = counted_aligned_alloc(n, a))
        return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned_alloc(n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned_alloc(n, a);
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

using lx::ui::child;

// ── Clock, randomness, options ──────────────────────────────────────────────────

[[nodiscard]] std::uint64_t now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

struct xorshift64 {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    [[nodiscard]] std::uint64_t next() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return state;
    }
};

struct options {
    bool scenarios[3]{true, true, true};
    const char* backend = "none";
    unsigned frames = 600;
    unsigned warmup = 120;
    unsigned period_us = 1000; ///< the frame clock; 0: no pacing
    const char* json = nullptr;
    int pin = -2; ///< -2: the last CPU; -1: none
    float scale = 1.f;
    const char* git = "";
    const char* preset = "release";
    bool gl_mapped_ring = false; ///< `gl`: the mapped-range ring instead of the persistent one
    bool gl_attribute_fetch = false; ///< `gl`: instances as vertex attributes, not a storage buffer
};

/// Keeps the optimiser from deleting the encode copy.
volatile std::uint32_t g_sink = 0;

// ── The `gl` backend ────────────────────────────────────────────────────────────
//
// One EGL context on the render node for the whole run, created on the bench thread (a
// context is current on one thread) before the first scenario.

struct gl_backend {
    int fd = -1;
    lx::gfx::egl_device device{};
    lx::gfx::ui_gl_renderer renderer{};
    std::uint64_t setup_cold_ns = 0; ///< the first `initialize` of the process
    std::uint64_t setup_warm_ns = 0; ///< a second one (shutdown, initialize again)
    unsigned from_cache = 0;         ///< programs the second one loaded from the binary cache

    gl_backend() = default;
    gl_backend(const gl_backend&) = delete;
    gl_backend& operator=(const gl_backend&) = delete;
    ~gl_backend() {
        // The renderer needs the context, the context the node.
        renderer.shutdown();
        device = lx::gfx::egl_device{};
        if (fd >= 0)
            ::close(fd);
    }
};

gl_backend* g_gl = nullptr;

/// What the JSON reports of the `gl` backend; outlives the context (the JSON is written after
/// the bench thread is gone).
struct gl_info {
    bool valid = false;
    lx::gfx::ui_gl_caps caps{};
    std::uint64_t setup_cold_ns = 0;
    std::uint64_t setup_warm_ns = 0;
    unsigned from_cache = 0;
};

gl_info g_gl_info{};

/// Brings the `gl` backend up; false (with the reason printed) when it cannot run here.
[[nodiscard]] bool start_gl(gl_backend& gl, const options& opt) {
    lx::gfx::ui_gl_options gl_options{};
    gl_options.persistent_ring = !opt.gl_mapped_ring;
    gl_options.fetch = opt.gl_attribute_fetch ? lx::gfx::ui_gl_fetch::attributes
                                              : lx::gfx::ui_gl_fetch::storage_buffer;
    gl.fd = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (gl.fd < 0) {
        std::fprintf(stderr, "lumen-ui-bench: gl: no render node (/dev/dri/renderD128)\n");
        return false;
    }
    auto device = lx::gfx::egl_device::create(gl.fd);
    if (!device) {
        std::fprintf(stderr, "lumen-ui-bench: gl: %s\n", device.get_error().message);
        return false;
    }
    gl.device = std::move(device).value();
    if (auto ok = gl.renderer.initialize(gl_options); !ok) {
        std::fprintf(stderr, "lumen-ui-bench: gl: %s\n", ok.get_error().message);
        return false;
    }
    gl.setup_cold_ns = gl.renderer.setup_stats().program_ns;
    if (gl.renderer.caps().software) {
        std::fprintf(stderr,
                     "lumen-ui-bench: gl: GL_RENDERER is a software rasteriser (%s) — refusing to "
                     "report its numbers as GPU numbers\n",
                     gl.renderer.caps().renderer);
        return false;
    }
    gl.renderer.shutdown();
    if (auto ok = gl.renderer.initialize(gl_options); !ok) {
        std::fprintf(stderr, "lumen-ui-bench: gl: %s\n", ok.get_error().message);
        return false;
    }
    gl.setup_warm_ns = gl.renderer.setup_stats().program_ns;
    gl.from_cache = gl.renderer.setup_stats().from_cache;
    gl.renderer.set_gpu_timing(true);
    const lx::gfx::ui_gl_caps& caps = gl.renderer.caps();
    g_gl_info = {true, caps, gl.setup_cold_ns, gl.setup_warm_ns, gl.from_cache};
    std::printf("gl: %s | %s | fetch %s | ring %s | gpu timing %s | program binary formats %d | "
                "program setup cold %.1f us, again %.1f us (%u from cache)\n",
                caps.renderer, caps.version,
                caps.fetch == lx::gfx::ui_gl_fetch::attributes ? "attributes" : "storage",
                caps.persistent_ring ? "persistent" : "mapped-range",
                caps.timer_query            ? "timer query"
                : caps.timer_query_reads_zero ? "fence-wall (timer query reads 0)"
                                              : "fence-wall",
                caps.program_binary_formats,
                static_cast<double>(gl.setup_cold_ns) / 1000.0,
                static_cast<double>(gl.setup_warm_ns) / 1000.0, gl.from_cache);
    return true;
}

// ── The `vulkan` backend ────────────────────────────────────────────────────────
//
// One device and one renderer for the run, on the bench thread (the queue is submitted to
// from one thread).

struct vk_backend {
    lx::gfx::device device{};
    lx::gfx::ui_vk_renderer renderer{};
    std::uint64_t setup_cold_ns = 0; ///< the first `initialize`: pipelines from the disk cache, if any
    std::uint64_t setup_warm_ns = 0; ///< a second one (shutdown, initialize again)

    vk_backend() = default;
    vk_backend(const vk_backend&) = delete;
    vk_backend& operator=(const vk_backend&) = delete;
    ~vk_backend() { renderer.shutdown(); } // before the device
};

vk_backend* g_vk = nullptr;

struct vk_info {
    bool valid = false;
    lx::gfx::ui_vk_caps caps{};
    std::uint64_t setup_cold_ns = 0;
    std::uint64_t setup_warm_ns = 0;
    std::uint64_t cache_bytes_loaded = 0;
};

vk_info g_vk_info{};

[[nodiscard]] bool start_vk(vk_backend& vk) {
    auto selected = lx::gfx::device_selector::select_best();
    if (!selected || !selected->context().valid()) {
        std::fprintf(stderr, "lumen-ui-bench: vulkan: no Vulkan device\n");
        return false;
    }
    vk.device = std::move(selected).value();
    if (auto ok = vk.renderer.initialize(vk.device.context()); !ok) {
        std::fprintf(stderr, "lumen-ui-bench: vulkan: %s\n", ok.get_error().message);
        return false;
    }
    vk.setup_cold_ns = vk.renderer.setup_stats().pipeline_ns;
    vk.renderer.shutdown();
    if (auto ok = vk.renderer.initialize(vk.device.context()); !ok) {
        std::fprintf(stderr, "lumen-ui-bench: vulkan: %s\n", ok.get_error().message);
        return false;
    }
    vk.setup_warm_ns = vk.renderer.setup_stats().pipeline_ns;
    vk.renderer.set_gpu_timing(true);
    const lx::gfx::ui_vk_caps& caps = vk.renderer.caps();
    g_vk_info = {true, caps, vk.setup_cold_ns, vk.setup_warm_ns,
                 vk.renderer.setup_stats().cache_bytes_loaded};
    std::printf("vulkan: %s%s | api %u.%u | gpu timing %s | pipeline setup %.1f us, again %.1f us "
                "(%llu cache bytes read)\n",
                caps.device_name,
                caps.software ? " (software: a CPU device — correctness only, no GPU numbers)" : "",
                caps.api_version >> 22u, (caps.api_version >> 12u) & 0x3FFu,
                caps.timestamps ? "timestamps" : "fence-wall",
                static_cast<double>(vk.setup_cold_ns) / 1000.0,
                static_cast<double>(vk.setup_warm_ns) / 1000.0,
                static_cast<unsigned long long>(g_vk_info.cache_bytes_loaded));
    return true;
}

// ── Stages and distributions ────────────────────────────────────────────────────

enum stage : unsigned {
    st_update,            ///< the scenario's per-frame mutation and invalidation (bench side)
    st_tick_idle,         ///< `ui_root::tick` when it produced nothing
    st_tick,              ///< `ui_root::tick` when it produced a frame
    st_describe,          ///< the describer (from `ui_root::last_stage_times`)
    st_reconcile,         ///< the reconciler
    st_layout,            ///< measure + arrange
    st_emit,              ///< emit into the draw list (grid orders assigned here)
    st_order,             ///< `batch_for_render`: surface refs, sort, batches
    st_encode,            ///< gather the sorted instances into the ring
    st_emit_order_encode, ///< G3's sum
    st_frame,             ///< tick + order + encode: the UI thread's cost of a produced frame
    st_raster,            ///< the backend drawing the frame (`cpu`: ui_cpu_renderer::draw)
    st_submit,            ///< `gl`/`vulkan`: the renderer's draw on the CPU (upload, gather, record, submit)
    st_gpu,               ///< `gl`/`vulkan`: the frame's GPU time (timer query / timestamps, or fence-wall)
    st_count,
};

constexpr const char* k_stage_names[st_count] = {
    "update", "tick_idle", "tick",   "describe",          "reconcile", "layout",
    "emit",   "order",     "encode", "emit_order_encode", "frame",     "raster",
    "submit", "gpu",
};

struct summary {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    unsigned samples = 0;
};

/// Nearest-rank percentiles, in microseconds. Sorts `ns` in place.
[[nodiscard]] summary summarize(std::vector<std::uint64_t>& ns) {
    summary s{};
    s.samples = static_cast<unsigned>(ns.size());
    if (ns.empty())
        return s;
    std::sort(ns.begin(), ns.end());
    const auto at = [&](double p) {
        auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(ns.size())));
        if (rank == 0)
            rank = 1;
        if (rank > ns.size())
            rank = ns.size();
        return static_cast<double>(ns[rank - 1]) / 1000.0;
    };
    s.p50 = at(0.50);
    s.p95 = at(0.95);
    s.p99 = at(0.99);
    s.max = static_cast<double>(ns.back()) / 1000.0;
    return s;
}

// ── Colours: GPUI / Zed "One Dark" tokens as literals (ADR-010 §5.1) ────────────

[[nodiscard]] constexpr lx::color tok(int r, int g, int b, float a = 1.f) {
    return lx::color::rgb(static_cast<float>(r) / 255.f, static_cast<float>(g) / 255.f,
                          static_cast<float>(b) / 255.f, a);
}

constexpr lx::color k_background = tok(0x28, 0x2c, 0x33);         // background
constexpr lx::color k_surface = tok(0x2f, 0x34, 0x3e);            // elevated_surface.background
constexpr lx::color k_border = tok(0x36, 0x3c, 0x46);             // border
constexpr lx::color k_element = tok(0x2f, 0x34, 0x3e);            // element.background
constexpr lx::color k_title_bar = tok(0x3b, 0x41, 0x4d);          // title_bar.background
constexpr lx::color k_text = tok(0xc8, 0xcc, 0xd4);               // text
constexpr lx::color k_text_muted = tok(0x87, 0x8a, 0x98);         // text.muted
constexpr lx::color k_accent = tok(0x74, 0xad, 0xe8);             // text.accent
constexpr lx::color k_shadow = tok(0x00, 0x00, 0x00, 0.40f);      // §5.2 elevation
constexpr lx::color k_none = lx::color::rgb(0.f, 0.f, 0.f, 0.f);

// ── Scene nodes ─────────────────────────────────────────────────────────────────
//
// Built with the declarative API the way an application builds a window: a descriptor per
// node, reconciled onto a retained node by `ui_root`, each retained node emitting ADR-010
// primitives through `draw_scope`. Nothing here pushes an instance by hand.

/// A rounded, bordered quad with an optional shadow; its children fill the bounds inside
/// `pad`. A window frame, a panel cell, the grid's background.
struct card_props {
    lx::ui::quad_style style{};
    lx::ui::shadow_style shadow{}; ///< `blur_sigma` 0: none
    lx::size2i size{};             ///< preferred; 0 fills the constraint
    lx::inset pad{};
};

class card_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "card"; }

    void apply(const card_props& props) { props_ = props; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, lx::ui::size_hint) const override {
        lx::size2i out = props_.size;
        if (out.width <= 0)
            out.width = c.max.width;
        if (out.height <= 0)
            out.height = c.max.height;
        if (out.width < c.min.width) out.width = c.min.width;
        if (out.height < c.min.height) out.height = c.min.height;
        if (out.width > c.max.width) out.width = c.max.width;
        if (out.height > c.max.height) out.height = c.max.height;
        return out;
    }

    void arrange(lx::rect2i bounds) override {
        bounds_ = bounds;
        layout_dirty_ = false;
        const lx::rect2i inner{bounds.x + props_.pad.left, bounds.y + props_.pad.top,
                               bounds.width - props_.pad.left - props_.pad.right,
                               bounds.height - props_.pad.top - props_.pad.bottom};
        for (unsigned i = 0; i < child_count_; ++i) {
            if (children_[i])
                children_[i]->arrange(inner);
        }
    }

    void emit(lx::ui::draw_scope& out) const override {
        if (!visible_)
            return;
        if (props_.shadow.blur_sigma > 0.f)
            out.shadow(bounds_, props_.shadow);
        out.quad(bounds_, props_.style);
        ui_node::emit(out);
    }

private:
    card_props props_{};
};

/// The panel: a full-width bar that lays its cells out left to right at `pitch`.
struct panel_props {
    lx::ui::quad_style style{};
    int pitch = 0;
    int inset = 0;
};

class panel_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "panel"; }

    void apply(const panel_props& props) { props_ = props; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, lx::ui::size_hint) const override {
        return c.max;
    }

    void arrange(lx::rect2i bounds) override {
        bounds_ = bounds;
        layout_dirty_ = false;
        for (unsigned i = 0; i < child_count_; ++i) {
            if (!children_[i])
                continue;
            children_[i]->arrange({bounds.x + static_cast<int>(i) * props_.pitch + props_.inset,
                                   bounds.y + props_.inset, props_.pitch - 2 * props_.inset,
                                   bounds.height - 2 * props_.inset});
        }
    }

    void emit(lx::ui::draw_scope& out) const override {
        if (!visible_)
            return;
        out.quad(bounds_, props_.style);
        ui_node::emit(out);
    }

private:
    panel_props props_{};
};

/// A text placeholder: a quad the size the text would shape to (Phase 2 replaces it with
/// glyph sprites). `text` is read at measure and may change in place between frames.
struct label_props {
    const char* text = "";
    lx::color color{};
    int glyph_w = 7;
    int line_h = 14;
};

class label_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "label"; }

    void apply(const label_props& props) { props_ = props; }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, lx::ui::size_hint) const override {
        lx::size2i out{static_cast<int>(std::strlen(props_.text)) * props_.glyph_w,
                       props_.line_h};
        if (out.width > c.max.width) out.width = c.max.width;
        if (out.height > c.max.height) out.height = c.max.height;
        return out;
    }

    /// Left-aligned, vertically centred in what the parent offers.
    void arrange(lx::rect2i bounds) override {
        const lx::size2i wanted = measure({{0, 0}, {bounds.width, bounds.height}},
                                          lx::ui::size_hint::preferred);
        bounds_ = {bounds.x, bounds.y + (bounds.height - wanted.height) / 2, wanted.width,
                   wanted.height};
        layout_dirty_ = false;
    }

    void emit(lx::ui::draw_scope& out) const override {
        if (!visible_)
            return;
        lx::ui::quad_style style{};
        style.background = props_.color;
        style.corner_radii[0] = style.corner_radii[1] = style.corner_radii[2] =
            style.corner_radii[3] = 2.f;
        out.quad(bounds_, style);
    }

private:
    label_props props_{};
};

/// An interior node of the S3 tree: `ui_node::k_max_children` bounds the fan-out, so
/// 10 000 leaves hang under two levels of these.
struct group_props {};

class group_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "group"; }
    void apply(const group_props&) {}
};

// ── S3 state: the grid every leaf places itself in ──────────────────────────────

struct s3_scene {
    unsigned count = 10000;
    unsigned cols = 125;
    unsigned rows = 80;
    int pitch_x = 15;
    int pitch_y = 13;
    int quad_w = 10;
    int quad_h = 8;
    int origin_x = 22;
    int origin_y = 20;
    lx::ui::quad_style style{};
    std::vector<std::int8_t> drift_x{};
    std::vector<std::int8_t> drift_y{};
    xorshift64 rng{};
    lx::ui::ui_node* root = nullptr;
};

s3_scene g_s3{};

/// One grid item: a rounded, bordered quad whose position is its grid cell plus the frame's
/// drift. Drift stays within ±2 px of a 15 × 13 px pitch around a 10 × 8 px quad, so no two
/// quads ever overlap and every one of them batches with the rest (§4.3).
struct drift_props {
    unsigned index = 0;
    lx::color fill{};
};

class drift_quad_node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "drift_quad"; }

    void apply(const drift_props& props) {
        // Packed once here, when the props change, never per frame (ADR-010 §4.5): the
        // emit copies halves.
        index_ = props.index;
        lx::ui::quad_style style = g_s3.style;
        style.background = props.fill;
        style_ = lx::ui::pack_quad_style(style);
    }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints, lx::ui::size_hint) const override {
        return {g_s3.quad_w, g_s3.quad_h};
    }

    void arrange(lx::rect2i) override {
        const auto col = static_cast<int>(index_ % g_s3.cols);
        const auto row = static_cast<int>(index_ / g_s3.cols);
        bounds_ = {g_s3.origin_x + col * g_s3.pitch_x + g_s3.drift_x[index_],
                   g_s3.origin_y + row * g_s3.pitch_y + g_s3.drift_y[index_], g_s3.quad_w,
                   g_s3.quad_h};
        layout_dirty_ = false;
    }

    void emit(lx::ui::draw_scope& out) const override {
        if (!visible_)
            return;
        out.quad(bounds_, style_);
    }

private:
    unsigned index_ = 0;
    lx::ui::packed_quad_style style_{};
};

// ── Scenario descriptions ───────────────────────────────────────────────────────

struct scenario_spec {
    const char* id = "";
    const char* title = "";
    lx::rect2i view{};
    unsigned arena_bytes = 0;
    lx::ui::describe_fn describe = nullptr;
    void (*before_frame)(unsigned frame) = nullptr;      ///< scene mutation + invalidation
    void (*on_built)(lx::ui::ui_node& root) = nullptr;   ///< once, after the first frame
    unsigned batch_cap = 0;                              ///< 0: no cap for this scenario
};

[[nodiscard]] int scaled(int px, float scale) {
    return static_cast<int>(std::lround(static_cast<float>(px) * scale));
}

// S1 — an idle window: a frame with a shadow, a title row and a body of stock widgets
// (`box`, `text`, `button` emit solid surface draws; the frame emits a shadow and a quad).
// Built once; nothing invalidates afterwards, so `ui_root::tick` must do nothing.

struct s1_scene {
    float scale = 1.f;
};

s1_scene g_s1{};

[[nodiscard]] child describe_s1(lx::ui::build_context&) {
    using namespace lx::ui;
    const float sc = g_s1.scale;

    child title_items[4] = {
        text("lumen-hello", k_text),
        button("min", nullptr, k_element),
        button("max", nullptr, k_element),
        button("close", nullptr, k_element),
    };
    child title = with_children(describe<box_node, box_props>({k_title_bar, true, scaled(8, sc)}),
                                title_items);

    child body_items[3] = {
        text("Hello from the declarative tree", k_text),
        text("nothing changes after the first frame", k_text_muted),
        button("OK", nullptr, k_accent),
    };
    child body = with_children(describe<box_node, box_props>({k_none, false, scaled(8, sc)}),
                               body_items);

    child column_items[2] = {title, body};
    child column = with_children(describe<box_node, box_props>({k_none, false, 0}), column_items);

    card_props frame{};
    frame.style.background = k_background;
    frame.style.border_color = k_border;
    for (float& r : frame.style.corner_radii)
        r = 8.f * sc;
    for (float& w : frame.style.border_widths)
        w = 1.f;
    frame.shadow.color = k_shadow;
    frame.shadow.blur_sigma = 12.f * sc;
    for (float& r : frame.shadow.corner_radii)
        r = 8.f * sc;
    frame.pad = {1, 1, 1, 1};
    return with_children(describe<card_node, card_props>(frame), {&column, 1u});
}

[[nodiscard]] scenario_spec make_s1(const options& opt) {
    g_s1.scale = opt.scale;
    scenario_spec s{};
    s.id = "S1";
    s.title = "idle window";
    s.view = {0, 0, scaled(640, opt.scale), scaled(480, opt.scale)};
    s.arena_bytes = 64 * 1024;
    s.describe = describe_s1;
    return s;
}

// S2 — the shell panel replica: 24 cells, each a bordered rounded quad with a text
// placeholder, GPUI's tokens as literal colours. Every 60th frame the clock cell's label
// changes in place and that label is paint-invalidated: the retained tree is reused with no
// describe, and the damage is that one label.

constexpr unsigned k_s2_cells = 24;
constexpr unsigned k_s2_clock = k_s2_cells - 1;
constexpr lx::ui::element_key k_s2_cell_key = 100;
constexpr lx::ui::element_key k_s2_label_key = 200;

constexpr const char* k_s2_texts[k_s2_cells] = {
    "activities", "ws 1",  "ws 2",  "ws 3",    "firefox",  "terminal", "editor", "files",
    "music",      "chat",  "mail",  "calendar", "notes",   "build",    "logs",   "cpu 12%",
    "mem 3.1G",   "net",   "bt",    "wifi",     "vol 60%", "bat 87%",  "en",     "00:00:00",
};

struct s2_scene {
    int height = 36;
    int pitch = 80;
    int inset = 4;
    int label_pad_x = 8;
    int glyph_w = 7;
    int line_h = 14;
    lx::ui::quad_style panel_style{};
    lx::ui::quad_style cell_style{};
    char clock_text[9] = "00:00:00";
    lx::ui::ui_node* clock_label = nullptr;
};

s2_scene g_s2{};

[[nodiscard]] child describe_s2(lx::ui::build_context&) {
    using namespace lx::ui;
    child cells[k_s2_cells]{};
    for (unsigned i = 0; i < k_s2_cells; ++i) {
        label_props lp{};
        lp.text = i == k_s2_clock ? g_s2.clock_text : k_s2_texts[i];
        lp.color = i == k_s2_clock ? k_text : k_text_muted;
        lp.glyph_w = g_s2.glyph_w;
        lp.line_h = g_s2.line_h;
        child label = describe<label_node, label_props>(lp, k_s2_label_key + i);

        card_props cp{};
        cp.style = g_s2.cell_style;
        cp.pad = {0, g_s2.label_pad_x, 0, g_s2.label_pad_x};
        cells[i] = with_children(describe<card_node, card_props>(cp, k_s2_cell_key + i),
                                 {&label, 1u});
    }
    panel_props pp{};
    pp.style = g_s2.panel_style;
    pp.pitch = g_s2.pitch;
    pp.inset = g_s2.inset;
    return with_children(describe<panel_node, panel_props>(pp), cells);
}

[[nodiscard]] lx::ui::ui_node* find_key(lx::ui::ui_node& node, lx::ui::element_key key) {
    if (node.key() == key)
        return &node;
    for (unsigned i = 0; i < node.child_count(); ++i) {
        if (lx::ui::ui_node* c = node.child_at(i)) {
            if (lx::ui::ui_node* hit = find_key(*c, key))
                return hit;
        }
    }
    return nullptr;
}

void s2_on_built(lx::ui::ui_node& root) {
    g_s2.clock_label = find_key(root, k_s2_label_key + k_s2_clock);
}

void s2_before_frame(unsigned frame) {
    if (frame % 60 != 0 || !g_s2.clock_label)
        return;
    // The clock advances a second per 60 frames; same length every time, so this is a
    // paint change, not a layout one.
    const unsigned seconds = frame / 60;
    const unsigned h = (seconds / 3600) % 24;
    const unsigned m = (seconds / 60) % 60;
    const unsigned s = seconds % 60;
    char* t = g_s2.clock_text;
    t[0] = static_cast<char>('0' + h / 10);
    t[1] = static_cast<char>('0' + h % 10);
    t[3] = static_cast<char>('0' + m / 10);
    t[4] = static_cast<char>('0' + m % 10);
    t[6] = static_cast<char>('0' + s / 10);
    t[7] = static_cast<char>('0' + s % 10);
    lx::ui::invalidate_paint(*g_s2.clock_label);
}

[[nodiscard]] scenario_spec make_s2(const options& opt) {
    const float sc = opt.scale;
    g_s2 = {};
    g_s2.height = scaled(36, sc);
    g_s2.pitch = scaled(80, sc);
    g_s2.inset = scaled(4, sc);
    g_s2.label_pad_x = scaled(8, sc);
    g_s2.glyph_w = scaled(7, sc);
    g_s2.line_h = scaled(14, sc);
    g_s2.panel_style.background = k_background;
    g_s2.panel_style.border_color = k_border;
    g_s2.panel_style.border_widths[2] = 1.f; // bottom
    g_s2.cell_style.background = k_surface;
    g_s2.cell_style.border_color = k_border;
    for (float& r : g_s2.cell_style.corner_radii)
        r = 4.f * sc;
    for (float& w : g_s2.cell_style.border_widths)
        w = 1.f;

    scenario_spec s{};
    s.id = "S2";
    s.title = "shell panel replica";
    s.view = {0, 0, g_s2.pitch * static_cast<int>(k_s2_cells), g_s2.height};
    s.arena_bytes = 64 * 1024;
    s.describe = describe_s2;
    s.before_frame = s2_before_frame;
    s.on_built = s2_on_built;
    s.batch_cap = 8;
    return s;
}

// S3 — 10 000 rounded, bordered quads in a grid, each drifting by a seeded delta every
// frame. Every frame the whole tree is dirty: the drift of every leaf changes and the root
// is animate-invalidated (ADR-010 §4.9's animation path), so every node is laid out and
// emitted again. The three-level tree is what `ui_node::k_max_children` makes of 10 000
// leaves: 25 groups × 25 groups × 16 leaves.

constexpr unsigned k_s3_leaves_per_group = 16;
constexpr unsigned k_s3_groups_per_band = 25;
constexpr unsigned k_s3_band = k_s3_leaves_per_group * k_s3_groups_per_band;

[[nodiscard]] lx::color s3_fill(unsigned index) {
    // A fixed hash per quad, so colours are deterministic and vary across the grid.
    std::uint32_t h = index * 2654435761u;
    h ^= h >> 15u;
    h *= 2246822519u;
    h ^= h >> 13u;
    const float r = 0.25f + 0.55f * static_cast<float>(h & 0xFFu) / 255.f;
    const float g = 0.25f + 0.55f * static_cast<float>((h >> 8u) & 0xFFu) / 255.f;
    const float b = 0.30f + 0.55f * static_cast<float>((h >> 16u) & 0xFFu) / 255.f;
    return lx::color::rgb(r, g, b);
}

[[nodiscard]] child describe_s3(lx::ui::build_context&) {
    using namespace lx::ui;
    child bands[ui_node::k_max_children]{};
    unsigned band_count = 0;
    for (unsigned first = 0; first < g_s3.count && band_count < ui_node::k_max_children;
         first += k_s3_band) {
        child groups[k_s3_groups_per_band]{};
        unsigned group_count = 0;
        const unsigned band_end = first + k_s3_band < g_s3.count ? first + k_s3_band : g_s3.count;
        for (unsigned g = first; g < band_end; g += k_s3_leaves_per_group) {
            child leaves[k_s3_leaves_per_group]{};
            unsigned leaf_count = 0;
            const unsigned group_end =
                g + k_s3_leaves_per_group < band_end ? g + k_s3_leaves_per_group : band_end;
            for (unsigned i = g; i < group_end; ++i)
                leaves[leaf_count++] = describe<drift_quad_node, drift_props>({i, s3_fill(i)});
            groups[group_count++] = with_children(describe<group_node, group_props>({}),
                                                  {leaves, leaf_count});
        }
        bands[band_count++] =
            with_children(describe<group_node, group_props>({}), {groups, group_count});
    }
    card_props bg{};
    bg.style.background = k_background;
    return with_children(describe<card_node, card_props>(bg), {bands, band_count});
}

void s3_on_built(lx::ui::ui_node& root) { g_s3.root = &root; }

void s3_before_frame(unsigned) {
    for (unsigned i = 0; i < g_s3.count; ++i) {
        const std::uint64_t r = g_s3.rng.next();
        int dx = g_s3.drift_x[i] + static_cast<int>(r % 3u) - 1;
        int dy = g_s3.drift_y[i] + static_cast<int>((r >> 8u) % 3u) - 1;
        dx = dx < -2 ? -2 : (dx > 2 ? 2 : dx);
        dy = dy < -2 ? -2 : (dy > 2 ? 2 : dy);
        g_s3.drift_x[i] = static_cast<std::int8_t>(dx);
        g_s3.drift_y[i] = static_cast<std::int8_t>(dy);
    }
    if (g_s3.root)
        lx::ui::invalidate_animate(*g_s3.root);
}

[[nodiscard]] scenario_spec make_s3(const options& opt) {
    g_s3 = {};
    // `--scale` multiplies the quad count; the background quad takes one slot of the cap.
    auto count = static_cast<long>(std::lround(10000.0 * static_cast<double>(opt.scale)));
    const long cap = static_cast<long>(lx::gfx::k_max_primitives) - 1;
    if (count < 1)
        count = 1;
    if (count > cap)
        count = cap;
    g_s3.count = static_cast<unsigned>(count);
    g_s3.rows = (g_s3.count + g_s3.cols - 1) / g_s3.cols;
    g_s3.style.background = k_surface;
    g_s3.style.border_color = k_border;
    for (float& r : g_s3.style.corner_radii)
        r = 3.f;
    for (float& w : g_s3.style.border_widths)
        w = 1.f;
    g_s3.drift_x.assign(g_s3.count, 0);
    g_s3.drift_y.assign(g_s3.count, 0);

    scenario_spec s{};
    s.id = "S3";
    s.title = "drifting rounded quads";
    s.view = {0, 0, static_cast<int>(g_s3.cols) * g_s3.pitch_x + 2 * g_s3.origin_x,
              static_cast<int>(g_s3.rows) * g_s3.pitch_y + 2 * g_s3.origin_y};
    s.arena_bytes = 8 * 1024 * 1024;
    s.describe = describe_s3;
    s.before_frame = s3_before_frame;
    s.on_built = s3_on_built;
    s.batch_cap = 8;
    return s;
}

// ── The measured loop ───────────────────────────────────────────────────────────

struct counters {
    unsigned frames_produced = 0;
    unsigned frames_idle = 0;
    unsigned frames_described = 0;
    unsigned nodes = 0;
    unsigned primitives_built = 0; ///< what the first frame emitted (the tree as built)
    unsigned primitives = 0;       ///< per produced frame, max over the window
    std::uint64_t primitives_total = 0;
    unsigned surfaces = 0;
    unsigned refs = 0;
    unsigned batches = 0;
    std::uint64_t instance_bytes = 0;
    unsigned damage_rects = 0;
    std::uint64_t damage_px = 0;
    double damage_fraction = 0.0;
    unsigned overflow_drops = 0;
    std::uint64_t raster_px = 0; ///< pixels the backend shaded, max per frame
    unsigned draw_calls = 0;     ///< `gl`: instanced and surface draws per frame, max
    unsigned gpu_disjoint = 0;   ///< `gl`: frames whose timer query was disjoint (not sampled)
};

struct memory_stats {
    std::uint64_t allocations = 0;
    std::uint64_t allocated_bytes = 0;
    double per_frame = 0.0;
    long long mallinfo_delta = 0;
    long rss_anon_kb = 0;
};

/// ADR-010 §8.1: a p95 is judged over at least this many frames.
constexpr unsigned k_min_gate_samples = 600;

struct gate {
    const char* id = "";
    /// "pass", "fail", or "insufficient": a p95 gate with fewer than `k_min_gate_samples`
    /// samples, which no verdict can be drawn from.
    const char* verdict = "fail";
    char detail[200]{};
};

struct scenario_result {
    scenario_spec spec{};
    unsigned frames = 0;
    unsigned warmup = 0;
    summary stages[st_count]{};
    counters cnt{};
    memory_stats mem{};
    gate gates[3]{};
    unsigned gate_count = 0;
};

[[nodiscard]] unsigned count_nodes(const lx::ui::ui_node& node) {
    unsigned n = 1;
    for (unsigned i = 0; i < node.child_count(); ++i) {
        if (const lx::ui::ui_node* c = node.child_at(i))
            n += count_nodes(*c);
    }
    return n;
}

/// What a backend does with the sorted stream: `draw_list::encode_instances`, each batch's
/// instances in order into the ring it uploads. Surface refs stay in the draw-command path
/// and copy nothing.
[[nodiscard]] std::uint64_t encode(const lx::scene::draw_list& draws, std::byte* ring,
                                   std::size_t capacity) {
    const std::size_t off = draws.encode_instances({ring, capacity});
    if (off >= sizeof(std::uint32_t) && off <= capacity) {
        std::uint32_t word = 0;
        std::memcpy(&word, ring, sizeof word);
        g_sink = g_sink ^ word;
    }
    return off;
}

[[nodiscard]] long read_rss_anon_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long kb = -1;
    while (std::fgets(line, sizeof line, f)) {
        if (std::strncmp(line, "RssAnon:", 8) == 0) {
            kb = std::strtol(line + 8, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

/// Sleeps until `deadline_ns` on CLOCK_MONOTONIC: the frame clock.
void sleep_until_ns(std::uint64_t deadline_ns) {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(deadline_ns / 1'000'000'000ull);
    ts.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000ull);
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

[[nodiscard]] scenario_result run_scenario(const scenario_spec& spec, const options& opt) {
    scenario_result out{};
    out.spec = spec;
    out.frames = opt.frames;
    out.warmup = opt.warmup;

    lx::ui::ui_damage().clear();
    lx::runtime::memory_arena arena{spec.arena_bytes};
    lx::ui::ui_root root{arena};
    root.set_describer(spec.describe);
    root.set_bounds(spec.view);
    lx::scene::draw_list draws{};

    // Everything the window touches is sized now, so the window itself allocates nothing.
    const std::size_t ring_bytes =
        static_cast<std::size_t>(lx::gfx::k_max_primitives) * sizeof(lx::gfx::quad_instance);
    std::vector<std::byte> ring(ring_bytes);
    std::vector<std::uint64_t> samples[st_count];
    for (auto& s : samples)
        s.reserve(opt.frames);

    // The `cpu` backend's target: XRGB8888 at the view's size, written inside the damage.
    const bool raster = std::strcmp(opt.backend, "cpu") == 0;
    std::vector<std::uint32_t> target_px;
    lx::gfx::pixel_surface target{};
    if (raster) {
        target_px.assign(static_cast<std::size_t>(spec.view.width) *
                             static_cast<std::size_t>(spec.view.height),
                         0u);
        target.pixels = reinterpret_cast<unsigned char*>(target_px.data());
        target.width = static_cast<unsigned>(spec.view.width);
        target.height = static_cast<unsigned>(spec.view.height);
        target.stride = target.width * 4u;
        target.format = static_cast<lx::fourcc>(lx::pixel_format::xrgb8888);
    }
    // Only built for `cpu`: `none` keeps the heap and mappings of Phase 0, so its baseline
    // stays comparable.
    std::optional<lx::gfx::ui_cpu_renderer> rasteriser{};
    if (raster)
        rasteriser.emplace();
    // `gl`: an RGBA8 framebuffer object of the view's size in the run's context.
    const bool gl = std::strcmp(opt.backend, "gl") == 0 && g_gl;
    std::optional<lx::gfx::ui_gl_target> gl_target{};
    if (gl) {
        auto t = lx::gfx::ui_gl_target::create(static_cast<unsigned>(spec.view.width),
                                               static_cast<unsigned>(spec.view.height));
        if (t)
            gl_target.emplace(std::move(t).value());
        else
            std::fprintf(stderr, "lumen-ui-bench: gl target: %s\n", t.get_error().message);
    }
    // `vulkan`: an R8G8B8A8_UNORM image of the view's size on the run's device.
    const bool vk = std::strcmp(opt.backend, "vulkan") == 0 && g_vk;
    std::optional<lx::gfx::ui_vk_target> vk_target{};
    if (vk) {
        auto t = lx::gfx::ui_vk_target::create(g_vk->device.context(),
                                               static_cast<unsigned>(spec.view.width),
                                               static_cast<unsigned>(spec.view.height));
        if (t)
            vk_target.emplace(std::move(t).value());
        else
            std::fprintf(stderr, "lumen-ui-bench: vulkan target: %s\n", t.get_error().message);
    }
    const lx::color clear = k_background;

    const double view_px =
        static_cast<double>(spec.view.width) * static_cast<double>(spec.view.height);
    counters& cnt = out.cnt;
    bool built = false;
    std::uint64_t allocs_start = 0;
    std::uint64_t bytes_start = 0;
    std::size_t uordblks_start = 0;

    const std::uint64_t period_ns = static_cast<std::uint64_t>(opt.period_us) * 1000ull;
    std::uint64_t deadline = now_ns();

    const unsigned total = opt.warmup + opt.frames;
    for (unsigned frame = 0; frame < total; ++frame) {
        if (period_ns) {
            // An absolute schedule, so a late frame does not shift every later one; a
            // frame that overran a whole period restarts the schedule instead of bursting.
            deadline += period_ns;
            const std::uint64_t now = now_ns();
            if (deadline + period_ns < now)
                deadline = now;
            else if (deadline > now)
                sleep_until_ns(deadline);
        }
        const bool measuring = frame >= opt.warmup;
        if (frame == opt.warmup) {
            uordblks_start = ::mallinfo2().uordblks;
            allocs_start = g_new_calls.load(std::memory_order_relaxed);
            bytes_start = g_new_bytes.load(std::memory_order_relaxed);
        }

        const std::uint64_t t_update0 = now_ns();
        if (spec.before_frame)
            spec.before_frame(frame);
        const std::uint64_t t_update1 = now_ns();

        // The ledger is consumed by the tick: read what this frame asks for before it.
        const lx::ui::damage_ledger& ledger = lx::ui::ui_damage();
        const unsigned damage_rects = ledger.count();
        std::uint64_t damage_px = 0;
        lx::gfx::damage_region damage{};
        for (unsigned i = 0; i < damage_rects; ++i) {
            const lx::rect2i r = ledger.rects()[i];
            if (r.width > 0 && r.height > 0)
                damage_px += static_cast<std::uint64_t>(r.width) * static_cast<std::uint64_t>(r.height);
            damage.add(r);
        }
        // A frame with no rects (an animation tick, the first build) repaints the view.
        if (damage.empty())
            damage.set_full();

        const std::uint64_t t0 = now_ns();
        const bool produced = root.tick(draws);
        const std::uint64_t t1 = now_ns();

        if (!produced) {
            if (measuring) {
                samples[st_update].push_back(t_update1 - t_update0);
                samples[st_tick_idle].push_back(t1 - t0);
                ++cnt.frames_idle;
            }
            continue;
        }

        const std::uint64_t t2 = now_ns();
        draws.batch_for_render();
        const std::uint64_t t3 = now_ns();
        if (lx::trace::timeline_enabled())
            lx::trace::timeline_counter("ui.batches", static_cast<long long>(draws.batches().size()));
        const std::uint64_t bytes = encode(draws, ring.data(), ring.size());
        const std::uint64_t t4 = now_ns();
        std::uint64_t raster_px = 0;
        lx::gfx::ui_stream stream{};
        stream.refs = draws.refs();
        stream.batches = draws.batches();
        stream.quads = draws.quad_instances();
        stream.shadows = draws.shadow_instances();
        stream.sprites = draws.sprite_instances();
        stream.underlines = draws.underline_instances();
        if (raster) {
            auto drawn = rasteriser->draw(target, stream, nullptr, damage, clear);
            if (drawn)
                raster_px = drawn->pixels_shaded;
        }
        unsigned draw_calls = 0;
        if (gl && gl_target) {
            auto drawn = g_gl->renderer.draw(*gl_target, stream, nullptr, damage, clear);
            if (drawn)
                draw_calls = drawn->draw_calls;
        }
        if (vk && vk_target) {
            auto drawn = g_vk->renderer.draw(*vk_target, stream, nullptr, damage, clear);
            if (drawn)
                draw_calls = drawn->draw_calls;
        }
        const std::uint64_t t5 = now_ns();
        // The GPU stage: waiting for it here is outside every CPU stage above.
        std::uint64_t gpu_ns = 0;
        bool gpu_ok = false;
        if (gl && gl_target) {
            auto timed = g_gl->renderer.wait_gpu_time();
            if (timed && timed->source != lx::gfx::ui_gl_timing::none) {
                gpu_ok = !timed->disjoint;
                gpu_ns = timed->ns;
                if (timed->disjoint && measuring)
                    ++cnt.gpu_disjoint;
            }
        }
        if (vk && vk_target) {
            auto timed = g_vk->renderer.wait_gpu_time();
            if (timed && timed->source != lx::gfx::ui_vk_timing::none) {
                gpu_ok = true;
                gpu_ns = timed->ns;
            }
        }

        if (!built) {
            built = true;
            if (root.tree()) {
                cnt.nodes = count_nodes(*root.tree());
                if (spec.on_built)
                    spec.on_built(*root.tree());
            }
            cnt.primitives_built = draws.primitive_count();
        }

        if (!measuring)
            continue;

        const lx::ui::ui_root::stage_times st = root.last_stage_times();
        samples[st_update].push_back(t_update1 - t_update0);
        samples[st_tick].push_back(t1 - t0);
        if (st.described) {
            samples[st_describe].push_back(st.describe);
            samples[st_reconcile].push_back(st.reconcile);
            ++cnt.frames_described;
        }
        samples[st_layout].push_back(st.layout);
        samples[st_emit].push_back(st.emit);
        samples[st_order].push_back(t3 - t2);
        samples[st_encode].push_back(t4 - t3);
        samples[st_emit_order_encode].push_back(st.emit + (t3 - t2) + (t4 - t3));
        samples[st_frame].push_back(t4 - t0);
        if (raster)
            samples[st_raster].push_back(t5 - t4);
        if ((gl && gl_target) || (vk && vk_target)) {
            samples[st_submit].push_back(t5 - t4);
            if (gpu_ok)
                samples[st_gpu].push_back(gpu_ns);
        }
        if (raster_px > cnt.raster_px)
            cnt.raster_px = raster_px;
        if (draw_calls > cnt.draw_calls)
            cnt.draw_calls = draw_calls;
        ++cnt.frames_produced;

        const unsigned primitives = draws.primitive_count();
        cnt.primitives_total += primitives;
        if (primitives > cnt.primitives) cnt.primitives = primitives;
        if (draws.size() > cnt.surfaces) cnt.surfaces = draws.size();
        const auto refs = static_cast<unsigned>(draws.refs().size());
        if (refs > cnt.refs) cnt.refs = refs;
        const auto batches = static_cast<unsigned>(draws.batches().size());
        if (batches > cnt.batches) cnt.batches = batches;
        if (bytes > cnt.instance_bytes) cnt.instance_bytes = bytes;
        if (damage_rects > cnt.damage_rects) cnt.damage_rects = damage_rects;
        if (damage_px > cnt.damage_px) cnt.damage_px = damage_px;
        cnt.overflow_drops += draws.overflow_drops();
    }

    const std::size_t uordblks_end = ::mallinfo2().uordblks;
    memory_stats& mem = out.mem;
    mem.allocations = g_new_calls.load(std::memory_order_relaxed) - allocs_start;
    mem.allocated_bytes = g_new_bytes.load(std::memory_order_relaxed) - bytes_start;
    mem.per_frame = opt.frames > 0 ? static_cast<double>(mem.allocations) / opt.frames : 0.0;
    mem.mallinfo_delta = static_cast<long long>(uordblks_end) - static_cast<long long>(uordblks_start);
    mem.rss_anon_kb = read_rss_anon_kb();

    if (view_px > 0.0)
        cnt.damage_fraction = static_cast<double>(cnt.damage_px) / view_px;

    for (unsigned i = 0; i < st_count; ++i)
        out.stages[i] = summarize(samples[i]);

    // ── Gates (ADR-010 §3), what Phase 0 can already check ──
    const auto add_gate = [&](const char* id, const char* verdict, const char* fmt,
                              auto... args) {
        if (out.gate_count >= 3)
            return;
        gate& g = out.gates[out.gate_count++];
        g.id = id;
        g.verdict = verdict;
        std::snprintf(g.detail, sizeof g.detail, fmt, args...);
    };
    const auto verdict = [](bool pass) { return pass ? "pass" : "fail"; };
    // A p95 bound is judged only over enough frames (§8.1); the gate's other bounds are
    // judged whatever the sample count.
    const auto p95_verdict = [&](const summary& st, bool p95_ok, bool rest_ok) {
        if (!rest_ok)
            return "fail";
        return st.samples < k_min_gate_samples ? "insufficient" : verdict(p95_ok);
    };
    // G5: no operator new and no net malloc growth over the steady window.
    const bool no_allocs = mem.allocations == 0 && mem.mallinfo_delta == 0;
    if (std::strcmp(spec.id, "S1") == 0) {
        const bool idle = cnt.frames_produced == 0 && cnt.primitives_total == 0;
        add_gate("G1", verdict(idle), "%u frames produced, %llu primitives emitted over %u idle ticks",
                 cnt.frames_produced, static_cast<unsigned long long>(cnt.primitives_total),
                 opt.frames);
    } else if (std::strcmp(spec.id, "S2") == 0) {
        const summary& st = out.stages[st_frame];
        const bool damage_ok = cnt.damage_rects <= 1 && cnt.damage_fraction <= 0.02;
        add_gate("G2", p95_verdict(st, st.p95 <= 150.0, damage_ok),
                 "frame p95 %.1f us (<= 150) over %u frames (>= %u), damage %u rect(s), %.2f%% "
                 "of the surface (<= 2%%)",
                 st.p95, st.samples, k_min_gate_samples, cnt.damage_rects,
                 cnt.damage_fraction * 100.0);
    } else if (std::strcmp(spec.id, "S3") == 0) {
        const summary& st = out.stages[st_emit_order_encode];
        const bool size_ok = cnt.batches <= 8 && cnt.instance_bytes <= 800u * 1024u;
        add_gate("G3", p95_verdict(st, st.p95 <= 1000.0, size_ok),
                 "emit+order+encode p95 %.1f us (<= 1000) over %u frames, %u batch(es) (<= 8), "
                 "%llu instance bytes (<= 819200)",
                 st.p95, st.samples, cnt.batches, static_cast<unsigned long long>(cnt.instance_bytes));
    }
    add_gate("G5", verdict(no_allocs), "%llu heap allocations in %u steady frames, mallinfo2 delta %lld B",
             static_cast<unsigned long long>(mem.allocations), opt.frames, mem.mallinfo_delta);
    if (vk && vk_target)
        g_vk->renderer.wait_idle(); // the target goes with this frame
    return out;
}

// ── Output ──────────────────────────────────────────────────────────────────────

struct host_info {
    char kernel[128] = "unknown";
    char nodename[128] = "unknown";
    char machine[64] = "unknown";
    char cpu[160] = "unknown";
    unsigned cpus = 1;
};

[[nodiscard]] host_info read_host() {
    host_info h{};
    utsname u{};
    if (::uname(&u) == 0) {
        std::snprintf(h.kernel, sizeof h.kernel, "%s", u.release);
        std::snprintf(h.nodename, sizeof h.nodename, "%s", u.nodename);
        std::snprintf(h.machine, sizeof h.machine, "%s", u.machine);
    }
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    h.cpus = n > 0 ? static_cast<unsigned>(n) : 1u;
    if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (std::strncmp(line, "model name", 10) == 0) {
                const char* colon = std::strchr(line, ':');
                if (colon) {
                    ++colon;
                    while (*colon == ' ' || *colon == '\t')
                        ++colon;
                    std::snprintf(h.cpu, sizeof h.cpu, "%s", colon);
                    if (char* nl = std::strchr(h.cpu, '\n'))
                        *nl = '\0';
                }
                break;
            }
        }
        std::fclose(f);
    }
    return h;
}

void print_table(const scenario_result& r) {
    std::printf("\n%s  %s  (%u nodes, %dx%d, %u frames after %u warm-up)\n", r.spec.id,
                r.spec.title, r.cnt.nodes, r.spec.view.width, r.spec.view.height, r.frames,
                r.warmup);
    std::printf("  %-20s %8s %10s %10s %10s %10s\n", "stage", "samples", "p50 us", "p95 us",
                "p99 us", "max us");
    for (unsigned i = 0; i < st_count; ++i) {
        const summary& s = r.stages[i];
        if (s.samples == 0)
            continue;
        std::printf("  %-20s %8u %10.3f %10.3f %10.3f %10.3f\n", k_stage_names[i], s.samples,
                    s.p50, s.p95, s.p99, s.max);
    }
    const counters& c = r.cnt;
    std::printf("  counters   frames_produced=%u frames_idle=%u frames_described=%u "
                "primitives=%u (built %u) surfaces=%u refs=%u batches=%u instance_bytes=%llu "
                "damage_rects=%u damage_px=%llu (%.2f%%) overflow_drops=%u raster_px=%llu "
                "draw_calls=%u gpu_disjoint=%u\n",
                c.frames_produced, c.frames_idle, c.frames_described, c.primitives,
                c.primitives_built, c.surfaces, c.refs, c.batches,
                static_cast<unsigned long long>(c.instance_bytes), c.damage_rects,
                static_cast<unsigned long long>(c.damage_px), c.damage_fraction * 100.0,
                c.overflow_drops, static_cast<unsigned long long>(c.raster_px), c.draw_calls,
                c.gpu_disjoint);
    const memory_stats& m = r.mem;
    std::printf("  memory     allocations=%llu (%.3f/frame, %llu B)  mallinfo2.uordblks delta=%lld B  "
                "RssAnon=%ld kB\n",
                static_cast<unsigned long long>(m.allocations), m.per_frame,
                static_cast<unsigned long long>(m.allocated_bytes), m.mallinfo_delta,
                m.rss_anon_kb);
    std::printf("  gates     ");
    for (unsigned i = 0; i < r.gate_count; ++i)
        std::printf(" %s %s (%s)", r.gates[i].id,
                    std::strcmp(r.gates[i].verdict, "fail") == 0 ? "FAIL" : r.gates[i].verdict,
                    r.gates[i].detail);
    std::printf("\n");
}

void json_string(std::FILE* f, const char* s) {
    std::fputc('"', f);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            std::fputc('\\', f);
        if (static_cast<unsigned char>(*s) < 0x20)
            continue;
        std::fputc(*s, f);
    }
    std::fputc('"', f);
}

void write_json(const options& opt, const host_info& host, const scenario_result* results,
                unsigned count) {
    std::FILE* f = std::fopen(opt.json, "w");
    if (!f) {
        std::fprintf(stderr, "lumen-ui-bench: cannot write %s\n", opt.json);
        return;
    }
    std::fputs("[\n", f);
    for (unsigned n = 0; n < count; ++n) {
        const scenario_result& r = results[n];
        std::fputs("  {\n    \"host\": {\"kernel\": ", f);
        json_string(f, host.kernel);
        std::fprintf(f, ", \"cpus\": %u, \"cpu\": ", host.cpus);
        json_string(f, host.cpu);
        std::fputs(", \"arch\": ", f);
        json_string(f, host.machine);
        std::fputs(", \"hostname\": ", f);
        json_string(f, host.nodename);
        std::fputs("},\n    \"driver\": ", f);
        // A CPU Vulkan device is its own driver: never compared with, nor taken for, a GPU.
        const bool vk_software = g_vk_info.valid && g_vk_info.caps.software &&
                                 std::strcmp(opt.backend, "vulkan") == 0;
        json_string(f, vk_software ? "vulkan-software" : opt.backend);
        std::fputs(",\n    \"git\": ", f);
        json_string(f, opt.git);
        std::fputs(",\n    \"preset\": ", f);
        json_string(f, opt.preset);
        std::fprintf(f, ",\n    \"scenario\": \"%s\",\n    \"title\": ", r.spec.id);
        json_string(f, r.spec.title);
        std::fprintf(f, ",\n    \"backend\": ");
        json_string(f, opt.backend);
        std::fprintf(f, ",\n    \"frames\": %u,\n    \"warmup\": %u,\n    \"period_us\": %u,\n"
                        "    \"scale\": %.3f,\n",
                     r.frames, r.warmup, opt.period_us, static_cast<double>(opt.scale));
        std::fprintf(f, "    \"view\": {\"width\": %d, \"height\": %d},\n", r.spec.view.width,
                     r.spec.view.height);
        std::fputs("    \"stages\": {\n", f);
        for (unsigned i = 0; i < st_count; ++i) {
            const summary& s = r.stages[i];
            std::fprintf(f,
                         "      \"%s\": {\"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"max\": %.3f, "
                         "\"samples\": %u}%s\n",
                         k_stage_names[i], s.p50, s.p95, s.p99, s.max, s.samples,
                         i + 1 < st_count ? "," : "");
        }
        std::fputs("    },\n", f);
        const counters& c = r.cnt;
        std::fprintf(f,
                     "    \"counters\": {\"frames_produced\": %u, \"frames_idle\": %u, "
                     "\"frames_described\": %u, \"nodes\": %u, \"primitives\": %u, "
                     "\"primitives_built\": %u, \"primitives_total\": %llu, \"surfaces\": %u, "
                     "\"refs\": %u, \"batches\": %u, \"instance_bytes\": %llu, "
                     "\"damage_rects\": %u, \"damage_px\": %llu, \"damage_fraction\": %.5f, "
                     "\"overflow_drops\": %u, \"raster_px\": %llu, \"draw_calls\": %u, "
                     "\"gpu_disjoint\": %u},\n",
                     c.frames_produced, c.frames_idle, c.frames_described, c.nodes, c.primitives,
                     c.primitives_built, static_cast<unsigned long long>(c.primitives_total),
                     c.surfaces, c.refs, c.batches,
                     static_cast<unsigned long long>(c.instance_bytes), c.damage_rects,
                     static_cast<unsigned long long>(c.damage_px), c.damage_fraction,
                     c.overflow_drops, static_cast<unsigned long long>(c.raster_px),
                     c.draw_calls, c.gpu_disjoint);
        const memory_stats& m = r.mem;
        std::fprintf(f,
                     "    \"memory\": {\"allocations\": %llu, \"allocated_bytes\": %llu, "
                     "\"allocations_per_steady_frame\": %.6f, \"mallinfo2_uordblks_delta\": %lld, "
                     "\"rss_anon_kb\": %ld},\n",
                     static_cast<unsigned long long>(m.allocations),
                     static_cast<unsigned long long>(m.allocated_bytes), m.per_frame,
                     m.mallinfo_delta, m.rss_anon_kb);
        if (g_gl_info.valid && std::strcmp(opt.backend, "gl") == 0) {
            const lx::gfx::ui_gl_caps& caps = g_gl_info.caps;
            std::fputs("    \"gpu\": {\"renderer\": ", f);
            json_string(f, caps.renderer);
            std::fputs(", \"version\": ", f);
            json_string(f, caps.version);
            std::fprintf(f,
                         ", \"fetch\": \"%s\", \"timing\": \"%s\", \"timer_query_reads_zero\": %s, "
                         "\"ring\": \"%s\", "
                         "\"program_binary_formats\": %d, "
                         "\"program_cache\": %s, \"program_setup_cold_us\": %.1f, "
                         "\"program_setup_again_us\": %.1f, \"programs_from_cache\": %u},\n",
                         caps.fetch == lx::gfx::ui_gl_fetch::attributes ? "attributes" : "storage",
                         caps.timer_query ? "timer_query" : "fence-wall",
                         caps.timer_query_reads_zero ? "true" : "false", caps.persistent_ring ? "persistent" : "mapped-range",
                         caps.program_binary_formats, caps.program_cache ? "true" : "false",
                         static_cast<double>(g_gl_info.setup_cold_ns) / 1000.0,
                         static_cast<double>(g_gl_info.setup_warm_ns) / 1000.0, g_gl_info.from_cache);
        } else if (g_vk_info.valid && std::strcmp(opt.backend, "vulkan") == 0) {
            const lx::gfx::ui_vk_caps& caps = g_vk_info.caps;
            std::fputs("    \"gpu\": {\"device\": ", f);
            json_string(f, caps.device_name);
            std::fprintf(f,
                         ", \"api\": \"%u.%u\", \"software\": %s, \"timing\": \"%s\", "
                         "\"pipeline_cache\": %s, \"pipeline_cache_bytes_loaded\": %llu, "
                         "\"pipeline_setup_cold_us\": %.1f, \"pipeline_setup_again_us\": %.1f},\n",
                         caps.api_version >> 22u, (caps.api_version >> 12u) & 0x3FFu,
                         caps.software ? "true" : "false",
                         caps.timestamps ? "timestamp" : "fence-wall",
                         caps.pipeline_cache ? "true" : "false",
                         static_cast<unsigned long long>(g_vk_info.cache_bytes_loaded),
                         static_cast<double>(g_vk_info.setup_cold_ns) / 1000.0,
                         static_cast<double>(g_vk_info.setup_warm_ns) / 1000.0);
        } else {
            std::fputs("    \"gpu\": {},\n", f);
        }
        std::fputs("    \"gates\": {", f);
        for (unsigned i = 0; i < r.gate_count; ++i)
            std::fprintf(f, "%s\"%s\": \"%s\"", i ? ", " : "", r.gates[i].id,
                         r.gates[i].verdict);
        std::fprintf(f, "}\n  }%s\n", n + 1 < count ? "," : "");
    }
    std::fputs("]\n", f);
    std::fclose(f);
}

// ── CLI ─────────────────────────────────────────────────────────────────────────

void usage() {
    std::fputs("usage: lumen-ui-bench [--scenario S1|S2|S3|all] [--backend none|cpu|gl|vulkan] [--frames N]\n"
               "                      [--warmup N] [--period-us US] [--json PATH] [--pin CPU|none]\n"
               "                      [--scale F] [--git SHA] [--preset NAME]\n"
               "                      [--gl-ring persistent|mapped] [--gl-fetch storage|attributes]\n",
               stderr);
}

[[nodiscard]] bool parse(int argc, char** argv, options& opt) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
        const auto take = [&]() -> const char* {
            if (!v) {
                std::fprintf(stderr, "lumen-ui-bench: %s needs a value\n", a);
                return nullptr;
            }
            ++i;
            return v;
        };
        if (std::strcmp(a, "--scenario") == 0) {
            const char* s = take();
            if (!s) return false;
            if (std::strcmp(s, "all") == 0) {
                opt.scenarios[0] = opt.scenarios[1] = opt.scenarios[2] = true;
            } else if (std::strlen(s) == 2 && s[0] == 'S' && s[1] >= '1' && s[1] <= '3') {
                opt.scenarios[0] = opt.scenarios[1] = opt.scenarios[2] = false;
                opt.scenarios[s[1] - '1'] = true;
            } else {
                std::fprintf(stderr, "lumen-ui-bench: unknown scenario %s\n", s);
                return false;
            }
        } else if (std::strcmp(a, "--backend") == 0) {
            const char* s = take();
            if (!s) return false;
            if (std::strcmp(s, "none") != 0 && std::strcmp(s, "cpu") != 0 &&
                std::strcmp(s, "gl") != 0 && std::strcmp(s, "vulkan") != 0) {
                std::fprintf(stderr,
                             "lumen-ui-bench: backend %s is not available (none, cpu, gl, vulkan)\n", s);
                return false;
            }
            opt.backend = s;
        } else if (std::strcmp(a, "--gl-ring") == 0) {
            const char* s = take();
            if (!s) return false;
            if (std::strcmp(s, "mapped") != 0 && std::strcmp(s, "persistent") != 0) {
                std::fprintf(stderr, "lumen-ui-bench: --gl-ring is persistent or mapped\n");
                return false;
            }
            opt.gl_mapped_ring = std::strcmp(s, "mapped") == 0;
        } else if (std::strcmp(a, "--gl-fetch") == 0) {
            const char* s = take();
            if (!s) return false;
            if (std::strcmp(s, "attributes") != 0 && std::strcmp(s, "storage") != 0) {
                std::fprintf(stderr, "lumen-ui-bench: --gl-fetch is attributes or storage\n");
                return false;
            }
            opt.gl_attribute_fetch = std::strcmp(s, "attributes") == 0;
        } else if (std::strcmp(a, "--frames") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.frames = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
        } else if (std::strcmp(a, "--warmup") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.warmup = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
        } else if (std::strcmp(a, "--period-us") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.period_us = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
        } else if (std::strcmp(a, "--json") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.json = s;
        } else if (std::strcmp(a, "--pin") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.pin = std::strcmp(s, "none") == 0 ? -1 : static_cast<int>(std::strtol(s, nullptr, 10));
        } else if (std::strcmp(a, "--scale") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.scale = std::strtof(s, nullptr);
            if (!(opt.scale > 0.f)) {
                std::fprintf(stderr, "lumen-ui-bench: --scale must be positive\n");
                return false;
            }
        } else if (std::strcmp(a, "--git") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.git = s;
        } else if (std::strcmp(a, "--preset") == 0) {
            const char* s = take();
            if (!s) return false;
            opt.preset = s;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage();
            return false;
        } else {
            std::fprintf(stderr, "lumen-ui-bench: unknown option %s\n", a);
            usage();
            return false;
        }
    }
    if (opt.frames == 0) {
        std::fprintf(stderr, "lumen-ui-bench: --frames must be at least 1\n");
        return false;
    }
    return true;
}

[[nodiscard]] int pin_cpu(int wanted) {
    if (wanted == -1)
        return -1;
    int cpu = wanted;
    if (cpu == -2) {
        const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
        cpu = n > 0 ? static_cast<int>(n - 1) : 0;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (::sched_setaffinity(0, sizeof set, &set) != 0) {
        std::fprintf(stderr, "lumen-ui-bench: cannot pin to CPU %d (%s); running unpinned\n", cpu,
                     std::strerror(errno));
        return -1;
    }
    return cpu;
}

// ── The bench thread ────────────────────────────────────────────────────────────
//
// The scenarios run on a thread whose stack is a fresh page-aligned mapping, not the main
// thread's: argv and the environment sit above the main stack and shift every frame of it,
// and `draw_scope`'s per-primitive temporaries then alias (4 KiB) against the draw list
// differently from one shell to the next — S2's emit moved 1.7× with 256 bytes more
// environment. On its own stack every address has the same low bits run after run;
// scripts/ui-bench.sh fixes the heap the same way (`setarch -R`).

struct bench_job {
    const options* opt = nullptr;
    const host_info* host = nullptr;
    scenario_result* results = nullptr;
    unsigned count = 0;
    bool backend_failed = false;
};

void* bench_thread(void* raw) {
    bench_job& job = *static_cast<bench_job*>(raw);
    const options& opt = *job.opt;
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    // The `gl` context belongs to this thread; it is torn down here too, before the stack goes.
    std::optional<gl_backend> gl{};
    if (std::strcmp(opt.backend, "gl") == 0) {
        gl.emplace();
        if (!start_gl(*gl, opt)) {
            job.backend_failed = true;
            return nullptr;
        }
        g_gl = &*gl;
        std::fflush(stdout);
    }
    std::optional<vk_backend> vk{};
    if (std::strcmp(opt.backend, "vulkan") == 0) {
        vk.emplace();
        if (!start_vk(*vk)) {
            job.backend_failed = true;
            return nullptr;
        }
        g_vk = &*vk;
        std::fflush(stdout);
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!opt.scenarios[i])
            continue;
        const scenario_spec spec = i == 0 ? make_s1(opt) : i == 1 ? make_s2(opt) : make_s3(opt);
        job.results[job.count] = run_scenario(spec, opt);
        print_table(job.results[job.count]);
        std::fflush(stdout);
        ++job.count;
    }
    g_gl = nullptr;
    g_vk = nullptr;
    return nullptr;
}

[[nodiscard]] bool run_on_own_stack(bench_job& job) {
    constexpr std::size_t k_stack_bytes = 16u << 20u;
    void* stack = ::mmap(nullptr, k_stack_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED)
        return false;
    pthread_attr_t attr;
    pthread_t thread;
    bool ok = ::pthread_attr_init(&attr) == 0 &&
              ::pthread_attr_setstack(&attr, stack, k_stack_bytes) == 0 &&
              ::pthread_create(&thread, &attr, bench_thread, &job) == 0;
    ::pthread_attr_destroy(&attr);
    if (ok)
        ok = ::pthread_join(thread, nullptr) == 0;
    ::munmap(stack, k_stack_bytes);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    options opt{};
    if (!parse(argc, argv, opt))
        return 2;

    // The bench thread inherits the mask.
    const int cpu = pin_cpu(opt.pin);
    // The cap warnings are the only thing the frame path logs; keep them, drop the rest.
    lx::trace::logger::global().set_min_level(lx::trace::level::warn);

    const host_info host = read_host();
    std::printf("lumen-ui-bench  backend=%s  frames=%u  warmup=%u  period-us=%u  scale=%.2f  pin=",
                opt.backend, opt.frames, opt.warmup, opt.period_us, static_cast<double>(opt.scale));
    if (cpu >= 0)
        std::printf("%d", cpu);
    else
        std::printf("none");
    std::printf("  git=%s  preset=%s\n", *opt.git ? opt.git : "-", opt.preset);
    std::printf("host: %s %s, %u cpus, %s\n", host.kernel, host.machine, host.cpus, host.cpu);
    std::fflush(stdout);

    scenario_result results[3]{};
    bench_job job{&opt, &host, results, 0};
    if (!run_on_own_stack(job)) {
        std::fprintf(stderr, "lumen-ui-bench: cannot start the bench thread (%s)\n",
                     std::strerror(errno));
        return 1;
    }
    if (job.backend_failed)
        return 2;

    if (opt.json)
        write_json(opt, host, results, job.count);

    // Never fail on a number — CTest only checks the exit status.
    return 0;
}
