module;

#include <cstdint>

import lx.foundation;
import lx.runtime;

export module lx.compositor:toplevel;

import :activation;

export namespace lx::compositor {

/// Compositor-side toplevel states, mapped to the xdg_toplevel values on the wire.
enum toplevel_state_bit : unsigned {
    tl_activated = 1u << 0,
    tl_resizing = 1u << 1,
    tl_maximized = 1u << 2,
    tl_fullscreen = 1u << 3,
    /// The session is paused (VT switched away): nothing is repainted. Sent as
    /// XDG_TOPLEVEL_STATE_SUSPENDED to xdg_wm_base v6 clients.
    tl_suspended = 1u << 4,
};

/// Same values as `xdg_toplevel.resize_edge` and `zlm_policy_bridge_v1.resize_edge`.
enum resize_edge_bits : unsigned {
    edge_none = 0,
    edge_top = 1,
    edge_bottom = 2,
    edge_left = 4,
    edge_right = 8,
};

/// Who draws the window frame (zxdg_toplevel_decoration_v1.mode, minus "unset").
enum class decoration_mode : unsigned char { client, server };
/// What the client asked for with set_mode / unset_mode.
enum class decoration_request : unsigned char { unset, client, server };

/// One xdg configure: what the compositor asks of the client.
struct toplevel_configure {
    unsigned serial = 0;
    /// Window-geometry size; 0x0 lets the client decide.
    lx::size2i size{};
    /// `toplevel_state_bit` mask.
    unsigned states = 0;
    /// Edges whose *opposite* side stays fixed when the commit answering this configure is
    /// applied — `edge_left` keeps the right edge where it was. Stored per configure rather
    /// than per grab, so a configure answered after the grab has ended (the final one,
    /// or an Escape restore) still anchors the right way.
    unsigned anchor_edges = 0;
    /// Global window geometry the anchor is computed from (the grab's start rect).
    lx::rect2i anchor{};
    /// Decoration mode this configure tells the client (zxdg_toplevel_decoration_v1); it
    /// takes effect with the commit that answers it, so the frame and the content drawn
    /// for it reach the screen in the same snapshot.
    decoration_mode decoration = decoration_mode::client;
    /// Maximize / unmaximize: the commit answering this configure puts the window-geometry
    /// origin at `place_at` (global) instead of keeping or anchoring it — the window lands
    /// at its maximized (or restored) position with the content drawn for that size, in the
    /// same snapshot. Not part of `same_request`: a position is the compositor's to apply.
    bool place = false;
    lx::point2i place_at{};

    /// Size, states and decoration mode: two configures that ask the client for the same
    /// thing.
    [[nodiscard]] bool same_request(const toplevel_configure& o) const {
        return size.width == o.size.width && size.height == o.size.height &&
               states == o.states && decoration == o.decoration;
    }
};

/// Compositor-owned toplevel. UI affinity only.
///
/// `geometry` is the one source of truth for where a window is: the global rect of its xdg
/// window geometry. The scene node's origin is derived from it (`geometry.xy -
/// geometry_offset`) and its bounds from the committed buffer — nothing else writes either.
struct toplevel_record {
    lx::toplevel_id id{};
    lx::client_id client{};
    lx::surface_id surface{};
    /// `wl_resource*`s as void* (no Wayland types in this partition). Every record is
    /// destroyed by whichever of the three objects is destroyed first, so these never
    /// dangle.
    void* surface_resource = nullptr;
    void* xdg_surface_resource = nullptr;
    void* xdg_toplevel_resource = nullptr;
    /// The shell's `zlm_toplevel_v1` for this window, or null when no shell is bound.
    void* shell_handle = nullptr;
    const char* app_id = "";
    const char* title = "";
    lx::toplevel_state state = lx::toplevel_state::normal;

    // ── geometry: the source of truth ──
    /// Global. x,y = window-geometry top-left; w,h = committed window-geometry size.
    lx::rect2i geometry{};
    /// Committed window-geometry origin inside the surface (CSD shadow width, typically).
    lx::point2i geometry_offset{};
    /// Committed surface extents (logical).
    lx::size2i surface_size{};

    // ── double-buffered xdg state (pending → current on wl_surface.commit) ──
    lx::rect2i window_geometry_pending{};
    bool window_geometry_pending_set = false;
    lx::rect2i window_geometry_requested{};
    bool window_geometry_set = false;
    /// Window-geometry size limits; 0 = unset on that axis.
    lx::size2i min_size{};
    lx::size2i max_size{};
    lx::size2i min_pending{};
    lx::size2i max_pending{};
    bool min_pending_set = false;
    bool max_pending_set = false;

    // ── configure pipeline ──
    /// What the window manager wants next. Written any time, coalesced, flushed per tick.
    toplevel_configure desired{};
    /// Last configure put on the wire.
    toplevel_configure last_sent{};
    static constexpr unsigned k_sent_cap = 4;
    /// Sent, not yet acked, oldest first.
    toplevel_configure sent[k_sent_cap]{};
    unsigned sent_count = 0;
    /// Highest serial dropped from `sent` on overflow; acks up to it are ignored, not fatal.
    unsigned dropped_upto = 0;
    /// Newest acked configure; applied by the next commit.
    toplevel_configure acked{};
    bool acked_unapplied = false;
    bool inflight = false;
    lx::runtime::clock_time inflight_since_ns = 0;
    /// Output size last sent as `configure_bounds`.
    lx::size2i bounds_sent{};
    bool initial_commit_seen = false;
    bool initial_configure_sent = false;
    bool ever_acked = false;
    bool mapped = false;

    // ── xdg-decoration (zxdg_toplevel_decoration_v1) ──
    /// The live decoration object (`wl_resource*`), or null.
    void* deco_resource = nullptr;
    unsigned deco_version = 0;
    decoration_request deco_requested = decoration_request::unset;
    /// The mode in effect: applied by the commit answering the configure that carried it;
    /// always client-side while no decoration object exists (`deco_resource` null).
    decoration_mode decoration_current = decoration_mode::client;
    /// Version 1 object with no decoration configure sent yet: attaching a buffer now is
    /// `unconfigured_buffer`.
    bool deco_awaiting_configure = false;
    /// The next configure must carry a decoration configure even if nothing else changed
    /// (a new decoration object, set_mode / unset_mode, which the spec says are answered by
    /// a configure).
    bool deco_configure_due = false;

    // ── maximize ──
    /// Window geometry before maximize, restored by unmaximize (`restore_set`: there was
    /// one — a window maximized before it ever mapped has none).
    lx::rect2i restore{};
    bool restore_set = false;
    /// xdg_toplevel.set_maximized before the initial configure: that configure maximizes.
    bool want_maximized = false;
    /// The next configure must go out even if it asks for nothing new: set_maximized /
    /// unset_maximized are always answered by a configure (xdg-shell), maximized already or
    /// not.
    bool configure_due = false;

    // ── attention (zlm_policy_bridge_v1.toplevel_flags) ──
    /// The client did not answer an xdg_wm_base.ping in time (`liveness_tracker`). Shown —
    /// title suffix, dimmed tree, the shell's flag — never acted on: the compositor does not
    /// kill clients.
    bool not_responding = false;
    /// An xdg_activation_v1.activate of this window was refused focus (focus-stealing
    /// prevention); the shell shows it. Cleared when the window gets the keyboard or unmaps.
    bool demands_attention = false;

    // ── xdg-activation before map ──
    /// activate arrived before the window mapped: decided again at map by the same rules
    /// (`decide_activation`), with the request's time. A refused one keeps the new window
    /// from taking the keyboard.
    bool activation_pending = false;
    /// The token was known (`pending_grant` is meaningful); false: an unknown token.
    bool activation_token_known = false;
    activation_grant pending_grant{};
    lx::runtime::clock_time activation_requested_ns = 0;

    lx::workspace_id workspace{};
    unsigned wire_id = 0;           ///< zlm_toplevel_v1 Wayland object id (P0)
    bool override_redirect = false; ///< XWayland / OR windows — no decorations, fixed stacking
    unsigned net_wm_type = 0;       ///< _NET_WM_WINDOW_TYPE atom value (XWayland bridge)
    unsigned net_wm_state = 0;      ///< _NET_WM_STATE bitmask (fullscreen, maximized, …)
};

/// Registry of toplevels and their stacking order. UI affinity.
///
/// Records live in fixed slots and never move, but a slot is reused after `destroy`, so
/// callers hold `toplevel_id`s across calls — never record pointers.
class toplevel_manager {
public:
    static constexpr unsigned k_max = 256;
    /// A configure unanswered this long no longer blocks the next one. Only matters for
    /// clients that ack and never commit; sway uses the same bound for its transactions.
    static constexpr lx::runtime::clock_time k_configure_timeout_ns = 200'000'000;

    [[nodiscard]] lx::result<lx::toplevel_id> create(lx::client_id client, lx::surface_id surface,
                                                     void* surface_resource,
                                                     void* xdg_surface_resource,
                                                     void* xdg_toplevel_resource);
    /// Removes the record, and from the stacking order if mapped.
    void destroy(lx::toplevel_id id);

    [[nodiscard]] toplevel_record* find_mut(lx::toplevel_id id);
    [[nodiscard]] const toplevel_record* find(lx::toplevel_id id) const;
    [[nodiscard]] const toplevel_record* find_by_surface(lx::surface_id surface) const;
    /// Records in use (mapped or not).
    [[nodiscard]] unsigned count() const { return used_count_; }
    [[nodiscard]] unsigned mapped_count() const { return order_count_; }

    /// Mapping pushes the toplevel on top of the stacking order; unmapping removes it.
    void set_mapped(lx::toplevel_id id, bool mapped);
    bool raise(lx::toplevel_id id);
    bool lower(lx::toplevel_id id);
    bool move_to(lx::toplevel_id id, unsigned index);
    /// 0 = bottom. `~0u` when not mapped.
    [[nodiscard]] unsigned stacking_index(lx::toplevel_id id) const;
    /// Top-most mapped toplevel, or a null id.
    [[nodiscard]] lx::toplevel_id top() const;

    /// Mapped toplevels top→bottom. `fn(const toplevel_record&) -> bool` returns false to stop.
    template<typename Fn>
    void for_each_top_down(Fn&& fn) const {
        for (unsigned i = order_count_; i-- > 0;) {
            if (!fn(records_[order_[i]]))
                return;
        }
    }
    /// Mapped toplevels bottom→top.
    template<typename Fn>
    void for_each_bottom_up(Fn&& fn) const {
        for (unsigned i = 0; i < order_count_; ++i)
            fn(records_[order_[i]]);
    }
    /// Every record in use, mapped or not, in slot order.
    template<typename Fn>
    void for_each(Fn&& fn) const {
        for (unsigned i = 0; i < k_max; ++i) {
            if (used_[i])
                fn(records_[i]);
        }
    }
    template<typename Fn>
    void for_each_mut(Fn&& fn) {
        for (unsigned i = 0; i < k_max; ++i) {
            if (used_[i])
                fn(records_[i]);
        }
    }

    void on_xdg_title(lx::toplevel_id id, const char* title);
    void on_xdg_app_id(lx::toplevel_id id, const char* app_id);
    void set_state(lx::toplevel_id id, lx::toplevel_state state);
    void set_workspace(lx::toplevel_id id, lx::workspace_id workspace);
    void set_override_redirect(lx::toplevel_id id, bool value);
    void set_net_wm_type(lx::toplevel_id id, unsigned type_atom);
    void set_net_wm_state(lx::toplevel_id id, unsigned state_mask);

    // ── configure state machine: pure, no wire I/O ──

    /// Applies double-buffered window geometry and size limits on wl_surface.commit.
    /// False when the result is invalid (min > max on an axis); nothing is applied then.
    [[nodiscard]] static bool apply_pending_state(toplevel_record& rec);
    /// The mode the compositor's policy picks: the client's explicit `client_side` is
    /// respected (KWin, sway); `server_side` or no preference gets the compositor's frame.
    /// Without a decoration object a client decorates itself.
    [[nodiscard]] static decoration_mode wanted_decoration(const toplevel_record& rec);
    /// True iff the desired configure differs from the last one sent (or a decoration
    /// configure is due) and either nothing is in flight or the one in flight has timed out.
    [[nodiscard]] static bool want_send(const toplevel_record& rec, lx::runtime::clock_time now_ns);
    /// True when `want_send` holds only because of the timeout.
    [[nodiscard]] static bool timed_out(const toplevel_record& rec, lx::runtime::clock_time now_ns);
    static void on_sent(toplevel_record& rec, toplevel_configure c, lx::runtime::clock_time now_ns);
    enum class ack_result { ok, duplicate, invalid };
    [[nodiscard]] static ack_result on_ack(toplevel_record& rec, unsigned serial);
    /// The commit after an ack applies it. Clears the in-flight flag once the newest sent
    /// configure has been answered. Returns false when there was no unapplied ack.
    static bool consume_ack(toplevel_record& rec, toplevel_configure& applied);
    /// xdg unmap (null buffer) resets the surface to its pre-initial-commit state.
    static void reset_for_remap(toplevel_record& rec);

private:
    [[nodiscard]] int slot_of(lx::toplevel_id id) const;
    [[nodiscard]] int order_of(unsigned slot) const;

    toplevel_record records_[k_max]{};
    bool used_[k_max]{};
    unsigned used_count_ = 0;
    /// Slot indices of mapped toplevels, bottom→top.
    std::uint16_t order_[k_max]{};
    unsigned order_count_ = 0;
    unsigned long long next_id_ = 1;
};

} // namespace lx::compositor


int lx::compositor::toplevel_manager::slot_of(lx::toplevel_id id) const {
    if (!id)
        return -1;
    for (unsigned i = 0; i < k_max; ++i) {
        if (used_[i] && records_[i].id == id)
            return static_cast<int>(i);
    }
    return -1;
}

int lx::compositor::toplevel_manager::order_of(unsigned slot) const {
    for (unsigned i = 0; i < order_count_; ++i) {
        if (order_[i] == slot)
            return static_cast<int>(i);
    }
    return -1;
}

lx::result<lx::toplevel_id>
lx::compositor::toplevel_manager::create(lx::client_id client, lx::surface_id surface,
                                         void* surface_resource, void* xdg_surface_resource,
                                         void* xdg_toplevel_resource) {
    for (unsigned i = 0; i < k_max; ++i) {
        if (used_[i])
            continue;
        used_[i] = true;
        ++used_count_;
        auto& rec = records_[i];
        rec = {};
        rec.id = lx::toplevel_id{next_id_++};
        rec.client = client;
        rec.surface = surface;
        rec.surface_resource = surface_resource;
        rec.xdg_surface_resource = xdg_surface_resource;
        rec.xdg_toplevel_resource = xdg_toplevel_resource;
        rec.wire_id = static_cast<unsigned>(rec.id.id());
        return rec.id;
    }
    return lx::make_error(lx::error_domain::invalid_argument, 0, "toplevel table full");
}

void lx::compositor::toplevel_manager::destroy(lx::toplevel_id id) {
    const int slot = slot_of(id);
    if (slot < 0)
        return;
    set_mapped(id, false);
    records_[static_cast<unsigned>(slot)] = {};
    used_[static_cast<unsigned>(slot)] = false;
    --used_count_;
}

lx::compositor::toplevel_record* lx::compositor::toplevel_manager::find_mut(lx::toplevel_id id) {
    const int slot = slot_of(id);
    return slot < 0 ? nullptr : &records_[static_cast<unsigned>(slot)];
}

const lx::compositor::toplevel_record*
lx::compositor::toplevel_manager::find(lx::toplevel_id id) const {
    const int slot = slot_of(id);
    return slot < 0 ? nullptr : &records_[static_cast<unsigned>(slot)];
}

const lx::compositor::toplevel_record*
lx::compositor::toplevel_manager::find_by_surface(lx::surface_id surface) const {
    if (!surface)
        return nullptr;
    for (unsigned i = 0; i < k_max; ++i) {
        if (used_[i] && records_[i].surface == surface)
            return &records_[i];
    }
    return nullptr;
}

void lx::compositor::toplevel_manager::set_mapped(lx::toplevel_id id, bool mapped) {
    const int slot = slot_of(id);
    if (slot < 0)
        return;
    auto& rec = records_[static_cast<unsigned>(slot)];
    const int pos = order_of(static_cast<unsigned>(slot));
    if (mapped) {
        if (pos < 0 && order_count_ < k_max)
            order_[order_count_++] = static_cast<std::uint16_t>(slot);
    } else if (pos >= 0) {
        // Order-preserving removal: the stacking of everything else must not change.
        for (unsigned i = static_cast<unsigned>(pos) + 1; i < order_count_; ++i)
            order_[i - 1] = order_[i];
        --order_count_;
    }
    rec.mapped = mapped;
}

bool lx::compositor::toplevel_manager::move_to(lx::toplevel_id id, unsigned index) {
    const int slot = slot_of(id);
    if (slot < 0)
        return false;
    const int from_i = order_of(static_cast<unsigned>(slot));
    if (from_i < 0)
        return false;
    const unsigned from = static_cast<unsigned>(from_i);
    const unsigned to = index < order_count_ ? index : order_count_ - 1;
    if (from == to)
        return false;
    if (from < to) {
        for (unsigned i = from; i < to; ++i)
            order_[i] = order_[i + 1];
    } else {
        for (unsigned i = from; i > to; --i)
            order_[i] = order_[i - 1];
    }
    order_[to] = static_cast<std::uint16_t>(slot);
    return true;
}

bool lx::compositor::toplevel_manager::raise(lx::toplevel_id id) {
    return order_count_ > 0 && move_to(id, order_count_ - 1);
}

bool lx::compositor::toplevel_manager::lower(lx::toplevel_id id) { return move_to(id, 0); }

unsigned lx::compositor::toplevel_manager::stacking_index(lx::toplevel_id id) const {
    const int slot = slot_of(id);
    if (slot < 0)
        return ~0u;
    const int pos = order_of(static_cast<unsigned>(slot));
    return pos < 0 ? ~0u : static_cast<unsigned>(pos);
}

lx::toplevel_id lx::compositor::toplevel_manager::top() const {
    return order_count_ > 0 ? records_[order_[order_count_ - 1]].id : lx::toplevel_id{};
}

void lx::compositor::toplevel_manager::on_xdg_title(lx::toplevel_id id, const char* title) {
    if (auto* rec = find_mut(id))
        rec->title = title ? title : "";
}

void lx::compositor::toplevel_manager::on_xdg_app_id(lx::toplevel_id id, const char* app_id) {
    if (auto* rec = find_mut(id))
        rec->app_id = app_id ? app_id : "";
}

void lx::compositor::toplevel_manager::set_state(lx::toplevel_id id, lx::toplevel_state state) {
    if (auto* rec = find_mut(id))
        rec->state = state;
}

void lx::compositor::toplevel_manager::set_workspace(lx::toplevel_id id,
                                                     lx::workspace_id workspace) {
    if (auto* rec = find_mut(id))
        rec->workspace = workspace;
}

void lx::compositor::toplevel_manager::set_override_redirect(lx::toplevel_id id, bool value) {
    if (auto* rec = find_mut(id))
        rec->override_redirect = value;
}

void lx::compositor::toplevel_manager::set_net_wm_type(lx::toplevel_id id, unsigned type_atom) {
    if (auto* rec = find_mut(id))
        rec->net_wm_type = type_atom;
}

void lx::compositor::toplevel_manager::set_net_wm_state(lx::toplevel_id id, unsigned state_mask) {
    if (auto* rec = find_mut(id))
        rec->net_wm_state = state_mask;
}

bool lx::compositor::toplevel_manager::apply_pending_state(toplevel_record& rec) {
    const lx::size2i min = rec.min_pending_set ? rec.min_pending : rec.min_size;
    const lx::size2i max = rec.max_pending_set ? rec.max_pending : rec.max_size;
    // xdg-shell: a max of 0 means "no limit" on that axis, so only two non-zero limits can
    // contradict each other.
    if ((min.width > 0 && max.width > 0 && min.width > max.width) ||
        (min.height > 0 && max.height > 0 && min.height > max.height))
        return false;
    rec.min_size = min;
    rec.max_size = max;
    rec.min_pending_set = false;
    rec.max_pending_set = false;
    if (rec.window_geometry_pending_set) {
        rec.window_geometry_requested = rec.window_geometry_pending;
        rec.window_geometry_set = true;
        rec.window_geometry_pending_set = false;
    }
    return true;
}

lx::compositor::decoration_mode
lx::compositor::toplevel_manager::wanted_decoration(const toplevel_record& rec) {
    if (!rec.deco_resource || rec.deco_requested == decoration_request::client)
        return decoration_mode::client;
    return decoration_mode::server;
}

bool lx::compositor::toplevel_manager::timed_out(const toplevel_record& rec,
                                                 lx::runtime::clock_time now_ns) {
    return rec.inflight && now_ns - rec.inflight_since_ns >= k_configure_timeout_ns;
}

bool lx::compositor::toplevel_manager::want_send(const toplevel_record& rec,
                                                 lx::runtime::clock_time now_ns) {
    // Never resend what the client already has: a no-op configure is exactly what some
    // clients ignore, and waiting on its ack is how KWin once deadlocked (D20397).
    if (rec.desired.same_request(rec.last_sent) && !rec.deco_configure_due && !rec.configure_due)
        return false;
    return !rec.inflight || timed_out(rec, now_ns);
}

void lx::compositor::toplevel_manager::on_sent(toplevel_record& rec, toplevel_configure c,
                                               lx::runtime::clock_time now_ns) {
    if (rec.sent_count == toplevel_record::k_sent_cap) {
        // Only reachable through the watchdog. An ack of a newer serial would consume the
        // oldest anyway; remember it so an ack of it is tolerated rather than fatal.
        rec.dropped_upto = rec.sent[0].serial;
        for (unsigned i = 1; i < rec.sent_count; ++i)
            rec.sent[i - 1] = rec.sent[i];
        --rec.sent_count;
    }
    rec.sent[rec.sent_count++] = c;
    rec.last_sent = c;
    rec.inflight = true;
    rec.inflight_since_ns = now_ns;
}

lx::compositor::toplevel_manager::ack_result
lx::compositor::toplevel_manager::on_ack(toplevel_record& rec, unsigned serial) {
    for (unsigned i = 0; i < rec.sent_count; ++i) {
        if (rec.sent[i].serial != serial)
            continue;
        // Acking a serial implicitly acks every older one (xdg_surface.ack_configure).
        rec.acked = rec.sent[i];
        const unsigned popped = i + 1;
        for (unsigned j = popped; j < rec.sent_count; ++j)
            rec.sent[j - popped] = rec.sent[j];
        rec.sent_count -= popped;
        rec.acked_unapplied = true;
        rec.ever_acked = true;
        return ack_result::ok;
    }
    // Serials are compared with wrap-safe signed differences.
    if ((rec.acked.serial != 0 && static_cast<int>(serial - rec.acked.serial) <= 0) ||
        (rec.dropped_upto != 0 && static_cast<int>(serial - rec.dropped_upto) <= 0))
        return ack_result::duplicate;
    return ack_result::invalid;
}

bool lx::compositor::toplevel_manager::consume_ack(toplevel_record& rec,
                                                   toplevel_configure& applied) {
    if (!rec.acked_unapplied)
        return false;
    rec.acked_unapplied = false;
    applied = rec.acked;
    if (static_cast<int>(rec.acked.serial - rec.last_sent.serial) >= 0)
        rec.inflight = false;
    return true;
}

void lx::compositor::toplevel_manager::reset_for_remap(toplevel_record& rec) {
    rec.initial_commit_seen = false;
    rec.initial_configure_sent = false;
    rec.ever_acked = false;
    rec.sent_count = 0;
    rec.dropped_upto = 0;
    rec.acked = {};
    rec.acked_unapplied = false;
    rec.inflight = false;
    rec.desired = {};
    rec.last_sent = {};
    rec.bounds_sent = {};
    rec.configure_due = false;
    rec.state = lx::toplevel_state::normal;
    rec.geometry = {};
    rec.geometry_offset = {};
}
