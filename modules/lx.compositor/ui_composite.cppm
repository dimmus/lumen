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
//   - Vulkan records the stream inside its linear F16 pass (`ui_vk_renderer::composite`):
//     instance colours decoded to premultiplied linear light, glyph coverage through the
//     `text.contrast` exponent, surfaces drawn as `composite` draws them.
//   - Screen capture's re-composites (CPU, and GL when the software pointer is left out)
//     draw the same stream into the capture buffer (`paint_cpu_capture`, `paint_gl_capture`),
//     the headless dump walks it in order (`paint_headless`).
//   - A path with no primitive renderer (GLES 2) takes the stream lowered to blits
//     (`gfx::lower_to_blits`): solid square quads become solid blits, everything else —
//     the title glyphs among it — is dropped, counted and warned about once per run of
//     frames that drop.
//
// Colour. A primitive's colour is premultiplied and sRGB-encoded (ADR-010 §4.5), and the CPU
// and GL compositors' UI renderers blend it in that encoded space, as in a client window;
// Vulkan blends it in linear light (above), so anti-aliased glyph edges differ between the
// two by what the blending space and the contrast exponent make of them.
//
// Glyphs. Sprites sample `chrome_atlas`, which the UI thread writes; every paint that reads it
// holds its mutex while the renderer reads or uploads the pages.
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

/// The compositor's glyph atlas (ADR-010 §4.11): server-side frames' title text and close
/// button, R8 tiles drawn as `mono_sprite`s by every backend. One atlas for every output: a
/// glyph's key carries its size × scale (`text::glyph_key`), so titles rasterised for outputs
/// of different scales are different tiles of the same pages — the "page set per scale" of the
/// ADR's open question is not needed for a few hundred glyphs.
///
/// Threads. The UI thread writes tiles (`decoration_manager` rasterising a changed title) and
/// looks tiles up while frames emit; the render thread's UI renderer reads the pages (the CPU
/// renderer samples them in place, GL and Vulkan upload their dirty rects and clear the dirty
/// lists). `mutex` serialises the page pixels and dirty lists: the UI thread holds it while it
/// rasterises, the render thread while a renderer reads. The tile table is the UI thread's
/// alone (no lock). A tile a published frame names is not reused before the atlas ages it out
/// (120 frames unused); a page rebuild (above 85 % with stale tiles — never reached by titles)
/// moves tiles, after which `begin_frame` says so and the caller damages every frame.
class chrome_atlas {
public:
    chrome_atlas() = default;
    chrome_atlas(const chrome_atlas&) = delete;
    chrome_atlas& operator=(const chrome_atlas&) = delete;

    [[nodiscard]] gfx::atlas& atlas() { return atlas_; }
    [[nodiscard]] const gfx::atlas& atlas() const { return atlas_; }
    [[nodiscard]] std::mutex& mutex() { return mutex_; }
    /// UI, once per tick before frames emit: advances the atlas's frame (the 120-frame rule)
    /// and runs a rebuild an allocation asked for, under the lock. True when tiles moved.
    [[nodiscard]] bool begin_frame();

private:
    gfx::atlas atlas_{};
    std::mutex mutex_{};
    std::uint64_t frame_ = 0;
};

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

struct vk_paint_result {
    /// `out_fence_fd` owned by the caller, as `vulkan_compositor::composite`'s.
    gfx::composite_stats composite{};
    gfx::ui_vk_draw_stats ui{};
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
    /// Its stream has sprite batches: drawing it reads an atlas.
    [[nodiscard]] static bool uses_atlas(const scene::immutable_frame_snapshot& snap);

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
    /// lowered path for frames with primitives. `atlas`: where the stream's sprites are
    /// (locked while they are drawn); null draws none (counted as skipped).
    [[nodiscard]] lx::result<cpu_paint_result> paint_cpu(gfx::cpu_compositor& compositor,
                                                         gfx::ui_cpu_renderer* ui,
                                                         const gfx::pixel_surface& target,
                                                         const scene::immutable_frame_snapshot& snap,
                                                         const gfx::damage_region& region,
                                                         chrome_atlas* atlas = nullptr);
    /// The GL compositor's frame into `framebuffer` (`width` × `height`, row 0 on top — a
    /// scanout target's), restricted to `region`, closed by one fence (`composite.out_fence_fd`,
    /// owned by the caller; -1 after a glFinish). `ui` null or not ready (a GLES 2 context):
    /// the lowered path for frames with primitives.
    [[nodiscard]] lx::result<gl_paint_result> paint_gl(gfx::gl_compositor& compositor,
                                                       gfx::ui_gl_renderer* ui,
                                                       unsigned framebuffer, unsigned width,
                                                       unsigned height,
                                                       const scene::immutable_frame_snapshot& snap,
                                                       const gfx::damage_region& region,
                                                       chrome_atlas* atlas = nullptr);
    /// The Vulkan compositor's frame into `target` (the whole of it: Vulkan repaints every
    /// frame). A snapshot with primitives and a ready `ui` goes through
    /// `ui_vk_renderer::composite` — the stream inside the compositor's linear pass, glyph
    /// coverage through `text_contrast`; otherwise `composite` of the draws as blits, exactly
    /// as before (a frame with primitives and no `ui`: lowered).
    [[nodiscard]] lx::result<vk_paint_result>
    paint_vk(gfx::vulkan_compositor& compositor, gfx::ui_vk_renderer* ui,
             gfx::render_target& target, const scene::immutable_frame_snapshot& snap,
             chrome_atlas* atlas = nullptr, float text_contrast = gfx::k_text_contrast_dark);
    /// The headless dump (`headless_backend`, RGB, no primitive renderer): the stream walked
    /// in order — surfaces and solid square quads as `frame_renderer` draws blits, mono
    /// sprites blended from the atlas (nearest), anything else dropped and counted. A snapshot
    /// without primitives is `draw_to_headless` of its draws, as before.
    void paint_headless(gfx::frame_renderer& renderer, gfx::headless_backend& target,
                        const scene::immutable_frame_snapshot& snap, chrome_atlas* atlas,
                        frame_paint_stats* stats = nullptr);

    /// Screen capture's re-composite of a frame with primitives (CPU): `surfaces` are the
    /// frame's draws as the capture adjusted them (the software pointer hidden, moved by the
    /// crop) and stand in for the stream's surface refs; every primitive moves by `-offset`
    /// (the crop) too. Drawn into `target` inside `region` (target pixels), cleared to `clear`.
    [[nodiscard]] lx::result<void>
    paint_cpu_capture(gfx::cpu_compositor& compositor, gfx::ui_cpu_renderer& ui,
                      const gfx::pixel_surface& target, const scene::immutable_frame_snapshot& snap,
                      std::span<const gfx::blit_command> surfaces, lx::point2i offset,
                      lx::color clear, const gfx::damage_region& region, chrome_atlas* atlas);
    /// The same into a GL framebuffer (`width` × `height`); the commands are fenced by the
    /// caller's capture fence.
    [[nodiscard]] lx::result<void>
    paint_gl_capture(gfx::gl_compositor& compositor, gfx::ui_gl_renderer& ui, unsigned framebuffer,
                     unsigned width, unsigned height, const scene::immutable_frame_snapshot& snap,
                     std::span<const gfx::blit_command> surfaces, lx::point2i offset,
                     lx::color clear, const gfx::damage_region& region, chrome_atlas* atlas);

    /// Frames painted through a UI renderer / lowered / as plain blits, since construction.
    [[nodiscard]] unsigned long long stream_frames() const { return stream_frames_; }
    [[nodiscard]] unsigned long long lowered_frames() const { return lowered_frames_; }
    [[nodiscard]] unsigned long long blit_frames() const { return blit_frames_; }
    [[nodiscard]] unsigned long long dropped_primitives() const { return dropped_total_; }

private:
    [[nodiscard]] gfx::blit_command* grow(gfx::blit_command*& storage, unsigned& capacity,
                                          unsigned need);
    void note_drops(unsigned dropped);
    /// The snapshot's stream over `surfaces`, its primitives and batch bounds moved by
    /// `-offset` (copies into the scratch below when the offset is not zero).
    [[nodiscard]] gfx::ui_stream capture_stream(const scene::immutable_frame_snapshot& snap,
                                                std::span<const gfx::blit_command> surfaces,
                                                lx::point2i offset);

    std::vector<gfx::quad_instance> moved_quads_{};
    std::vector<gfx::shadow_instance> moved_shadows_{};
    std::vector<gfx::sprite_instance> moved_sprites_{};
    std::vector<gfx::underline_instance> moved_underlines_{};
    std::vector<gfx::batch> moved_batches_{};

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

bool lx::compositor::chrome_atlas::begin_frame() {
    bool pending = false;
    for (unsigned p = 0; p < gfx::k_atlas_pages; ++p)
        pending = pending || atlas_.rebuild_pending(p);
    if (!pending) {
        // Only the frame stamp (the tile table's, UI-only): no lock.
        atlas_.begin_frame(++frame_);
        return false;
    }
    const std::lock_guard<std::mutex> lock{mutex_};
    atlas_.begin_frame(++frame_);
    return true;
}

bool lx::compositor::frame_painter::has_primitives(const scene::immutable_frame_snapshot& snap) {
    return snap.draws().primitive_count() > 0 && !snap.batches().empty();
}

bool lx::compositor::frame_painter::uses_atlas(const scene::immutable_frame_snapshot& snap) {
    for (const gfx::batch& b : snap.batches())
        if (b.kind == gfx::primitive_kind::mono_sprite || b.kind == gfx::primitive_kind::poly_sprite ||
            b.kind == gfx::primitive_kind::path)
            return true;
    return false;
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
    const scene::immutable_frame_snapshot& snap, const gfx::damage_region& region,
    chrome_atlas* atlas) {
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
    std::unique_lock<std::mutex> lock{};
    const gfx::atlas* pages = nullptr;
    if (atlas && uses_atlas(snap)) {
        lock = std::unique_lock<std::mutex>{atlas->mutex()};
        pages = &atlas->atlas();
    }
    auto d = ui->draw(target, stream(snap), pages, region, snap.clear_color());
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
    const gfx::damage_region& region, chrome_atlas* atlas) {
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
    {
        std::unique_lock<std::mutex> lock{};
        gfx::atlas* pages = nullptr;
        if (atlas && uses_atlas(snap)) {
            lock = std::unique_lock<std::mutex>{atlas->mutex()};
            pages = &atlas->atlas();
        }
        auto d = ui->draw(framebuffer, width, height, stream(snap), pages, region,
                          snap.clear_color());
        if (!d)
            return d.get_error();
        out.ui = d.value();
    }
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

lx::result<lx::compositor::vk_paint_result> lx::compositor::frame_painter::paint_vk(
    gfx::vulkan_compositor& compositor, gfx::ui_vk_renderer* ui, gfx::render_target& target,
    const scene::immutable_frame_snapshot& snap, chrome_atlas* atlas, float text_contrast) {
    vk_paint_result out{};
    if (!has_primitives(snap) || !ui || !ui->ready()) {
        const bool lowered = has_primitives(snap);
        const auto blits = flattened(snap, &out.paint);
        auto c = compositor.composite(target, snap.clear_color(), blits.data(),
                                      static_cast<unsigned>(blits.size()));
        if (!c)
            return c.get_error();
        out.composite = c.value();
        ++(lowered ? lowered_frames_ : blit_frames_);
        return out;
    }
    (void)surfaces(snap);
    gfx::ui_vk_composite_options options{};
    options.text_contrast = text_contrast;
    std::unique_lock<std::mutex> lock{};
    gfx::atlas* pages = nullptr;
    if (atlas && uses_atlas(snap)) {
        lock = std::unique_lock<std::mutex>{atlas->mutex()};
        pages = &atlas->atlas();
    }
    auto d = ui->composite(compositor, target, snap.clear_color(), stream(snap), pages, options);
    if (!d)
        return d.get_error();
    out.composite = d->composite;
    out.ui = d->ui;
    // Primitives the renderer could not draw count as skipped draws, as on GL.
    out.composite.draws_skipped += out.ui.primitives_skipped;
    out.paint.stream = true;
    out.paint.primitives = snap.draws().primitive_count();
    ++stream_frames_;
    return out;
}

void lx::compositor::frame_painter::paint_headless(gfx::frame_renderer& renderer,
                                                   gfx::headless_backend& target,
                                                   const scene::immutable_frame_snapshot& snap,
                                                   chrome_atlas* atlas, frame_paint_stats* stats) {
    const std::span<const gfx::blit_command> plain = surfaces(snap);
    if (!has_primitives(snap)) {
        (void)renderer.draw_to_headless(plain.data(), static_cast<unsigned>(plain.size()), target);
        ++blit_frames_;
        return;
    }
    const gfx::ui_stream s = stream(snap);
    std::unique_lock<std::mutex> lock{};
    if (atlas && uses_atlas(snap))
        lock = std::unique_lock<std::mutex>{atlas->mutex()};
    unsigned dropped = 0;
    for (const gfx::primitive_ref& ref : s.refs) {
        switch (ref.kind) {
        case gfx::primitive_kind::surface:
            if (ref.index < s.surfaces.size())
                (void)renderer.draw_to_headless(&s.surfaces[ref.index], 1, target);
            break;
        case gfx::primitive_kind::quad: {
            gfx::ui_stream one = s;
            one.refs = {&ref, 1};
            gfx::blit_command b{};
            gfx::ui_lower_stats st{};
            if (gfx::lower_to_blits(one, &b, 1, st) == 1)
                (void)renderer.draw_to_headless(&b, 1, target);
            else
                ++dropped;
            break;
        }
        case gfx::primitive_kind::mono_sprite: {
            if (!lock.owns_lock() || ref.index >= s.sprites.size()) {
                ++dropped;
                break;
            }
            const gfx::sprite_instance& sp = s.sprites[ref.index];
            const gfx::atlas_page_view page = atlas->atlas().page(sp.page);
            if (!page.valid() || page.format != gfx::atlas_format::r8 || sp.uv[2] > page.size ||
                sp.uv[3] > page.size || sp.uv[2] <= sp.uv[0] || sp.uv[3] <= sp.uv[1]) {
                ++dropped;
                break;
            }
            const lx::rect2i dst{static_cast<int>(sp.bounds[0]), static_cast<int>(sp.bounds[1]),
                                 static_cast<int>(sp.bounds[2]), static_cast<int>(sp.bounds[3])};
            const lx::rect2i clip{static_cast<int>(sp.clip[0]), static_cast<int>(sp.clip[1]),
                                  static_cast<int>(sp.clip[2]), static_cast<int>(sp.clip[3])};
            const lx::color c = lx::color::rgb(gfx::from_half(sp.color[0]), gfx::from_half(sp.color[1]),
                                               gfx::from_half(sp.color[2]), gfx::from_half(sp.color[3]));
            (void)target.blend_coverage(dst, clip,
                                        page.pixels + static_cast<std::size_t>(sp.uv[1]) * page.stride +
                                            sp.uv[0],
                                        page.stride, static_cast<unsigned>(sp.uv[2] - sp.uv[0]),
                                        static_cast<unsigned>(sp.uv[3] - sp.uv[1]), c);
            break;
        }
        default:
            ++dropped;
            break;
        }
    }
    note_drops(dropped);
    if (stats) {
        stats->primitives = snap.draws().primitive_count();
        stats->dropped = dropped;
    }
    ++lowered_frames_;
}

lx::gfx::ui_stream
lx::compositor::frame_painter::capture_stream(const scene::immutable_frame_snapshot& snap,
                                              std::span<const gfx::blit_command> surfaces,
                                              lx::point2i offset) {
    gfx::ui_stream s = stream(snap);
    s.surfaces = surfaces;
    if (offset.x == 0 && offset.y == 0)
        return s;
    const float dx = static_cast<float>(offset.x);
    const float dy = static_cast<float>(offset.y);
    const auto move = [&](auto& v, auto span) {
        v.assign(span.begin(), span.end()); // grows to the largest stream seen, then reused
        for (auto& p : v) {
            p.bounds[0] -= dx;
            p.bounds[1] -= dy;
            if (p.clip[2] > 0.f && p.clip[3] > 0.f) {
                p.clip[0] -= dx;
                p.clip[1] -= dy;
            }
        }
    };
    move(moved_quads_, s.quads);
    move(moved_shadows_, s.shadows);
    move(moved_sprites_, s.sprites);
    move(moved_underlines_, s.underlines);
    moved_batches_.assign(s.batches.begin(), s.batches.end());
    for (gfx::batch& b : moved_batches_) {
        b.bounds.x -= offset.x;
        b.bounds.y -= offset.y;
    }
    s.quads = moved_quads_;
    s.shadows = moved_shadows_;
    s.sprites = moved_sprites_;
    s.underlines = moved_underlines_;
    s.batches = moved_batches_;
    return s;
}

lx::result<void> lx::compositor::frame_painter::paint_cpu_capture(
    gfx::cpu_compositor& compositor, gfx::ui_cpu_renderer& ui, const gfx::pixel_surface& target,
    const scene::immutable_frame_snapshot& snap, std::span<const gfx::blit_command> surfaces,
    lx::point2i offset, lx::color clear, const gfx::damage_region& region, chrome_atlas* atlas) {
    const gfx::ui_stream s = capture_stream(snap, surfaces, offset);
    ui.set_surface_compositor(&compositor);
    std::unique_lock<std::mutex> lock{};
    const gfx::atlas* pages = nullptr;
    if (atlas && uses_atlas(snap)) {
        lock = std::unique_lock<std::mutex>{atlas->mutex()};
        pages = &atlas->atlas();
    }
    auto d = ui.draw(target, s, pages, region, clear);
    if (!d)
        return d.get_error();
    return {};
}

lx::result<void> lx::compositor::frame_painter::paint_gl_capture(
    gfx::gl_compositor& compositor, gfx::ui_gl_renderer& ui, unsigned framebuffer, unsigned width,
    unsigned height, const scene::immutable_frame_snapshot& snap,
    std::span<const gfx::blit_command> surfaces, lx::point2i offset, lx::color clear,
    const gfx::damage_region& region, chrome_atlas* atlas) {
    const gfx::ui_stream s = capture_stream(snap, surfaces, offset);
    ui.set_surface_compositor(&compositor);
    std::unique_lock<std::mutex> lock{};
    gfx::atlas* pages = nullptr;
    if (atlas && uses_atlas(snap)) {
        lock = std::unique_lock<std::mutex>{atlas->mutex()};
        pages = &atlas->atlas();
    }
    auto d = ui.draw(framebuffer, width, height, s, pages, region, clear);
    if (!d)
        return d.get_error();
    return {};
}
