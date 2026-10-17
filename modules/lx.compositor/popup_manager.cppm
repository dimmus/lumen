module;

#include <cstdio>

import lx.foundation;
import lx.trace;
import lx.scene;

export module lx.compositor:popup;

import :positioner;
import :surface;
import :surface_tree;
import :toplevel;
import :layer_shell;

export namespace lx::compositor {

/// What a popup's parent is. `none` only for a popup created with a null parent, which a
/// layer surface adopts before the popup's initial commit (layer-shell, step D).
enum class popup_parent_kind : unsigned char { none, toplevel, popup, layer };

/// One xdg_popup (keyed by its wl_surface's id — one role per surface). UI affinity.
struct popup_record {
    lx::surface_id surface{};
    lx::client_id client{};
    /// `wl_client*` (void*: no Wayland types in this partition).
    void* client_native = nullptr;
    void* surface_res = nullptr;
    void* xdg_surface_res = nullptr;
    void* popup_res = nullptr;
    popup_parent_kind parent_kind = popup_parent_kind::none;
    /// The parent's wl_surface; null once the parent is gone.
    lx::surface_id parent{};
    /// The toplevel at the root of the chain (click-to-focus target, activation owner);
    /// null for a chain rooted at a layer surface.
    lx::toplevel_id owner{};
    positioner_rules rules{};
    /// Applied placement, relative to the parent's window geometry.
    lx::rect2i rel{};
    /// Last placement put on the wire.
    lx::rect2i rel_sent{};
    static constexpr unsigned k_sent_cap = 8;
    /// Sent, not yet acked, oldest first: serial and the placement it carried.
    unsigned sent[k_sent_cap]{};
    lx::rect2i sent_rect[k_sent_cap]{};
    unsigned sent_count = 0;
    /// Highest serial dropped from the ring on overflow; acks up to it are tolerated.
    unsigned dropped_upto = 0;
    unsigned acked = 0;
    lx::rect2i acked_rect{};
    bool ack_unapplied = false;
    bool ever_acked = false;
    /// Double-buffered xdg_surface.set_window_geometry.
    lx::rect2i window_geometry_pending{};
    bool window_geometry_pending_set = false;
    lx::rect2i window_geometry_requested{};
    bool window_geometry_set = false;
    /// Effective window-geometry origin in surface coordinates.
    lx::point2i geometry_offset{};
    /// Global window-geometry origin while mapped.
    lx::point2i origin{};
    /// xdg_popup.reposition token still owed a `repositioned` before the next configure
    /// (a reposition before the initial commit is folded into the first configure).
    unsigned pending_token = 0;
    bool pending_token_set = false;
    unsigned long long created_seq = 0;
    bool initial_commit_seen = false;
    bool mapped = false;
    /// xdg_popup.grab was requested (an explicit grab, even if denied).
    bool grab_requested = false;
    /// On the grab stack.
    bool grabbing = false;
    /// popup_done sent: inert until destroyed; commits release their buffers undrawn.
    bool dismissed = false;
    bool used = false;
};

/// Wire operations, installed by the protocol layer (no Wayland types here).
struct popup_ops {
    /// `repositioned(token)` when `repositioned`, then `xdg_popup.configure(rel)` and
    /// `xdg_surface.configure(serial)`.
    void (*send_configure)(void* popup_res, void* xdg_surface_res, lx::rect2i rel,
                           unsigned serial, bool repositioned, unsigned token) = nullptr;
    void (*send_popup_done)(void* popup_res) = nullptr;
    unsigned (*next_serial)(void* user) = nullptr;
    /// False for a client being torn down: nothing may be sent to it (X7).
    bool (*client_alive)(void* client_native) = nullptr;
    void* user = nullptr;
};

/// What the popup manager needs from the window manager, as callbacks (the window manager
/// sits above this partition).
struct popup_host {
    /// Global rectangle a popup rooted at `root` (a toplevel's or layer surface's
    /// `root_surface`) is kept inside: the usable area of its window's output for window
    /// popups, the whole output for layer-surface popups.
    lx::rect2i (*constraint_area)(void* user, popup_parent_kind root,
                                  lx::surface_id root_surface) = nullptr;
    /// The grab stack or a grabbing popup's mapping changed: keyboard focus is re-resolved.
    void (*focus_changed)(void* user) = nullptr;
    void* user = nullptr;
};

/// What a commit of a popup's surface carried, after its tree was applied.
struct popup_commit_info {
    bool has_buffer = false;
    /// Effective window geometry, surface-local (`effective_window_geometry`).
    lx::rect2i window_geometry{};
};

/// xdg_popup state, placement, configure flow, the popup grab and dismissal. UI affinity,
/// Wayland-free: the protocol layer validates requests and moves wl_surface state; this
/// decides where each popup goes, when it maps and who gets popup_done.
///
/// Every popup is the root of its own surface tree, with its container in the scene's
/// popup layer (above every window and layer surface, below the cursor), appended on map:
/// "stacked on top of all previously created popups".
class popup_manager {
public:
    static constexpr unsigned k_max = 128;
    static constexpr unsigned k_max_grab = 16;

    void bind(toplevel_manager& toplevels, surface_manager& surfaces, surface_tree& tree,
              scene::container_node& popup_layer);
    void set_ops(popup_ops ops) { ops_ = ops; }
    void set_host(popup_host host) { host_ = host; }
    /// Layer-shell (optional): popups whose parent is a layer surface are placed against it.
    void set_layers(const layer_shell* layers) { layers_ = layers; }

    enum class create_error : unsigned char { none, no_memory };
    /// xdg_surface.get_popup, after the protocol layer validated roles, parent and
    /// positioner. Makes the surface a popup root.
    [[nodiscard]] create_error create(lx::surface_id surface, lx::client_id client,
                                      void* client_native, void* surface_res,
                                      void* xdg_surface_res, void* popup_res,
                                      popup_parent_kind parent_kind, lx::surface_id parent,
                                      lx::toplevel_id owner, const positioner_rules& rules);
    [[nodiscard]] popup_record* find(lx::surface_id surface);
    [[nodiscard]] const popup_record* find(lx::surface_id surface) const;
    /// zwlr_layer_surface_v1.get_popup: a popup created with a null parent adopts the layer
    /// surface, before its initial commit. False when it already has a parent or made its
    /// initial commit (the protocol layer posts the error).
    [[nodiscard]] bool set_layer_parent(lx::surface_id popup, lx::surface_id layer);
    /// The layer surface at the root of the popup's chain, or null (a window's chain).
    [[nodiscard]] lx::surface_id root_layer(const popup_record& rec) const;
    /// Content committed to the popup's surface is imported and drawn (until dismissed).
    [[nodiscard]] bool drawable(lx::surface_id surface) const;

    // ── configure / commit ──
    enum class ack_result : unsigned char { ok, duplicate, invalid };
    [[nodiscard]] ack_result on_ack(lx::surface_id surface, unsigned serial);
    /// The pending window geometry becomes the requested one (wl_surface.commit).
    void apply_pending_state(lx::surface_id surface);
    /// After the surface's tree was applied: the initial commit sends the first configure;
    /// later ones apply an acked placement (position changes only here, so a reposition is
    /// frame-perfect with the content drawn for it), map, unmap and move.
    void on_commit(lx::surface_id surface, const popup_commit_info& info);
    /// xdg_popup.reposition (v3): new rules, `repositioned(token)` + configure now (or, before
    /// the initial commit, with the first configure). Applied at the commit after the ack.
    void reposition(lx::surface_id surface, const positioner_rules& rules, unsigned token);

    // ── grab ──
    enum class grab_result : unsigned char { ok, denied, invalid_grab, invalid_parent,
                                             not_topmost };
    /// xdg_popup.grab. `serial_ok`: the serial is this client's last press (button or key);
    /// `wm_grab`: an interactive move/resize or a drag is running. A denied grab dismisses
    /// the popup at once; the errors are the caller's to post.
    [[nodiscard]] grab_result grab(lx::surface_id surface, bool serial_ok, bool wm_grab);
    [[nodiscard]] bool grab_active() const { return grab_count_ > 0; }
    [[nodiscard]] lx::client_id grab_client() const { return grab_client_; }
    /// The topmost mapped grabbing popup — keyboard focus while a grab is active — or null.
    [[nodiscard]] const popup_record* keyboard_target() const;
    /// Dismisses every grabbing popup and their descendants, newest first.
    void dismiss_all(const char* reason);

    // ── lifetime ──
    /// A live xdg_popup has this popup as its parent (destroying it is
    /// not_the_topmost_popup).
    [[nodiscard]] bool has_live_children(lx::surface_id surface) const;
    /// Any of the popup's objects is going away: unmapped, off the grab stack, record freed.
    /// Idempotent. Children still alive (a dying client) are dismissed and orphaned.
    void destroy(lx::surface_id surface);
    /// A parent (toplevel or popup) was unmapped or destroyed: every descendant popup is
    /// dismissed top-down; with `destroyed`, its children lose their parent.
    void on_parent_unmapped(lx::surface_id parent, const char* reason);
    void on_parent_destroyed(lx::surface_id parent);
    /// The parent's global window geometry moved or resized: its popups follow (and, for
    /// reactive ones, are placed again), recursively.
    void on_parent_moved(lx::surface_id parent);

    // ── hit test ──
    /// Topmost mapped popup surface (any surface of its tree) under the global point.
    [[nodiscard]] bool hit(double gx, double gy, tree_hit& out, const popup_record*& rec) const;
    [[nodiscard]] unsigned mapped_count() const { return order_count_; }

private:
    [[nodiscard]] int slot_of(lx::surface_id surface) const;
    /// The parent's global window geometry; false when the parent is not mapped (or gone).
    [[nodiscard]] bool parent_geometry(const popup_record& rec, lx::rect2i& out) const;
    [[nodiscard]] popup_parent_kind root_kind(const popup_record& rec) const;
    /// Constraint rect relative to the parent's window geometry, honouring
    /// set_parent_size / set_parent_configure for a parent toplevel mid-resize.
    [[nodiscard]] lx::rect2i constraint_for(const popup_record& rec) const;
    void send_place(popup_record& rec, const char* reason, bool repositioned, unsigned token);
    void map(popup_record& rec);
    void unmap(popup_record& rec, const char* reason);
    /// Recomputes the global origin from the parent and moves the tree. True when it moved.
    bool place_root(popup_record& rec);
    void dismiss_one(popup_record& rec, const char* reason);
    void dismiss_subtree(popup_record& rec, const char* reason, unsigned depth);
    void dismiss_children(lx::surface_id parent, const char* reason, unsigned depth);
    void stack_remove(lx::surface_id surface);
    void order_remove(unsigned slot);
    void notify_focus();
    /// Keyboard focus is re-resolved once for a whole cascade (a chain dismissed top-down),
    /// not once per popup: the client would see an enter/leave pair for every level.
    void hold_focus() { ++focus_hold_; }
    void release_focus();
    void log_line(const char* line) const;

    toplevel_manager* toplevels_ = nullptr;
    surface_manager* surfaces_ = nullptr;
    surface_tree* tree_ = nullptr;
    scene::container_node* layer_ = nullptr;
    const layer_shell* layers_ = nullptr;
    popup_ops ops_{};
    popup_host host_{};
    popup_record records_[k_max]{};
    id_slot_index<k_max * 2> index_{};
    /// Mapped popups' slots, bottom→top (the popup layer's order).
    unsigned order_[k_max]{};
    unsigned order_count_ = 0;
    /// Grabbing popups, bottom→top; all of one client.
    lx::surface_id grab_stack_[k_max_grab]{};
    unsigned grab_count_ = 0;
    lx::client_id grab_client_{};
    unsigned long long next_seq_ = 1;
    unsigned focus_hold_ = 0;
    bool focus_dirty_ = false;
};

} // namespace lx::compositor

namespace {

constexpr const char* k_popup_category = "compositor.popup";

[[nodiscard]] bool popup_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] const char* axes(unsigned bits) {
    switch (bits & 3u) {
    case 1: return "x";
    case 2: return "y";
    case 3: return "xy";
    default: return "-";
    }
}

[[nodiscard]] const char* parent_kind_name(lx::compositor::popup_parent_kind k) {
    switch (k) {
    case lx::compositor::popup_parent_kind::toplevel: return "toplevel";
    case lx::compositor::popup_parent_kind::popup: return "popup";
    case lx::compositor::popup_parent_kind::layer: return "layer";
    case lx::compositor::popup_parent_kind::none: break;
    }
    return "none";
}

[[nodiscard]] bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

void lx::compositor::popup_manager::bind(toplevel_manager& toplevels, surface_manager& surfaces,
                                         surface_tree& tree, scene::container_node& popup_layer) {
    toplevels_ = &toplevels;
    surfaces_ = &surfaces;
    tree_ = &tree;
    layer_ = &popup_layer;
}

void lx::compositor::popup_manager::log_line(const char* line) const {
    lx::trace::logger::global().log(lx::trace::level::debug, k_popup_category, line);
}

int lx::compositor::popup_manager::slot_of(lx::surface_id surface) const {
    return index_.find(surface.id());
}

lx::compositor::popup_record* lx::compositor::popup_manager::find(lx::surface_id surface) {
    const int slot = slot_of(surface);
    return slot >= 0 ? &records_[static_cast<unsigned>(slot)] : nullptr;
}

const lx::compositor::popup_record*
lx::compositor::popup_manager::find(lx::surface_id surface) const {
    const int slot = slot_of(surface);
    return slot >= 0 ? &records_[static_cast<unsigned>(slot)] : nullptr;
}

bool lx::compositor::popup_manager::drawable(lx::surface_id surface) const {
    const popup_record* r = find(surface);
    return r && !r->dismissed;
}

lx::compositor::popup_manager::create_error
lx::compositor::popup_manager::create(lx::surface_id surface, lx::client_id client,
                                      void* client_native, void* surface_res,
                                      void* xdg_surface_res, void* popup_res,
                                      popup_parent_kind parent_kind, lx::surface_id parent,
                                      lx::toplevel_id owner, const positioner_rules& rules) {
    if (find(surface))
        return create_error::no_memory;
    for (unsigned i = 0; i < k_max; ++i) {
        if (records_[i].used)
            continue;
        if (!tree_ || !tree_->make_root(surface, root_kind::popup) ||
            !index_.insert(surface.id(), static_cast<int>(i)))
            return create_error::no_memory;
        popup_record& r = records_[i];
        r = {};
        r.used = true;
        r.surface = surface;
        r.client = client;
        r.client_native = client_native;
        r.surface_res = surface_res;
        r.xdg_surface_res = xdg_surface_res;
        r.popup_res = popup_res;
        r.parent_kind = parent_kind;
        r.parent = parent;
        r.owner = owner;
        r.rules = rules;
        r.created_seq = next_seq_++;
        tree_->set_root_tag(surface, 0);
        if (popup_debug()) {
            char line[160];
            std::snprintf(line, sizeof(line), "popup create surface=%llu parent=%llu parent_kind=%s",
                          surface.id(), parent.id(), parent_kind_name(parent_kind));
            log_line(line);
        }
        return create_error::none;
    }
    return create_error::no_memory;
}

bool lx::compositor::popup_manager::parent_geometry(const popup_record& rec,
                                                    lx::rect2i& out) const {
    switch (rec.parent_kind) {
    case popup_parent_kind::toplevel: {
        const toplevel_record* t = toplevels_ ? toplevels_->find_by_surface(rec.parent) : nullptr;
        if (!t)
            return false;
        out = t->geometry;
        return t->mapped;
    }
    case popup_parent_kind::popup: {
        const popup_record* p = find(rec.parent);
        if (!p)
            return false;
        out = {p->origin.x, p->origin.y, p->rel.width, p->rel.height};
        return p->mapped;
    }
    case popup_parent_kind::layer: {
        // A layer surface has no window geometry: the popup is placed against its surface.
        const layer_record* l = layers_ ? layers_->find(rec.parent) : nullptr;
        if (!l)
            return false;
        out = l->box;
        return l->mapped;
    }
    case popup_parent_kind::none:
        break;
    }
    return false;
}

bool lx::compositor::popup_manager::set_layer_parent(lx::surface_id popup, lx::surface_id layer) {
    popup_record* r = find(popup);
    if (!r || r->parent_kind != popup_parent_kind::none || r->initial_commit_seen || !layer)
        return false;
    r->parent_kind = popup_parent_kind::layer;
    r->parent = layer;
    r->owner = {};
    if (popup_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "popup parent surface=%llu parent=%llu parent_kind=layer",
                      popup.id(), layer.id());
        log_line(line);
    }
    return true;
}

lx::surface_id lx::compositor::popup_manager::root_layer(const popup_record& rec) const {
    const popup_record* r = &rec;
    for (unsigned depth = 0; r && r->parent_kind == popup_parent_kind::popup && depth < k_max;
         ++depth)
        r = find(r->parent);
    return r && r->parent_kind == popup_parent_kind::layer ? r->parent : lx::surface_id{};
}

lx::compositor::popup_parent_kind
lx::compositor::popup_manager::root_kind(const popup_record& rec) const {
    const popup_record* r = &rec;
    for (unsigned depth = 0; r && r->parent_kind == popup_parent_kind::popup && depth < k_max;
         ++depth)
        r = find(r->parent);
    return r ? r->parent_kind : popup_parent_kind::none;
}

lx::rect2i lx::compositor::popup_manager::constraint_for(const popup_record& rec) const {
    // The chain's root: the toplevel or layer surface the first popup hangs off.
    const popup_record* root = &rec;
    for (unsigned depth = 0; root && root->parent_kind == popup_parent_kind::popup && depth < k_max;
         ++depth)
        root = find(root->parent);
    lx::rect2i area = host_.constraint_area
                          ? host_.constraint_area(host_.user, root_kind(rec),
                                                  root ? root->parent : lx::surface_id{})
                          : lx::rect2i{};
    lx::rect2i parent{};
    (void)parent_geometry(rec, parent);
    lx::point2i origin{parent.x, parent.y};
    // set_parent_size + set_parent_configure (v3): the popup is placed for a parent
    // configure not applied yet. Only an anchored resize (from the left or top edge) moves
    // the parent's origin; that origin is where the anchor puts the future size.
    if (rec.parent_kind == popup_parent_kind::toplevel && rec.rules.parent_configure_set &&
        rec.rules.parent_size_set && toplevels_) {
        if (const toplevel_record* t = toplevels_->find_by_surface(rec.parent)) {
            const toplevel_configure* match = nullptr;
            for (unsigned i = 0; i < t->sent_count; ++i)
                if (t->sent[i].serial == rec.rules.parent_configure)
                    match = &t->sent[i];
            if (!match && t->acked_unapplied && t->acked.serial == rec.rules.parent_configure)
                match = &t->acked;
            if (match) {
                if (match->anchor_edges & edge_left)
                    origin.x = match->anchor.x + match->anchor.width - rec.rules.parent_size.width;
                if (match->anchor_edges & edge_top)
                    origin.y = match->anchor.y + match->anchor.height - rec.rules.parent_size.height;
            }
        }
    }
    return {area.x - origin.x, area.y - origin.y, area.width, area.height};
}

void lx::compositor::popup_manager::send_place(popup_record& rec, const char* reason,
                                               bool repositioned, unsigned token) {
    if (!ops_.send_configure || !rec.popup_res || !rec.xdg_surface_res)
        return;
    const lx::rect2i constraint = constraint_for(rec);
    const positioner_result placed = positioner_place(rec.rules, constraint);
    const unsigned serial = ops_.next_serial ? ops_.next_serial(ops_.user) : rec.acked + 1;
    if (rec.sent_count == popup_record::k_sent_cap) {
        rec.dropped_upto = rec.sent[0];
        for (unsigned i = 1; i < rec.sent_count; ++i) {
            rec.sent[i - 1] = rec.sent[i];
            rec.sent_rect[i - 1] = rec.sent_rect[i];
        }
        --rec.sent_count;
    }
    rec.sent[rec.sent_count] = serial;
    rec.sent_rect[rec.sent_count] = placed.rect;
    ++rec.sent_count;
    rec.rel_sent = placed.rect;
    ops_.send_configure(rec.popup_res, rec.xdg_surface_res, placed.rect, serial, repositioned,
                        token);
    if (popup_debug()) {
        char line[384];
        std::snprintf(line, sizeof(line),
                      "popup position surface=%llu parent=%llu x=%d y=%d w=%d h=%d anchor=%u "
                      "gravity=%u adjust=%u flipped=%s slid=%s resized=%s constraint=%d,%d,%dx%d "
                      "serial=%u reason=%s token=%u",
                      rec.surface.id(), rec.parent.id(), placed.rect.x, placed.rect.y,
                      placed.rect.width, placed.rect.height, rec.rules.anchor, rec.rules.gravity,
                      rec.rules.adjustment, axes(placed.flipped), axes(placed.slid),
                      axes(placed.resized), constraint.x, constraint.y, constraint.width,
                      constraint.height, serial, reason, token);
        log_line(line);
    }
}

lx::compositor::popup_manager::ack_result
lx::compositor::popup_manager::on_ack(lx::surface_id surface, unsigned serial) {
    popup_record* r = find(surface);
    if (!r)
        return ack_result::duplicate;
    for (unsigned i = 0; i < r->sent_count; ++i) {
        if (r->sent[i] != serial)
            continue;
        // Acking a serial acks every older one too (xdg_surface.ack_configure).
        r->acked = serial;
        r->acked_rect = r->sent_rect[i];
        const unsigned popped = i + 1;
        for (unsigned j = popped; j < r->sent_count; ++j) {
            r->sent[j - popped] = r->sent[j];
            r->sent_rect[j - popped] = r->sent_rect[j];
        }
        r->sent_count -= popped;
        r->ack_unapplied = true;
        r->ever_acked = true;
        if (popup_debug()) {
            char line[96];
            std::snprintf(line, sizeof(line), "popup ack surface=%llu serial=%u", surface.id(),
                          serial);
            log_line(line);
        }
        return ack_result::ok;
    }
    if ((r->acked != 0 && static_cast<int>(serial - r->acked) <= 0) ||
        (r->dropped_upto != 0 && static_cast<int>(serial - r->dropped_upto) <= 0))
        return ack_result::duplicate;
    return ack_result::invalid;
}

void lx::compositor::popup_manager::apply_pending_state(lx::surface_id surface) {
    popup_record* r = find(surface);
    if (!r || !r->window_geometry_pending_set)
        return;
    r->window_geometry_requested = r->window_geometry_pending;
    r->window_geometry_set = true;
    r->window_geometry_pending_set = false;
}

bool lx::compositor::popup_manager::place_root(popup_record& rec) {
    lx::rect2i parent{};
    if (!parent_geometry(rec, parent))
        return false;
    const lx::point2i origin{parent.x + rec.rel.x, parent.y + rec.rel.y};
    const bool moved = origin.x != rec.origin.x || origin.y != rec.origin.y;
    rec.origin = origin;
    tree_->set_root_origin(rec.surface,
                           {origin.x - rec.geometry_offset.x, origin.y - rec.geometry_offset.y});
    return moved;
}

void lx::compositor::popup_manager::map(popup_record& rec) {
    lx::rect2i parent{};
    if (!parent_geometry(rec, parent)) {
        if (popup_debug()) {
            char line[96];
            std::snprintf(line, sizeof(line), "popup map-deferred surface=%llu reason=parent-unmapped",
                          rec.surface.id());
            log_line(line);
        }
        return;
    }
    rec.origin = {parent.x + rec.rel.x, parent.y + rec.rel.y};
    tree_->set_root_origin(rec.surface, {rec.origin.x - rec.geometry_offset.x,
                                         rec.origin.y - rec.geometry_offset.y});
    if (!layer_ || !tree_->map_root(rec.surface, *layer_, ~0u)) {
        lx::trace::logger::global().log(lx::trace::level::warn, k_popup_category,
                                        "popup map failed: popup layer full");
        return;
    }
    rec.mapped = true;
    const int slot = slot_of(rec.surface);
    if (slot >= 0 && order_count_ < k_max)
        order_[order_count_++] = static_cast<unsigned>(slot);
    if (popup_debug()) {
        char line[160];
        std::snprintf(line, sizeof(line), "popup map surface=%llu x=%d y=%d w=%d h=%d",
                      rec.surface.id(), rec.origin.x, rec.origin.y, rec.rel.width,
                      rec.rel.height);
        log_line(line);
    }
    if (rec.grabbing)
        notify_focus();
}

void lx::compositor::popup_manager::order_remove(unsigned slot) {
    for (unsigned i = 0; i < order_count_; ++i) {
        if (order_[i] != slot)
            continue;
        for (unsigned j = i + 1; j < order_count_; ++j)
            order_[j - 1] = order_[j];
        --order_count_;
        return;
    }
}

void lx::compositor::popup_manager::unmap(popup_record& rec, const char* reason) {
    if (!rec.mapped)
        return;
    // Children first: none of them may stay on screen without the popup they hang off.
    dismiss_children(rec.surface, "parent-unmapped", 0);
    rec.mapped = false;
    tree_->unmap_root(rec.surface);
    const int slot = slot_of(rec.surface);
    if (slot >= 0)
        order_remove(static_cast<unsigned>(slot));
    if (popup_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "popup unmap surface=%llu reason=%s", rec.surface.id(),
                      reason);
        log_line(line);
    }
    if (rec.grabbing)
        notify_focus();
}

void lx::compositor::popup_manager::on_commit(lx::surface_id surface,
                                              const popup_commit_info& info) {
    popup_record* r = find(surface);
    if (!r || r->dismissed)
        return;
    if (!r->initial_commit_seen) {
        // The initial commit (no buffer: the prechecks rejected one) asks for the first
        // configure, sent in the same dispatch.
        r->initial_commit_seen = true;
        const bool owed = r->pending_token_set;
        r->pending_token_set = false;
        send_place(*r, owed ? "reposition" : "initial", owed, r->pending_token);
        return;
    }
    r->geometry_offset = {info.window_geometry.x, info.window_geometry.y};
    if (r->ack_unapplied) {
        r->ack_unapplied = false;
        r->rel = r->acked_rect;
    }
    // The window geometry size is what children anchor to; the position is the compositor's.
    if (info.window_geometry.width > 0 && info.window_geometry.height > 0) {
        r->rel.width = info.window_geometry.width;
        r->rel.height = info.window_geometry.height;
    }

    if (!r->mapped) {
        if (info.has_buffer && info.window_geometry.width > 0 && info.window_geometry.height > 0)
            map(*r);
        return;
    }
    if (!info.has_buffer) {
        unmap(*r, "null-buffer");
        // "Unmapping … returns it to the state it had right after get_popup": a new initial
        // commit and configure follow. Off the grab stack: the grab went with the mapping.
        if (r->grabbing) {
            stack_remove(r->surface);
            notify_focus();
        }
        const positioner_rules rules = r->rules;
        r->initial_commit_seen = false;
        r->ever_acked = false;
        r->sent_count = 0;
        r->dropped_upto = 0;
        r->acked = 0;
        r->ack_unapplied = false;
        r->grab_requested = false;
        r->rules = rules;
        return;
    }
    if (place_root(*r))
        on_parent_moved(r->surface);
}

void lx::compositor::popup_manager::reposition(lx::surface_id surface,
                                               const positioner_rules& rules, unsigned token) {
    popup_record* r = find(surface);
    if (!r || r->dismissed)
        return;
    r->rules = rules;
    if (!r->initial_commit_seen) {
        r->pending_token = token;
        r->pending_token_set = true;
        return;
    }
    send_place(*r, "reposition", true, token);
}

// ── grab ───────────────────────────────────────────────────────────────────

void lx::compositor::popup_manager::notify_focus() {
    if (focus_hold_ > 0) {
        focus_dirty_ = true;
        return;
    }
    if (host_.focus_changed)
        host_.focus_changed(host_.user);
}

void lx::compositor::popup_manager::release_focus() {
    if (focus_hold_ > 0 && --focus_hold_ == 0 && focus_dirty_) {
        focus_dirty_ = false;
        notify_focus();
    }
}

void lx::compositor::popup_manager::stack_remove(lx::surface_id surface) {
    for (unsigned i = 0; i < grab_count_; ++i) {
        if (!(grab_stack_[i] == surface))
            continue;
        for (unsigned j = i + 1; j < grab_count_; ++j)
            grab_stack_[j - 1] = grab_stack_[j];
        grab_stack_[--grab_count_] = {};
        if (popup_record* r = find(surface))
            r->grabbing = false;
        break;
    }
    // "If the topmost grabbing popup is destroyed, the grab will be returned to the parent
    // of the popup": the new top. An empty stack ends the grab.
    if (grab_count_ == 0)
        grab_client_ = {};
}

lx::compositor::popup_manager::grab_result
lx::compositor::popup_manager::grab(lx::surface_id surface, bool serial_ok, bool wm_grab) {
    popup_record* r = find(surface);
    if (!r || r->dismissed)
        return grab_result::denied; // inert: dismissed popups ignore requests
    if (r->mapped)
        return grab_result::invalid_grab;
    const char* deny = nullptr;
    if (r->parent_kind == popup_parent_kind::popup) {
        const popup_record* p = find(r->parent);
        if (p && !p->grab_requested)
            return grab_result::invalid_parent;
        if (p && p->grabbing && !(grab_count_ > 0 && grab_stack_[grab_count_ - 1] == p->surface))
            return grab_result::not_topmost;
        if (!p || p->dismissed)
            deny = "parent-dismissed";
    }
    r->grab_requested = true;
    if (!deny && !serial_ok)
        deny = "serial";
    if (!deny && wm_grab)
        deny = "wm-grab";
    if (!deny && grab_count_ >= k_max_grab)
        deny = "stack-full";
    if (deny) {
        if (popup_debug()) {
            char line[128];
            std::snprintf(line, sizeof(line), "popup grab surface=%llu result=denied reason=%s",
                          surface.id(), deny);
            log_line(line);
        }
        dismiss_one(*r, "grab-denied");
        return grab_result::denied;
    }
    // One client's grab at a time: a new client's grab ends the other's chain first.
    if (grab_count_ > 0 && !(grab_client_ == r->client))
        dismiss_all("replaced");
    grab_stack_[grab_count_++] = surface;
    grab_client_ = r->client;
    r->grabbing = true;
    if (popup_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "popup grab surface=%llu result=ok reason=-",
                      surface.id());
        log_line(line);
    }
    notify_focus();
    return grab_result::ok;
}

const lx::compositor::popup_record* lx::compositor::popup_manager::keyboard_target() const {
    for (unsigned i = grab_count_; i-- > 0;) {
        const popup_record* r = find(grab_stack_[i]);
        if (r && r->mapped && !r->dismissed)
            return r;
    }
    return nullptr;
}

void lx::compositor::popup_manager::dismiss_one(popup_record& rec, const char* reason) {
    if (rec.dismissed)
        return;
    rec.dismissed = true;
    if (popup_debug()) {
        char line[128];
        std::snprintf(line, sizeof(line), "popup dismiss surface=%llu reason=%s",
                      rec.surface.id(), reason);
        log_line(line);
    }
    // "At the same time the surface will be unmapped."
    const bool alive = !ops_.client_alive || ops_.client_alive(rec.client_native);
    if (alive && ops_.send_popup_done && rec.popup_res)
        ops_.send_popup_done(rec.popup_res);
    if (rec.mapped)
        unmap(rec, "dismissed");
    if (rec.grabbing) {
        stack_remove(rec.surface);
        notify_focus();
    }
}

void lx::compositor::popup_manager::dismiss_children(lx::surface_id parent, const char* reason,
                                                     unsigned depth) {
    if (depth > k_max)
        return;
    // Newest first: the order a client must destroy them in, which dismissal follows.
    for (;;) {
        popup_record* newest = nullptr;
        for (auto& r : records_) {
            if (r.used && !r.dismissed && r.parent == parent &&
                r.parent_kind != popup_parent_kind::none &&
                (!newest || r.created_seq > newest->created_seq))
                newest = &r;
        }
        if (!newest)
            return;
        dismiss_subtree(*newest, reason, depth + 1);
    }
}

void lx::compositor::popup_manager::dismiss_subtree(popup_record& rec, const char* reason,
                                                    unsigned depth) {
    dismiss_children(rec.surface, reason, depth);
    dismiss_one(rec, reason);
}

void lx::compositor::popup_manager::dismiss_all(const char* reason) {
    if (grab_count_ == 0)
        return;
    hold_focus();
    while (grab_count_ > 0) {
        const lx::surface_id top = grab_stack_[grab_count_ - 1];
        popup_record* r = find(top);
        if (!r) {
            stack_remove(top);
            continue;
        }
        dismiss_subtree(*r, reason, 0);
        if (r->grabbing) // defensive: dismiss_one always takes it off the stack
            stack_remove(top);
    }
    grab_client_ = {};
    notify_focus();
    release_focus();
}

// ── lifetime ───────────────────────────────────────────────────────────────

bool lx::compositor::popup_manager::has_live_children(lx::surface_id surface) const {
    for (const auto& r : records_)
        if (r.used && r.parent == surface && r.parent_kind == popup_parent_kind::popup)
            return true;
    return false;
}

void lx::compositor::popup_manager::destroy(lx::surface_id surface) {
    popup_record* r = find(surface);
    if (!r)
        return;
    // Children can only still be here when the client is going away (otherwise the
    // destroy request was refused); they lose their parent either way.
    on_parent_destroyed(surface);
    const bool focus = r->grabbing || r->mapped;
    if (r->mapped) {
        r->mapped = false;
        tree_->unmap_root(surface);
        const int slot = slot_of(surface);
        if (slot >= 0)
            order_remove(static_cast<unsigned>(slot));
        if (popup_debug()) {
            char line[96];
            std::snprintf(line, sizeof(line), "popup unmap surface=%llu reason=destroyed",
                          surface.id());
            log_line(line);
        }
    }
    if (r->grabbing)
        stack_remove(surface);
    if (popup_debug()) {
        char line[64];
        std::snprintf(line, sizeof(line), "popup destroy surface=%llu", surface.id());
        log_line(line);
    }
    index_.erase(surface.id());
    *r = {};
    if (focus)
        notify_focus();
}

void lx::compositor::popup_manager::on_parent_unmapped(lx::surface_id parent, const char* reason) {
    hold_focus();
    dismiss_children(parent, reason, 0);
    release_focus();
}

void lx::compositor::popup_manager::on_parent_destroyed(lx::surface_id parent) {
    hold_focus();
    dismiss_children(parent, "parent-destroyed", 0);
    release_focus();
    for (auto& r : records_) {
        if (r.used && r.parent == parent) {
            r.parent = {};
            r.parent_kind = popup_parent_kind::none;
        }
    }
}

void lx::compositor::popup_manager::on_parent_moved(lx::surface_id parent) {
    // Bounded recursion: a chain is at most k_max deep, and each level is one popup.
    for (unsigned i = 0; i < order_count_; ++i) {
        popup_record& r = records_[order_[i]];
        if (!r.used || !r.mapped || !(r.parent == parent))
            continue;
        if (r.rules.reactive && !r.dismissed) {
            // A reactive popup is placed again for the parent's new position; it keeps its
            // current one until the client acks and commits that configure.
            const positioner_result placed = positioner_place(r.rules, constraint_for(r));
            if (!same_rect(placed.rect, r.rel_sent))
                send_place(r, "reactive", false, 0);
        }
        if (place_root(r)) {
            tree_->sync_scene(r.surface, "move");
            on_parent_moved(r.surface);
        }
    }
}

bool lx::compositor::popup_manager::hit(double gx, double gy, tree_hit& out,
                                        const popup_record*& rec) const {
    for (unsigned i = order_count_; i-- > 0;) {
        const popup_record& r = records_[order_[i]];
        if (!r.used || !r.mapped)
            continue;
        if (tree_->hit(r.surface, gx, gy, out)) {
            rec = &r;
            return true;
        }
    }
    return false;
}
