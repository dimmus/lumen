module;

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.text;

export module lx.ui:text_node;

import std;

import :element;
import :node;

export namespace lx::ui {

// ── Text blocks (ADR-010 §4.6, Phase 2) ─────────────────────────────────────────
//
// `text_block_node` holds a retained `text::text_layout` and draws it as atlas sprites: one
// `mono_sprite` per glyph (R8 coverage × the span's colour), a `poly_sprite` per colour glyph
// (RGBA8, emoji), an `underline` per run whose span asks for one — straight for
// `text_underline::single`, wavy for `text_underline::wavy` and for `span_role::spell`.
//
// The layout is invalidated only when its inputs change: `apply` hands every prop to the
// layout's setters, which ignore a value equal to the current one, so a reconcile that
// re-applies the same props costs one comparison per field and the text bytes. Width comes
// from `arrange` (the bounds' width over the scale) when the block wraps; `measure` asks the
// layout at the constraint's width (unbounded at `layout::constraints`' 99 999).
//
// Coordinates: the node's bounds are device pixels (what `draw_scope` takes); the layout is
// laid out at `scale` (its 26.6 units are device pixels too); `font.size_pt` and the logical
// max width are scaled by it. Glyphs are rasterised at size × scale, placed by
// `text::glyph_pen` — x on a quarter pixel (the subpixel variant), y on a whole pixel — so a
// glyph sprite always lands 1:1 on whole pixels and samples its tile exactly.
//
// Emission does no per-glyph work for a line outside the scope's clip: lines are stacked
// top to bottom, so the visible ones are found by a binary search over their tops (widened
// by one em for ink above or below the line box) before any glyph is looked at. Per drawn
// glyph: one glyph-cache probe, one atlas lookup + touch, one sprite. A glyph not in the
// cache is rasterised on that frame (counted in `last_emit().rasterized`) under the text
// engine's face lock, taken once per emit and only when something missed. No allocation per
// frame: the layout's storage and the glyph cache are retained.
//
// UI affinity (the glyph cache writes the atlas): `emit` runs inside the frame build. The
// layout's queries lock the engine's face cache lazily, so the layout is brought up to date
// before the face lock is taken.

/// What a text block lays out with and rasterises into. Null `engine` members are the
/// shared caches; without `glyphs` the block lays out and measures but draws nothing.
struct text_raster_context {
    lx::text::text_engine engine{};
    lx::text::glyph_cache* glyphs = nullptr;
};

struct text_block_props {
    /// UTF-8; copied into the layout by `apply` (it need not outlive the frame).
    const char* text = "";
    lx::text::font_desc font{};
    /// Straight alpha, in the surface's blend space; a span with a colour replaces it.
    lx::color color = lx::color::rgb(1.f, 1.f, 1.f);
    float scale = 1.f;
    lx::text::text_align align = lx::text::text_align::start;
    lx::text::text_wrap wrap = lx::text::text_wrap::word;
    unsigned max_lines = 0;
    bool ellipsize = false;
    /// Line height as a multiple of the font size; 0 is the font's own metrics.
    float line_height = 0.f;
    lx::text::text_direction direction = lx::text::text_direction::automatic;
    /// Copied by `apply`.
    std::span<const lx::text::text_attr> attrs{};
    /// The theme's hinting; it applies below scale 1.5 only (`text::effective_hinting`).
    lx::text::glyph_hinting hinting = lx::text::glyph_hinting::none;
    text_raster_context context{};
};

/// What the last `emit` did.
struct text_emit_stats {
    unsigned lines = 0;        ///< lines in the layout
    unsigned lines_drawn = 0;  ///< lines inside the clip (their glyphs were looked at)
    unsigned glyphs = 0;       ///< glyphs looked at
    unsigned sprites = 0;      ///< mono and poly sprites emitted (before the scope's clip)
    unsigned color_sprites = 0;
    unsigned underlines = 0;
    unsigned rasterized = 0;   ///< glyph cache misses rasterised on this frame
    unsigned failures = 0;     ///< glyphs that could not be rasterised or placed
};

class text_block_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const element_type_id id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "text_block"; }

    void apply(const text_block_props& props);

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;

    [[nodiscard]] const lx::text::text_layout& layout() const { return layout_; }
    [[nodiscard]] text_emit_stats last_emit() const { return stats_; }

private:
    /// The layout's max width (logical px) for a device-pixel width; 0 is unbounded.
    [[nodiscard]] float logical_width(int device_px) const;

    mutable lx::text::text_layout layout_{};
    lx::color color_ = lx::color::rgb(1.f, 1.f, 1.f);
    float scale_ = 1.f;
    bool wraps_ = true;
    lx::text::glyph_hinting hinting_ = lx::text::glyph_hinting::none;
    lx::text::glyph_cache* glyphs_ = nullptr;
    /// The engine's face cache (null: the shared one): its lock guards rasterising.
    lx::text::face_cache* faces_ = nullptr;
    mutable text_emit_stats stats_{};
};

/// Describes a `text_block_node` with `props`.
[[nodiscard]] child text_block(const text_block_props& props, element_key key = 0);

} // namespace lx::ui

// ── Implementation ────────────────────────────────────────────────────────────

namespace {

/// The span colour (sRGB 0xRRGGBBAA, straight) as an `lx::color`.
[[nodiscard]] lx::color lx_ui_text_rgba(std::uint32_t rgba) {
    return lx::color::rgb(static_cast<float>((rgba >> 24u) & 0xFFu) / 255.f,
                          static_cast<float>((rgba >> 16u) & 0xFFu) / 255.f,
                          static_cast<float>((rgba >> 8u) & 0xFFu) / 255.f,
                          static_cast<float>(rgba & 0xFFu) / 255.f);
}

[[nodiscard]] lx::gfx::packed_rgba lx_ui_text_pack(lx::color c, float opacity) {
    c.a *= opacity;
    return lx::gfx::premultiplied_half(c);
}

/// 26.6 → whole pixels, rounded to nearest (floor of x + ½).
[[nodiscard]] constexpr std::int32_t lx_ui_round_26_6(std::int32_t v) { return (v + 32) >> 6; }

} // namespace

void lx::ui::text_block_node::apply(const text_block_props& p) {
    lx::text::text_layout& l = layout_;
    l.set_engine(p.context.engine);
    l.set_text(p.text ? p.text : "");
    l.set_font(p.font);
    l.set_scale(p.scale > 0.f ? p.scale : 1.f);
    l.set_align(p.align);
    l.set_wrap(p.wrap);
    l.set_max_lines(p.max_lines);
    l.set_ellipsize(p.ellipsize);
    l.set_line_height(p.line_height);
    l.set_direction(p.direction);
    l.set_attrs(p.attrs);
    color_ = p.color;
    scale_ = p.scale > 0.f ? p.scale : 1.f;
    wraps_ = p.wrap == lx::text::text_wrap::word;
    hinting_ = p.hinting;
    glyphs_ = p.context.glyphs;
    faces_ = p.context.engine.faces;
    if (l.dirty())
        mark_layout_dirty();
}

float lx::ui::text_block_node::logical_width(int device_px) const {
    if (!wraps_ || device_px <= 0 || device_px >= 99999)
        return 0.f;
    return static_cast<float>(device_px) / scale_;
}

lx::size2i lx::ui::text_block_node::measure(lx::layout::constraints c, size_hint) const {
    layout_.set_max_width(logical_width(c.max.width));
    lx::size2i out = layout_.bounding_size();
    if (out.width < c.min.width)
        out.width = c.min.width;
    if (out.height < c.min.height)
        out.height = c.min.height;
    if (out.width > c.max.width)
        out.width = c.max.width;
    if (out.height > c.max.height)
        out.height = c.max.height;
    return out;
}

void lx::ui::text_block_node::arrange(lx::rect2i bounds) {
    bounds_ = bounds;
    layout_.set_max_width(logical_width(bounds.width));
    layout_dirty_ = false;
}

void lx::ui::text_block_node::emit(draw_scope& out) const {
    stats_ = {};
    if (!visible_ || !glyphs_)
        return;
    // Lay out now, before the face lock below: the queries lock the engine lazily.
    if (!layout_.update())
        return;
    const std::span<const lx::text::layout_line> lines = layout_.lines();
    stats_.lines = static_cast<unsigned>(lines.size());
    if (lines.empty())
        return;

    // The clip in the layout's space (26.6 from its top), widened by an em for ink outside
    // the line boxes.
    const lx::rect2i clip = out.clip();
    const lx::point2i origin = out.origin();
    const std::int32_t em = layout_.size_26_6();
    const std::int64_t top_px = static_cast<std::int64_t>(clip.y) - origin.y - bounds_.y;
    const std::int64_t clip_top = top_px * 64 - em;
    const std::int64_t clip_bottom = (top_px + clip.height) * 64 + em;
    // First line whose box reaches below the clip's top.
    std::size_t lo = 0;
    std::size_t hi = lines.size();
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (static_cast<std::int64_t>(lines[mid].top) + lines[mid].height <= clip_top)
            lo = mid + 1;
        else
            hi = mid;
    }

    lx::text::face_cache& faces = faces_ ? *faces_ : lx::text::face_cache::shared();
    // Taken at the first miss, held to the end of the emit (the layout is clean: its queries
    // below take no lock).
    std::unique_lock<std::mutex> lock{};

    const float opacity = out.opacity();
    const lx::gfx::packed_rgba node_color = lx_ui_text_pack(color_, opacity);
    const lx::gfx::packed_rgba white = lx_ui_text_pack(lx::color::rgb(1.f, 1.f, 1.f), opacity);
    const lx::text::glyph_hinting hinting = lx::text::effective_hinting(hinting_, scale_);
    const std::span<const lx::text::text_attr> attrs = layout_.attrs();
    const lx::point2i at{bounds_.x, bounds_.y};

    for (std::size_t li = lo; li < lines.size(); ++li) {
        const lx::text::layout_line& line = lines[li];
        if (static_cast<std::int64_t>(line.top) >= clip_bottom)
            break;
        ++stats_.lines_drawn;
        for (const lx::text::layout_run& run : layout_.runs(line)) {
            const lx::text::text_attr* attr =
                run.attr < attrs.size() ? &attrs[run.attr] : nullptr;
            const bool span_color = attr && attr->color != 0;
            const lx::gfx::packed_rgba run_color =
                span_color ? lx_ui_text_pack(lx_ui_text_rgba(attr->color), opacity) : node_color;
            std::int32_t pen = run.x;
            for (const lx::text::layout_glyph& g : layout_.glyphs(run)) {
                ++stats_.glyphs;
                const lx::text::glyph_pen_pos p =
                    lx::text::glyph_pen(pen + g.x_offset, line.baseline - g.y_offset);
                pen += g.x_advance;
                const lx::text::glyph_key key{run.face, run.size_26_6, g.glyph, p.variant,
                                              hinting};
                const lx::text::glyph_entry* found = glyphs_->find(key);
                if (!found) {
                    if (!lock.owns_lock())
                        lock = faces.lock();
                    auto got = glyphs_->get_locked(faces, key);
                    if (!got) {
                        ++stats_.failures;
                        continue;
                    }
                    found = *got;
                    ++stats_.rasterized;
                }
                const lx::text::glyph_entry e = *found;
                if (e.empty())
                    continue;
                lx::gfx::atlas_tile tile{};
                if (!glyphs_->place(e, tile)) {
                    ++stats_.failures;
                    continue;
                }
                const lx::rect2i box{at.x + p.x + e.left, at.y + p.y - e.top, e.width, e.height};
                if (e.color) {
                    out.glyph(lx::gfx::primitive_kind::poly_sprite, tile.page, box, tile.uv(),
                              white);
                    ++stats_.color_sprites;
                } else {
                    out.glyph(lx::gfx::primitive_kind::mono_sprite, tile.page, box, tile.uv(),
                              run_color);
                }
                ++stats_.sprites;
            }

            // The span's underline, under the run's advance.
            if (!attr || run.hanging || run.width <= 0)
                continue;
            const bool wavy = attr->underline == lx::text::text_underline::wavy ||
                              attr->role == lx::text::span_role::spell;
            if (!wavy && attr->underline != lx::text::text_underline::single)
                continue;
            const lx::text::face_metrics* m = layout_.metrics(run.face);
            const std::int32_t offset = m ? m->underline_offset : run.size_26_6 / 14;
            const std::int32_t thick_26_6 = m ? m->underline_thickness : 64;
            std::int32_t thick = lx_ui_round_26_6(thick_26_6);
            if (thick < 1)
                thick = 1;
            const std::int32_t x0 = lx_ui_round_26_6(run.x);
            const std::int32_t x1 = lx_ui_round_26_6(run.x + run.width);
            const std::int32_t y_top = lx_ui_round_26_6(line.baseline + offset);
            const lx::color c = span_color ? lx_ui_text_rgba(attr->color) : color_;
            if (wavy) {
                // Amplitude and thickness t: the curve fills 3t around its centre line.
                out.underline({at.x + x0, at.y + y_top - thick, x1 - x0, 3 * thick}, c,
                              static_cast<float>(thick), true);
            } else {
                out.underline({at.x + x0, at.y + y_top, x1 - x0, thick}, c,
                              static_cast<float>(thick));
            }
            ++stats_.underlines;
        }
    }
}

lx::ui::child lx::ui::text_block(const text_block_props& props, element_key key) {
    return describe<text_block_node, text_block_props>(props, key);
}
