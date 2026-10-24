module;

import lx.foundation;
import lx.gfx;
import lx.scene;
import lx.trace;
import lx.wayland.server;

export module lx.compositor:surface;

import std;

export namespace lx::compositor {

/// What a surface shows (wp_content_type_v1, plus the compositor's own subsurface UI hint):
/// scanout priority once direct scanout lands, and the connector's `content type` for the
/// fullscreen window of an output whose connector has that property.
enum class content_hint { none, photo, video, game, subsurface_ui };

/// How a client buffer's storage reaches the compositor. shm is not a dmabuf and must
/// not be pushed through the dmabuf import path.
enum class buffer_kind { none, dmabuf, shm };

/// A client's damage: a few rects, merged by the output region's rule (25 % waste). A
/// single bounding box turns a terminal's cursor line and status line into the whole window
/// between them — uploaded and repainted on every keystroke. Four rects hold the shapes
/// clients send; more merge into the cheapest pair.
using client_damage = lx::gfx::basic_damage_region<4>;

struct surface_commit {
    lx::surface_id surface{};
    lx::buffer_id buffer{};
    lx::client_id client{};
    /// Surface-local (logical) damage of this update, already merged from wl_surface.damage
    /// and the converted damage_buffer. Empty = no content damage.
    lx::rect2i damage{};
    /// The same damage as separate rects inside `damage` (its bounds), when the client sent
    /// more than one; empty: `damage` alone.
    client_damage damage_rects{};
    /// Logical surface size (buffer scale, transform and viewport applied).
    lx::size2i surface_size{};
    /// Buffer box shown (empty: all of it) and the buffer transform (`:viewport`).
    lx::rect2f src{};
    lx::buffer_transform transform = lx::buffer_transform::normal;
    bool fullscreen = false;
    /// The content has no translucent pixel: a format without alpha, or an opaque region
    /// covering the buffer. False (blend) unless the protocol layer proves it.
    bool opaque = false;
};

/// A wl_surface input region: the exact rect set from wl_region add/subtract, in fixed
/// storage so neither the protocol requests nor hit-testing allocate. Rects may overlap;
/// `contains` asks whether any covers the point.
///
/// Both overflow paths over-accept input — `add` collapses to the bounding box, `subtract`
/// stops cutting — because a surface that receives a click meant for the window below is a
/// lesser failure than a window that cannot be clicked at all.
struct input_region {
    static constexpr unsigned k_max = 16;
    lx::rect2i rects[k_max]{};
    unsigned count = 0;
    /// The wl_surface default: every point of the surface accepts input.
    bool infinite = true;
    /// Set when an operation had to over-approximate (overflow).
    bool approximated = false;

    /// wl_region starts empty.
    void reset_empty() {
        count = 0;
        infinite = false;
        approximated = false;
    }
    void add(lx::rect2i r);
    void subtract(lx::rect2i r);
    [[nodiscard]] bool contains(int x, int y) const;
};

/// Open-addressing map from a nonzero id to a table slot, in fixed storage: linear
/// probing at a load factor of at most one half, and backward-shift deletion, so lookups
/// stay O(1) however much the table churns (no tombstones). The hit test runs one lookup
/// per surface per pointer motion; a linear scan of the table there is not affordable.
template<unsigned Buckets>
class id_slot_index {
    static_assert(Buckets != 0 && (Buckets & (Buckets - 1)) == 0, "power of two");

public:
    [[nodiscard]] int find(unsigned long long id) const {
        if (id == 0)
            return -1;
        for (unsigned i = home(id), n = 0; n < Buckets; ++n, i = (i + 1) & k_mask) {
            if (keys_[i] == id)
                return slots_[i];
            if (keys_[i] == 0)
                return -1;
        }
        return -1;
    }
    /// False when `id` is 0 or the index is full. An existing id is updated in place.
    bool insert(unsigned long long id, int slot) {
        if (id == 0)
            return false;
        for (unsigned i = home(id), n = 0; n < Buckets; ++n, i = (i + 1) & k_mask) {
            if (keys_[i] == id || keys_[i] == 0) {
                keys_[i] = id;
                slots_[i] = slot;
                return true;
            }
        }
        return false;
    }
    void erase(unsigned long long id) {
        if (id == 0)
            return;
        unsigned i = home(id);
        unsigned n = 0;
        for (; n < Buckets; ++n, i = (i + 1) & k_mask) {
            if (keys_[i] == id)
                break;
            if (keys_[i] == 0)
                return;
        }
        if (n == Buckets)
            return;
        // Backward shift: pull every later member of the probe run whose home does not lie
        // cyclically in (hole, j] into the hole, so no lookup ever stops short of its key.
        unsigned hole = i;
        for (unsigned j = (hole + 1) & k_mask; keys_[j] != 0; j = (j + 1) & k_mask) {
            const unsigned h = home(keys_[j]);
            const bool stays = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (stays)
                continue;
            keys_[hole] = keys_[j];
            slots_[hole] = slots_[j];
            hole = j;
        }
        keys_[hole] = 0;
        slots_[hole] = -1;
    }

private:
    static constexpr unsigned k_mask = Buckets - 1;
    [[nodiscard]] static unsigned home(unsigned long long id) {
        return static_cast<unsigned>((id * 0x9E3779B97F4A7C15ull) >> 40) & k_mask;
    }
    unsigned long long keys_[Buckets]{};
    int slots_[Buckets]{};
};

/// Client surfaces' content and their scene nodes. UI affinity.
///
/// A commit records content; it does not decide visibility. Only a mapped surface (part of
/// a mapped root's surface tree) gets a node, through `ensure_node`, and `surface_tree`
/// owns where it sits — this class never moves or restacks a node on its own.
class surface_manager {
public:
    static constexpr unsigned k_max_surfaces = 256;

    void set_import_cache(lx::gfx::dmabuf_import_cache* cache);
    /// The composite backend imports dma-bufs itself, on the render thread (GL: an
    /// EGLImage per commit). Then a dma-buf gets a stable compositor texture id like an shm
    /// buffer, and the UI-side (Vulkan) import cache is not used — on a host whose only
    /// Vulkan device is a software rasterizer that import cannot succeed at all.
    void set_render_thread_import(bool enabled) { render_thread_import_ = enabled; }
    /// Damage goes to `graph`. Nodes live in whatever container `ensure_node` names.
    void set_scene(lx::scene::scene_graph* graph);

    /// wl_compositor.create_surface: makes the entry and remembers its `wl_resource*`
    /// (void*: no Wayland types here). False when the table is full.
    [[nodiscard]] bool register_surface(lx::surface_id surface, void* resource);
    /// The surface's `wl_resource*`, or null.
    [[nodiscard]] void* resource(lx::surface_id surface) const;

    /// Register a dmabuf description for a buffer (client attach path). Takes ownership
    /// of the plane FDs in `desc`.
    void register_dmabuf(lx::buffer_id buffer, lx::gfx::dmabuf_desc desc);

    /// Register a shared-memory buffer. Its pixels are uploaded by the render backend
    /// against the texture id returned by `texture_for`, not imported as a dmabuf.
    void register_shm(lx::buffer_id buffer, unsigned width, unsigned height,
                      lx::fourcc format);

    /// Stable texture id assigned to a registered shm buffer, or 0.
    [[nodiscard]] unsigned texture_for(lx::buffer_id buffer) const;
    /// The wl_buffer object is gone: frees its record (and any plane fds it holds). Content
    /// a surface already shows is unaffected — it lives in the surface entry and texture.
    /// Without this the table filled after 256 buffers and every later commit failed.
    void forget_buffer(lx::buffer_id buffer);

    [[nodiscard]] lx::result<void> on_commit(surface_commit commit);

    /// Metadata-only copy for callers that must not take plane FD ownership. The import
    /// path must not use this — an FD-less desc cannot be imported.
    [[nodiscard]] lx::gfx::dmabuf_desc extract_dmabuf_desc(lx::buffer_id buffer) const;
    void attach_imported(lx::surface_id surface, lx::gfx::imported_image image);
    void destroy(lx::surface_id surface);
    void set_override_redirect(lx::surface_id surface, bool value);
    void set_content_hint(lx::surface_id surface, content_hint hint);
    /// The content is one colour (wp_single_pixel_buffer_v1): no texture, no upload — the
    /// node draws `color` (premultiplied, encoded) over its logical `size`. Replaces any
    /// image; the next `on_commit` of a real buffer replaces it. Damages the node.
    void set_solid(lx::surface_id surface, lx::color color, lx::size2i size, bool opaque);
    /// wp_alpha_modifier_v1: the node's opacity (1 = none). Damages the node when it changes;
    /// kept for a node created later.
    void set_alpha(lx::surface_id surface, float alpha);
    [[nodiscard]] float alpha(lx::surface_id surface) const;

    [[nodiscard]] lx::gfx::imported_image lookup(lx::surface_id surface) const;
    [[nodiscard]] content_hint content_hint_for(lx::surface_id surface) const;

    /// Size of the last successfully committed buffer, in pixels; empty before the first
    /// one. Texture-side only — geometry uses `surface_size`.
    [[nodiscard]] lx::size2i buffer_size(lx::surface_id surface) const;
    /// Logical size of the surface's content (buffer scale, transform, viewport applied):
    /// its extents in the layout, what roles, the tree and hit-testing use.
    [[nodiscard]] lx::size2i surface_size(lx::surface_id surface) const;
    /// A state-only update changed the geometry (a new viewport, scale or transform on the
    /// buffer already shown): bounds and source follow, damaged like a resize.
    void set_geometry(lx::surface_id surface, lx::size2i size, lx::rect2f src,
                      lx::buffer_transform transform);
    /// Creates the surface's scene node (bounds = surface size) at the top of `parent`.
    /// True if the node exists afterwards. A full container fails (warned once).
    [[nodiscard]] bool ensure_node(lx::surface_id surface, lx::scene::container_node& parent);
    /// Removes the node from its container and deletes it. Notes no damage — the caller
    /// knows what it unmapped.
    void drop_node(lx::surface_id surface);
    /// Content opacity (see `surface_commit::opaque`); pushed into the node, damaging it
    /// when the blend changes.
    void set_opaque(lx::surface_id surface, bool opaque);
    /// Something under a still pointer may have changed: a node appeared, went away,
    /// moved or changed size. The window manager re-hit-tests once per tick on it.
    void note_layout_changed() { layout_changed_ = true; }
    [[nodiscard]] bool layout_changed() const { return layout_changed_; }
    [[nodiscard]] bool consume_layout_changed() {
        const bool changed = layout_changed_;
        layout_changed_ = false;
        return changed;
    }
    [[nodiscard]] lx::scene::surface_node* node(lx::surface_id surface) const;
    /// Global (logical) `{origin, surface extents}` of the node, or empty when there is none.
    [[nodiscard]] lx::rect2i node_rect(lx::surface_id surface) const;
    /// Moves the node. Keeps scale and opacity, notes no damage: the window manager damages
    /// old and new rects as separate clips, which this cannot know to do.
    void set_origin(lx::surface_id surface, lx::point2i origin);

    /// Committed input region (wl_surface.set_input_region). Default: infinite.
    void set_input_region(lx::surface_id surface, const input_region& region);
    /// Hit test for one surface: true when the global point lies inside the node's extents
    /// and its input region; `sx`,`sy` receive the surface-local position. One slot lookup,
    /// no allocation — the window manager runs this per mapped window on every motion.
    [[nodiscard]] bool hit(lx::surface_id surface, double x, double y, double& sx,
                           double& sy) const;

private:
    static constexpr unsigned k_max_buffers = 256;

    struct surface_entry {
        lx::surface_id id{};
        void* resource = nullptr;
        lx::buffer_id buffer{};
        lx::gfx::imported_image image{};
        lx::size2i buffer_size{};
        lx::size2i surface_size{};
        lx::rect2f src{};
        lx::buffer_transform transform = lx::buffer_transform::normal;
        content_hint hint = content_hint::none;
        /// Solid colour content (`set_solid`) instead of `image`.
        bool solid = false;
        lx::color solid_color = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
        float alpha = 1.f;
        lx::scene::surface_node* node = nullptr;
        /// The container `node` was added to.
        lx::scene::container_node* node_parent = nullptr;
        input_region input{};
        bool opaque = false;
        bool override_redirect = false;
        bool used = false;
    };

    struct buffer_entry {
        lx::buffer_id id{};
        lx::gfx::dmabuf_desc desc{};
        buffer_kind kind = buffer_kind::none;
        unsigned texture_id = 0;
        bool used = false;
    };

    [[nodiscard]] int find_surface(lx::surface_id id) const;
    [[nodiscard]] int find_or_alloc_surface(lx::surface_id id);
    [[nodiscard]] int find_buffer(lx::buffer_id id) const;
    /// Output pixels a content change of `e` may reach beyond its own rect: a resampled
    /// draw's linear filter extends half a source texel (in output pixels) past it.
    [[nodiscard]] int resample_margin(const surface_entry& e) const;
    /// Damage of `e`'s content: every view, or — while its window's tree container is not
    /// shown (another workspace, minimized) — only the capture views that may still draw it,
    /// so a hidden window's commits repaint no output (design-session-and-wm W.3).
    void note_content_damage(const surface_entry& e, lx::rect2i region, int margin) const;

    lx::gfx::dmabuf_import_cache* import_cache_ = nullptr;
    lx::scene::scene_graph* scene_ = nullptr;
    /// Allocated 16 entries at a time as surfaces appear (`lx::sparse_slots`).
    lx::sparse_slots<surface_entry, k_max_surfaces> surfaces_{};
    id_slot_index<k_max_surfaces * 2> index_{};
    bool layout_changed_ = false;
    bool warned_container_full_ = false;
    bool render_thread_import_ = false;
    lx::sparse_slots<buffer_entry, k_max_buffers> buffers_{};
    /// Compositor-assigned texture ids (shm, render-thread dma-buf) live in a space disjoint
    /// from import-cache ids.
    unsigned next_shm_texture_id_ = 0x4000'0000u;
};

} // namespace lx::compositor


void lx::compositor::surface_manager::set_import_cache(lx::gfx::dmabuf_import_cache* cache) {
    import_cache_ = cache;
}

void lx::compositor::surface_manager::set_scene(lx::scene::scene_graph* graph) {
    scene_ = graph;
}

bool lx::compositor::surface_manager::register_surface(lx::surface_id surface, void* resource) {
    const int slot = find_or_alloc_surface(surface);
    if (slot < 0)
        return false;
    surfaces_[static_cast<unsigned>(slot)].resource = resource;
    return true;
}

void* lx::compositor::surface_manager::resource(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].resource;
    return nullptr;
}

void lx::compositor::surface_manager::register_dmabuf(lx::buffer_id buffer,
                                                      lx::gfx::dmabuf_desc desc) {
    if (!buffer)
        return;
    if (const int existing = find_buffer(buffer); existing >= 0) {
        auto& entry = buffers_[static_cast<unsigned>(existing)];
        entry.desc = std::move(desc);
        entry.kind = buffer_kind::dmabuf;
        if (render_thread_import_ && entry.texture_id == 0)
            entry.texture_id = next_shm_texture_id_++;
        return;
    }
    for (unsigned i = 0; i < k_max_buffers; ++i) {
        if (buffers_[i].used)
            continue;
        buffers_[i].used = true;
        buffers_[i].id = buffer;
        buffers_[i].desc = std::move(desc);
        buffers_[i].kind = buffer_kind::dmabuf;
        if (render_thread_import_)
            buffers_[i].texture_id = next_shm_texture_id_++;
        return;
    }
}

void lx::compositor::surface_manager::register_shm(lx::buffer_id buffer, unsigned width,
                                                   unsigned height, lx::fourcc format) {
    if (!buffer || width == 0 || height == 0)
        return;

    int slot = find_buffer(buffer);
    if (slot < 0) {
        for (unsigned i = 0; i < k_max_buffers; ++i) {
            if (buffers_[i].used)
                continue;
            buffers_[i].used = true;
            buffers_[i].id = buffer;
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0)
        return;

    auto& entry = buffers_[static_cast<unsigned>(slot)];
    entry.kind = buffer_kind::shm;
    entry.desc = {};
    entry.desc.width = width;
    entry.desc.height = height;
    entry.desc.format = format;
    if (entry.texture_id == 0)
        entry.texture_id = next_shm_texture_id_++;
}

unsigned lx::compositor::surface_manager::texture_for(lx::buffer_id buffer) const {
    if (const int slot = find_buffer(buffer); slot >= 0)
        return buffers_[static_cast<unsigned>(slot)].texture_id;
    return 0;
}

void lx::compositor::surface_manager::forget_buffer(lx::buffer_id buffer) {
    if (const int slot = find_buffer(buffer); slot >= 0)
        buffers_[static_cast<unsigned>(slot)] = {};
}

int lx::compositor::surface_manager::find_surface(lx::surface_id id) const {
    return index_.find(id.id());
}

int lx::compositor::surface_manager::find_or_alloc_surface(lx::surface_id id) {
    if (!id)
        return -1;
    if (const int existing = find_surface(id); existing >= 0)
        return existing;
    for (unsigned i = 0; i < k_max_surfaces; ++i) {
        if (surfaces_[i].used)
            continue;
        if (!index_.insert(id.id(), static_cast<int>(i)))
            return -1;
        surfaces_[i] = {};
        surfaces_[i].used = true;
        surfaces_[i].id = id;
        return static_cast<int>(i);
    }
    return -1;
}

int lx::compositor::surface_manager::find_buffer(lx::buffer_id id) const {
    for (unsigned i = 0; i < k_max_buffers; ++i)
        if (buffers_[i].used && buffers_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

lx::result<void> lx::compositor::surface_manager::on_commit(surface_commit commit) {
    if (!commit.surface) {
        return lx::invalid_argument("null surface on commit");
    }

    const int buffer_slot = find_buffer(commit.buffer);
    if (buffer_slot < 0 || buffers_[static_cast<unsigned>(buffer_slot)].kind == buffer_kind::none) {
        // Buffer-less commit is valid Wayland (state-only). Do not invent a placeholder
        // buffer — that masked missing protocol wiring as a successful import.
        if (!commit.buffer)
            return {};
        return lx::make_error(lx::wayland_err::protocol_violation,
                              "surface commit with unknown buffer");
    }

    // Import from the registered buffer itself, not a metadata copy: dmabuf_desc owns its
    // plane FDs, and a copy leaves them at -1 so nothing would ever reach Vulkan.
    const auto& buffer_entry_ref = buffers_[static_cast<unsigned>(buffer_slot)];
    const lx::gfx::dmabuf_desc& desc = buffer_entry_ref.desc;
    if (!desc.width || !desc.height) {
        return lx::make_error(lx::wayland_err::protocol_violation,
                              "surface commit with zero-sized buffer");
    }

    lx::gfx::imported_image image{};
    if (buffer_entry_ref.kind == buffer_kind::shm ||
        (render_thread_import_ && buffer_entry_ref.texture_id != 0)) {
        // shm pixels are uploaded, and a render-thread dma-buf is imported, by the render
        // backend against a stable texture id; nothing is imported here.
        image.image_id = buffer_entry_ref.texture_id;
        image.memory_id = image.image_id;
        image.width = desc.width;
        image.height = desc.height;
        image.format = desc.format;
    } else if (import_cache_) {
        if (!desc.plane_count) {
            return lx::make_error(lx::wayland_err::protocol_violation,
                                  "dmabuf buffer with no planes");
        }
        lx::gfx::import_cache_key key{};
        key.client = commit.client ? commit.client
                                   : lx::client_id{static_cast<lx::client_id::id_type>(commit.surface.id())};
        key.buffer = commit.buffer ? commit.buffer
                                   : lx::buffer_id{static_cast<lx::buffer_id::id_type>(commit.surface.id())};
        key.modifier = desc.planes[0].modifier;
        auto acquired = import_cache_->acquire(key, desc);
        if (!acquired)
            return acquired.get_error();
        image = static_cast<decltype(acquired)&&>(acquired).value();
    } else {
        lx::gfx::dmabuf_importer soft{};
        auto imported = soft.import(desc);
        if (!imported)
            return imported.get_error();
        image = static_cast<decltype(imported)&&>(imported).value();
    }

    attach_imported(commit.surface, image);

    const int slot = find_or_alloc_surface(commit.surface);
    if (slot < 0) {
        return lx::make_error(lx::error_domain::wayland, 0, "surface table full");
    }
    auto& entry = surfaces_[static_cast<unsigned>(slot)];
    const lx::size2i old_size = entry.surface_size;
    const int old_margin = resample_margin(entry);
    entry.buffer = commit.buffer;
    entry.image = image;
    const bool was_solid = entry.solid;
    entry.solid = false;
    entry.buffer_size = {static_cast<int>(desc.width), static_cast<int>(desc.height)};
    entry.surface_size = commit.surface_size.empty() ? entry.buffer_size : commit.surface_size;
    const bool source_changed = entry.src.x != commit.src.x || entry.src.y != commit.src.y ||
                                entry.src.width != commit.src.width ||
                                entry.src.height != commit.src.height ||
                                entry.transform != commit.transform;
    entry.src = commit.src;
    entry.transform = commit.transform;
    const bool opacity_changed = entry.opaque != commit.opaque;
    entry.opaque = commit.opaque;

    // Only a mapped surface has a node. Its bounds are the surface (logical) — never the
    // damage rect, which for a partial redraw is a fraction of the window — and its damage
    // is the client's surface-local rect moved to where the node actually is.
    if (entry.node) {
        const lx::point2i o = entry.node->origin();
        entry.node->attach(image);
        if (was_solid)
            entry.node->set_solid(false, {});
        entry.node->set_opaque(entry.opaque);
        entry.node->set_bounds({0, 0, entry.surface_size.width, entry.surface_size.height});
        entry.node->set_content(entry.src, entry.transform);
        if (scene_) {
            const int margin = resample_margin(entry);
            if (old_size.width != entry.surface_size.width ||
                old_size.height != entry.surface_size.height || opacity_changed ||
                source_changed || was_solid) {
                // A size change uncovers or covers what lies outside the other extent, and a
                // blend change repaints everything under the node: both rects, separately.
                note_content_damage(entry, {o.x, o.y, old_size.width, old_size.height}, old_margin);
                note_content_damage(
                    entry, {o.x, o.y, entry.surface_size.width, entry.surface_size.height}, margin);
                layout_changed_ = true;
            } else if (!commit.damage_rects.empty()) {
                for (unsigned i = 0; i < commit.damage_rects.count; ++i) {
                    const lx::rect2i& d = commit.damage_rects.rects[i];
                    note_content_damage(entry, {d.x + o.x, d.y + o.y, d.width, d.height}, margin);
                }
            } else if (commit.damage.width > 0 && commit.damage.height > 0) {
                note_content_damage(entry,
                                    {commit.damage.x + o.x, commit.damage.y + o.y,
                                     commit.damage.width, commit.damage.height},
                                    margin);
            }
        }
    }

    return {};
}

lx::gfx::dmabuf_desc lx::compositor::surface_manager::extract_dmabuf_desc(lx::buffer_id buffer) const {
    // Metadata-only copy — plane FDs stay with the registered buffer entry (move-only).
    lx::gfx::dmabuf_desc out{};
    if (const int slot = find_buffer(buffer); slot >= 0) {
        const auto& src = buffers_[static_cast<unsigned>(slot)].desc;
        out.width = src.width;
        out.height = src.height;
        out.format = src.format;
        out.plane_count = src.plane_count;
        for (unsigned i = 0; i < src.plane_count && i < 4; ++i) {
            out.planes[i].offset = src.planes[i].offset;
            out.planes[i].stride = src.planes[i].stride;
            out.planes[i].modifier = src.planes[i].modifier;
        }
    }
    return out;
}

void lx::compositor::surface_manager::attach_imported(lx::surface_id surface,
                                                      lx::gfx::imported_image image) {
    const int slot = find_or_alloc_surface(surface);
    if (slot < 0)
        return;
    surfaces_[static_cast<unsigned>(slot)].image = image;
    if (surfaces_[static_cast<unsigned>(slot)].node)
        surfaces_[static_cast<unsigned>(slot)].node->attach(image);
}

void lx::compositor::surface_manager::destroy(lx::surface_id surface) {
    const int slot = find_surface(surface);
    if (slot < 0)
        return;
    auto& entry = surfaces_[static_cast<unsigned>(slot)];
    if (entry.node) {
        if (scene_ && entry.node->is_visible()) {
            const lx::point2i o = entry.node->origin();
            const lx::rect2i b = entry.node->bounds();
            note_content_damage(entry, {o.x, o.y, b.width, b.height}, resample_margin(entry));
        }
        if (entry.node_parent)
            (void)entry.node_parent->remove(entry.node);
        delete entry.node;
        layout_changed_ = true;
    }
    index_.erase(surface.id());
    entry = {};
}

lx::size2i lx::compositor::surface_manager::buffer_size(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].buffer_size;
    return {};
}

lx::size2i lx::compositor::surface_manager::surface_size(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].surface_size;
    return {};
}

int lx::compositor::surface_manager::resample_margin(const surface_entry& e) const {
    const double s = scene_ ? scene_->projection().scale : 1.0;
    double sw = e.src.empty() ? e.buffer_size.width : e.src.width;
    double sh = e.src.empty() ? e.buffer_size.height : e.src.height;
    if ((static_cast<unsigned>(e.transform) & 1u) != 0) {
        const double t = sw;
        sw = sh;
        sh = t;
    }
    if (!(sw > 0.0) || !(sh > 0.0) || e.surface_size.empty())
        return 0;
    const double kx = s * e.surface_size.width / sw;
    const double ky = s * e.surface_size.height / sh;
    const bool whole = e.src.x == std::floor(e.src.x) && e.src.y == std::floor(e.src.y);
    if (kx == 1.0 && ky == 1.0 && whole)
        return 0;
    const double k = kx > ky ? kx : ky;
    const int m = static_cast<int>(std::ceil((k > 1.0 ? k : 1.0) / 2.0));
    return m < 1 ? 1 : m;
}

void lx::compositor::surface_manager::set_geometry(lx::surface_id surface, lx::size2i size,
                                                   lx::rect2f src,
                                                   lx::buffer_transform transform) {
    const int slot = find_surface(surface);
    if (slot < 0)
        return;
    auto& e = surfaces_[static_cast<unsigned>(slot)];
    if (size.width == e.surface_size.width && size.height == e.surface_size.height &&
        src.x == e.src.x && src.y == e.src.y && src.width == e.src.width &&
        src.height == e.src.height && transform == e.transform)
        return;
    const lx::size2i old_size = e.surface_size;
    const int old_margin = resample_margin(e);
    e.surface_size = size;
    e.src = src;
    e.transform = transform;
    if (!e.node)
        return;
    const lx::point2i o = e.node->origin();
    e.node->set_bounds({0, 0, size.width, size.height});
    e.node->set_content(src, transform);
    if (scene_) {
        note_content_damage(e, {o.x, o.y, old_size.width, old_size.height}, old_margin);
        note_content_damage(e, {o.x, o.y, size.width, size.height}, resample_margin(e));
    }
    layout_changed_ = true;
}

bool lx::compositor::surface_manager::ensure_node(lx::surface_id surface,
                                                  lx::scene::container_node& parent) {
    const int slot = find_surface(surface);
    if (slot < 0)
        return false;
    auto& entry = surfaces_[static_cast<unsigned>(slot)];
    if (entry.node)
        return true;
    auto* node = new lx::scene::surface_node{surface};
    if (!parent.add(node)) {
        delete node;
        if (!warned_container_full_) {
            warned_container_full_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, "compositor.surface",
                                            "scene container full — surface not drawn. "
                                            "Reported once.");
        }
        return false;
    }
    node->attach(entry.image);
    node->set_solid(entry.solid, entry.solid_color);
    node->set_opaque(entry.opaque);
    node->set_bounds({0, 0, entry.surface_size.width, entry.surface_size.height});
    node->set_content(entry.src, entry.transform);
    if (entry.alpha < 1.f)
        node->set_transform(node->origin(), node->scale(), entry.alpha);
    entry.node = node;
    entry.node_parent = &parent;
    layout_changed_ = true;
    return true;
}

void lx::compositor::surface_manager::drop_node(lx::surface_id surface) {
    const int slot = find_surface(surface);
    if (slot < 0)
        return;
    auto& entry = surfaces_[static_cast<unsigned>(slot)];
    if (!entry.node)
        return;
    if (entry.node_parent)
        (void)entry.node_parent->remove(entry.node);
    delete entry.node;
    entry.node = nullptr;
    entry.node_parent = nullptr;
    layout_changed_ = true;
}

void lx::compositor::surface_manager::set_opaque(lx::surface_id surface, bool opaque) {
    const int slot = find_surface(surface);
    if (slot < 0)
        return;
    auto& entry = surfaces_[static_cast<unsigned>(slot)];
    if (entry.opaque == opaque)
        return;
    entry.opaque = opaque;
    if (!entry.node)
        return;
    entry.node->set_opaque(opaque);
    // Blending changes every pixel the node covers that is not fully opaque.
    if (scene_)
        note_content_damage(entry, node_rect(surface), resample_margin(entry));
}

lx::scene::surface_node* lx::compositor::surface_manager::node(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].node;
    return nullptr;
}

lx::rect2i lx::compositor::surface_manager::node_rect(lx::surface_id surface) const {
    const auto* n = node(surface);
    if (!n)
        return {};
    const lx::point2i o = n->origin();
    const lx::rect2i b = n->bounds();
    return {o.x + b.x, o.y + b.y, b.width, b.height};
}

void lx::compositor::surface_manager::set_origin(lx::surface_id surface, lx::point2i origin) {
    auto* n = node(surface);
    if (!n)
        return;
    const lx::point2i o = n->origin();
    if (o.x == origin.x && o.y == origin.y)
        return;
    n->set_transform(origin, n->scale(), n->opacity());
    layout_changed_ = true;
}

lx::gfx::imported_image lx::compositor::surface_manager::lookup(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].image;
    return {};
}

lx::compositor::content_hint lx::compositor::surface_manager::content_hint_for(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].hint;
    return content_hint::none;
}

void lx::compositor::surface_manager::set_override_redirect(lx::surface_id surface, bool value) {
    if (const int slot = find_or_alloc_surface(surface); slot >= 0)
        surfaces_[static_cast<unsigned>(slot)].override_redirect = value;
}

void lx::compositor::surface_manager::set_content_hint(lx::surface_id surface,
                                                         content_hint hint) {
    if (const int slot = find_or_alloc_surface(surface); slot >= 0)
        surfaces_[static_cast<unsigned>(slot)].hint = hint;
}

void lx::compositor::surface_manager::set_solid(lx::surface_id surface, lx::color color,
                                                lx::size2i size, bool opaque) {
    const int slot = find_or_alloc_surface(surface);
    if (slot < 0)
        return;
    auto& e = surfaces_[static_cast<unsigned>(slot)];
    const lx::size2i old_size = e.surface_size;
    const int old_margin = resample_margin(e);
    const bool changed = !e.solid || e.solid_color.r != color.r || e.solid_color.g != color.g ||
                         e.solid_color.b != color.b || e.solid_color.a != color.a ||
                         e.opaque != opaque || old_size.width != size.width ||
                         old_size.height != size.height;
    e.solid = true;
    e.solid_color = color;
    e.image = {};
    e.buffer = {};
    e.buffer_size = {1, 1};
    e.surface_size = size;
    e.src = {};
    e.transform = lx::buffer_transform::normal;
    e.opaque = opaque;
    if (!e.node || !changed)
        return;
    const lx::point2i o = e.node->origin();
    e.node->attach({});
    e.node->set_solid(true, color);
    e.node->set_opaque(opaque);
    e.node->set_bounds({0, 0, size.width, size.height});
    e.node->set_content({}, lx::buffer_transform::normal);
    if (scene_) {
        note_content_damage(e, {o.x, o.y, old_size.width, old_size.height}, old_margin);
        note_content_damage(e, {o.x, o.y, size.width, size.height}, 0);
    }
    if (old_size.width != size.width || old_size.height != size.height)
        layout_changed_ = true;
}

void lx::compositor::surface_manager::set_alpha(lx::surface_id surface, float alpha) {
    const int slot = find_or_alloc_surface(surface);
    if (slot < 0)
        return;
    auto& e = surfaces_[static_cast<unsigned>(slot)];
    alpha = alpha < 0.f ? 0.f : (alpha > 1.f ? 1.f : alpha);
    if (e.alpha == alpha)
        return;
    e.alpha = alpha;
    if (!e.node)
        return;
    e.node->set_transform(e.node->origin(), e.node->scale(), alpha);
    // Every pixel the node covers blends differently.
    if (scene_)
        note_content_damage(e, node_rect(surface), resample_margin(e));
}

float lx::compositor::surface_manager::alpha(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].alpha;
    return 1.f;
}

// ── input_region ───────────────────────────────────────────────────────────

namespace {

[[nodiscard]] long long right_of(lx::rect2i r) { return static_cast<long long>(r.x) + r.width; }
[[nodiscard]] long long bottom_of(lx::rect2i r) {
    return static_cast<long long>(r.y) + r.height;
}

[[nodiscard]] lx::rect2i rect_from_edges(long long x0, long long y0, long long x1, long long y1) {
    constexpr long long lo = -2147483647LL - 1;
    constexpr long long hi = 2147483647LL;
    const auto clamp = [](long long v) { return v < lo ? lo : (v > hi ? hi : v); };
    x0 = clamp(x0);
    y0 = clamp(y0);
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(clamp(x1 - x0)),
            static_cast<int>(clamp(y1 - y0))};
}

} // namespace

void lx::compositor::input_region::add(lx::rect2i r) {
    if (r.width <= 0 || r.height <= 0)
        return;
    infinite = false;
    // Already covered by one rect: nothing to add.
    for (unsigned i = 0; i < count; ++i) {
        const lx::rect2i& e = rects[i];
        if (r.x >= e.x && r.y >= e.y && right_of(r) <= right_of(e) &&
            bottom_of(r) <= bottom_of(e))
            return;
    }
    if (count < k_max) {
        rects[count++] = r;
        return;
    }
    // Overflow: the bounding box of everything, which over-accepts.
    long long x0 = r.x, y0 = r.y, x1 = right_of(r), y1 = bottom_of(r);
    for (unsigned i = 0; i < count; ++i) {
        x0 = rects[i].x < x0 ? rects[i].x : x0;
        y0 = rects[i].y < y0 ? rects[i].y : y0;
        x1 = right_of(rects[i]) > x1 ? right_of(rects[i]) : x1;
        y1 = bottom_of(rects[i]) > y1 ? bottom_of(rects[i]) : y1;
    }
    rects[0] = rect_from_edges(x0, y0, x1, y1);
    count = 1;
    approximated = true;
}

void lx::compositor::input_region::subtract(lx::rect2i cut) {
    if (cut.width <= 0 || cut.height <= 0)
        return;
    // Subtracting from "everything" would need a complement; wl_region never asks for that
    // (it starts empty), and a surface's infinite default is never edited, only replaced.
    if (infinite)
        return;
    const long long cx0 = cut.x, cy0 = cut.y, cx1 = right_of(cut), cy1 = bottom_of(cut);
    lx::rect2i out[k_max]{};
    unsigned n = 0;
    unsigned i = 0;
    for (; i < count; ++i) {
        const lx::rect2i e = rects[i];
        const long long ex0 = e.x, ey0 = e.y, ex1 = right_of(e), ey1 = bottom_of(e);
        if (cx1 <= ex0 || cx0 >= ex1 || cy1 <= ey0 || cy0 >= ey1) {
            out[n++] = e; // fits: n + (count - i) <= k_max holds throughout
            continue;
        }
        // Up to four pieces: full-width bands above and below the cut, then the left and
        // right remainders beside it.
        lx::rect2i pieces[4]{};
        unsigned p = 0;
        const long long my0 = cy0 > ey0 ? cy0 : ey0;
        const long long my1 = cy1 < ey1 ? cy1 : ey1;
        if (cy0 > ey0)
            pieces[p++] = rect_from_edges(ex0, ey0, ex1, cy0);
        if (cy1 < ey1)
            pieces[p++] = rect_from_edges(ex0, cy1, ex1, ey1);
        if (cx0 > ex0)
            pieces[p++] = rect_from_edges(ex0, my0, cx0, my1);
        if (cx1 < ex1)
            pieces[p++] = rect_from_edges(cx1, my0, ex1, my1);
        // Pieces must also leave room for the rects not yet visited.
        if (n + p + (count - i - 1) > k_max)
            break;
        for (unsigned j = 0; j < p; ++j)
            out[n++] = pieces[j];
    }
    if (i < count) {
        // Overflow: this rect and every one after it stay whole, which over-accepts.
        for (; i < count && n < k_max; ++i)
            out[n++] = rects[i];
        approximated = true;
    }
    for (unsigned j = 0; j < n; ++j)
        rects[j] = out[j];
    count = n;
}

bool lx::compositor::input_region::contains(int x, int y) const {
    if (infinite)
        return true;
    for (unsigned i = 0; i < count; ++i) {
        const lx::rect2i& r = rects[i];
        if (x >= r.x && y >= r.y && x < right_of(r) && y < bottom_of(r))
            return true;
    }
    return false;
}

void lx::compositor::surface_manager::set_input_region(lx::surface_id surface,
                                                       const input_region& region) {
    if (const int slot = find_or_alloc_surface(surface); slot >= 0)
        surfaces_[static_cast<unsigned>(slot)].input = region;
}

bool lx::compositor::surface_manager::hit(lx::surface_id surface, double x, double y,
                                          double& sx, double& sy) const {
    const int slot = find_surface(surface);
    if (slot < 0)
        return false;
    const auto& entry = surfaces_[static_cast<unsigned>(slot)];
    const auto* n = entry.node;
    if (!n || !n->is_visible())
        return false;
    const lx::point2i o = n->origin();
    const lx::rect2i b = n->bounds();
    const double lx_ = x - (o.x + b.x);
    const double ly_ = y - (o.y + b.y);
    if (lx_ < 0.0 || ly_ < 0.0 || lx_ >= b.width || ly_ >= b.height)
        return false;
    // Non-negative here, so truncation is floor.
    if (!entry.input.contains(static_cast<int>(lx_), static_cast<int>(ly_)))
        return false;
    sx = lx_;
    sy = ly_;
    return true;
}

void lx::compositor::surface_manager::note_content_damage(const surface_entry& e,
                                                          lx::rect2i region, int margin) const {
    if (!scene_)
        return;
    if (e.node_parent && !e.node_parent->is_visible()) {
        constexpr std::uint32_t k_capture_views =
            ~((std::uint32_t{1} << lx::scene::k_capture_view_base) - 1u);
        scene_->note_damage_views(region, margin, k_capture_views);
        return;
    }
    scene_->note_damage(region, margin);
}
