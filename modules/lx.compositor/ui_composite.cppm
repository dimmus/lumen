module;

// ADR-010 §4.11: how the compositor's backends draw one view's snapshot now that it may
// carry UI primitives (server-side decorations' fills).
//
//   - A snapshot without primitives takes exactly the path it always took: its draw
//     commands converted to `blit_command`s in draw order and handed to the backend's
//     composite. Nothing here touches a primitive renderer, an order grid or a stream.
//   - With primitives, CPU and GL draw the sorted stream through `:ui_cpu` / `:ui_gl` into
//     the same target and damage rects: runs of `surface` refs go through the compositor's
//     own blit code (`cpu_compositor::blend`, `gl_compositor::composite_into`), primitive
//     batches through the UI renderer, in stream order.
//   - A path with no primitive renderer (GLES 2, the Vulkan composite until `:ui_vk` records
//     into its linear pass, screen capture's re-composites, the headless dump) takes the
//     stream lowered to blits (`gfx::lower_to_blits`): solid square quads become solid
//     blits, everything else is dropped, counted and warned about once per run of frames
//     that drop.
//
// Colour. A primitive's colour is premultiplied and sRGB-encoded (ADR-010 §4.5), and the CPU
// and GL compositors' UI renderers blend it in that encoded space, as in a client window.
// The compositor's chrome is opaque and square in this stage, so no blending happens and the
// result is the colour itself whichever space a backend blends surfaces in; on Vulkan the
// lowered solid blits are decoded and drawn inside its linear F16 pass like any solid draw.
//
// Render affinity; the storage grows to the largest frame seen and is reused, so a steady
// frame allocates nothing.

export module lx.compositor:ui_composite;

import std;

import lx.foundation;
import lx.gfx;
import lx.scene;
import lx.trace;

export namespace lx::compositor {

/// What one frame's paint did.
struct frame_paint_stats {
    /// The frame had primitives and a UI renderer drew its stream.
    bool stream = false;
    unsigned primitives = 0;
    /// Quads drawn as solid blits (a path without a primitive renderer).
    unsigned lowered = 0;
    /// Primitives that path could not draw.
    unsigned dropped = 0;
};

struct cpu_paint_result {
    gfx::cpu_composite_stats composite{};
    gfx::ui_draw_stats ui{};
    frame_paint_stats paint{};
};

struct gl_paint_result {
    gfx::gl_composite_stats composite{};
    gfx::ui_gl_draw_stats ui{};
    frame_paint_stats paint{};
};

class frame_painter {
public:
    frame_painter() = default;
    ~frame_painter();
    frame_painter(const frame_painter&) = delete;
    frame_painter& operator=(const frame_painter&) = delete;

    /// The snapshot carries UI primitives (a stream with at least one primitive batch).
    [[nodiscard]] static bool has_primitives(const scene::immutable_frame_snapshot& snap);

    /// The snapshot's draw commands as blits, in draw order — what `surface` refs index.
    [[nodiscard]] std::span<const gfx::blit_command>
    surfaces(const scene::immutable_frame_snapshot& snap);
    /// What a backend without a primitive renderer composites: `surfaces(snap)` when the
    /// snapshot has no primitives, else its stream lowered in sorted order. Counts what was
    /// lowered and dropped into `stats` when given, and warns once per run of dropping frames.
    [[nodiscard]] std::span<const gfx::blit_command>
    flattened(const scene::immutable_frame_snapshot& snap, frame_paint_stats* stats = nullptr);
    /// The snapshot's stream over the blits `surfaces(snap)` last built.
    [[nodiscard]] gfx::ui_stream stream(const scene::immutable_frame_snapshot& snap) const;

    /// The CPU compositor's frame into `target`, restricted to `region`. `ui` null: the
    /// lowered path for frames with primitives.
    [[nodiscard]] lx::result<cpu_paint_result> paint_cpu(gfx::cpu_compositor& compositor,
                                                         gfx::ui_cpu_renderer* ui,
                                                         const gfx::pixel_surface& target,
                                                         const scene::immutable_frame_snapshot& snap,
                                                         const gfx::damage_region& region);
    /// The GL compositor's frame into `framebuffer` (`width` × `height`, row 0 on top — a
    /// scanout target's), restricted to `region`, closed by one fence (`composite.out_fence_fd`,
    /// owned by the caller; -1 after a glFinish). `ui` null or not ready (a GLES 2 context):
    /// the lowered path for frames with primitives.
    [[nodiscard]] lx::result<gl_paint_result> paint_gl(gfx::gl_compositor& compositor,
                                                       gfx::ui_gl_renderer* ui,
                                                       unsigned framebuffer, unsigned width,
                                                       unsigned height,
                                                       const scene::immutable_frame_snapshot& snap,
                                                       const gfx::damage_region& region);

    /// Frames painted through a UI renderer / lowered / as plain blits, since construction.
    [[nodiscard]] unsigned long long stream_frames() const { return stream_frames_; }
    [[nodiscard]] unsigned long long lowered_frames() const { return lowered_frames_; }
    [[nodiscard]] unsigned long long blit_frames() const { return blit_frames_; }
    [[nodiscard]] unsigned long long dropped_primitives() const { return dropped_total_; }

private:
    [[nodiscard]] gfx::blit_command* grow(gfx::blit_command*& storage, unsigned& capacity,
                                          unsigned need);
    void note_drops(unsigned dropped);

    gfx::blit_command* blits_ = nullptr;
    unsigned blit_capacity_ = 0;
    unsigned blit_count_ = 0;
    gfx::blit_command* flat_ = nullptr;
    unsigned flat_capacity_ = 0;
    bool drop_warned_ = false;
    unsigned long long stream_frames_ = 0;
    unsigned long long lowered_frames_ = 0;
    unsigned long long blit_frames_ = 0;
    unsigned long long dropped_total_ = 0;
};

} // namespace lx::compositor

lx::compositor::frame_painter::~frame_painter() {
    delete[] blits_;
    delete[] flat_;
}

lx::gfx::blit_command* lx::compositor::frame_painter::grow(gfx::blit_command*& storage,
                                                         unsigned& capacity, unsigned need) {
    if (need == 0)
        return storage;
    if (need > capacity) {
        // Grows to the largest frame seen, never per frame once there.
        unsigned cap = capacity > 0 ? capacity : 64u;
        while (cap < need)
            cap *= 2;
        delete[] storage;
        storage = new gfx::blit_command[cap];
        capacity = cap;
    }
    return storage;
}

bool lx::compositor::frame_painter::has_primitives(const scene::immutable_frame_snapshot& snap) {
    return snap.draws().primitive_count() > 0 && !snap.batches().empty();
}

std::span<const lx::gfx::blit_command>
lx::compositor::frame_painter::surfaces(const scene::immutable_frame_snapshot& snap) {
    const auto& draws = snap.draws();
    const unsigned n = draws.size();
    gfx::blit_command* out = grow(blits_, blit_capacity_, n);
    for (unsigned i = 0; i < n; ++i) {
        const auto& cmd = draws.data()[i];
        gfx::blit_command& b = out[i];
        b = {};
        b.texture_id = cmd.texture.id();
        b.dst = cmd.dst;
        b.src = cmd.src;
        b.clip = cmd.clip;
        b.tint = cmd.tint;
        b.opacity = cmd.opacity;
        b.blend = cmd.blend;
        b.buffer_xform = cmd.buffer_xform;
        b.src_space = cmd.src_space;
        b.scaled = cmd.scaled;
        b.occluded = cmd.occluded;
        b.solid = cmd.solid;
        b.solid_color = cmd.solid_color;
    }
    blit_count_ = n;
    return {out, n};
}

lx::gfx::ui_stream
lx::compositor::frame_painter::stream(const scene::immutable_frame_snapshot& snap) const {
    const auto& d = snap.draws();
    gfx::ui_stream s{};
    s.refs = d.refs();
    s.batches = d.batches();
    s.quads = d.quad_instances();
    s.shadows = d.shadow_instances();
    s.sprites = d.sprite_instances();
    s.underlines = d.underline_instances();
    s.surfaces = {blits_, blit_count_};
    return s;
}

void lx::compositor::frame_painter::note_drops(unsigned dropped) {
    dropped_total_ += dropped;
    if (dropped == 0) {
        drop_warned_ = false;
        return;
    }
    if (drop_warned_)
        return;
    drop_warned_ = true;
    lx::trace::logger::global().logf(lx::trace::level::warn, "compositor.render",
                                     "no primitive renderer on this path: dropped={} "
                                     "(solid square quads are drawn as blits)",
                                     dropped);
}

std::span<const lx::gfx::blit_command>
lx::compositor::frame_painter::flattened(const scene::immutable_frame_snapshot& snap,
                                         frame_paint_stats* stats) {
    const std::span<const gfx::blit_command> plain = surfaces(snap);
    if (!has_primitives(snap))
        return plain;
    const gfx::ui_stream s = stream(snap);
    const unsigned need = static_cast<unsigned>(s.refs.size());
    gfx::blit_command* out = grow(flat_, flat_capacity_, need);
    gfx::ui_lower_stats lower{};
    const unsigned n = gfx::lower_to_blits(s, out, need, lower);
    note_drops(lower.dropped);
    if (stats) {
        stats->primitives = snap.draws().primitive_count();
        stats->lowered = lower.lowered;
        stats->dropped = lower.dropped;
    }
    return {out, n};
}

lx::result<lx::compositor::cpu_paint_result> lx::compositor::frame_painter::paint_cpu(
    gfx::cpu_compositor& compositor, gfx::ui_cpu_renderer* ui, const gfx::pixel_surface& target,
    const scene::immutable_frame_snapshot& snap, const gfx::damage_region& region) {
    cpu_paint_result out{};
    if (!has_primitives(snap) || !ui) {
        const bool lowered = has_primitives(snap);
        const auto blits = flattened(snap, &out.paint);
        auto c = compositor.composite_region(target, snap.clear_color(), blits.data(),
                                             static_cast<unsigned>(blits.size()), region);
        if (!c)
            return c.get_error();
        out.composite = c.value();
        ++(lowered ? lowered_frames_ : blit_frames_);
        return out;
    }
    (void)surfaces(snap);
    ui->set_surface_compositor(&compositor);
    auto d = ui->draw(target, stream(snap), nullptr, region, snap.clear_color());
    if (!d)
        return d.get_error();
    out.ui = d.value();
    // The surfaces' own diagnostics, as `composite_region` reports them on the blit path:
    // the caller's unregistered-texture and unsupported-format warnings fire on stream frames.
    out.composite.draws_skipped = out.ui.surface_draws_skipped;
    out.composite.draws_unsupported = out.ui.surface_draws_unsupported;
    out.composite.draws_culled = out.ui.surface_draws_culled;
    out.paint.stream = true;
    out.paint.primitives = snap.draws().primitive_count();
    ++stream_frames_;
    return out;
}

lx::result<lx::compositor::gl_paint_result> lx::compositor::frame_painter::paint_gl(
    gfx::gl_compositor& compositor, gfx::ui_gl_renderer* ui, unsigned framebuffer, unsigned width,
    unsigned height, const scene::immutable_frame_snapshot& snap,
    const gfx::damage_region& region) {
    gl_paint_result out{};
    if (!has_primitives(snap) || !ui || !ui->ready()) {
        const bool lowered = has_primitives(snap);
        const auto blits = flattened(snap, &out.paint);
        auto c = compositor.composite_into(framebuffer, width, height, snap.clear_color(),
                                           blits.data(), static_cast<unsigned>(blits.size()),
                                           region, true, true);
        if (!c)
            return c.get_error();
        out.composite = c.value();
        ++(lowered ? lowered_frames_ : blit_frames_);
        return out;
    }
    if (region.empty())
        return out;
    (void)surfaces(snap);
    ui->set_surface_compositor(&compositor);
    auto d = ui->draw(framebuffer, width, height, stream(snap), nullptr, region,
                      snap.clear_color());
    if (!d)
        return d.get_error();
    out.ui = d.value();
    auto fence = compositor.fence_frame();
    if (!fence)
        return fence.get_error();
    out.composite.out_fence_fd = fence.value();
    out.composite.draws_submitted = out.ui.draw_calls;
    // Primitives the renderer could not draw and surfaces of an unregistered texture: what
    // the caller's `report_unregistered_draw` looks for.
    out.composite.draws_skipped = out.ui.primitives_skipped + out.ui.surface_draws_skipped;
    out.composite.draws_culled = out.ui.surface_draws_culled;
    out.paint.stream = true;
    out.paint.primitives = snap.draws().primitive_count();
    ++stream_frames_;
    return out;
}
