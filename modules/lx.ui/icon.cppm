module;

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

export module lx.ui:icon;

import std;
import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.trace;

import :element;
import :invalidate;
import :node;
import :theme;

// ── Icons (ADR-010 §5.5) ───────────────────────────────────────────────────────────────
//
// Lumen bundles a subset of Lucide (ISC; assets/icons/lucide, the README there lists them
// with their source tag and hashes): 24-unit, stroke-only, monochrome SVGs. An icon is drawn
// as a `mono_sprite`: its coverage at the size it is drawn, in an R8 tile of the atlas,
// tinted at draw time with an `icon.*` token — so one raster serves every colour and theme.
//
// `icon_cache` rasterises each (icon, device size) once: librsvg renders the SVG into a
// cairo ARGB32 buffer (currentColor is black, the default) and the alpha channel becomes the
// tile's coverage. A size is the icon's px × scale rounded, so 16 px at scale 1.25 is its own
// 20 px raster, 1:1 on the output. The table holds 256 entries (an evicted tile — unused for
// `gfx::k_atlas_stale_frames` and its page needed — is rasterised again on its next use,
// counted). By default a miss rasterises at once, on the UI thread, the first time an icon is
// drawn at a size (counted in `rasterized`; a steady frame never misses). With `set_async`,
// a miss is rasterised on the worker affinity instead (a pool of four jobs, their buffers
// allocated with the cache) and the coverage written into the atlas on the UI strand, which
// damages the bounds of every icon that asked for it meanwhile; the icon draws nothing until
// then (ADR §4.12).
//
// Placement (ADR §4.13): this partition is in lx.ui rather than lx.gfx because the icon set,
// its files and the tint tokens are the UI's, and librsvg is loaded with dlopen on the first
// raster (as lx.shell:svg does), so no library edge is added to any module. Built without
// librsvg headers (no LUMEN_HAS_RSVG), or on a host without librsvg-2.so.2, every raster
// fails (`ui_err::icon_raster`) and icons draw nothing.

export namespace lx::ui {

enum class icon : std::uint8_t {
    chevron_down,
    chevron_up,
    chevron_left,
    chevron_right,
    x,
    check,
    minus,
    plus,
    search,
    menu,
    ellipsis,
    ellipsis_vertical,
    circle,
    circle_dot,
    info,
    triangle_alert,
    circle_check,
    circle_x,
    circle_alert,
    bell,
    bell_off,
    settings,
    power,
    volume,
    volume_1,
    volume_2,
    volume_x,
    wifi,
    wifi_off,
    battery,
    battery_low,
    battery_charging,
    bluetooth,
    calendar,
    clock,
    folder,
    file,
    terminal,
    copy,
    trash,
    pencil,
    external_link,
    lock,
    log_out,
    refresh_cw,
    sun,
    moon,
    monitor,
    arrow_left,
    arrow_right,
    house,
    user,
    keyboard,
    count,
};

inline constexpr unsigned k_icon_count = static_cast<unsigned>(icon::count);

/// The Lucide file name of `i`, without ".svg".
[[nodiscard]] std::string_view icon_name(icon i);

/// Zed's icon sizes (§5.5), px at scale 1.
inline constexpr unsigned k_icon_px = 16;
inline constexpr unsigned k_icon_small_px = 14;
inline constexpr unsigned k_icon_xsmall_px = 12;

/// The directory of the bundled SVGs, from the first of: `LUMEN_ICON_DIR`, the source
/// tree's assets/icons/lucide, `<exe>/../share/lumen/icons/lucide`, the configured install.
/// "" when none exists.
[[nodiscard]] const char* icon_dir();
/// librsvg loads and the icon directory exists.
[[nodiscard]] bool icons_available();

/// Rasterises the SVG at `path` into `coverage` (`size_px`², rows packed): the alpha of the
/// document rendered into a `size_px`² viewport. Any thread. Errors: `ui_err::icon_raster`.
[[nodiscard]] lx::result<void> rasterize_icon(const char* path, unsigned size_px,
                                              std::span<std::uint8_t> coverage);

struct icon_cache_counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t rasterized = 0;   ///< rasters written into the atlas
    std::uint64_t failures = 0;     ///< rasters that failed (not retried for 120 frames)
    std::uint64_t rerasterized = 0; ///< of `rasterized`: tiles the atlas had evicted
    std::uint64_t async_posted = 0;
    std::uint64_t async_landed = 0;
};

/// (icon, device px) → an R8 atlas tile. UI affinity (it writes the atlas).
class icon_cache {
public:
    static constexpr unsigned k_capacity = 256;
    static constexpr unsigned k_max_px = 128;
    static constexpr unsigned k_async_jobs = 4;

    /// `dir`: the SVG directory (null: `icon_dir()`).
    explicit icon_cache(lx::gfx::atlas& atlas, const char* dir = nullptr);
    ~icon_cache();
    icon_cache(const icon_cache&) = delete;
    icon_cache& operator=(const icon_cache&) = delete;

    /// The tile of `id` at `size_px` device pixels, stamped used this atlas frame. A miss
    /// rasterises it now, or with `set_async` posts it to the worker and answers false until
    /// it lands, then damages `pending_damage` (the asking node's bounds). False for an icon
    /// that cannot be drawn (no librsvg, a failed raster: tried again after 120 frames).
    [[nodiscard]] bool place(icon id, unsigned size_px, lx::gfx::atlas_tile& out,
                             lx::rect2i pending_damage = {});
    /// Rasterises into the atlas now if it is not there (a warm-up).
    [[nodiscard]] lx::result<lx::gfx::atlas_tile_id> get(icon id, unsigned size_px);

    /// Misses go to `executor`'s worker strand (null: synchronous rasterising again).
    void set_async(lx::runtime::executor* executor);

    [[nodiscard]] icon_cache_counters counters() const { return counters_; }
    [[nodiscard]] lx::gfx::atlas& atlas() const { return *atlas_; }
    [[nodiscard]] const char* dir() const { return dir_; }

private:
    enum class entry_state : std::uint8_t { empty, ready, failed, pending };
    struct entry {
        icon id = icon::count;
        std::uint16_t size = 0;
        entry_state state = entry_state::empty;
        lx::gfx::atlas_tile_id tile{};
        std::uint64_t stamp = 0;  ///< atlas frame of the last use or failure
        lx::rect2i damage{};      ///< pending: bounds to repaint when it lands
    };
    struct async_job;
    struct async_state;

    [[nodiscard]] entry* find_or_claim(icon id, unsigned size);
    [[nodiscard]] lx::result<lx::gfx::atlas_tile_id> raster_now(entry& e);
    void post(entry& e);
    static void run_job(void* job);
    static void land_job(void* job);
    void land(async_job& j);

    lx::gfx::atlas* atlas_ = nullptr;
    char dir_[512]{};
    entry entries_[k_capacity]{};
    lx::runtime::executor* executor_ = nullptr;
    async_state* async_ = nullptr;
    icon_cache_counters counters_{};
};

struct icon_props {
    icon id = icon::circle;
    /// Logical px (16, 14, 12); the raster is `size_px` × `scale` device px.
    float size_px = static_cast<float>(k_icon_px);
    float scale = 1.f;
    tok color = tok::icon;
    icon_cache* cache = nullptr;
};

/// An icon, square at its size × scale (device px), centred in its bounds.
class icon_node final : public ui_node {
public:
    [[nodiscard]] static element_type_id static_type();
    [[nodiscard]] element_type_id type() const override;
    [[nodiscard]] const char* type_name() const override;

    /// True when the size changed.
    bool apply(const icon_props& props);
    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, size_hint hint) const override;
    void emit(draw_scope& out) const override;

    [[nodiscard]] unsigned device_px() const { return device_px_; }

private:
    icon id_ = icon::circle;
    unsigned device_px_ = k_icon_px;
    lx::color color_{};
    std::uint64_t generation_ = 0;
    tok token_ = tok::icon;
    icon_cache* cache_ = nullptr;
};

[[nodiscard]] child icon_view(icon id, icon_cache* cache, float size_px = 16.f,
                              tok color = tok::icon, float scale = 1.f);

} // namespace lx::ui

// ── implementation ──────────────────────────────────────────────────────────────────

namespace {

constexpr std::string_view k_icon_names[] = {
    "chevron-down", "chevron-up", "chevron-left", "chevron-right", "x", "check", "minus",
    "plus", "search", "menu", "ellipsis", "ellipsis-vertical", "circle", "circle-dot", "info",
    "triangle-alert", "circle-check", "circle-x", "circle-alert", "bell", "bell-off",
    "settings", "power", "volume", "volume-1", "volume-2", "volume-x", "wifi", "wifi-off",
    "battery", "battery-low", "battery-charging", "bluetooth", "calendar", "clock", "folder",
    "file", "terminal", "copy", "trash", "pencil", "external-link", "lock", "log-out",
    "refresh-cw", "sun", "moon", "monitor", "arrow-left", "arrow-right", "house", "user",
    "keyboard",
};
static_assert(std::size(k_icon_names) == lx::ui::k_icon_count);

[[nodiscard]] bool lx_ui_icon_is_dir(const char* path) {
    struct stat st{};
    return path && path[0] && ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void lx_ui_icon_copy(char* out, std::size_t capacity, const char* in) {
    std::size_t n = in ? std::strlen(in) : 0;
    if (n >= capacity)
        n = capacity - 1;
    if (n)
        std::memcpy(out, in, n);
    out[n] = '\0';
}


[[nodiscard]] bool lx_ui_icon_path(char (&out)[768], const char* dir, lx::ui::icon id) {
    const std::string_view name = lx::ui::icon_name(id);
    const int w = std::snprintf(out, sizeof(out), "%s/%.*s.svg", dir, static_cast<int>(name.size()),
                                name.data());
    return w > 0 && static_cast<std::size_t>(w) < sizeof(out) && dir[0];
}

} // namespace

std::string_view lx::ui::icon_name(icon i) {
    const auto n = static_cast<unsigned>(i);
    return n < k_icon_count ? k_icon_names[n] : std::string_view{};
}

const char* lx::ui::icon_dir() {
    static char dir[512]{};
    static bool resolved = false;
    if (resolved)
        return dir;
    resolved = true;
    if (const char* env = ::getenv("LUMEN_ICON_DIR"); env && env[0]) {
        lx_ui_icon_copy(dir, sizeof(dir), env);
        return dir;
    }
#if defined(LUMEN_ICON_DIR_BUILD)
    if (lx_ui_icon_is_dir(LUMEN_ICON_DIR_BUILD)) {
        lx_ui_icon_copy(dir, sizeof(dir), LUMEN_ICON_DIR_BUILD);
        return dir;
    }
#endif
#if defined(LUMEN_ICON_DIR_INSTALL_RELATIVE)
    {
        char exe[512]{};
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) {
            exe[n] = '\0';
            for (int cut = 0; cut < 2; ++cut)
                if (char* slash = std::strrchr(exe, '/'))
                    *slash = '\0';
            char rel[512]{};
            const int w = std::snprintf(rel, sizeof(rel), "%s/%s", exe,
                                        LUMEN_ICON_DIR_INSTALL_RELATIVE);
            if (w > 0 && static_cast<std::size_t>(w) < sizeof(rel) && lx_ui_icon_is_dir(rel)) {
                lx_ui_icon_copy(dir, sizeof(dir), rel);
                return dir;
            }
        }
    }
#endif
#if defined(LUMEN_ICON_DIR_INSTALL)
    if (lx_ui_icon_is_dir(LUMEN_ICON_DIR_INSTALL))
        lx_ui_icon_copy(dir, sizeof(dir), LUMEN_ICON_DIR_INSTALL);
#endif
    return dir;
}

// ── icon_cache ──

struct lx::ui::icon_cache::async_job {
    char path[768]{};
    std::uint8_t coverage[k_max_px * k_max_px]{};
    lx::result<void> outcome{};
    async_state* state = nullptr;
    icon id = icon::count;
    std::uint16_t size = 0;
    bool busy = false;
};

/// The jobs and who owns them: the cache, and each job in flight (a cache destroyed while a
/// job runs leaves the block to the last landing). UI affinity, except `run_job`'s raster.
struct lx::ui::icon_cache::async_state {
    async_job jobs[k_async_jobs]{};
    icon_cache* owner = nullptr;
    lx::runtime::executor* executor = nullptr;
    unsigned in_flight = 0;
};

lx::ui::icon_cache::icon_cache(lx::gfx::atlas& atlas, const char* dir) : atlas_{&atlas} {
    lx_ui_icon_copy(dir_, sizeof(dir_), dir ? dir : icon_dir());
}

lx::ui::icon_cache::~icon_cache() {
    if (!async_)
        return;
    if (async_->in_flight == 0)
        delete async_;
    else
        async_->owner = nullptr; // the last landing frees it
}

void lx::ui::icon_cache::set_async(lx::runtime::executor* executor) {
    executor_ = executor;
    if (executor && !async_) {
        async_ = new async_state{};
        async_->owner = this;
    }
    if (async_)
        async_->executor = executor;
}

lx::ui::icon_cache::entry* lx::ui::icon_cache::find_or_claim(icon id, unsigned size) {
    const unsigned h = (static_cast<unsigned>(id) * 2654435761u ^ size * 40503u) % k_capacity;
    entry* victim = nullptr;
    for (unsigned probe = 0; probe < k_capacity; ++probe) {
        entry& e = entries_[(h + probe) % k_capacity];
        if (e.state != entry_state::empty && e.id == id && e.size == size)
            return &e;
        if (e.state == entry_state::empty) {
            if (!victim)
                victim = &e;
            break; // linear probing never leaves a hole: a miss
        }
    }
    if (!victim) {
        // Full: the least recently used entry not in flight.
        for (entry& e : entries_)
            if (e.state != entry_state::pending && (!victim || e.stamp < victim->stamp))
                victim = &e;
        if (!victim)
            return nullptr;
        if (victim->state == entry_state::ready)
            atlas_->release(victim->tile);
    }
    *victim = entry{};
    victim->id = id;
    victim->size = static_cast<std::uint16_t>(size);
    return victim;
}

lx::result<lx::gfx::atlas_tile_id> lx::ui::icon_cache::raster_now(entry& e) {
    char path[768];
    if (!lx_ui_icon_path(path, dir_, e.id))
        return lx::make_error(lx::ui_err::icon_raster, "icon_cache: no icon directory");
    alignas(16) std::uint8_t coverage[k_max_px * k_max_px];
    if (auto r = rasterize_icon(path, e.size, coverage); !r)
        return r.get_error();
    return atlas_->insert(lx::gfx::atlas_format::r8, e.size, e.size,
                          {coverage, std::size_t{e.size} * e.size}, e.size);
}

lx::result<lx::gfx::atlas_tile_id> lx::ui::icon_cache::get(icon id, unsigned size_px) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    if (static_cast<unsigned>(id) >= k_icon_count || size_px == 0 || size_px > k_max_px)
        return lx::invalid_argument("icon_cache: bad icon or size");
    entry* e = find_or_claim(id, size_px);
    if (!e)
        return lx::make_error(lx::ui_err::icon_raster, "icon_cache: table full of pending rasters");
    lx::gfx::atlas_tile tile{};
    if (e->state == entry_state::ready && atlas_->lookup(e->tile, tile))
        return e->tile;
    const bool evicted = e->state == entry_state::ready;
    auto r = raster_now(*e);
    e->stamp = atlas_->frame();
    if (!r) {
        e->state = entry_state::failed;
        ++counters_.failures;
        return r.get_error();
    }
    e->state = entry_state::ready;
    e->tile = *r;
    ++counters_.rasterized;
    if (evicted)
        ++counters_.rerasterized;
    return *r;
}

bool lx::ui::icon_cache::place(icon id, unsigned size_px, lx::gfx::atlas_tile& out,
                               lx::rect2i pending_damage) {
    if (static_cast<unsigned>(id) >= k_icon_count || size_px == 0 || size_px > k_max_px)
        return false;
    entry* e = find_or_claim(id, size_px);
    if (!e)
        return false;
    const std::uint64_t frame = atlas_->frame();
    switch (e->state) {
    case entry_state::ready:
        if (atlas_->use(e->tile, out)) {
            e->stamp = frame;
            ++counters_.hits;
            return true;
        }
        break; // evicted: rasterise again
    case entry_state::failed:
        if (frame - e->stamp < lx::gfx::k_atlas_stale_frames)
            return false;
        break;
    case entry_state::pending:
        e->damage = e->damage.width > 0 ? lx::rect2i{std::min(e->damage.x, pending_damage.x),
                                                     std::min(e->damage.y, pending_damage.y),
                                                     std::max(e->damage.x + e->damage.width,
                                                              pending_damage.x + pending_damage.width) -
                                                         std::min(e->damage.x, pending_damage.x),
                                                     std::max(e->damage.y + e->damage.height,
                                                              pending_damage.y + pending_damage.height) -
                                                         std::min(e->damage.y, pending_damage.y)}
                                        : pending_damage;
        return false;
    case entry_state::empty: break;
    }
    ++counters_.misses;
    if (executor_ && async_) {
        e->damage = pending_damage;
        post(*e);
        return false;
    }
    if (!get(id, size_px))
        return false;
    return atlas_->use(e->tile, out);
}

void lx::ui::icon_cache::post(entry& e) {
    async_job* free_job = nullptr;
    for (async_job& j : async_->jobs)
        if (!j.busy) {
            free_job = &j;
            break;
        }
    if (!free_job || !lx_ui_icon_path(free_job->path, dir_, e.id)) {
        // Every job busy: asked again on the next frame that draws it.
        e.state = e.state == entry_state::ready ? entry_state::empty : e.state;
        return;
    }
    async_job& j = *free_job;
    j.state = async_;
    j.id = e.id;
    j.size = e.size;
    j.busy = true;
    e.state = entry_state::pending;
    if (!async_->executor->post(lx::runtime::affinity::worker, lx::runtime::task{&icon_cache::run_job, &j})) {
        j.busy = false;
        e.state = entry_state::empty;
        return;
    }
    ++async_->in_flight;
    ++counters_.async_posted;
}

void lx::ui::icon_cache::run_job(void* raw) {
    lx::runtime::assert_affinity(lx::runtime::affinity::worker);
    auto* j = static_cast<async_job*>(raw);
    j->outcome = rasterize_icon(j->path, j->size, j->coverage);
    const lx::runtime::task back{&icon_cache::land_job, j};
    for (unsigned attempt = 0; !j->state->executor->post(lx::runtime::affinity::ui, back); ++attempt) {
        if (attempt == 100000) {
            lx::trace::logger::global().log(lx::trace::level::error, "ui.icon",
                                            "icon_cache: UI strand full, raster lost");
            return;
        }
        std::this_thread::yield();
    }
}

void lx::ui::icon_cache::land_job(void* raw) {
    lx::runtime::assert_affinity(lx::runtime::affinity::ui);
    auto* j = static_cast<async_job*>(raw);
    async_state* st = j->state;
    --st->in_flight;
    j->busy = false;
    if (!st->owner) {
        if (st->in_flight == 0)
            delete st;
        return;
    }
    st->owner->land(*j);
}

void lx::ui::icon_cache::land(async_job& j) {
    ++counters_.async_landed;
    entry* e = nullptr;
    for (entry& c : entries_)
        if (c.state == entry_state::pending && c.id == j.id && c.size == j.size)
            e = &c;
    if (!e)
        return;
    e->stamp = atlas_->frame();
    if (!j.outcome) {
        e->state = entry_state::failed;
        ++counters_.failures;
        return;
    }
    auto tile = atlas_->insert(lx::gfx::atlas_format::r8, j.size, j.size,
                               {j.coverage, std::size_t{j.size} * j.size}, j.size);
    if (!tile) {
        e->state = entry_state::failed;
        ++counters_.failures;
        return;
    }
    e->state = entry_state::ready;
    e->tile = *tile;
    ++counters_.rasterized;
    damage_ledger& ledger = ui_damage();
    if (e->damage.width > 0 && e->damage.height > 0)
        ledger.add(e->damage);
    ledger.request(invalidation::paint);
    e->damage = {};
}

// ── icon_node ──

lx::ui::element_type_id lx::ui::icon_node::static_type() {
    static const element_type_id id = next_element_type_id();
    return id;
}
lx::ui::element_type_id lx::ui::icon_node::type() const {
    return static_type();
}
const char* lx::ui::icon_node::type_name() const {
    return "icon";
}

bool lx::ui::icon_node::apply(const icon_props& props) {
    const float scale = props.scale > 0.f ? props.scale : 1.f;
    const auto px = static_cast<unsigned>(std::clamp<long>(std::lround(props.size_px * scale), 1,
                                                           icon_cache::k_max_px));
    const bool resized = px != device_px_;
    const std::uint64_t generation = theme_generation();
    if (resized || props.id != id_ || props.color != token_ || generation != generation_ ||
        props.cache != cache_)
        invalidate_paint(*this);
    id_ = props.id;
    device_px_ = px;
    cache_ = props.cache;
    token_ = props.color;
    generation_ = generation;
    color_ = active_theme().color(props.color);
    return resized;
}

lx::size2i lx::ui::icon_node::measure(lx::layout::constraints c, size_hint) const {
    const int s = static_cast<int>(device_px_);
    return {std::clamp(s, c.min.width, std::max(c.min.width, c.max.width)),
            std::clamp(s, c.min.height, std::max(c.min.height, c.max.height))};
}

void lx::ui::icon_node::emit(draw_scope& out) const {
    if (!visible_ || !cache_ || color_.a <= 0.f)
        return;
    const int s = static_cast<int>(device_px_);
    const lx::rect2i at{bounds_.x + (bounds_.width - s) / 2, bounds_.y + (bounds_.height - s) / 2, s, s};
    // Outside the clip (a frame's damage): no cache lookup, no atlas stamp.
    if (out.culled(at))
        return;
    lx::gfx::atlas_tile tile{};
    if (!cache_->place(id_, device_px_, tile, surface_rect(*this, bounds_)))
        return;
    out.sprite(lx::gfx::primitive_kind::mono_sprite, tile.page, at, tile.uv(), color_);
}

lx::ui::child lx::ui::icon_view(icon id, icon_cache* cache, float size_px, tok color, float scale) {
    return describe<icon_node, icon_props>({id, size_px, scale, color, cache});
}
