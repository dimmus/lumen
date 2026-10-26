module;

#include <stdio.h>

import lx.foundation;
import lx.trace;
import lx.scene;

export module lx.compositor:surface_tree;

import std;

import :surface;

export namespace lx::compositor {

/// What a root surface plays. A subsurface is never a root: it always has a parent.
/// `dnd_icon`: a drag-and-drop icon (wl_data_device.start_drag), drawn above the popups.
enum class root_kind : unsigned char { none, toplevel, popup, layer, dnd_icon, lock, input_popup };

inline constexpr unsigned k_tree_max_surfaces = 256;
/// Children per parent; one more slot holds the parent's own "self" marker.
inline constexpr unsigned k_tree_max_children = 31;
/// Nesting bound. Every walk is recursive, so the depth has to be.
inline constexpr unsigned k_tree_max_depth = 32;

/// Paint order of a parent and its direct children, bottom→top. The parent's own id is the
/// "self" marker: where the parent itself paints among its children (place_below the
/// parent puts a child under it).
struct tree_order {
    lx::surface_id ids[k_tree_max_children + 1]{};
    unsigned count = 0;

    [[nodiscard]] int index_of(lx::surface_id id) const {
        for (unsigned i = 0; i < count; ++i)
            if (ids[i] == id)
                return static_cast<int>(i);
        return -1;
    }
    bool remove(lx::surface_id id) {
        const int at = index_of(id);
        if (at < 0)
            return false;
        for (unsigned i = static_cast<unsigned>(at) + 1; i < count; ++i)
            ids[i - 1] = ids[i];
        ids[--count] = {};
        return true;
    }
    bool insert_at(unsigned index, lx::surface_id id) {
        if (count >= k_tree_max_children + 1)
            return false;
        if (index > count)
            index = count;
        for (unsigned i = count; i > index; --i)
            ids[i] = ids[i - 1];
        ids[index] = id;
        ++count;
        return true;
    }
    [[nodiscard]] bool same(const tree_order& o) const {
        if (count != o.count)
            return false;
        for (unsigned i = 0; i < count; ++i)
            if (!(ids[i] == o.ids[i]))
                return false;
        return true;
    }
};

/// A parent's cached child state (its children's cached positions and its cached order),
/// moved out of the tree while a content update that carries it waits in the queue
/// (`surface_tree::take_cached_child_state`), and put back when that update is applied.
struct tree_child_state {
    bool order_set = false;
    tree_order order{};
    unsigned count = 0;
    lx::surface_id ids[k_tree_max_children]{};
    lx::point2i pos[k_tree_max_children]{};
};

/// The surface of a tree under a point, from `surface_tree::hit`.
struct tree_hit {
    lx::surface_id surface{};
    /// Surface-local position of the point.
    double sx = 0.0;
    double sy = 0.0;
};

/// Surface trees: wl_subsurface parentage, double-buffered child state and paint order,
/// and each root's scene container. UI affinity. Wayland-free — the protocol layer moves
/// the wl_surface state; this keeps the bookkeeping that decides what is drawn where.
///
/// Every root surface (toplevel, popup, layer) owns one container holding its whole tree
/// flattened in paint order, with an optional underlay (the server-side frame) at the
/// bottom. Restacking a window is one move of its container. Node origins stay global
/// (`lx.scene` has no relative transforms): positions are kept relative here and pushed
/// into the nodes whenever a root moves or its tree changes.
class surface_tree {
public:
    enum class link_error : unsigned char { none, bad_surface, bad_parent, no_memory };
    enum class restack_error : unsigned char { none, bad_surface };
    /// Called for every node the tree drops (unmap, unlink, destroy), after the drop. The
    /// window manager moves pointer focus off a surface that stops being drawn.
    using unmap_hook = void (*)(void* user, lx::surface_id surface);
    /// Called whenever a node's global rect changes — created, moved, resized, or dropped
    /// (with an empty rect) — at the three places node rects are set. What output
    /// membership (wl_surface.enter/leave) is computed from. No allocation.
    using rect_hook = void (*)(void* user, lx::surface_id surface, lx::rect2i rect);

    void bind(surface_manager& surfaces, scene::scene_graph& scene) {
        surfaces_ = &surfaces;
        scene_ = &scene;
    }
    void set_unmap_hook(unmap_hook fn, void* user) {
        unmap_hook_ = fn;
        unmap_user_ = user;
    }
    void set_rect_hook(rect_hook fn, void* user) {
        rect_hook_ = fn;
        rect_user_ = user;
    }
    /// The global rect of `id`'s node as last placed, empty when it has none.
    [[nodiscard]] lx::rect2i node_rect(lx::surface_id id) const;

    // ── membership ──

    /// wl_compositor.create_surface. False when the table is full.
    [[nodiscard]] bool add_surface(lx::surface_id id);
    /// wl_surface destroyed. Leaves its parent's orders, unmaps its subtree and orphans its
    /// children (unmapped, inert until their wl_subsurface is destroyed). A root loses its
    /// container.
    void remove_surface(lx::surface_id id);

    // ── wl_subsurface ──

    /// get_subsurface. The child is appended on top of the parent's *pending* order —
    /// "adding a sub-surface is double-buffered on the parent". Checks the tree rules only
    /// (`bad_surface`: unknown, a root, or already linked; `bad_parent`: unknown, itself or
    /// one of its descendants); the protocol layer checks roles.
    [[nodiscard]] link_error link(lx::surface_id child, lx::surface_id parent);
    /// wl_subsurface destroyed: immediate. The subtree is unmapped and the surface forgets
    /// its position and z-order.
    void unlink(lx::surface_id child);
    void set_position_pending(lx::surface_id child, lx::point2i p);
    /// place_above / place_below in the parent's pending order. `sibling` must be the parent
    /// or another child in that order. Inert (no error) once the parent is gone.
    [[nodiscard]] restack_error place(lx::surface_id child, lx::surface_id sibling, bool above);
    void set_sync(lx::surface_id child, bool sync);
    /// Synchronized itself or through any ancestor. Roots never are.
    [[nodiscard]] bool effectively_sync(lx::surface_id id) const;

    // ── commit choreography (pure bookkeeping; the caller moves the wl_surface state) ──

    /// The parent committed: its children's pending positions and its pending order become
    /// the cached ones.
    void cache_child_state(lx::surface_id parent);
    /// The parent's cached state is being applied: cached positions and order become
    /// current.
    void apply_child_state(lx::surface_id parent);
    /// Moves the parent's cached child state into `out` (a queued update carries it) and
    /// clears it; `put_cached_child_state` makes a carried state the cached one again (for
    /// `apply_child_state`, or to restore what was stashed around a queued apply).
    void take_cached_child_state(lx::surface_id parent, tree_child_state& out);
    void put_cached_child_state(lx::surface_id parent, const tree_child_state& in);
    /// Effectively synchronized children (every linked child, in pending order), whose
    /// cached state is applied together with the parent's.
    [[nodiscard]] unsigned sync_children(lx::surface_id parent, lx::surface_id* out,
                                         unsigned cap) const;
    /// Pre-order over `id` and every descendant. `fn(surface_id) -> bool` returns whether
    /// to descend into that surface's children.
    template<class Fn>
    void for_each_preorder(lx::surface_id id, Fn&& fn) const {
        preorder(id, fn, 0);
    }
    void set_has_buffer(lx::surface_id id, bool has_buffer);
    /// Topmost ancestor (the surface itself when it has no parent).
    [[nodiscard]] lx::surface_id root_of(lx::surface_id id) const;

    // ── queries ──

    [[nodiscard]] bool contains(lx::surface_id id) const { return find(id) != nullptr; }
    [[nodiscard]] lx::surface_id parent_of(lx::surface_id id) const;
    /// Has a live wl_subsurface (an orphan still does, until it is destroyed).
    [[nodiscard]] bool linked(lx::surface_id id) const;
    [[nodiscard]] bool sync_flag(lx::surface_id id) const;
    [[nodiscard]] bool mapped(lx::surface_id id) const;
    /// Current position relative to the parent.
    [[nodiscard]] lx::point2i position(lx::surface_id id) const;
    /// Current paint order of `id` as a parent; null when unknown.
    [[nodiscard]] const tree_order* order(lx::surface_id id) const;
    [[nodiscard]] const tree_order* order_pending(lx::surface_id id) const;

    // ── roots ──

    /// Gives `id` a root role and its container (allocated once, here). True when it is
    /// (now) a root of that kind.
    [[nodiscard]] bool make_root(lx::surface_id id, root_kind kind);
    [[nodiscard]] root_kind kind(lx::surface_id id) const;
    /// Role object id for the `tree` log line (the toplevel id, for instance).
    void set_root_tag(lx::surface_id root, unsigned long long tag);
    /// The node painted under the whole tree (the server-side frame); not owned.
    void set_underlay(lx::surface_id root, scene::scene_node* node);
    /// Puts the root's container into `layer` at `index_or_top` (clamped to the top) and
    /// creates the nodes of every mapped surface. False when the layer is full.
    [[nodiscard]] bool map_root(lx::surface_id root, scene::container_node& layer,
                                unsigned index_or_top);
    /// Takes the container out of its layer and drops every node, damaging each.
    void unmap_root(lx::surface_id root);
    /// Moves a mapped root's container, nodes and all, to the top of another layer
    /// (zwlr_layer_surface_v1.set_layer), damaging every node. False when it is not mapped
    /// or the new layer is full (it then stays where it was).
    [[nodiscard]] bool move_root(lx::surface_id root, scene::container_node& layer);
    [[nodiscard]] bool root_mapped(lx::surface_id root) const;
    /// Global origin of the root surface. Moves every node of a mapped, synced tree at
    /// once, damaging old and new rects separately; a dirty tree only records it, and the
    /// caller's `sync_scene` (which must follow) places every node from it.
    void set_root_origin(lx::surface_id root, lx::point2i origin);
    [[nodiscard]] lx::point2i root_origin(lx::surface_id root) const;
    /// The tree of `any` changed; the next `sync_scene` of its root rebuilds the scene side.
    void mark_dirty(lx::surface_id any);
    /// Brings the root's container in line with the tree (no-op unless dirty): mapping,
    /// node creation and removal, global origins, paint order. Damage per node; logs one
    /// `tree` line when anything changed.
    void sync_scene(lx::surface_id root, const char* cause);
    /// Root-local bounding box of every surface that is mapped once the root is: the root's
    /// buffer and every child with content in the current orders. May reach left of or
    /// above the root (a CSD title bar is a subsurface at y < 0). Empty without content.
    [[nodiscard]] lx::rect2i subtree_bbox(lx::surface_id root) const;
    [[nodiscard]] scene::container_node* container(lx::surface_id root) const;
    /// Notes every node rect of a mapped root as damaged (a restack).
    void damage_root(lx::surface_id root) const;
    /// Draws every node of the root's tree — including nodes created later — through a grey
    /// tint (a client that stopped answering pings), damaging the tree when it changes.
    void set_root_dim(lx::surface_id root, bool dim);
    [[nodiscard]] bool root_dimmed(lx::surface_id root) const;
    /// Topmost surface of the root's tree whose extents and input region contain the global
    /// point, in reverse paint order. No allocation; one O(1) lookup per node.
    [[nodiscard]] bool hit(lx::surface_id root, double gx, double gy, tree_hit& out) const;
    /// `tree root=… nodes=…` at debug (category compositor.scene).
    void log_tree(lx::surface_id root, const char* cause) const;

private:
    struct entry {
        lx::surface_id id{};
        lx::surface_id parent{};
        bool used = false;
        bool linked = false;
        bool sync = true;
        bool has_buffer = false;
        bool mapped = false;
        /// sync_scene scratch: reached by this pass's paint walk.
        bool walk_mark = false;
        lx::point2i pos{};
        lx::point2i pos_pending{};
        lx::point2i pos_cached{};
        bool pos_pending_set = false;
        bool pos_cached_set = false;
        tree_order order{};
        tree_order order_pending{};
        tree_order order_cached{};
        bool order_pending_dirty = false;
        bool order_cached_dirty = false;
        /// Global node rect at the last sync, for change detection.
        lx::rect2i last_rect{};
        // ── root data ──
        root_kind kind = root_kind::none;
        scene::container_node* container = nullptr;
        scene::container_node* layer = nullptr;
        scene::scene_node* underlay = nullptr;
        lx::point2i root_origin{};
        unsigned long long tag = 0;
        bool root_mapped = false;
        bool scene_dirty = false;
        bool dim = false;
    };
    struct paint_item {
        lx::surface_id id{};
        lx::point2i origin{};
    };

    /// `entry*`, or `const entry*` on a const tree.
    [[nodiscard]] auto find(this auto& self, lx::surface_id id) -> decltype(&self.entries_[0]) {
        const int slot = self.index_.find(id.id());
        return slot >= 0 ? &self.entries_[static_cast<unsigned>(slot)] : nullptr;
    }

    template<class Fn>
    void preorder(lx::surface_id id, Fn& fn, unsigned depth) const {
        const entry* e = find(id);
        if (!e || depth > k_tree_max_depth)
            return;
        if (!fn(id))
            return;
        // A copy: `fn` may apply state that edits this surface's orders.
        const tree_order children = e->order_pending;
        for (unsigned i = 0; i < children.count; ++i) {
            if (children.ids[i] == id)
                continue;
            const entry* c = find(children.ids[i]);
            if (c && c->parent == id)
                preorder(children.ids[i], fn, depth + 1);
        }
    }

    void paint_walk(entry& e, lx::point2i origin, unsigned depth);
    void clear_marks(entry& e, unsigned depth);
    /// Drops every node of `e`'s subtree not reached by the paint walk (all of them with
    /// `all`), logging `reason` for `e` and `child_reason` below it.
    bool drop_unmarked(entry& e, unsigned depth, bool all, const char* reason,
                       const char* child_reason);
    void drop_one(entry& e, const char* reason);
    void bbox_walk(const entry& e, lx::point2i origin, unsigned depth, long long& x0,
                   long long& y0, long long& x1, long long& y1, bool& any) const;
    void log_line(const char* category, const char* line) const;
    template <typename... Args>
    void log_line(const char* category, std::format_string<Args...> fmt, Args&&... args) const;
    [[nodiscard]] bool debug() const;

    surface_manager* surfaces_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    unmap_hook unmap_hook_ = nullptr;
    void* unmap_user_ = nullptr;
    rect_hook rect_hook_ = nullptr;
    void* rect_user_ = nullptr;
    /// Allocated 16 entries at a time (`lx::sparse_slots`): an entry is nearly a kilobyte.
    lx::sparse_slots<entry, k_tree_max_surfaces> entries_{};
    id_slot_index<k_tree_max_surfaces * 2> index_{};
    /// sync_scene scratch: the root's mapped surfaces in paint order.
    std::inplace_vector<paint_item, k_tree_max_surfaces> paint_{};
    bool warned_layer_full_ = false;
};

} // namespace lx::compositor


namespace {

constexpr const char* k_surface_category = "compositor.surface";
constexpr const char* k_scene_category = "compositor.scene";

[[nodiscard]] bool rect_same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// The not-responding tint: Mutter and KWin grey a hung window out; 0.65 keeps it legible.
constexpr lx::color k_dim_tint = lx::color::rgb(0.65f, 0.65f, 0.65f, 1.f);
constexpr lx::color k_no_tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);

[[nodiscard]] const char* kind_name(lx::compositor::root_kind k) {
    switch (k) {
    case lx::compositor::root_kind::toplevel:
        return "toplevel";
    case lx::compositor::root_kind::popup:
        return "popup";
    case lx::compositor::root_kind::layer:
        return "layer";
    case lx::compositor::root_kind::dnd_icon:
        return "dnd_icon";
    case lx::compositor::root_kind::lock:
        return "lock";
    case lx::compositor::root_kind::input_popup:
        return "input_popup";
    case lx::compositor::root_kind::none:
        break;
    }
    return "none";
}

} // namespace

// ── lookup ─────────────────────────────────────────────────────────────────

bool lx::compositor::surface_tree::debug() const {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

void lx::compositor::surface_tree::log_line(const char* category, const char* line) const {
    lx::trace::logger::global().log(lx::trace::level::debug, category, line);
}

template <typename... Args>
void lx::compositor::surface_tree::log_line(const char* category, std::format_string<Args...> fmt,
                                            Args&&... args) const {
    lx::trace::logger::global().logf(lx::trace::level::debug, category, fmt,
                                     std::forward<Args>(args)...);
}

// ── membership ─────────────────────────────────────────────────────────────


bool lx::compositor::surface_tree::add_surface(lx::surface_id id) {
    if (!id)
        return false;
    if (find(id))
        return true;
    for (unsigned i = 0; i < k_tree_max_surfaces; ++i) {
        if (entries_[i].used)
            continue;
        if (!index_.insert(id.id(), static_cast<int>(i)))
            return false;
        entry& e = entries_[i];
        e = {};
        e.used = true;
        e.id = id;
        // Every surface paints itself; its children are placed around this marker.
        (void)e.order.insert_at(0, id);
        e.order_pending = e.order;
        e.order_cached = e.order;
        return true;
    }
    return false;
}

void lx::compositor::surface_tree::drop_one(entry& e, const char* reason) {
    e.mapped = false;
    const bool had_rect = e.last_rect.width > 0 && e.last_rect.height > 0;
    e.last_rect = {};
    if (had_rect && rect_hook_)
        rect_hook_(rect_user_, e.id, {});
    if (!surfaces_ || !surfaces_->node(e.id))
        return;
    if (scene_)
        scene_->note_damage(surfaces_->node_rect(e.id));
    surfaces_->drop_node(e.id);
    if (reason && e.parent && debug()) {
        log_line(k_surface_category, "subsurface unmap surface={} reason={}", e.id.id(), reason);
    }
    if (unmap_hook_)
        unmap_hook_(unmap_user_, e.id);
}

bool lx::compositor::surface_tree::drop_unmarked(entry& e, unsigned depth, bool all,
                                                 const char* reason, const char* child_reason) {
    bool dropped = false;
    const bool keep = !all && e.walk_mark;
    if (!keep && (e.mapped || (surfaces_ && surfaces_->node(e.id)))) {
        const bool had_node = surfaces_ && surfaces_->node(e.id);
        drop_one(e, reason ? reason : (e.has_buffer ? "parent-unmapped" : "null-buffer"));
        dropped = had_node;
    }
    e.mapped = keep;
    if (depth >= k_tree_max_depth)
        return dropped;
    for (unsigned i = 0; i < e.order_pending.count; ++i) {
        const lx::surface_id cid = e.order_pending.ids[i];
        if (cid == e.id)
            continue;
        entry* c = find(cid);
        if (c && c->parent == e.id)
            dropped |= drop_unmarked(*c, depth + 1, all, child_reason, child_reason);
    }
    return dropped;
}

void lx::compositor::surface_tree::remove_surface(lx::surface_id id) {
    entry* e = find(id);
    if (!e)
        return;
    const lx::surface_id old_root = root_of(id);
    if (e->parent) {
        if (entry* p = find(e->parent)) {
            (void)p->order.remove(id);
            (void)p->order_pending.remove(id);
            (void)p->order_cached.remove(id);
        }
    }
    if (e->kind != root_kind::none && e->root_mapped)
        unmap_root(id);
    // The surface and everything below it stops being drawn; its children are orphaned.
    (void)drop_unmarked(*e, 0, true, "destroyed", "parent-destroyed");
    for (unsigned i = 0; i < e->order_pending.count; ++i) {
        const lx::surface_id cid = e->order_pending.ids[i];
        if (cid == id)
            continue;
        entry* c = find(cid);
        if (!c || c->parent != id)
            continue;
        c->parent = {};
        c->pos = c->pos_pending = c->pos_cached = {};
        c->pos_pending_set = c->pos_cached_set = false;
    }
    if (e->container) {
        if (e->layer)
            (void)e->layer->remove(e->container);
        delete e->container;
    }
    if (!(old_root == id)) {
        if (entry* r = find(old_root))
            r->scene_dirty = true;
    }
    index_.erase(id.id());
    *e = {};
}

// ── wl_subsurface ──────────────────────────────────────────────────────────

lx::compositor::surface_tree::link_error
lx::compositor::surface_tree::link(lx::surface_id child, lx::surface_id parent) {
    entry* c = find(child);
    if (!c || c->linked || c->kind != root_kind::none)
        return link_error::bad_surface;
    entry* p = find(parent);
    if (!p || parent == child)
        return link_error::bad_parent;
    // The parent must not be one of the child's descendants: walk the parent's ancestry.
    unsigned depth = 0;
    for (const entry* a = p; a; a = a->parent ? find(a->parent) : nullptr) {
        if (a->id == child)
            return link_error::bad_parent;
        if (++depth > k_tree_max_depth)
            return link_error::no_memory;
    }
    if (p->order_pending.count >= k_tree_max_children + 1)
        return link_error::no_memory;
    (void)p->order_pending.insert_at(p->order_pending.count, child);
    p->order_pending_dirty = true;
    c->parent = parent;
    c->linked = true;
    c->sync = true;
    c->pos = c->pos_pending = c->pos_cached = {};
    c->pos_pending_set = c->pos_cached_set = false;
    c->mapped = false;
    return link_error::none;
}

void lx::compositor::surface_tree::unlink(lx::surface_id child) {
    entry* c = find(child);
    if (!c || !c->linked)
        return;
    if (c->parent) {
        if (entry* p = find(c->parent)) {
            (void)p->order.remove(child);
            (void)p->order_pending.remove(child);
            (void)p->order_cached.remove(child);
        }
        mark_dirty(c->parent);
    }
    (void)drop_unmarked(*c, 0, true, "unlinked", "parent-unmapped");
    c->linked = false;
    c->parent = {};
    c->sync = true;
    c->pos = c->pos_pending = c->pos_cached = {};
    c->pos_pending_set = c->pos_cached_set = false;
}

void lx::compositor::surface_tree::set_position_pending(lx::surface_id child, lx::point2i p) {
    entry* c = find(child);
    if (!c || !c->linked || !c->parent)
        return;
    c->pos_pending = p;
    c->pos_pending_set = true;
}

lx::compositor::surface_tree::restack_error
lx::compositor::surface_tree::place(lx::surface_id child, lx::surface_id sibling, bool above) {
    entry* c = find(child);
    if (!c || !c->linked || !c->parent)
        return restack_error::none; // inert: the parent is gone
    entry* p = find(c->parent);
    if (!p)
        return restack_error::none;
    if (sibling == child)
        return restack_error::bad_surface;
    if (!(sibling == c->parent)) {
        const entry* s = find(sibling);
        if (!s || !(s->parent == c->parent) || p->order_pending.index_of(sibling) < 0)
            return restack_error::bad_surface;
    }
    (void)p->order_pending.remove(child);
    const int at = p->order_pending.index_of(sibling);
    (void)p->order_pending.insert_at(static_cast<unsigned>(at) + (above ? 1u : 0u), child);
    p->order_pending_dirty = true;
    return restack_error::none;
}

void lx::compositor::surface_tree::set_sync(lx::surface_id child, bool sync) {
    if (entry* c = find(child))
        c->sync = sync;
}

bool lx::compositor::surface_tree::effectively_sync(lx::surface_id id) const {
    const entry* e = find(id);
    for (unsigned depth = 0; e && depth <= k_tree_max_depth; ++depth) {
        if (!e->linked || !e->parent)
            return false;
        if (e->sync)
            return true;
        e = find(e->parent);
    }
    return false;
}

// ── commit choreography ────────────────────────────────────────────────────

void lx::compositor::surface_tree::cache_child_state(lx::surface_id parent) {
    entry* p = find(parent);
    if (!p)
        return;
    for (unsigned i = 0; i < p->order_pending.count; ++i) {
        const lx::surface_id cid = p->order_pending.ids[i];
        if (cid == parent)
            continue;
        entry* c = find(cid);
        if (!c || !(c->parent == parent) || !c->pos_pending_set)
            continue;
        c->pos_cached = c->pos_pending;
        c->pos_cached_set = true;
        c->pos_pending_set = false;
    }
    if (p->order_pending_dirty) {
        p->order_cached = p->order_pending;
        p->order_cached_dirty = true;
        p->order_pending_dirty = false;
    }
}

void lx::compositor::surface_tree::take_cached_child_state(lx::surface_id parent,
                                                           tree_child_state& out) {
    out.order_set = false;
    out.count = 0;
    entry* p = find(parent);
    if (!p)
        return;
    for (unsigned i = 0; i < p->order_pending.count && out.count < k_tree_max_children; ++i) {
        const lx::surface_id cid = p->order_pending.ids[i];
        if (cid == parent)
            continue;
        entry* c = find(cid);
        if (!c || !(c->parent == parent) || !c->pos_cached_set)
            continue;
        out.ids[out.count] = cid;
        out.pos[out.count] = c->pos_cached;
        ++out.count;
        c->pos_cached_set = false;
    }
    if (p->order_cached_dirty) {
        out.order = p->order_cached;
        out.order_set = true;
        p->order_cached_dirty = false;
    }
}

void lx::compositor::surface_tree::put_cached_child_state(lx::surface_id parent,
                                                          const tree_child_state& in) {
    entry* p = find(parent);
    if (!p)
        return;
    // Replaces whatever is cached now (the caller took it out first).
    for (unsigned i = 0; i < p->order_pending.count; ++i) {
        entry* c = find(p->order_pending.ids[i]);
        if (c && c->parent == parent && !(c->id == parent))
            c->pos_cached_set = false;
    }
    for (unsigned i = 0; i < in.count; ++i) {
        entry* c = find(in.ids[i]);
        if (!c || !(c->parent == parent))
            continue; // unlinked or re-parented since: its old position means nothing now
        c->pos_cached = in.pos[i];
        c->pos_cached_set = true;
    }
    p->order_cached_dirty = in.order_set;
    if (in.order_set) {
        // Children that left since the order was cached are dropped from it.
        tree_order o{};
        for (unsigned i = 0; i < in.order.count; ++i) {
            const lx::surface_id id = in.order.ids[i];
            const entry* c = find(id);
            if (id == parent || (c && c->parent == parent))
                o.ids[o.count++] = id;
        }
        p->order_cached = o;
    }
}

void lx::compositor::surface_tree::apply_child_state(lx::surface_id parent) {
    entry* p = find(parent);
    if (!p)
        return;
    bool changed = false;
    for (unsigned i = 0; i < p->order_pending.count; ++i) {
        const lx::surface_id cid = p->order_pending.ids[i];
        if (cid == parent)
            continue;
        entry* c = find(cid);
        if (!c || !(c->parent == parent) || !c->pos_cached_set)
            continue;
        c->pos_cached_set = false;
        if (c->pos.x == c->pos_cached.x && c->pos.y == c->pos_cached.y)
            continue;
        c->pos = c->pos_cached;
        changed = true;
        if (debug()) {
            log_line(k_surface_category, "subsurface position surface={} x={} y={}", cid.id(),
                     c->pos.x, c->pos.y);
        }
    }
    if (p->order_cached_dirty) {
        p->order_cached_dirty = false;
        if (!p->order.same(p->order_cached)) {
            p->order = p->order_cached;
            changed = true;
            if (debug()) {
                char order[480]{};
                char* end = order;
                char* const last = order + sizeof(order) - 1;
                for (unsigned i = 0; i < p->order.count && last - end > 24; ++i)
                    end = p->order.ids[i] == parent
                              ? std::format_to_n(end, last - end, "{}self", i ? "," : "").out
                              : std::format_to_n(end, last - end, "{}{}", i ? "," : "",
                                                 p->order.ids[i].id())
                                    .out;
                log_line(k_surface_category, "subsurface restack parent={} order={}",
                         parent.id(), order);
            }
        }
    }
    if (changed)
        mark_dirty(parent);
}

unsigned lx::compositor::surface_tree::sync_children(lx::surface_id parent, lx::surface_id* out,
                                                     unsigned cap) const {
    const entry* p = find(parent);
    if (!p || !out)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < p->order_pending.count && n < cap; ++i) {
        const lx::surface_id cid = p->order_pending.ids[i];
        if (cid == parent)
            continue;
        const entry* c = find(cid);
        if (c && c->parent == parent && effectively_sync(cid))
            out[n++] = cid;
    }
    return n;
}

void lx::compositor::surface_tree::set_has_buffer(lx::surface_id id, bool has_buffer) {
    entry* e = find(id);
    if (!e || e->has_buffer == has_buffer)
        return;
    e->has_buffer = has_buffer;
    mark_dirty(id);
}

lx::surface_id lx::compositor::surface_tree::root_of(lx::surface_id id) const {
    const entry* e = find(id);
    if (!e)
        return id;
    for (unsigned depth = 0; e->parent && depth <= k_tree_max_depth; ++depth) {
        const entry* p = find(e->parent);
        if (!p)
            break;
        e = p;
    }
    return e->id;
}

// ── queries ────────────────────────────────────────────────────────────────

lx::surface_id lx::compositor::surface_tree::parent_of(lx::surface_id id) const {
    const entry* e = find(id);
    return e ? e->parent : lx::surface_id{};
}
bool lx::compositor::surface_tree::linked(lx::surface_id id) const {
    const entry* e = find(id);
    return e && e->linked;
}
bool lx::compositor::surface_tree::sync_flag(lx::surface_id id) const {
    const entry* e = find(id);
    return e && e->sync;
}
bool lx::compositor::surface_tree::mapped(lx::surface_id id) const {
    const entry* e = find(id);
    return e && e->mapped;
}
lx::point2i lx::compositor::surface_tree::position(lx::surface_id id) const {
    const entry* e = find(id);
    return e ? e->pos : lx::point2i{};
}
const lx::compositor::tree_order* lx::compositor::surface_tree::order(lx::surface_id id) const {
    const entry* e = find(id);
    return e ? &e->order : nullptr;
}
const lx::compositor::tree_order*
lx::compositor::surface_tree::order_pending(lx::surface_id id) const {
    const entry* e = find(id);
    return e ? &e->order_pending : nullptr;
}

// ── roots ──────────────────────────────────────────────────────────────────

bool lx::compositor::surface_tree::make_root(lx::surface_id id, root_kind kind) {
    entry* e = find(id);
    if (!e || kind == root_kind::none)
        return false;
    if (e->kind != root_kind::none)
        return e->kind == kind;
    if (e->linked || e->parent)
        return false;
    e->container = new scene::container_node{};
    // The window's nodes round their edges relative to its root origin (`project_node`),
    // which the container carries.
    e->container->set_anchor(true);
    e->container->set_transform(e->root_origin);
    e->kind = kind;
    return true;
}

lx::compositor::root_kind lx::compositor::surface_tree::kind(lx::surface_id id) const {
    const entry* e = find(id);
    return e ? e->kind : root_kind::none;
}

void lx::compositor::surface_tree::set_root_tag(lx::surface_id root, unsigned long long tag) {
    if (entry* e = find(root))
        e->tag = tag;
}

void lx::compositor::surface_tree::set_underlay(lx::surface_id root, scene::scene_node* node) {
    entry* e = find(root);
    if (!e || !e->container || e->underlay == node)
        return;
    if (e->underlay)
        (void)e->container->remove(e->underlay);
    e->underlay = node;
    if (node && e->container->add(node))
        (void)e->container->move_to(node, 0);
    e->scene_dirty = true;
}

bool lx::compositor::surface_tree::map_root(lx::surface_id root, scene::container_node& layer,
                                            unsigned index_or_top) {
    entry* e = find(root);
    if (!e || !e->container)
        return false;
    if (e->root_mapped)
        return true;
    if (!layer.add(e->container)) {
        if (!warned_layer_full_) {
            warned_layer_full_ = true;
            lx::trace::logger::global().log(lx::trace::level::warn, k_scene_category,
                                            "scene layer full — window not drawn. Reported "
                                            "once.");
        }
        return false;
    }
    if (layer.child_count() > 0 && index_or_top < layer.child_count() - 1)
        (void)layer.move_to(e->container, index_or_top);
    e->layer = &layer;
    e->root_mapped = true;
    e->scene_dirty = true;
    sync_scene(root, "map");
    return true;
}

void lx::compositor::surface_tree::unmap_root(lx::surface_id root) {
    entry* e = find(root);
    if (!e || !e->root_mapped)
        return;
    // Out of the layer before any node goes: a hit test run from the unmap hook must not
    // find a half-dismantled tree.
    e->root_mapped = false;
    if (e->layer && e->container)
        (void)e->layer->remove(e->container);
    e->layer = nullptr;
    (void)drop_unmarked(*e, 0, true, nullptr, "parent-unmapped");
    e->scene_dirty = false;
    if (surfaces_)
        surfaces_->note_layout_changed();
    log_tree(root, "unmap");
}

bool lx::compositor::surface_tree::move_root(lx::surface_id root, scene::container_node& layer) {
    entry* e = find(root);
    if (!e || !e->root_mapped || !e->container)
        return false;
    if (e->layer == &layer)
        return true;
    scene::container_node* old = e->layer;
    if (old)
        (void)old->remove(e->container);
    if (!layer.add(e->container)) {
        if (old)
            (void)old->add(e->container);
        return false;
    }
    e->layer = &layer;
    // Same rects, another z: everything it covered or uncovered changes.
    damage_root(root);
    if (surfaces_)
        surfaces_->note_layout_changed();
    log_tree(root, "restack");
    return true;
}

bool lx::compositor::surface_tree::root_mapped(lx::surface_id root) const {
    const entry* e = find(root);
    return e && e->root_mapped;
}

lx::point2i lx::compositor::surface_tree::root_origin(lx::surface_id root) const {
    const entry* e = find(root);
    return e ? e->root_origin : lx::point2i{};
}

void lx::compositor::surface_tree::set_root_origin(lx::surface_id root, lx::point2i origin) {
    entry* e = find(root);
    if (!e || (e->root_origin.x == origin.x && e->root_origin.y == origin.y))
        return;
    const int dx = origin.x - e->root_origin.x;
    const int dy = origin.y - e->root_origin.y;
    e->root_origin = origin;
    if (e->container)
        e->container->set_transform(origin);
    // A dirty tree is recomputed from `root_origin` by the sync its caller runs next (a
    // commit's finish, or the window manager right after a move), which damages what moved.
    if (!e->root_mapped || !e->container || !surfaces_ || e->scene_dirty)
        return;
    // Every node moves by the same delta. Old and new rects are separate clips, never
    // their bounding box.
    for (unsigned i = 0; i < e->container->child_count(); ++i) {
        scene::scene_node* n = e->container->child_at(i);
        if (!n || n == e->underlay || n->kind() != scene::node_kind::surface)
            continue;
        const lx::surface_id sid = static_cast<scene::surface_node*>(n)->surface();
        const lx::rect2i old_rect = surfaces_->node_rect(sid);
        surfaces_->set_origin(sid, {n->origin().x + dx, n->origin().y + dy});
        const lx::rect2i new_rect = surfaces_->node_rect(sid);
        if (scene_) {
            scene_->note_damage(old_rect);
            scene_->note_damage(new_rect);
        }
        if (entry* s = find(sid)) {
            s->last_rect = new_rect;
            if (rect_hook_)
                rect_hook_(rect_user_, sid, new_rect);
        }
    }
    log_tree(root, "move");
}

void lx::compositor::surface_tree::mark_dirty(lx::surface_id any) {
    if (entry* r = find(root_of(any)))
        r->scene_dirty = true;
}

void lx::compositor::surface_tree::clear_marks(entry& e, unsigned depth) {
    e.walk_mark = false;
    if (depth >= k_tree_max_depth)
        return;
    for (unsigned i = 0; i < e.order_pending.count; ++i) {
        const lx::surface_id cid = e.order_pending.ids[i];
        if (cid == e.id)
            continue;
        entry* c = find(cid);
        if (c && c->parent == e.id)
            clear_marks(*c, depth + 1);
    }
}

void lx::compositor::surface_tree::paint_walk(entry& e, lx::point2i origin, unsigned depth) {
    // `e` is mapped: it has content and every ancestor is mapped. Its children paint
    // around its self marker, in its *current* order.
    for (unsigned i = 0; i < e.order.count; ++i) {
        const lx::surface_id sid = e.order.ids[i];
        if (sid == e.id) {
            e.walk_mark = true;
            if (paint_.size() < k_tree_max_surfaces)
                paint_.push_back({sid, origin});
            continue;
        }
        entry* c = find(sid);
        if (!c || !c->linked || !(c->parent == e.id) || !c->has_buffer ||
            depth >= k_tree_max_depth)
            continue;
        paint_walk(*c, {origin.x + c->pos.x, origin.y + c->pos.y}, depth + 1);
    }
}

void lx::compositor::surface_tree::sync_scene(lx::surface_id root, const char* cause) {
    entry* r = find(root);
    if (!r || !r->scene_dirty)
        return;
    r->scene_dirty = false;
    if (r->kind == root_kind::none || !r->root_mapped || !r->container || !surfaces_)
        return;
    scene::container_node& box = *r->container;

    // 1. Mapping: walk the current orders from the root.
    clear_marks(*r, 0);
    paint_.clear();
    if (r->has_buffer)
        paint_walk(*r, r->root_origin, 0);

    // 2. Everything no longer mapped loses its node.
    bool changed = drop_unmarked(*r, 0, false, nullptr, nullptr);

    // 3. Nodes for what became mapped; global origins for all of them.
    for (unsigned i = 0; i < paint_.size(); ++i) {
        const paint_item& it = paint_[i];
        entry* e = find(it.id);
        if (!e)
            continue;
        bool created = false;
        if (!surfaces_->node(it.id)) {
            if (!surfaces_->ensure_node(it.id, box)) {
                e->mapped = false;
                continue;
            }
            created = true;
            if (r->dim)
                surfaces_->node(it.id)->set_tint(k_dim_tint);
        }
        const lx::rect2i old_rect = surfaces_->node_rect(it.id);
        surfaces_->set_origin(it.id, it.origin);
        const lx::rect2i new_rect = surfaces_->node_rect(it.id);
        if (scene_) {
            if (created) {
                scene_->note_damage(new_rect);
            } else if (!rect_same(old_rect, new_rect)) {
                scene_->note_damage(old_rect);
                scene_->note_damage(new_rect);
            }
        }
        if (created && e->parent && debug()) {
            log_line(k_surface_category, "subsurface map surface={} root={} x={} y={} w={} h={}",
                     it.id.id(), root.id(), new_rect.x, new_rect.y, new_rect.width,
                     new_rect.height);
        }
        const bool moved = created || !rect_same(e->last_rect, new_rect);
        if (moved)
            changed = true;
        e->last_rect = new_rect;
        if (moved && rect_hook_)
            rect_hook_(rect_user_, it.id, new_rect);
    }

    // 4. Paint order: underlay, then the flattened tree.
    unsigned index = 0;
    if (r->underlay) {
        if (box.child_at(0) != r->underlay)
            (void)box.move_to(r->underlay, 0);
        index = 1;
    }
    for (unsigned i = 0; i < paint_.size(); ++i) {
        scene::surface_node* n = surfaces_->node(paint_[i].id);
        if (!n)
            continue;
        if (box.child_at(index) != n) {
            (void)box.move_to(n, index);
            // A restacked node covers or uncovers its siblings inside its own rect.
            if (scene_)
                scene_->note_damage(surfaces_->node_rect(paint_[i].id));
            changed = true;
        }
        ++index;
    }

    if (changed) {
        surfaces_->note_layout_changed();
        log_tree(root, cause);
    }
}

void lx::compositor::surface_tree::bbox_walk(const entry& e, lx::point2i origin, unsigned depth,
                                             long long& x0, long long& y0, long long& x1,
                                             long long& y1, bool& any) const {
    for (unsigned i = 0; i < e.order.count; ++i) {
        const lx::surface_id sid = e.order.ids[i];
        if (sid == e.id) {
            const lx::size2i size = surfaces_ ? surfaces_->surface_size(sid) : lx::size2i{};
            if (size.empty())
                continue;
            const long long ax = origin.x, ay = origin.y;
            const long long bx = ax + size.width, by = ay + size.height;
            if (!any) {
                x0 = ax, y0 = ay, x1 = bx, y1 = by;
                any = true;
            } else {
                x0 = ax < x0 ? ax : x0;
                y0 = ay < y0 ? ay : y0;
                x1 = bx > x1 ? bx : x1;
                y1 = by > y1 ? by : y1;
            }
            continue;
        }
        const entry* c = find(sid);
        if (!c || !c->linked || !(c->parent == e.id) || !c->has_buffer ||
            depth >= k_tree_max_depth)
            continue;
        bbox_walk(*c, {origin.x + c->pos.x, origin.y + c->pos.y}, depth + 1, x0, y0, x1, y1, any);
    }
}

lx::rect2i lx::compositor::surface_tree::subtree_bbox(lx::surface_id root) const {
    const entry* r = find(root);
    if (!r || !r->has_buffer)
        return {};
    long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    bbox_walk(*r, {0, 0}, 0, x0, y0, x1, y1, any);
    if (!any)
        return {};
    constexpr long long lo = -1'000'000'000LL, hi = 1'000'000'000LL;
    const auto clamp = [](long long v) { return v < lo ? lo : (v > hi ? hi : v); };
    x0 = clamp(x0), y0 = clamp(y0), x1 = clamp(x1), y1 = clamp(y1);
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

lx::scene::container_node* lx::compositor::surface_tree::container(lx::surface_id root) const {
    const entry* e = find(root);
    return e ? e->container : nullptr;
}

void lx::compositor::surface_tree::damage_root(lx::surface_id root) const {
    const entry* e = find(root);
    if (!e || !e->root_mapped || !e->container || !surfaces_ || !scene_)
        return;
    for (unsigned i = 0; i < e->container->child_count(); ++i) {
        scene::scene_node* n = e->container->child_at(i);
        if (!n || n == e->underlay || n->kind() != scene::node_kind::surface)
            continue;
        scene_->note_damage(surfaces_->node_rect(static_cast<scene::surface_node*>(n)->surface()));
    }
}

void lx::compositor::surface_tree::set_root_dim(lx::surface_id root, bool dim) {
    entry* e = find(root);
    if (!e || e->dim == dim)
        return;
    e->dim = dim;
    if (!e->root_mapped || !e->container || !surfaces_)
        return;
    for (unsigned i = 0; i < e->container->child_count(); ++i) {
        scene::scene_node* n = e->container->child_at(i);
        if (!n || n == e->underlay || n->kind() != scene::node_kind::surface)
            continue;
        n->set_tint(dim ? k_dim_tint : k_no_tint);
    }
    damage_root(root);
    if (debug()) {
        log_line(k_scene_category, "tree root={} dim={}", root.id(), dim ? 1 : 0);
    }
}

bool lx::compositor::surface_tree::root_dimmed(lx::surface_id root) const {
    const entry* e = find(root);
    return e && e->dim;
}

lx::rect2i lx::compositor::surface_tree::node_rect(lx::surface_id id) const {
    const entry* e = find(id);
    return e ? e->last_rect : lx::rect2i{};
}

bool lx::compositor::surface_tree::hit(lx::surface_id root, double gx, double gy,
                                       tree_hit& out) const {
    const entry* e = find(root);
    if (!e || !e->root_mapped || !e->container || !surfaces_)
        return false;
    for (unsigned i = e->container->child_count(); i-- > 0;) {
        scene::scene_node* n = e->container->child_at(i);
        if (!n || n == e->underlay || n->kind() != scene::node_kind::surface)
            continue;
        const lx::surface_id sid = static_cast<scene::surface_node*>(n)->surface();
        double sx = 0.0;
        double sy = 0.0;
        if (surfaces_->hit(sid, gx, gy, sx, sy)) {
            out = {sid, sx, sy};
            return true;
        }
    }
    return false;
}

void lx::compositor::surface_tree::log_tree(lx::surface_id root, const char* cause) const {
    if (!debug())
        return;
    const entry* e = find(root);
    if (!e)
        return;
    // Fixed storage: up to 256 nodes at ~40 bytes each.
    static char line[12288];
    int n = std::snprintf(line, sizeof(line), "tree root=%llu kind=%s id=%llu cause=%s nodes=",
                          root.id(), kind_name(e->kind), e->tag, cause ? cause : "-");
    unsigned written = 0;
    if (e->root_mapped && e->container && surfaces_) {
        for (unsigned i = 0; i < e->container->child_count() && n > 0 &&
                             static_cast<unsigned>(n) < sizeof(line) - 64;
             ++i) {
            scene::scene_node* node = e->container->child_at(i);
            if (!node || node == e->underlay || node->kind() != scene::node_kind::surface)
                continue;
            const lx::surface_id sid = static_cast<scene::surface_node*>(node)->surface();
            const lx::rect2i r = surfaces_->node_rect(sid);
            const int w = std::snprintf(line + n, sizeof(line) - static_cast<unsigned>(n),
                                        "%s%llu%s@%d,%d,%dx%d", written ? ";" : "", sid.id(),
                                        sid == root ? "*" : "", r.x, r.y, r.width, r.height);
            if (w > 0)
                n += w;
            ++written;
        }
    }
    if (written == 0 && n > 0 && static_cast<unsigned>(n) < sizeof(line) - 2)
        std::snprintf(line + n, sizeof(line) - static_cast<unsigned>(n), "-");
    log_line(k_scene_category, line);
}
