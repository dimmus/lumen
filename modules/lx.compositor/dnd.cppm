module;

import lx.foundation;
import lx.trace;
import lx.scene;

export module lx.compositor:dnd;

import std;

import :xcursor;
import :cursor;
import :surface_tree;
import :data_transfer;

export namespace lx::compositor {

/// Why a drag-and-drop operation ended (the `dnd end reason=` log line).
enum class dnd_end_reason : unsigned char {
    drop,
    no_target,
    not_accepted,
    no_action,
    escape,
    source_destroyed,
    client_gone
};

[[nodiscard]] const char* dnd_end_name(dnd_end_reason r);

/// The surface under the pointer during a drag, as the window manager hit it: a client
/// surface only (frames are not drop targets). Wayland-free: the resource is a `void*`.
struct dnd_target {
    void* surface_resource = nullptr;
    lx::surface_id surface{};
    lx::client_id client{};
    /// Surface-local pointer position.
    double sx = 0.0;
    double sy = 0.0;
};

/// The wire side of a drag (offers, negotiation, events), installed by wire_dnd.cpp so no
/// Wayland type appears here. All called on the UI thread, from the grab below.
struct dnd_ops {
    /// The pointer target changed (`surface_res` null: none). Leave the old target, offer
    /// and `enter` the new one.
    void (*focus)(void* user, void* surface_res, double sx, double sy) = nullptr;
    /// Motion over the current target, surface-local. Must not allocate or log.
    void (*motion)(void* user, unsigned time_ms, double sx, double sy) = nullptr;
    /// Every button released over the current target (or none). True when a drop was
    /// delivered; false with `why` set when the operation must be cancelled instead.
    bool (*drop)(void* user, dnd_end_reason& why) = nullptr;
    /// Cancel the operation: leave the target, offers inert, `cancelled` to the source.
    void (*cancel)(void* user, dnd_end_reason why) = nullptr;
    /// The user action chosen by the held modifiers changed (`dnd_action_bits`).
    void (*user_action)(void* user, unsigned action) = nullptr;
    void* user = nullptr;
};

/// The compositor's drag-and-drop grab: a third pointer mode beside the implicit grab and
/// the interactive move/resize, driven by the window manager's pointer path (which owns
/// hit-testing and the button mask) and by the wire (which owns objects and negotiation).
/// UI affinity. Wayland-free.
///
/// While active, no client has pointer focus: motion goes to the drag target as
/// `wl_data_device.motion`, buttons are withheld, the release drops or cancels. The icon is a
/// surface tree rooted in its own scene layer (above popups, below the cursor), moved once
/// per tick with the pointer — the interactive-grab pattern, so icon and cursor land in the
/// same commit. Nothing here allocates; the hot path (`on_motion`) is O(1) beyond the hit
/// test the caller already did.
class dnd_grab {
public:
    /// `tree` and `icon_layer` may be left unbound (tests): the icon is then only tracked.
    void bind(surface_tree& tree, scene::container_node& icon_layer, cursor_manager* cursor) {
        tree_ = &tree;
        icon_layer_ = &icon_layer;
        cursor_ = cursor;
    }
    void set_ops(dnd_ops ops) { ops_ = ops; }

    /// The wire validated start_drag. `self_only`: the source was null, so the operation
    /// stays within `origin_client` (enter/motion/drop only to its surfaces). `icon` may be
    /// empty; when set it must already be a `root_kind::dnd_icon` root. `px`/`py`: the pointer.
    void begin(lx::client_id origin_client, bool self_only, lx::surface_id icon, double px,
               double py);
    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] lx::surface_id icon() const { return icon_; }
    [[nodiscard]] lx::surface_id target() const { return target_; }
    [[nodiscard]] lx::client_id origin_client() const { return origin_; }
    /// The icon's global origin as last applied (pointer + accumulated offset).
    [[nodiscard]] lx::point2i icon_origin() const { return applied_; }
    [[nodiscard]] lx::point2i icon_offset() const { return offset_; }

    // ── From the window manager (UI affinity, O(1), no allocation) ──

    /// Pointer motion: `target` is what is under the pointer (null: nothing droppable).
    /// A change of target surface is a focus change; otherwise a motion to the target,
    /// unless `send_motion` is false (a re-hit after the scene changed under a still pointer).
    void on_motion(const dnd_target* target, double px, double py, unsigned time_ms,
                   bool send_motion = true);
    /// Every pointer button is up: drop on the target, or cancel.
    void on_released();
    /// Escape pressed: cancel.
    void on_escape();
    /// Keyboard modifiers changed while the drag runs (also called once at begin).
    void on_modifiers(bool shift, bool ctrl, bool alt);
    /// Once per tick: the icon follows the pointer (one origin change per tick at most).
    void on_tick();
    /// The surface tree dropped `surface`'s node. When it was the target, focus moves to
    /// nothing now; the next motion picks a new one.
    void on_surface_unmapped(lx::surface_id surface);

    // ── From the wire ──

    /// A wl_surface is being destroyed: forget it as target or icon (the drag continues).
    void on_surface_destroyed(lx::surface_id surface);
    /// The wire ended the operation itself (source destroyed, origin client gone) after
    /// telling the target: only the grab side is torn down here.
    void end(dnd_end_reason why);
    /// The icon surface committed (its role's part of the commit, before the tree sync):
    /// `dx`/`dy` is this update's offset (attach or wl_surface.offset), accumulated.
    void on_icon_commit(lx::surface_id icon, bool has_buffer, int dx, int dy);
    /// The negotiated action changed, or the target did: the pointer shows it.
    void set_feedback(bool has_target, unsigned action);

private:
    void finish(dnd_end_reason why);
    [[nodiscard]] lx::point2i wanted_origin() const {
        return {static_cast<int>(std::lround(px_)) + offset_.x,
                static_cast<int>(std::lround(py_)) + offset_.y};
    }
    /// Moves the icon root to the wanted origin when it changed. `sync`: bring the nodes
    /// along now (a tick); without it the caller's own tree sync follows (a commit).
    void place_icon(bool sync);

    surface_tree* tree_ = nullptr;
    scene::container_node* icon_layer_ = nullptr;
    cursor_manager* cursor_ = nullptr;
    dnd_ops ops_{};

    bool active_ = false;
    bool self_only_ = false;
    lx::client_id origin_{};
    lx::surface_id icon_{};
    /// Accumulated wl_surface offsets of the icon: its top-left relative to the hotspot.
    lx::point2i offset_{};
    lx::point2i applied_{};
    bool icon_placed_ = false;
    double px_ = 0.0;
    double py_ = 0.0;
    lx::surface_id target_{};
    unsigned user_action_ = 0;
};

} // namespace lx::compositor

namespace {

constexpr const char* k_dnd_category = "compositor.dnd";

[[nodiscard]] bool dnd_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

} // namespace

const char* lx::compositor::dnd_end_name(dnd_end_reason r) {
    switch (r) {
    case dnd_end_reason::drop:
        return "drop";
    case dnd_end_reason::no_target:
        return "no_target";
    case dnd_end_reason::not_accepted:
        return "not_accepted";
    case dnd_end_reason::no_action:
        return "no_action";
    case dnd_end_reason::escape:
        return "escape";
    case dnd_end_reason::source_destroyed:
        return "source_destroyed";
    case dnd_end_reason::client_gone:
        return "client_gone";
    }
    return "?";
}

void lx::compositor::dnd_grab::begin(lx::client_id origin_client, bool self_only,
                                     lx::surface_id icon, double px, double py) {
    active_ = true;
    self_only_ = self_only;
    origin_ = origin_client;
    icon_ = icon;
    offset_ = {};
    applied_ = {};
    icon_placed_ = false;
    px_ = px;
    py_ = py;
    target_ = {};
    user_action_ = 0;
    if (cursor_)
        cursor_->set_override(cursor_shape::grabbing);
    // The icon's top-left starts at the hotspot. Mapped now even without content: an empty
    // root draws nothing, and the first content commit then needs no extra step.
    if (icon_ && tree_ && icon_layer_) {
        place_icon(false);
        (void)tree_->map_root(icon_, *icon_layer_, ~0u);
    }
}

void lx::compositor::dnd_grab::place_icon(bool sync) {
    const lx::point2i o = wanted_origin();
    if (icon_placed_ && o.x == applied_.x && o.y == applied_.y)
        return;
    applied_ = o;
    icon_placed_ = true;
    if (!tree_ || !icon_)
        return;
    tree_->set_root_origin(icon_, o);
    if (sync)
        tree_->sync_scene(icon_, "move");
}

void lx::compositor::dnd_grab::on_motion(const dnd_target* t, double px, double py,
                                         unsigned time_ms, bool send_motion) {
    if (!active_)
        return;
    px_ = px;
    py_ = py;
    // A drag with no source stays inside its client: other clients' surfaces are no target.
    if (t && self_only_ && !(t->client == origin_))
        t = nullptr;
    const lx::surface_id s = t ? t->surface : lx::surface_id{};
    if (!(s == target_)) {
        target_ = s;
        if (ops_.focus)
            ops_.focus(ops_.user, t ? t->surface_resource : nullptr, t ? t->sx : 0.0,
                       t ? t->sy : 0.0);
        return;
    }
    if (t && send_motion && ops_.motion)
        ops_.motion(ops_.user, time_ms, t->sx, t->sy);
}

void lx::compositor::dnd_grab::on_released() {
    if (!active_)
        return;
    dnd_end_reason why = dnd_end_reason::drop;
    const bool dropped = ops_.drop && ops_.drop(ops_.user, why);
    if (!dropped) {
        if (why == dnd_end_reason::drop)
            why = dnd_end_reason::no_target;
        if (ops_.cancel)
            ops_.cancel(ops_.user, why);
    }
    finish(dropped ? dnd_end_reason::drop : why);
}

void lx::compositor::dnd_grab::on_escape() {
    if (!active_)
        return;
    if (ops_.cancel)
        ops_.cancel(ops_.user, dnd_end_reason::escape);
    finish(dnd_end_reason::escape);
}

void lx::compositor::dnd_grab::on_modifiers(bool shift, bool ctrl, bool alt) {
    if (!active_)
        return;
    const unsigned a = user_dnd_action(shift, ctrl, alt);
    if (a == user_action_)
        return;
    user_action_ = a;
    if (ops_.user_action)
        ops_.user_action(ops_.user, a);
}

void lx::compositor::dnd_grab::on_tick() {
    if (active_ && icon_)
        place_icon(true);
}

void lx::compositor::dnd_grab::on_surface_unmapped(lx::surface_id surface) {
    if (!active_ || !surface || !(surface == target_))
        return;
    target_ = {};
    if (ops_.focus)
        ops_.focus(ops_.user, nullptr, 0.0, 0.0);
}

void lx::compositor::dnd_grab::on_surface_destroyed(lx::surface_id surface) {
    if (!active_ || !surface)
        return;
    if (surface == icon_) {
        // The client destroyed its icon: the drag goes on without one. Taken out of the
        // layer here, while the tree still knows the root.
        if (tree_)
            tree_->unmap_root(icon_);
        icon_ = {};
    }
    on_surface_unmapped(surface);
}

void lx::compositor::dnd_grab::end(dnd_end_reason why) {
    if (active_)
        finish(why);
}

void lx::compositor::dnd_grab::on_icon_commit(lx::surface_id icon, bool, int dx, int dy) {
    if (!active_ || !icon || !(icon == icon_))
        return;
    if (dx == 0 && dy == 0)
        return;
    // Recorded only: the commit's own tree sync (which follows) places every node from the
    // new origin, damaging old and new rects.
    offset_.x += dx;
    offset_.y += dy;
    place_icon(false);
}

void lx::compositor::dnd_grab::set_feedback(bool has_target, unsigned action) {
    if (!active_ || !cursor_)
        return;
    cursor_shape shape = cursor_shape::no_drop;
    if (has_target) {
        switch (action) {
        case dnd_copy:
            shape = cursor_shape::copy;
            break;
        case dnd_move:
            shape = cursor_shape::move;
            break;
        case dnd_ask:
            shape = cursor_shape::dnd_ask;
            break;
        default:
            shape = cursor_shape::no_drop;
            break;
        }
    }
    cursor_->set_override(shape);
}

void lx::compositor::dnd_grab::finish(dnd_end_reason why) {
    active_ = false;
    target_ = {};
    if (icon_ && tree_)
        tree_->unmap_root(icon_);
    icon_ = {};
    if (cursor_)
        cursor_->clear_override();
    if (dnd_debug()) {
        lx::trace::logger::global().logf(lx::trace::level::debug, k_dnd_category,
                                         "dnd end reason={}", dnd_end_name(why));
    }
}
