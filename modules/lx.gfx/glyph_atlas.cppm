module;

import lx.foundation;

export module lx.gfx:glyph_atlas;

export namespace lx::gfx {

struct glyph_key {
    unsigned codepoint = 0;
    float size_px = 0.f;
};

struct glyph_metrics {
    lx::rect2i bounds{};
    lx::point2i bearing{};
    float advance = 0.f;
    lx::texture_id atlas_texture{};
};

class glyph_atlas {
public:
    [[nodiscard]] lx::result<glyph_metrics> lookup(glyph_key key);
    void tick(unsigned frame_index);

private:
    unsigned next_slot_ = 1;
    unsigned last_frame_ = 0;
};

} // namespace lx::gfx


lx::result<lx::gfx::glyph_metrics> lx::gfx::glyph_atlas::lookup(glyph_key key) {
    glyph_metrics m{};
    m.bounds = {0, 0, static_cast<int>(key.size_px), static_cast<int>(key.size_px)};
    m.advance = key.size_px * 0.6f;
    m.atlas_texture = lx::texture_id{next_slot_++};
    return m;
}

void lx::gfx::glyph_atlas::tick(unsigned frame_index) { last_frame_ = frame_index; }
