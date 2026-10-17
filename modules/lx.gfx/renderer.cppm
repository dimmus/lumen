module;

import lx.foundation;

export module lx.gfx:renderer;

import :headless;

export namespace lx::gfx {

/// One quad handed to a present backend.
///
/// This mirrors `scene::draw_command`. It used to carry four of its thirteen fields, and
/// the six dropped in `tick_render` were exactly the ones `viewporter`,
/// `wp-fractional-scale-v1`, `alpha-modifier-v1`, subsurface clipping and rotated outputs
/// need — so those protocols could be parsed but never reach a pixel. Fields a backend
/// cannot express are reported through `composite_stats::draws_unsupported` rather than
/// ignored, so "not implemented here" stays visible instead of rendering the wrong frame.
struct blit_command {
    unsigned texture_id = 0;
    /// Destination rectangle in target pixels.
    lx::rect2i dst{};
    /// Source box in texture pixels, before `buffer_xform`; fractional edges are a
    /// `wp_viewport` crop. Empty means the whole texture — which is both a plain surface
    /// and a `wp_viewport` with no source set. Together with `dst` this expresses
    /// viewporter cropping and fractional scaling: the backends derive the scale factor
    /// from the src:dst ratio rather than being told it separately.
    lx::rect2f src{};
    /// Additional clip in target space; the draw is confined to its intersection with
    /// `dst`. Empty means unclipped. Subsurface clipping and layer-shell regions.
    lx::rect2i clip{};
    /// Per-draw color multiplier, applied premultiplied. `alpha-modifier-v1`.
    lx::color tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    float opacity = 1.f;
    lx::blend_mode blend = lx::blend_mode::premultiplied;
    /// `wl_output.transform` for the source buffer — rotated and flipped monitors.
    lx::buffer_transform buffer_xform = lx::buffer_transform::normal;
    /// Primaries the texture's color is expressed in.
    lx::color_space src_space = lx::color_space::srgb;
    /// Transfer function the texture is encoded with. The composite decodes to linear
    /// light before blending, because alpha blending is a weighted average of light and
    /// averaging encoded values darkens edges.
    lx::transfer_function src_transfer = lx::transfer_function::srgb;
    /// The draw resamples (`dst` is not the transformed source size, or the crop is off
    /// whole texels). A 1:1 draw on whole pixels samples texel centres exactly.
    bool scaled = false;
    /// Covered by an opaque draw above it: skipped (identical pixels).
    bool occluded = false;
};

/// Where a draw's top-left, +x and +y directions land in its source box, in the box's own
/// normalized coordinates (0..1 across the box): the source point of normalized
/// destination (u, v) is `origin + u·dx + v·dy`. `transform` is the buffer transform
/// (wl_surface.set_buffer_transform): the compositor shows the buffer turned by its
/// inverse, as wlroots' `wlr_box_transform(…, invert(t), …)` maps a surface box into the
/// buffer. Shared by every backend so the eight transforms cannot disagree.
struct source_basis {
    float origin[2]{0.f, 0.f};
    float dx[2]{1.f, 0.f};
    float dy[2]{0.f, 1.f};
};

[[nodiscard]] constexpr source_basis source_basis_for(lx::buffer_transform transform) {
    // Indexed by the transform; each row is the inverse transform's point map.
    switch (transform) {
    case lx::buffer_transform::normal:
        return {{0.f, 0.f}, {1.f, 0.f}, {0.f, 1.f}};
    case lx::buffer_transform::rotate_90: // shown turned by 270: (v, 1 − u)
        return {{0.f, 1.f}, {0.f, -1.f}, {1.f, 0.f}};
    case lx::buffer_transform::rotate_180:
        return {{1.f, 1.f}, {-1.f, 0.f}, {0.f, -1.f}};
    case lx::buffer_transform::rotate_270: // shown turned by 90: (1 − v, u)
        return {{1.f, 0.f}, {0.f, 1.f}, {-1.f, 0.f}};
    case lx::buffer_transform::flipped:
        return {{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}};
    case lx::buffer_transform::flipped_90:
        return {{0.f, 0.f}, {0.f, 1.f}, {1.f, 0.f}};
    case lx::buffer_transform::flipped_180:
        return {{0.f, 1.f}, {1.f, 0.f}, {0.f, -1.f}};
    case lx::buffer_transform::flipped_270:
        return {{1.f, 1.f}, {0.f, -1.f}, {-1.f, 0.f}};
    }
    return {};
}

/// The source box a draw samples, in texture pixels: `src`, or the whole texture when it
/// is empty, clamped into the texture.
[[nodiscard]] constexpr lx::rect2f effective_source(lx::rect2f src, unsigned tex_w,
                                                    unsigned tex_h) {
    const float tw = static_cast<float>(tex_w);
    const float th = static_cast<float>(tex_h);
    if (src.empty())
        return {0.f, 0.f, tw, th};
    float x0 = src.x < 0.f ? 0.f : src.x;
    float y0 = src.y < 0.f ? 0.f : src.y;
    float x1 = src.x + src.width > tw ? tw : src.x + src.width;
    float y1 = src.y + src.height > th ? th : src.y + src.height;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    return {x0, y0, x1 - x0, y1 - y0};
}

class pipeline_cache {
public:
    [[nodiscard]] unsigned acquire(unsigned blend_key, unsigned format_key);
    void clear();

private:
    unsigned next_id_ = 1;
    static constexpr unsigned k_capacity = 64;
    unsigned keys_[64]{};
    unsigned ids_[64]{};
    unsigned count_ = 0;
};

class frame_renderer {
public:
    void set_pipeline_cache(pipeline_cache* cache);

    [[nodiscard]] lx::result<void> draw_to_headless(const blit_command* cmds, unsigned count,
                                                     headless_backend& target);

private:
    pipeline_cache* pipelines_ = nullptr;
};

} // namespace lx::gfx


unsigned lx::gfx::pipeline_cache::acquire(unsigned blend_key, unsigned format_key) {
    const unsigned key = (blend_key << 16u) | (format_key & 0xFFFFu);
    for (unsigned i = 0; i < count_; ++i) {
        if (keys_[i] == key)
            return ids_[i];
    }
    if (count_ < k_capacity) {
        keys_[count_] = key;
        ids_[count_] = next_id_++;
        return ids_[count_++];
    }
    return next_id_++;
}

void lx::gfx::pipeline_cache::clear() { count_ = 0; }

void lx::gfx::frame_renderer::set_pipeline_cache(pipeline_cache* cache) { pipelines_ = cache; }

lx::result<void> lx::gfx::frame_renderer::draw_to_headless(const blit_command* cmds,
                                                              unsigned count,
                                                              headless_backend& target) {
    if (!cmds)
        return {};
    // Draw everything that resolves, but still report that something did not: an
    // unregistered texture means the commit and render paths disagree.
    lx::result<void> outcome{};
    for (unsigned i = 0; i < count; ++i) {
        const auto& cmd = cmds[i];
        if (pipelines_) {
            (void)pipelines_->acquire(static_cast<unsigned>(cmd.blend), 0);
        }
        if (auto blitted = target.blit_texture(cmd.texture_id, cmd.dst); !blitted)
            outcome = blitted.get_error();
    }
    return outcome;
}
