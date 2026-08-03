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
    /// Source sub-rectangle in texture pixels. Empty means the whole texture — which is
    /// both a plain surface and a `wp_viewport` with no source set. Together with `dst`
    /// this expresses viewporter cropping and fractional scaling: the backends derive the
    /// scale factor from the src:dst ratio rather than being told it separately.
    lx::rect2i src{};
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
};

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
