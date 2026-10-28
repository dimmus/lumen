module;

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.text;
import lx.trace;

export module lx.ui:text_node;

import std;

import :element;
import :invalidate;
import :node;

namespace {

constexpr lx::trace::category log_text{"ui.text"};

} // namespace

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
// Coordinates: the node's bounds are layout pixels (what `draw_scope` takes, logical in a
// scaled window); the layout is laid out at the raster scale — the props' `scale` × the UI's
// device scale (`ui_device_scale`, ADR-010 §4.10) — so its 26.6 units are device pixels;
// `font.size_pt` and the logical max width are scaled by it, and `measure` answers device
// sizes over the device scale (rounded up). Glyphs are rasterised at size × raster scale,
// placed by `text::glyph_pen` — x on a quarter pixel (the subpixel variant), y on a whole
// pixel — from the block's projected origin (`draw_scope::device_rect`), so a glyph sprite
// always lands 1:1 on whole device pixels and samples its tile exactly at any scale.
//
// Emission does no per-glyph work for a line outside the scope's clip: lines are stacked
// top to bottom, so the visible ones are found by a binary search over their tops (widened
// by one em for ink above or below the line box) before any glyph is looked at. A glyph is
// placed once — one glyph-cache probe and one atlas lookup + touch
// (`glyph_cache::find_placed`) — and its box and tile kept per layout glyph (`placed_`) until
// the layout runs a pass again, so a frame that only moves the text (a scroll) costs one
// `atlas::use` per glyph (which also tells an evicted tile, placed again through the glyph
// cache) and one sprite. A glyph not in the glyph cache is rasterised on that frame (counted
// in `last_emit().rasterized`) under the text engine's face lock, taken once per emit and
// only when something missed. A glyph that cannot be rasterised is remembered as failed and
// not tried again for `gfx::k_atlas_stale_frames` frames (or until the layout runs a pass),
// so a glyph that never succeeds costs no FreeType work per frame. When the failure is a
// face the face cache evicted since the shape pass (its 32-face LRU reuses the slot under a
// new id), the block reshapes: inline text on the next frame (the layout's shape pass
// invalidated, the node's layout too), off-thread text through another worker job. Order: a line's sprites share one order from the overlap grid
// (`draw_scope::group_order` over their union, at most 128 sprites a group; a run's underline
// closes the group so it draws above the run), so they cost no grid lookup each and batch in
// push order; each group's order is at least the previous one's + 1, so a block's groups
// ascend and the sort has nothing to do. No allocation per frame: the layout's storage, the
// placements and the glyph cache are retained.
//
// Large text (`k_text_async_min_bytes` or more, with a `text_async_shaping`) is shaped and
// wrapped on the worker affinity; the layout on show stays until the result lands (below,
// "Off-thread shaping").
//
// UI affinity (the glyph cache writes the atlas): `emit` runs inside the frame build. The
// layout's queries lock the engine's face cache lazily, so the layout is brought up to date
// before the face lock is taken.

/// Text of at least this many UTF-8 bytes is shaped off the UI thread when the block has a
/// `text_async_shaping` (ADR-010 §4.6, the boundary §8.3 S4 measured). On the F8 host a cold
/// reshape (shape cache empty for the text) costs about 0.21 µs a byte — 0.13 ms for a
/// 669-byte paragraph, 1.4 ms for a 6.9 KB page, 31–38 ms for S4's 139 KB document — so 4 KiB
/// keeps an inline reshape under about 1 ms, half of G4's 2 ms frame; a whole document goes
/// to the worker.
inline constexpr std::uint32_t k_text_async_min_bytes = 4096;

/// Off-thread shaping for large text. `engine` is the worker's: its own shape cache and
/// itemizer (a shape cache is single-threaded — a hit takes no lock), over the same face
/// cache as the block's engine (face ids, and so the glyph cache's keys, belong to it). The
/// worker affinity's strand of `executor` (null: `executor::global()`) runs the shape and
/// wrap passes; the result comes back through the UI strand. Several blocks may share one.
struct text_async_shaping {
    lx::text::text_engine engine{};
    lx::runtime::executor* executor = nullptr;
    std::uint32_t min_bytes = k_text_async_min_bytes;
};

/// What a text block lays out with and rasterises into. Null `engine` members are the
/// shared caches; without `glyphs` the block lays out and measures but draws nothing; with
/// `async`, text of `async->min_bytes` or more is shaped on the worker affinity.
struct text_raster_context {
    lx::text::text_engine engine{};
    lx::text::glyph_cache* glyphs = nullptr;
    text_async_shaping* async = nullptr;
};

/// The off-thread shaping of one block.
struct text_async_stats {
    std::uint64_t posted = 0;     ///< shape jobs sent to the worker
    std::uint64_t landed = 0;     ///< results swapped in
    std::uint64_t discarded = 0;  ///< results dropped (the block went back to inline text)
    std::uint64_t inline_fallbacks = 0; ///< the worker strand was full: shaped inline instead
    bool pending = false;         ///< a job is on the worker or its result not yet landed
    bool active = false;          ///< the block's text is shaped off-thread
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
    unsigned cached = 0;       ///< glyphs placed from the node's placement cache
};

class text_block_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type() {
        static const element_type_id id = next_element_type_id();
        return id;
    }
    [[nodiscard]] element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "text_block"; }

    text_block_node() = default;
    ~text_block_node() override;

    /// Marks the layout dirty itself when an input the layout reads changed, so it tells
    /// the reconciler there is nothing more to do (false).
    bool apply(const text_block_props& props);

    /// The text's size wrapped at the constraint's width; for `size_hint::min` when it wraps,
    /// its min-content width (`text_layout::min_content_width`, the longest word) and the
    /// height wrapped there.
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void arrange(lx::rect2i bounds) override;
    void emit(draw_scope& out) const override;

    [[nodiscard]] const lx::text::text_layout& layout() const { return layout_; }
    /// The UI device scale the layout is laid out at (`ui_device_scale` when it last
    /// measured, arranged or drew): its 26.6 units are layout px × this.
    [[nodiscard]] float device_scale() const { return ui_scale_; }
    /// A length of the layout's (device px) in layout px, rounded up.
    [[nodiscard]] int to_layout_px(int device_px) const;
    [[nodiscard]] text_emit_stats last_emit() const { return stats_; }
    [[nodiscard]] text_async_stats async_stats() const;

private:
    struct shape_job;

    /// `apply` for text below the async boundary (or without `async`): the layout's setters.
    void apply_inline(const text_block_props& p);
    /// `apply` for large text: the inputs go to the job, a shape on the worker if they changed.
    void apply_async(const text_block_props& p, text_async_shaping& async);
    /// Posts the job's inputs to the worker; inline when the strand is full.
    void start_job();
    /// UI affinity: the worker's result arrived (the job's `generation`).
    void land();
    static void run_job(void* job);
    static void land_job(void* job);
    /// The layout's max width (its logical units) for a width in layout px; 0 is unbounded.
    [[nodiscard]] float logical_width(int width_px) const;
    /// Follows a change of `ui_device_scale` since the layout was set up (a reducer's
    /// subtree is not re-applied when the root's scale moves).
    void sync_scale() const;
    /// UI affinity: a face the layout's runs name is gone from the face cache. Reshapes —
    /// inline on the next frame, or through another worker job for off-thread text.
    void faces_evicted();

    mutable lx::text::text_layout layout_{};
    lx::color color_ = lx::color::rgb(1.f, 1.f, 1.f);
    /// The raster scale: the props' scale × the UI's device scale.
    mutable float scale_ = 1.f;
    float prop_scale_ = 1.f;
    mutable float ui_scale_ = 1.f;
    bool wraps_ = true;
    lx::text::glyph_hinting hinting_ = lx::text::glyph_hinting::none;
    lx::text::glyph_cache* glyphs_ = nullptr;
    /// The engine's face cache (null: the shared one): its lock guards rasterising.
    lx::text::face_cache* faces_ = nullptr;
    mutable text_emit_stats stats_{};

    /// A layout glyph as last drawn: its box from the block's top left, its atlas tile.
    struct placed_glyph {
        std::int32_t x = 0;
        std::int32_t y = 0;
        /// `gfx::atlas_tile_id::value`; state 3: the atlas frame of the failure (low bits).
        std::uint32_t tile = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        /// 0: not placed yet, 1: empty (a space), 2: a sprite, 3: failed (retried
        /// `gfx::k_atlas_stale_frames` frames on)
        std::uint8_t state = 0;
        bool color = false;
    };
    /// One entry per layout glyph (`layout_run::glyph_begin` + i; ellipsis runs are not
    /// cached), valid while the layout's passes, the glyph cache and the hinting are those it
    /// was filled under. Sized when the layout changes, never per frame.
    mutable std::vector<placed_glyph> placed_{};
    mutable std::uint64_t placed_passes_ = ~0ull;
    mutable const lx::text::glyph_cache* placed_glyphs_ = nullptr;
    mutable lx::text::glyph_hinting placed_hinting_ = lx::text::glyph_hinting::none;
    mutable bool placed_valid_ = false;

    // Off-thread shaping: `job_` is allocated on the first large text and kept (its layouts'
    // storage with it); the node's generation counts shape-input changes of large text,
    // `shown_` the one `layout_` holds.
    shape_job* job_ = nullptr;
    text_async_shaping* async_ = nullptr;
    lx::text::text_engine engine_{};
    std::uint64_t async_generation_ = 0;
    std::uint64_t shown_generation_ = 0;
    bool async_active_ = false;
    text_async_stats async_stats_{};
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

/// Glyphs buffered by `text_block_node::emit` until their group's order is known.
constexpr unsigned k_text_glyph_group = 128;

struct lx_ui_pending_glyph {
    lx::rect2i box{};
    lx::gfx::sprite_uv uv{};
    lx::gfx::packed_rgba color{};
    lx::gfx::primitive_kind kind = lx::gfx::primitive_kind::mono_sprite;
    std::uint8_t page = 0;
};

[[nodiscard]] lx::rect2i lx_ui_text_union(lx::rect2i a, lx::rect2i b) {
    const int x0 = a.x < b.x ? a.x : b.x;
    const int y0 = a.y < b.y ? a.y : b.y;
    const int x1 = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    const int y1 = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return {x0, y0, x1 - x0, y1 - y0};
}

/// 26.6 → whole pixels, rounded to nearest (floor of x + ½).
[[nodiscard]] constexpr std::int32_t lx_ui_round_26_6(std::int32_t v) { return (v + 32) >> 6; }

} // namespace

// ── Off-thread shaping (ADR-010 §4.6) ──
//
// Text of `async->min_bytes` or more is not laid out by `apply`: its inputs go to the job's
// `next` layout (an input record, never laid out), and when its shape inputs changed the job
// copies them into `staging` and posts `run_job` to the worker strand. The worker runs
// `staging.update()` (shape + wrap, with the async engine: its own shape cache and itemizer,
// the block's face cache) and posts `land_job` to the UI strand. Landing swaps `staging` into
// `layout_`, points it at the block's engine (`rebind_engine`: same face cache, no reshape),
// applies the current wrap inputs (a re-wrap at most) and invalidates the node's layout; the
// layout shown until then is the previous one. A change that arrives while a job runs only
// bumps the generation: the landing job's result is shown and another job is posted with the
// newest inputs. Text that drops below the boundary is laid out inline at once and a job
// still running is discarded when it lands. The node's destructor leaves a running job to
// `land_job`, which frees it. Allocation: the job (two layouts) once per node, the layouts'
// vectors as they grow — never per frame.

struct lx::ui::text_block_node::shape_job {
    lx::text::text_layout staging{};
    lx::text::text_layout next{};
    lx::result<void> outcome{};
    /// Null once the node is gone (UI affinity).
    text_block_node* node = nullptr;
    lx::runtime::executor* executor = nullptr;
    std::uint64_t generation = 0;
    bool running = false;
};

lx::ui::text_block_node::~text_block_node() {
    if (!job_)
        return;
    if (job_->running)
        job_->node = nullptr; // `land_job` frees it
    else
        delete job_;
}

bool lx::ui::text_block_node::apply(const text_block_props& p) {
    // A colour change repaints the block where it is; a layout change repaints it where it
    // was and where the arrange puts it (`mark_repaint`, below).
    if (p.color.r != color_.r || p.color.g != color_.g || p.color.b != color_.b ||
        p.color.a != color_.a)
        invalidate_paint(*this);
    color_ = p.color;
    prop_scale_ = p.scale > 0.f ? p.scale : 1.f;
    ui_scale_ = ui_device_scale();
    scale_ = prop_scale_ * ui_scale_;
    wraps_ = p.wrap == lx::text::text_wrap::word;
    hinting_ = p.hinting;
    glyphs_ = p.context.glyphs;
    faces_ = p.context.engine.faces;
    engine_ = p.context.engine;
    text_async_shaping* async = p.context.async;
    const auto faces_of = [](lx::text::face_cache* f) {
        return f ? f : &lx::text::face_cache::shared();
    };
    // The worker needs caches of its own over the block's face cache.
    if (async && (!async->engine.shapes || !async->engine.items ||
                  faces_of(async->engine.faces) != faces_of(p.context.engine.faces) ||
                  async->engine.shapes == p.context.engine.shapes ||
                  async->engine.items == p.context.engine.items))
        async = nullptr;
    const std::size_t bytes = p.text ? std::strlen(p.text) : 0;
    if (async && bytes >= async->min_bytes)
        apply_async(p, *async);
    else
        apply_inline(p);
    return false;
}

void lx::ui::text_block_node::apply_inline(const text_block_props& p) {
    if (async_active_) {
        // Back to inline text: a job still running is discarded when it lands.
        async_active_ = false;
        ++async_generation_;
    }
    lx::text::text_layout& l = layout_;
    l.set_engine(p.context.engine);
    l.set_text(p.text ? p.text : "");
    l.set_font(p.font);
    l.set_scale(scale_);
    l.set_align(p.align);
    l.set_wrap(p.wrap);
    l.set_max_lines(p.max_lines);
    l.set_ellipsize(p.ellipsize);
    l.set_line_height(p.line_height);
    l.set_direction(p.direction);
    l.set_attrs(p.attrs);
    if (l.dirty()) {
        mark_layout_dirty();
        mark_repaint();
    }
}

void lx::ui::text_block_node::apply_async(const text_block_props& p, text_async_shaping& async) {
    if (!job_) {
        job_ = new shape_job{};
        job_->node = this;
    }
    async_ = &async;
    lx::text::text_layout& next = job_->next;
    const std::uint64_t before = next.shape_generation();
    next.set_text(p.text);
    next.set_font(p.font);
    next.set_scale(scale_);
    next.set_attrs(p.attrs);
    next.set_direction(p.direction);
    next.set_align(p.align);
    next.set_wrap(p.wrap);
    next.set_max_lines(p.max_lines);
    next.set_ellipsize(p.ellipsize);
    next.set_line_height(p.line_height);
    if (!async_active_ || next.shape_generation() != before) {
        async_active_ = true;
        ++async_generation_;
    }
    // The layout on show keeps its shaping; a new face cache makes it meaningless.
    layout_.rebind_engine(p.context.engine);
    if (layout_.shape_dirty()) {
        layout_ = lx::text::text_layout{p.context.engine};
        shown_generation_ = 0;
        placed_valid_ = false;
    }
    // Its wrap inputs follow at once: a re-wrap is cheap.
    layout_.assign_inputs(next, false);
    if (async_generation_ != shown_generation_ && !job_->running)
        start_job();
    if (layout_.dirty()) {
        mark_layout_dirty();
        mark_repaint();
    }
}

void lx::ui::text_block_node::start_job() {
    shape_job& j = *job_;
    lx::runtime::executor& ex = async_->executor ? *async_->executor
                                                 : lx::runtime::executor::global();
    j.staging.set_engine(async_->engine);
    j.staging.assign_inputs(j.next);
    // The width the block was last laid out at; arrange re-wraps if it moved meanwhile.
    j.staging.set_max_width(layout_.max_width());
    j.generation = async_generation_;
    j.executor = &ex;
    j.running = true;
    if (ex.post(lx::runtime::affinity::worker, lx::runtime::task{&text_block_node::run_job, &j})) {
        ++async_stats_.posted;
        return;
    }
    // The worker strand is full: shape here rather than show stale text indefinitely.
    j.running = false;
    ++async_stats_.inline_fallbacks;
    log_text.warn("text_block: worker strand full, shaping inline");
    layout_.assign_inputs(j.next);
    shown_generation_ = async_generation_;
    placed_valid_ = false;
    mark_layout_dirty();
}

void lx::ui::text_block_node::run_job(void* raw) {
    lx::runtime::assert_affinity(lx::runtime::affinity::worker);
    auto* j = static_cast<shape_job*>(raw);
    j->outcome = j->staging.update();
    // Back to the UI strand. It drains every frame, so a full queue clears: retry, yielding.
    const lx::runtime::task back{&text_block_node::land_job, j};
    for (unsigned attempt = 0; !j->executor->post(lx::runtime::affinity::ui, back); ++attempt) {
        if (attempt == 100000) {
            log_text.error("text_block: UI strand full, shape result lost");
            return;
        }
        std::this_thread::yield();
    }
}

void lx::ui::text_block_node::land_job(void* raw) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    auto* j = static_cast<shape_job*>(raw);
    j->running = false;
    if (!j->node) {
        delete j;
        return;
    }
    j->node->land();
}

void lx::ui::text_block_node::land() {
    shape_job& j = *job_;
    if (!async_active_) {
        ++async_stats_.discarded;
        return;
    }
    // Newer than what is on show even when the inputs moved on: show it, then go again.
    std::swap(layout_, j.staging);
    layout_.rebind_engine(engine_);
    layout_.assign_inputs(j.next, false);
    shown_generation_ = j.generation;
    placed_valid_ = false;
    ++async_stats_.landed;
    invalidate_layout(*this);
    if (async_generation_ != shown_generation_)
        start_job();
}

lx::ui::text_async_stats lx::ui::text_block_node::async_stats() const {
    text_async_stats s = async_stats_;
    s.pending = job_ && job_->running;
    s.active = async_active_;
    return s;
}

float lx::ui::text_block_node::logical_width(int width_px) const {
    if (!wraps_ || width_px <= 0 || width_px >= 99999)
        return 0.f;
    // Layout px → the layout's own logical units: device px over the raster scale, which
    // is the props' scale (the UI's device scale cancels).
    return static_cast<float>(width_px) / prop_scale_;
}

void lx::ui::text_block_node::sync_scale() const {
    const float s = ui_device_scale();
    if (s == ui_scale_)
        return;
    // The window moved to an output of another scale: lay the text out (and rasterise it)
    // at the new device scale. `ui_root` relays the whole tree out when its scale changes.
    ui_scale_ = s;
    scale_ = prop_scale_ * ui_scale_;
    layout_.set_scale(scale_);
    placed_valid_ = false;
}

int lx::ui::text_block_node::to_layout_px(int device_px) const {
    if (ui_scale_ == 1.f)
        return device_px;
    // Up, so the box holds every device pixel of the text (the tolerance absorbs the float
    // error of an exact multiple).
    return static_cast<int>(std::ceil(static_cast<double>(device_px) / ui_scale_ - 1e-4));
}

void lx::ui::text_block_node::faces_evicted() {
    if (async_active_) {
        // The worker shapes again from the current inputs; the layout on show stays until
        // the result lands (its missing glyphs are remembered as failed meanwhile).
        ++async_generation_;
        if (job_ && !job_->running)
            start_job();
        return;
    }
    layout_.invalidate_shape();
    invalidate_layout(*this);
}

lx::size2i lx::ui::text_block_node::measure(lx::layout::constraints c, size_hint hint) const {
    sync_scale();
    // A measure wraps the layout at the width it asks about; `emit` puts the bounds' width
    // back, since a parent may measure after it arranged (a cached arrange is not re-run).
    int width = c.max.width;
    if (hint == size_hint::min && wraps_)
        width = std::min(width, std::max(to_layout_px(layout_.min_content_width()), 1));
    layout_.set_max_width(logical_width(width));
    lx::size2i out = layout_.bounding_size();
    out = {to_layout_px(out.width), to_layout_px(out.height)};
    if (out.width < c.min.width)
        out.width = c.min.width;
    if (out.height < c.min.height)
        out.height = c.min.height;
    if (out.width > c.max.width)
        out.width = c.max.width;
    // A height the parent does not fix is the text's own, even past the space offered: a
    // line taller than its row overflows it centred (a 14 px label on GPUI's φ line, 23 px,
    // in a 22 px button), as CSS and GPUI lay it out, rather than being cut to the row.
    if (out.height > c.max.height && c.min.height == c.max.height)
        out.height = c.max.height;
    return out;
}

void lx::ui::text_block_node::arrange(lx::rect2i bounds) {
    sync_scale();
    bounds_ = bounds;
    layout_.set_max_width(logical_width(bounds.width));
    layout_dirty_ = false;
}

void lx::ui::text_block_node::emit(draw_scope& out) const {
    stats_ = {};
    if (!visible_ || !glyphs_)
        return;
    sync_scale();
    layout_.set_max_width(logical_width(bounds_.width));
    // Lay out now, before the face lock below: the queries lock the engine lazily.
    if (!layout_.update())
        return;
    const std::span<const lx::text::layout_line> lines = layout_.lines();
    stats_.lines = static_cast<unsigned>(lines.size());
    if (lines.empty())
        return;

    // The clip in the layout's space (26.6 from its top), widened by an em for ink outside
    // the line boxes.
    // Device space: the layout's units are device pixels (it is laid out at the device
    // scale), placed from the block's projected origin, so every glyph lands on whole device
    // pixels and samples its tile 1:1 at any scale.
    const lx::rect2i clip = out.device_clip();
    const lx::rect2i dbounds = out.device_rect(bounds_);
    const std::int32_t em = layout_.size_26_6();
    const std::int64_t top_px = static_cast<std::int64_t>(clip.y) - dbounds.y;
    const std::int64_t clip_top = top_px * 64 - em;
    const std::int64_t clip_bottom = (top_px + clip.height) * 64 + em;
    // Across: the lines start at most the overflow left of the bounds (end-aligned text wider
    // than them) and end at most the overflow right of them; an em more each side for ink. A
    // block wholly beside the clip — a label outside a frame's damage — looks at no glyph.
    {
        const std::int64_t text_w = (static_cast<std::int64_t>(layout_.width()) + 63) / 64;
        const std::int64_t over = std::max<std::int64_t>(text_w - dbounds.width, 0);
        const std::int64_t em_px = (static_cast<std::int64_t>(em) + 63) / 64;
        const std::int64_t left = static_cast<std::int64_t>(dbounds.x) - over - em_px;
        const std::int64_t right =
            static_cast<std::int64_t>(dbounds.x) + dbounds.width + over + em_px;
        if (right <= clip.x || left >= static_cast<std::int64_t>(clip.x) + clip.width)
            return;
    }
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
    const lx::point2i at{dbounds.x, dbounds.y};
    lx::gfx::atlas& atlas = glyphs_->atlas();
    const std::uint32_t atlas_frame = static_cast<std::uint32_t>(atlas.frame());
    // A run's face was evicted from the face cache since the shape pass.
    bool face_gone = false;

    // The placement cache: dropped when the layout ran a pass since it was filled.
    const lx::text::text_layout_counters lc = layout_.counters();
    const std::uint64_t passes = lc.shape_passes + lc.wrap_passes;
    if (!placed_valid_ || placed_passes_ != passes || placed_glyphs_ != glyphs_ ||
        placed_hinting_ != hinting) {
        std::size_t total = 0;
        for (const lx::text::layout_run& r : layout_.runs())
            if (!r.ellipsis && std::size_t{r.glyph_begin} + r.glyph_count > total)
                total = std::size_t{r.glyph_begin} + r.glyph_count;
        placed_.assign(total, placed_glyph{});
        placed_passes_ = passes;
        placed_glyphs_ = glyphs_;
        placed_hinting_ = hinting;
        placed_valid_ = true;
    }

    // A line's sprites are emitted in groups sharing one order (`draw_scope::group_order`
    // over the group's union): a group closes at the line's end, before an underline (drawn
    // above its run) and when the buffer is full.
    lx_ui_pending_glyph pending[k_text_glyph_group];
    unsigned pending_count = 0;
    lx::rect2i pending_union{};
    std::uint32_t next_order = 0;
    const auto flush = [&] {
        if (pending_count == 0)
            return;
        const std::uint32_t order = out.device_group_order(pending_union, next_order);
        next_order = order + 1;
        for (unsigned i = 0; i < pending_count; ++i) {
            const lx_ui_pending_glyph& g = pending[i];
            out.device_glyph(g.kind, g.page, g.box, g.uv, g.color, order);
        }
        pending_count = 0;
        pending_union = {};
    };

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
            const std::span<const lx::text::layout_glyph> run_glyphs = layout_.glyphs(run);
            placed_glyph* cache = !run.ellipsis && std::size_t{run.glyph_begin} +
                                                           run_glyphs.size() <= placed_.size()
                                      ? placed_.data() + run.glyph_begin
                                      : nullptr;
            for (std::size_t gi = 0; gi < run_glyphs.size(); ++gi) {
                const lx::text::layout_glyph& g = run_glyphs[gi];
                ++stats_.glyphs;
                if (cache && cache[gi].state != 0) {
                    const placed_glyph& c = cache[gi];
                    lx::gfx::atlas_tile tile{};
                    if (c.state == 1) {
                        pen += g.x_advance;
                        ++stats_.cached;
                        continue;
                    }
                    if (c.state == 3 && atlas_frame - c.tile < lx::gfx::k_atlas_stale_frames) {
                        // Failed recently: not tried again yet.
                        pen += g.x_advance;
                        ++stats_.failures;
                        continue;
                    }
                    if (c.state == 2 && atlas.use({c.tile}, tile)) {
                        pen += g.x_advance;
                        if (pending_count == k_text_glyph_group)
                            flush();
                        lx_ui_pending_glyph& pg = pending[pending_count++];
                        pg.box = {at.x + c.x, at.y + c.y, c.width, c.height};
                        pg.uv = tile.uv();
                        pg.page = tile.page;
                        if (c.color) {
                            pg.kind = lx::gfx::primitive_kind::poly_sprite;
                            pg.color = white;
                            ++stats_.color_sprites;
                        } else {
                            pg.kind = lx::gfx::primitive_kind::mono_sprite;
                            pg.color = run_color;
                        }
                        pending_union = pending_count == 1 ? pg.box
                                                           : lx_ui_text_union(pending_union, pg.box);
                        ++stats_.sprites;
                        ++stats_.cached;
                        continue;
                    }
                    // The atlas evicted the tile, or a failure is old enough to retry:
                    // placed again below.
                }
                const lx::text::glyph_pen_pos p =
                    lx::text::glyph_pen(pen + g.x_offset, line.baseline - g.y_offset);
                pen += g.x_advance;
                const lx::text::glyph_key key{run.face, run.size_26_6, g.glyph, p.variant,
                                              hinting};
                lx::gfx::atlas_tile tile{};
                const lx::text::glyph_entry* found = glyphs_->find_placed(key, tile);
                if (!found) {
                    if (!lock.owns_lock())
                        lock = faces.lock();
                    auto got = glyphs_->get_locked(faces, key);
                    if (!got || (!(*got)->empty() && !glyphs_->place(**got, tile))) {
                        ++stats_.failures;
                        if (!got && !faces.alive_locked(run.face))
                            face_gone = true;
                        if (cache) {
                            cache[gi].state = 3;
                            cache[gi].tile = atlas_frame;
                        }
                        continue;
                    }
                    found = *got;
                    ++stats_.rasterized;
                }
                const lx::text::glyph_entry e = *found;
                if (cache) {
                    placed_glyph& c = cache[gi];
                    c.x = p.x + e.left;
                    c.y = p.y - e.top;
                    c.tile = e.tile.value;
                    c.width = e.width;
                    c.height = e.height;
                    c.state = e.empty() ? 1 : 2;
                    c.color = e.color;
                }
                if (e.empty())
                    continue;
                if (pending_count == k_text_glyph_group)
                    flush();
                lx_ui_pending_glyph& pg = pending[pending_count++];
                pg.box = {at.x + p.x + e.left, at.y + p.y - e.top, e.width, e.height};
                pg.uv = tile.uv();
                pg.page = tile.page;
                if (e.color) {
                    pg.kind = lx::gfx::primitive_kind::poly_sprite;
                    pg.color = white;
                    ++stats_.color_sprites;
                } else {
                    pg.kind = lx::gfx::primitive_kind::mono_sprite;
                    pg.color = run_color;
                }
                pending_union = pending_count == 1 ? pg.box : lx_ui_text_union(pending_union, pg.box);
                ++stats_.sprites;
            }

            // The span's underline, under the run's advance.
            if (!attr || run.hanging || run.width <= 0)
                continue;
            const bool wavy = attr->underline == lx::text::text_underline::wavy ||
                              attr->role == lx::text::span_role::spell;
            if (!wavy && attr->underline != lx::text::text_underline::single)
                continue;
            // Above the run's glyphs: they take their order first.
            flush();
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
                out.device_underline({at.x + x0, at.y + y_top - thick, x1 - x0, 3 * thick}, c,
                                     static_cast<float>(thick), true);
            } else {
                out.device_underline({at.x + x0, at.y + y_top, x1 - x0, thick}, c,
                                     static_cast<float>(thick));
            }
            ++stats_.underlines;
        }
        flush();
    }
    if (face_gone) {
        if (lock.owns_lock())
            lock.unlock();
        // Nodes are never const objects; emit is const only to the draw walk.
        const_cast<text_block_node*>(this)->faces_evicted();
    }
}

lx::ui::child lx::ui::text_block(const text_block_props& props, element_key key) {
    return describe<text_block_node, text_block_props>(props, key);
}
