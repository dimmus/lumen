module;

import lx.foundation;
import lx.trace;
import lx.scene;

export module lx.compositor:layer_shell;

import std;

import :surface;
import :surface_tree;

export namespace lx::compositor {

/// zwlr_layer_surface_v1.anchor bits (also the exclusive-edge values).
inline constexpr unsigned k_layer_anchor_top = 1;
inline constexpr unsigned k_layer_anchor_bottom = 2;
inline constexpr unsigned k_layer_anchor_left = 4;
inline constexpr unsigned k_layer_anchor_right = 8;

/// zwlr_layer_shell_v1.layer, bottom-most first. The values index the scene's four
/// layer-shell containers.
enum class layer_level : unsigned char { background = 0, bottom = 1, top = 2, overlay = 3 };
inline constexpr unsigned k_layer_levels = 4;

/// zwlr_layer_surface_v1.keyboard_interactivity.
enum class layer_interactivity : unsigned char { none = 0, exclusive = 1, on_demand = 2 };

/// Double-buffered layer-surface state, in wire units. `size` 0 on an axis: the compositor
/// picks it (only valid with both anchors of that axis, checked at commit).
struct layer_props {
    unsigned anchor = 0;
    lx::size2i size{};
    int zone = 0;
    int margin_top = 0;
    int margin_right = 0;
    int margin_bottom = 0;
    int margin_left = 0;
    /// v5 set_exclusive_edge; 0 = deduced from the anchors.
    unsigned exclusive_edge = 0;
    unsigned layer = 0;
    unsigned interactivity = 0;

    [[nodiscard]] bool operator==(const layer_props& o) const {
        return anchor == o.anchor && size.width == o.size.width &&
               size.height == o.size.height && zone == o.zone && margin_top == o.margin_top &&
               margin_right == o.margin_right && margin_bottom == o.margin_bottom &&
               margin_left == o.margin_left && exclusive_edge == o.exclusive_edge &&
               layer == o.layer && interactivity == o.interactivity;
    }
};

/// One surface's input to `arrange_layers`.
struct layer_arrange_in {
    const layer_props* props = nullptr;
    /// Committed surface size (used for placement once mapped).
    lx::size2i committed{};
    bool mapped = false;
    /// Initial commit done and not closed: gets a configure and a box.
    bool participates = false;
};

struct layer_arrange_out {
    /// Global surface rect.
    lx::rect2i box{};
    /// Size the next configure asks for (0 on an axis: the client picks).
    lx::size2i configure{};
    /// The compositor-assigned size is not positive: the surface is closed.
    bool closed = false;
    /// The edge this surface's exclusive zone was applied to (0: none).
    unsigned zone_edge = 0;
};

/// The edge a positive exclusive zone applies to: the explicit `exclusive_edge` (v5) if
/// set; else the one anchored edge, or the edge anchored together with both perpendicular
/// ones. Anything else (a corner, two parallel edges, all four, none) takes no zone (the
/// spec: "treated the same as zero"). 0 for a zone ≤ 0.
[[nodiscard]] unsigned layer_exclusive_edge(const layer_props& p);

/// The commit rules on a layer surface's state, checked before anything is applied.
enum class layer_state_error : unsigned char { none, invalid_size, invalid_exclusive_edge };
[[nodiscard]] layer_state_error validate_layer_state(const layer_props& p);

/// Arranges one output's layer surfaces — sway / wlroots `wlr_scene_layer_surface_v1_
/// configure`, with two deliberate deviations recorded in docs/architecture.md: a smaller
/// committed size is centred between two opposite anchors (the spec's text), and an
/// exclusive zone counts only once its surface is mapped.
///
/// `in` is ordered overlay, top, bottom, background, each layer in creation order. Two
/// passes over that order: surfaces with a positive zone first (each shrinks `usable` for
/// the ones after it), then the rest. A zone of -1 places against the whole output.
/// `usable` starts as `full` and is clamped to a non-negative size.
void arrange_layers(lx::rect2i full, const layer_arrange_in* in, layer_arrange_out* out,
                    unsigned n, lx::rect2i& usable);

/// One zwlr_layer_surface_v1 (keyed by its wl_surface's id). UI affinity.
struct layer_record {
    lx::surface_id surface{};
    lx::client_id client{};
    /// `wl_client*`, `wl_resource*`s (void*: no Wayland types in this partition).
    void* client_native = nullptr;
    void* surface_res = nullptr;
    void* layer_res = nullptr;
    unsigned version = 1;
    /// The output it belongs to (slot), resolved at creation: the one given, else the one
    /// under the pointer (ADR 008 M12). Arranged against that output only.
    unsigned output = 0;
    char name_space[64]{};
    /// Requests edit `pending`; a commit copies it to `current`.
    layer_props pending{};
    layer_props current{};
    static constexpr unsigned k_sent_cap = 8;
    /// Configures sent and not acked, oldest first.
    unsigned sent[k_sent_cap]{};
    unsigned sent_count = 0;
    unsigned dropped_upto = 0;
    unsigned acked = 0;
    bool ever_acked = false;
    /// A configure was sent since get_layer_surface (or since the last unmap).
    bool configured = false;
    lx::size2i last_configure{};
    /// Committed surface size.
    lx::size2i committed{};
    /// Global surface rect from the last arrangement.
    lx::rect2i box{};
    /// The layer whose container holds the tree while mapped.
    unsigned mapped_layer = 0;
    /// Last logged exclusive zone (for the `layer exclusive` line on change only).
    unsigned logged_edge = ~0u;
    int logged_zone = 0;
    unsigned long long created_seq = 0;
    /// `current` changed at the last commit.
    bool props_changed = false;
    bool interactivity_changed = false;
    bool initial_commit_seen = false;
    bool mapped = false;
    /// `closed` sent: inert until destroyed; commits release their buffers undrawn.
    bool closed = false;
    bool used = false;
};

/// Wire operations, installed by the protocol layer.
struct layer_ops {
    void (*send_configure)(void* layer_res, unsigned serial, int width, int height) = nullptr;
    void (*send_closed)(void* layer_res) = nullptr;
    unsigned (*next_serial)(void* user) = nullptr;
    /// False for a client being torn down: nothing may be sent to it.
    bool (*client_alive)(void* client_native) = nullptr;
    void* user = nullptr;
};

/// What the layer shell needs from the window manager (which sits above it).
struct layer_host {
    /// Output `slot`'s logical rect — what its layer surfaces are arranged against; empty
    /// when the output is gone.
    lx::rect2i (*output_area)(void* user, unsigned slot) = nullptr;
    /// Output `slot`'s usable area (its rect minus its exclusive zones) changed.
    void (*usable_changed)(void* user, unsigned slot, lx::rect2i usable) = nullptr;
    /// A layer surface mapped, unmapped or changed its keyboard interactivity.
    void (*focus_changed)(void* user) = nullptr;
    /// A layer surface just mapped (called before `focus_changed`): where a surface that
    /// `focus_on_map` accepts takes the keyboard.
    void (*surface_mapped)(void* user, lx::surface_id surface) = nullptr;
    /// A mapped layer surface moved: its popups follow.
    void (*surface_moved)(void* user, lx::surface_id surface) = nullptr;
    /// A layer surface is unmapped (`reason`) or, with "destroyed", going away: its popups
    /// are dismissed first.
    void (*surface_unmapped)(void* user, lx::surface_id surface, const char* reason) = nullptr;
    void* user = nullptr;
};

/// What a commit of a layer surface carried, after its tree was applied.
struct layer_commit_info {
    bool has_buffer = false;
    lx::size2i size{};
};

/// wlr-layer-shell: records, the configure flow, arrangement and exclusive zones, and the
/// keyboard-interactivity queries. UI affinity, Wayland-free.
///
/// Every layer surface is the root of its own surface tree, whose container sits in one of
/// the scene's four layer-shell containers (background and bottom under the windows, top
/// and overlay above them, all under the popup layer), appended on map. The usable area
/// this produces is what toplevel placement, move constraints and maximize use.
class layer_shell {
public:
    static constexpr unsigned k_max = 64;

    /// `layers[i]` is the container of `layer_level(i)`.
    void bind(surface_manager& surfaces, surface_tree& tree,
              scene::container_node* const (&layers)[k_layer_levels]);
    void set_ops(layer_ops ops) { ops_ = ops; }
    void set_host(layer_host host) { host_ = host; }

    enum class create_error : unsigned char { none, no_memory };
    /// zwlr_layer_shell_v1.get_layer_surface, after the protocol layer validated the role,
    /// the buffer and the layer, and resolved the output (`output`, a slot). Makes the
    /// surface a layer root.
    [[nodiscard]] create_error create(lx::surface_id surface, lx::client_id client,
                                      void* client_native, void* surface_res, void* layer_res,
                                      unsigned version, unsigned layer, const char* name_space,
                                      unsigned output = 0);
    /// `layer_record*`, or `const layer_record*` on a const layer shell.
    [[nodiscard]] auto find(this auto& self, lx::surface_id surface)
        -> decltype(&self.records_[0]) {
        const int slot = self.slot_of(surface);
        return slot >= 0 ? &self.records_[static_cast<unsigned>(slot)] : nullptr;
    }
    /// Content committed to the surface is imported and drawn (until closed).
    [[nodiscard]] bool drawable(lx::surface_id surface) const;

    enum class ack_result : unsigned char { ok, duplicate, invalid };
    [[nodiscard]] ack_result on_ack(lx::surface_id surface, unsigned serial);
    /// wl_surface.commit, after the commit rules passed: pending → current.
    void apply_pending(lx::surface_id surface);
    /// After the surface's tree was applied: the initial commit gets its configure, a first
    /// buffer after an ack maps, a null buffer unmaps, and any change re-arranges — in this
    /// same UI call, so a new size lands at its new position in the same snapshot.
    void on_commit(lx::surface_id surface, const layer_commit_info& info);
    /// Any of the surface's objects is going away. Idempotent.
    void destroy(lx::surface_id surface);

    /// Re-arranges every output's layer surfaces against it (the layout changed). The
    /// commit path arranges the committing surface's output by itself.
    void arrange();
    /// Re-arranges output `slot`'s layer surfaces.
    void arrange_output(unsigned slot) { arrange_all(slot, lx::surface_id{}); }
    /// Output `slot` minus its exclusive zones, from its last arrangement.
    [[nodiscard]] lx::rect2i usable(unsigned slot) const {
        return slot < k_max_outputs ? usable_[slot] : lx::rect2i{};
    }
    /// An arrangement of output `slot` has run (before that, `usable` means nothing).
    [[nodiscard]] bool arranged(unsigned slot) const {
        return slot < k_max_outputs && arranged_[slot];
    }
    /// The output (slot) a layer surface belongs to, -1 for none.
    [[nodiscard]] int output_of(lx::surface_id surface) const;
    /// Output `slot` went away: its layer surfaces are sent `closed` and unmapped (sway);
    /// the records stay until the client destroys them, and their zones go with them.
    void close_output(unsigned slot);
    /// Sends `closed` to one layer surface (created for an output that is already gone).
    void close_surface(lx::surface_id surface);
    static constexpr unsigned k_max_outputs = 16;

    /// The zone hold (ADR 011 S6): when a mapped surface of the client given here — the
    /// shell — is destroyed while its output still exists (the shell crashed, or was stopped
    /// to be restarted; a logout clears the client first), the exclusive zone it reserved
    /// stays in the usable area until `release_zone_hold`, or until a surface with an
    /// exclusive zone maps on that output (the restarted shell's panel). Maximized windows
    /// then do not grow into the panel's strip and shrink back a moment later. Whether the
    /// shell destroyed its surfaces before disconnecting or the connection took them makes no
    /// difference. Null: no hold.
    void set_zone_hold_client(void* client_native) { hold_client_ = client_native; }
    /// Called when a hold begins: the owner ends it after its bound (5 s) with
    /// `release_zone_hold`.
    void set_zone_hold_started(void (*started)(void* user), void* user) {
        hold_started_ = started;
        hold_started_user_ = user;
    }
    /// Ends the hold on every output and arranges them again.
    void release_zone_hold();
    [[nodiscard]] bool zone_hold_active() const;

    /// The surface that takes the keyboard from everything but its own popups: the topmost
    /// mapped surface with `exclusive` interactivity on the top or overlay layer (overlay
    /// first, newest first). Null when none.
    [[nodiscard]] const layer_record* exclusive_focus() const;
    /// Mapped, and its interactivity lets it have keyboard focus.
    [[nodiscard]] bool focusable(lx::surface_id surface) const;
    /// A press on it may focus it: on_demand anywhere, exclusive on bottom/background
    /// ("normal focus semantics" there).
    [[nodiscard]] bool click_focusable(lx::surface_id surface) const;
    /// An on_demand surface mapped on the top or overlay layer takes the keyboard when it
    /// maps (sway's and Hyprland's rule: a launcher or menu opened from a key binding can
    /// be typed into at once). Exclusive ones take it through `exclusive_focus` anyway.
    [[nodiscard]] bool focus_on_map(lx::surface_id surface) const;
    /// The layer a mapped surface is in (`layer_level` as unsigned), or -1.
    [[nodiscard]] int mapped_level(lx::surface_id surface) const;

    /// Topmost mapped surface (any surface of its tree) of `level` under the global point.
    [[nodiscard]] bool hit(layer_level level, double gx, double gy, tree_hit& out,
                           const layer_record*& rec) const;
    [[nodiscard]] unsigned mapped_count() const { return mapped_total_; }

private:
    [[nodiscard]] int slot_of(lx::surface_id surface) const { return index_.find(surface.id()); }
    /// Arranges output `output`'s surfaces (`committing`: the surface whose own commit
    /// places it).
    void arrange_all(unsigned output, lx::surface_id committing);
    void send_configure(layer_record& r, lx::size2i size);
    void map(layer_record& r);
    void unmap(layer_record& r, const char* reason);
    void close(layer_record& r, const char* reason);
    void reset_after_unmap(layer_record& r);
    void mapped_add(unsigned level, unsigned slot);
    void mapped_remove(unsigned level, unsigned slot);
    void log_line(const char* line) const;
    template <typename... Args>
    void log_line(std::format_string<Args...> fmt, Args&&... args) const;

    surface_manager* surfaces_ = nullptr;
    surface_tree* tree_ = nullptr;
    scene::container_node* layers_[k_layer_levels]{};
    layer_ops ops_{};
    layer_host host_{};
    layer_record records_[k_max]{};
    id_slot_index<k_max * 2> index_{};
    /// Every record's slot in creation order (the arrangement order within a layer).
    std::inplace_vector<unsigned, k_max> all_{};
    /// Mapped records' slots per layer, bottom→top (each layer container's order).
    unsigned mapped_[k_layer_levels][k_max]{};
    unsigned mapped_count_[k_layer_levels]{};
    unsigned mapped_total_ = 0;
    lx::rect2i usable_[k_max_outputs]{};
    bool arranged_[k_max_outputs]{};
    unsigned long long next_seq_ = 1;
    /// What a held zone keeps reserved, per output: insets from each edge.
    struct held_zone {
        int top = 0, bottom = 0, left = 0, right = 0;
        bool active = false;
    };
    void* hold_client_ = nullptr;
    void (*hold_started_)(void*) = nullptr;
    void* hold_started_user_ = nullptr;
    held_zone held_[k_max_outputs]{};
    /// `r`'s zone becomes part of its output's hold (it is going with its client).
    void hold_zone_of(const layer_record& r);
};

} // namespace lx::compositor

namespace {

constexpr const char* k_layer_category = "compositor.layer";

[[nodiscard]] bool layer_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

[[nodiscard]] const char* level_name(unsigned level) {
    switch (level) {
    case 0: return "background";
    case 1: return "bottom";
    case 2: return "top";
    case 3: return "overlay";
    default: return "?";
    }
}

[[nodiscard]] const char* edge_name(unsigned edge) {
    switch (edge) {
    case lx::compositor::k_layer_anchor_top: return "top";
    case lx::compositor::k_layer_anchor_bottom: return "bottom";
    case lx::compositor::k_layer_anchor_left: return "left";
    case lx::compositor::k_layer_anchor_right: return "right";
    default: return "none";
    }
}

[[nodiscard]] bool same_rect(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] int clamp_int(long long v) {
    if (v > 1'000'000'000)
        return 1'000'000'000;
    if (v < -1'000'000'000)
        return -1'000'000'000;
    return static_cast<int>(v);
}

/// One axis of a layer surface's box. `lo`/`hi`: anchored to the low (left/top) and high
/// (right/bottom) edge.
struct axis_result {
    int pos = 0;
    int len = 0;
    int configure = 0;
    bool closed = false;
};

[[nodiscard]] axis_result place_axis(bool lo_anchor, bool hi_anchor, int margin_lo,
                                     int margin_hi, int size, int committed, bool mapped,
                                     int origin, int extent) {
    axis_result r{};
    const long long lo = static_cast<long long>(origin) + (lo_anchor ? margin_lo : 0);
    const long long hi = static_cast<long long>(origin) + extent - (hi_anchor ? margin_hi : 0);
    // size 0 is only valid with both anchors (checked at commit): the compositor assigns
    // the span between them.
    const long long cfg = size > 0 ? size : hi - lo;
    if (cfg <= 0) {
        r.closed = true;
        return r;
    }
    // Once mapped, the surface is placed at the size it actually committed — a client may
    // pick a smaller one, which the spec centres between two opposite anchors.
    const long long len = mapped && committed > 0 ? committed : cfg;
    long long pos = 0;
    if (lo_anchor && hi_anchor)
        pos = lo + (hi - lo - len) / 2;
    else if (lo_anchor)
        pos = lo;
    else if (hi_anchor)
        pos = hi - len;
    else
        pos = static_cast<long long>(origin) + (static_cast<long long>(extent) - len) / 2;
    r.pos = clamp_int(pos);
    r.len = clamp_int(len);
    r.configure = clamp_int(size > 0 ? size : cfg);
    return r;
}

} // namespace

// ── Pure helpers ───────────────────────────────────────────────────────────

unsigned lx::compositor::layer_exclusive_edge(const layer_props& p) {
    if (p.zone <= 0)
        return 0;
    if (p.exclusive_edge != 0)
        return p.exclusive_edge;
    constexpr unsigned t = k_layer_anchor_top, b = k_layer_anchor_bottom,
                       l = k_layer_anchor_left, r = k_layer_anchor_right;
    switch (p.anchor) {
    case t:
    case t | l | r:
        return t;
    case b:
    case b | l | r:
        return b;
    case l:
    case l | t | b:
        return l;
    case r:
    case r | t | b:
        return r;
    default:
        return 0;
    }
}

lx::compositor::layer_state_error lx::compositor::validate_layer_state(const layer_props& p) {
    const bool lr = (p.anchor & (k_layer_anchor_left | k_layer_anchor_right)) ==
                    (k_layer_anchor_left | k_layer_anchor_right);
    const bool tb = (p.anchor & (k_layer_anchor_top | k_layer_anchor_bottom)) ==
                    (k_layer_anchor_top | k_layer_anchor_bottom);
    if ((p.size.width == 0 && !lr) || (p.size.height == 0 && !tb))
        return layer_state_error::invalid_size;
    // "The edge must be one the surface is anchored to."
    if (p.exclusive_edge != 0 && (p.anchor & p.exclusive_edge) != p.exclusive_edge)
        return layer_state_error::invalid_exclusive_edge;
    return layer_state_error::none;
}

void lx::compositor::arrange_layers(lx::rect2i full, const layer_arrange_in* in,
                                    layer_arrange_out* out, unsigned n, lx::rect2i& usable) {
    usable = full;
    for (unsigned i = 0; i < n; ++i)
        out[i] = {};
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < n; ++i) {
            const layer_arrange_in& a = in[i];
            if (!a.participates || !a.props)
                continue;
            const layer_props& p = *a.props;
            const bool exclusive = p.zone > 0;
            if ((pass == 0) != exclusive)
                continue;
            // -1: "extend it all the way to the edges it is anchored to", ignoring zones.
            const lx::rect2i bounds = p.zone == -1 ? full : usable;
            const axis_result x =
                place_axis((p.anchor & k_layer_anchor_left) != 0,
                           (p.anchor & k_layer_anchor_right) != 0, p.margin_left,
                           p.margin_right, p.size.width, a.committed.width, a.mapped, bounds.x,
                           bounds.width);
            const axis_result y =
                place_axis((p.anchor & k_layer_anchor_top) != 0,
                           (p.anchor & k_layer_anchor_bottom) != 0, p.margin_top,
                           p.margin_bottom, p.size.height, a.committed.height, a.mapped,
                           bounds.y, bounds.height);
            layer_arrange_out& o = out[i];
            if (x.closed || y.closed) {
                o.closed = true;
                continue;
            }
            o.box = {x.pos, y.pos, x.len, y.len};
            o.configure = {p.size.width > 0 ? p.size.width : x.configure,
                           p.size.height > 0 ? p.size.height : y.configure};
            if (!exclusive || !a.mapped)
                continue;
            const unsigned edge = layer_exclusive_edge(p);
            o.zone_edge = edge;
            const long long z = p.zone;
            switch (edge) {
            case k_layer_anchor_top: {
                const long long d = z + p.margin_top;
                usable.y = clamp_int(usable.y + d);
                usable.height = clamp_int(usable.height - d);
                break;
            }
            case k_layer_anchor_bottom:
                usable.height = clamp_int(usable.height - (z + p.margin_bottom));
                break;
            case k_layer_anchor_left: {
                const long long d = z + p.margin_left;
                usable.x = clamp_int(usable.x + d);
                usable.width = clamp_int(usable.width - d);
                break;
            }
            case k_layer_anchor_right:
                usable.width = clamp_int(usable.width - (z + p.margin_right));
                break;
            default:
                break;
            }
            if (usable.width < 0)
                usable.width = 0;
            if (usable.height < 0)
                usable.height = 0;
        }
    }
}

// ── layer_shell ────────────────────────────────────────────────────────────

void lx::compositor::layer_shell::bind(surface_manager& surfaces, surface_tree& tree,
                                       scene::container_node* const (&layers)[k_layer_levels]) {
    surfaces_ = &surfaces;
    tree_ = &tree;
    for (unsigned i = 0; i < k_layer_levels; ++i)
        layers_[i] = layers[i];
}

void lx::compositor::layer_shell::log_line(const char* line) const {
    lx::trace::logger::global().log(lx::trace::level::debug, k_layer_category, line);
}

template <typename... Args>
void lx::compositor::layer_shell::log_line(std::format_string<Args...> fmt, Args&&... args) const {
    lx::trace::logger::global().logf(lx::trace::level::debug, k_layer_category, fmt,
                                     std::forward<Args>(args)...);
}

bool lx::compositor::layer_shell::drawable(lx::surface_id surface) const {
    const layer_record* r = find(surface);
    return r && !r->closed;
}

lx::compositor::layer_shell::create_error
lx::compositor::layer_shell::create(lx::surface_id surface, lx::client_id client,
                                    void* client_native, void* surface_res, void* layer_res,
                                    unsigned version, unsigned layer, const char* name_space,
                                    unsigned output) {
    if (find(surface) || all_.size() >= k_max)
        return create_error::no_memory;
    for (unsigned i = 0; i < k_max; ++i) {
        if (records_[i].used)
            continue;
        if (!tree_ || !tree_->make_root(surface, root_kind::layer) ||
            !index_.insert(surface.id(), static_cast<int>(i)))
            return create_error::no_memory;
        layer_record& r = records_[i];
        r = {};
        r.used = true;
        r.surface = surface;
        r.client = client;
        r.client_native = client_native;
        r.surface_res = surface_res;
        r.layer_res = layer_res;
        r.version = version;
        r.output = output < k_max_outputs ? output : 0;
        r.pending.layer = layer;
        r.current.layer = layer;
        std::snprintf(r.name_space, sizeof(r.name_space), "%s", name_space ? name_space : "");
        r.created_seq = next_seq_++;
        all_.push_back(i);
        tree_->set_root_tag(surface, 0);
        if (layer_debug()) {
            log_line("layer create surface={} layer={} namespace={} output={}", surface.id(),
                     level_name(layer), r.name_space, r.output);
        }
        return create_error::none;
    }
    return create_error::no_memory;
}

lx::compositor::layer_shell::ack_result
lx::compositor::layer_shell::on_ack(lx::surface_id surface, unsigned serial) {
    layer_record* r = find(surface);
    if (!r)
        return ack_result::duplicate;
    for (unsigned i = 0; i < r->sent_count; ++i) {
        if (r->sent[i] != serial)
            continue;
        // Acking a serial acks every older one with it.
        r->acked = serial;
        const unsigned popped = i + 1;
        for (unsigned j = popped; j < r->sent_count; ++j)
            r->sent[j - popped] = r->sent[j];
        r->sent_count -= popped;
        r->ever_acked = true;
        if (layer_debug()) {
            log_line("layer ack surface={} serial={}", surface.id(), serial);
        }
        return ack_result::ok;
    }
    if ((r->acked != 0 && static_cast<int>(serial - r->acked) <= 0) ||
        (r->dropped_upto != 0 && static_cast<int>(serial - r->dropped_upto) <= 0))
        return ack_result::duplicate;
    return ack_result::invalid;
}

void lx::compositor::layer_shell::apply_pending(lx::surface_id surface) {
    layer_record* r = find(surface);
    if (!r || r->closed)
        return;
    if (r->pending == r->current)
        return;
    r->interactivity_changed = r->pending.interactivity != r->current.interactivity;
    r->current = r->pending;
    r->props_changed = true;
}

void lx::compositor::layer_shell::send_configure(layer_record& r, lx::size2i size) {
    if (!ops_.send_configure || !r.layer_res)
        return;
    if (ops_.client_alive && !ops_.client_alive(r.client_native))
        return;
    const unsigned serial = ops_.next_serial ? ops_.next_serial(ops_.user) : r.acked + 1;
    if (r.sent_count == layer_record::k_sent_cap) {
        r.dropped_upto = r.sent[0];
        for (unsigned i = 1; i < r.sent_count; ++i)
            r.sent[i - 1] = r.sent[i];
        --r.sent_count;
    }
    r.sent[r.sent_count++] = serial;
    r.configured = true;
    r.last_configure = size;
    ops_.send_configure(r.layer_res, serial, size.width, size.height);
    if (layer_debug()) {
        log_line("layer configure surface={} serial={} w={} h={}", r.surface.id(), serial,
                 size.width, size.height);
    }
}

void lx::compositor::layer_shell::mapped_add(unsigned level, unsigned slot) {
    if (level >= k_layer_levels || mapped_count_[level] >= k_max)
        return;
    mapped_[level][mapped_count_[level]++] = slot;
    ++mapped_total_;
}

void lx::compositor::layer_shell::mapped_remove(unsigned level, unsigned slot) {
    if (level >= k_layer_levels)
        return;
    unsigned* list = mapped_[level];
    for (unsigned i = 0; i < mapped_count_[level]; ++i) {
        if (list[i] != slot)
            continue;
        for (unsigned j = i + 1; j < mapped_count_[level]; ++j)
            list[j - 1] = list[j];
        --mapped_count_[level];
        --mapped_total_;
        return;
    }
}

void lx::compositor::layer_shell::arrange() {
    for (unsigned o = 0; o < k_max_outputs; ++o) {
        const lx::rect2i area = host_.output_area ? host_.output_area(host_.user, o) : lx::rect2i{};
        if (area.width > 0 && area.height > 0)
            arrange_all(o, lx::surface_id{});
        else
            arranged_[o] = false;
    }
}

int lx::compositor::layer_shell::output_of(lx::surface_id surface) const {
    const layer_record* r = find(surface);
    return r ? static_cast<int>(r->output) : -1;
}

void lx::compositor::layer_shell::close_output(unsigned slot) {
    for (unsigned i = 0; i < all_.size(); ++i) {
        layer_record& r = records_[all_[i]];
        if (r.used && r.output == slot && !r.closed)
            close(r, "output-removed");
    }
    arranged_[slot < k_max_outputs ? slot : 0] = false;
    // A gone output's held zone must not apply to whatever takes its slot next.
    if (slot < k_max_outputs)
        held_[slot] = {};
}

void lx::compositor::layer_shell::close_surface(lx::surface_id surface) {
    if (layer_record* r = find(surface))
        close(*r, "output-gone");
}

void lx::compositor::layer_shell::arrange_all(unsigned output, lx::surface_id committing) {
    if (output >= k_max_outputs)
        return;
    const lx::rect2i full = host_.output_area ? host_.output_area(host_.user, output)
                                              : lx::rect2i{0, 0, 1920, 1080};
    lx::rect2i usable = full;
    unsigned n = 0;
    // Bounded: a round that closes surfaces runs once more without them.
    for (unsigned round = 0; round < 2; ++round) {
        unsigned slots[k_max];
        layer_arrange_in in[k_max];
        layer_arrange_out out[k_max];
        n = 0;
        for (unsigned lv = k_layer_levels; lv-- > 0;) {
            for (const unsigned slot : all_) {
                layer_record& r = records_[slot];
                if (!r.used || r.current.layer != lv || r.output != output)
                    continue;
                in[n] = {&r.current, r.committed, r.mapped, r.initial_commit_seen && !r.closed};
                slots[n++] = slot;
            }
        }
        arrange_layers(full, in, out, n, usable);
        if (const held_zone& h = held_[output]; h.active) {
            // Whatever the remaining surfaces reserve, and at least what the gone shell did.
            const int x0 = std::max(usable.x, full.x + h.left);
            const int y0 = std::max(usable.y, full.y + h.top);
            const int x1 = std::min(usable.x + usable.width, full.x + full.width - h.right);
            const int y1 = std::min(usable.y + usable.height, full.y + full.height - h.bottom);
            usable = {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
        }
        bool closed_any = false;
        for (unsigned k = 0; k < n; ++k) {
            if (!in[k].participates)
                continue;
            layer_record& r = records_[slots[k]];
            if (out[k].closed) {
                close(r, "negative-size");
                closed_any = true;
                continue;
            }
            // A configure for a surface that has none yet, or whose assigned size changed.
            const lx::size2i cfg = out[k].configure;
            if (!r.configured || cfg.width != r.last_configure.width ||
                cfg.height != r.last_configure.height)
                send_configure(r, cfg);
            if (r.mapped) {
                const bool moved = !same_rect(r.box, out[k].box);
                r.box = out[k].box;
                if (moved) {
                    // The committing surface's tree is dirty: its commit's own sync places
                    // it. Any other surface moves now — in the same UI call, so the same
                    // snapshot as the change that pushed it.
                    tree_->set_root_origin(r.surface, {r.box.x, r.box.y});
                    if (!(r.surface == committing))
                        tree_->sync_scene(r.surface, "move");
                    if (host_.surface_moved)
                        host_.surface_moved(host_.user, r.surface);
                }
                const unsigned edge = out[k].zone_edge;
                if (layer_debug() && (edge != r.logged_edge || r.current.zone != r.logged_zone)) {
                    r.logged_edge = edge;
                    r.logged_zone = r.current.zone;
                    log_line("layer exclusive surface={} edge={} zone={}", r.surface.id(),
                             edge_name(edge), r.current.zone);
                }
            }
        }
        if (!closed_any)
            break;
    }
    if (layer_debug()) {
        log_line("layer arrange output={} usable={},{},{}x{} surfaces={}", output, usable.x,
                 usable.y, usable.width, usable.height, n);
    }
    if (!arranged_[output] || !same_rect(usable, usable_[output])) {
        arranged_[output] = true;
        usable_[output] = usable;
        if (host_.usable_changed)
            host_.usable_changed(host_.user, output, usable_[output]);
    }
}

void lx::compositor::layer_shell::map(layer_record& r) {
    const int slot = slot_of(r.surface);
    const unsigned level = r.current.layer < k_layer_levels ? r.current.layer : 0;
    scene::container_node* layer = layers_[level];
    if (slot < 0 || !layer)
        return;
    r.mapped = true;
    r.mapped_layer = level;
    if (r.output < k_max_outputs && held_[r.output].active && r.current.zone > 0) {
        held_[r.output] = {};
        lx::trace::logger::global().logf(lx::trace::level::info, k_layer_category,
                                         "zone hold released on output {}: a new zone mapped",
                                         r.output);
    }
    // Placed first (the tree only records the origin while unmapped), so mapping creates
    // every node where it belongs; the arrangement also counts the zone now that it maps.
    arrange_all(r.output, r.surface);
    if (!tree_->map_root(r.surface, *layer, ~0u)) {
        r.mapped = false;
        lx::trace::logger::global().log(lx::trace::level::warn, k_layer_category,
                                        "layer map failed: layer container full");
        arrange_all(r.output, lx::surface_id{});
        return;
    }
    mapped_add(level, static_cast<unsigned>(slot));
    {
        lx::trace::logger::global().logf(lx::trace::level::info, k_layer_category,
                                         "layer map surface={} layer={} namespace={} x={} y={} "
                                         "w={} h={}",
                                         r.surface.id(), level_name(level), r.name_space, r.box.x,
                                         r.box.y, r.box.width, r.box.height);
    }
    if (host_.surface_mapped)
        host_.surface_mapped(host_.user, r.surface);
    if (host_.focus_changed)
        host_.focus_changed(host_.user);
}

void lx::compositor::layer_shell::unmap(layer_record& r, const char* reason) {
    if (!r.mapped)
        return;
    // Popups first: none may stay on screen without the surface they hang off.
    if (host_.surface_unmapped)
        host_.surface_unmapped(host_.user, r.surface, reason);
    r.mapped = false;
    tree_->unmap_root(r.surface);
    const int slot = slot_of(r.surface);
    if (slot >= 0)
        mapped_remove(r.mapped_layer, static_cast<unsigned>(slot));
    r.logged_edge = ~0u;
    {
        lx::trace::logger::global().logf(lx::trace::level::info, k_layer_category,
                                         "layer unmap surface={} reason={}", r.surface.id(),
                                         reason);
    }
    if (host_.focus_changed)
        host_.focus_changed(host_.user);
}

void lx::compositor::layer_shell::reset_after_unmap(layer_record& r) {
    // "The layer_surface returns to the state it had right after get_layer_surface": a new
    // initial commit and configure follow. The double-buffered properties stay.
    r.initial_commit_seen = false;
    r.ever_acked = false;
    r.configured = false;
    r.last_configure = {};
    r.sent_count = 0;
    r.dropped_upto = 0;
    r.acked = 0;
    r.committed = {};
    r.box = {};
}

void lx::compositor::layer_shell::close(layer_record& r, const char* reason) {
    if (r.closed)
        return;
    r.closed = true;
    if (layer_debug()) {
        log_line("layer closed surface={} reason={}", r.surface.id(), reason);
    }
    const bool alive = !ops_.client_alive || ops_.client_alive(r.client_native);
    if (alive && ops_.send_closed && r.layer_res)
        ops_.send_closed(r.layer_res);
    unmap(r, "closed");
}

void lx::compositor::layer_shell::on_commit(lx::surface_id surface,
                                            const layer_commit_info& info) {
    layer_record* r = find(surface);
    if (!r || r->closed)
        return;
    const bool props_changed = r->props_changed;
    const bool interactivity_changed = r->interactivity_changed;
    r->props_changed = false;
    r->interactivity_changed = false;

    if (!r->initial_commit_seen) {
        // The initial commit (no buffer: the commit rules rejected one) gets the first
        // configure, sent in the same dispatch by the arrangement.
        r->initial_commit_seen = true;
        arrange_all(r->output, surface);
        return;
    }
    if (!r->mapped) {
        if (info.has_buffer && r->ever_acked) {
            r->committed = info.size;
            map(*r);
        } else if (props_changed) {
            arrange_all(r->output, surface); // not mapped yet: a new size or anchor gets a new configure
        }
        return;
    }
    if (!info.has_buffer) {
        unmap(*r, "null-buffer");
        reset_after_unmap(*r);
        arrange_all(r->output, lx::surface_id{});
        return;
    }
    // set_layer: the tree moves to the other layer's container, on top there.
    const unsigned level = r->current.layer < k_layer_levels ? r->current.layer : 0;
    bool layer_moved = false;
    if (level != r->mapped_layer && layers_[level] && tree_->move_root(surface, *layers_[level])) {
        const int slot = slot_of(surface);
        if (slot >= 0) {
            mapped_remove(r->mapped_layer, static_cast<unsigned>(slot));
            mapped_add(level, static_cast<unsigned>(slot));
        }
        r->mapped_layer = level;
        layer_moved = true;
    }
    const bool resized =
        info.size.width != r->committed.width || info.size.height != r->committed.height;
    r->committed = info.size;
    if (props_changed || resized)
        arrange_all(r->output, surface);
    // Exclusive keyboard focus depends on the layer too (top/overlay take it, bottom and
    // background behave as on_demand), so a layer move re-resolves focus like an
    // interactivity change does.
    if ((interactivity_changed || layer_moved) && host_.focus_changed)
        host_.focus_changed(host_.user);
}

void lx::compositor::layer_shell::destroy(lx::surface_id surface) {
    layer_record* r = find(surface);
    if (!r)
        return;
    const bool participated = r->initial_commit_seen && !r->closed;
    const unsigned output = r->output;
    // The shell's panel going away other than at a logout (a crash, a stop for a restart),
    // and not because its output went: its zone is held (ADR 011 S6).
    if (r->mapped && !r->closed && hold_client_ && r->client_native == hold_client_)
        hold_zone_of(*r);
    if (r->mapped)
        unmap(*r, "destroyed");
    else if (host_.surface_unmapped)
        host_.surface_unmapped(host_.user, surface, "destroyed");
    const int slot = slot_of(surface);
    if (const auto it =
            std::ranges::find_if(all_, [slot](unsigned s) { return static_cast<int>(s) == slot; });
        it != all_.end())
        all_.erase(it);
    index_.erase(surface.id());
    *r = {};
    // Its exclusive zone goes with it.
    if (participated)
        arrange_all(output, lx::surface_id{});
}

void lx::compositor::layer_shell::hold_zone_of(const layer_record& r) {
    if (r.output >= k_max_outputs || r.current.zone <= 0)
        return;
    held_zone& h = held_[r.output];
    const layer_props& p = r.current;
    switch (layer_exclusive_edge(p)) {
    case k_layer_anchor_top: h.top = std::max(h.top, p.zone + p.margin_top); break;
    case k_layer_anchor_bottom: h.bottom = std::max(h.bottom, p.zone + p.margin_bottom); break;
    case k_layer_anchor_left: h.left = std::max(h.left, p.zone + p.margin_left); break;
    case k_layer_anchor_right: h.right = std::max(h.right, p.zone + p.margin_right); break;
    default: return;
    }
    const bool started = !zone_hold_active();
    h.active = true;
    lx::trace::logger::global().logf(lx::trace::level::info, k_layer_category,
                                     "zone hold on output {}: top={} bottom={} left={} right={} "
                                     "(the shell went without a logout)",
                                     r.output, h.top, h.bottom, h.left, h.right);
    if (started && hold_started_)
        hold_started_(hold_started_user_);
}

void lx::compositor::layer_shell::release_zone_hold() {
    for (unsigned o = 0; o < k_max_outputs; ++o) {
        if (!held_[o].active)
            continue;
        held_[o] = {};
        lx::trace::logger::global().logf(lx::trace::level::info, k_layer_category,
                                         "zone hold released on output {}", o);
        arrange_all(o, lx::surface_id{});
    }
}

bool lx::compositor::layer_shell::zone_hold_active() const {
    for (const auto& h : held_)
        if (h.active)
            return true;
    return false;
}

const lx::compositor::layer_record* lx::compositor::layer_shell::exclusive_focus() const {
    for (unsigned lv = k_layer_levels; lv-- > static_cast<unsigned>(layer_level::top);) {
        for (unsigned i = mapped_count_[lv]; i-- > 0;) {
            const layer_record& r = records_[mapped_[lv][i]];
            if (r.used && r.mapped &&
                r.current.interactivity == static_cast<unsigned>(layer_interactivity::exclusive))
                return &r;
        }
    }
    return nullptr;
}

bool lx::compositor::layer_shell::focusable(lx::surface_id surface) const {
    const layer_record* r = find(surface);
    return r && r->mapped && !r->closed &&
           r->current.interactivity != static_cast<unsigned>(layer_interactivity::none);
}

bool lx::compositor::layer_shell::click_focusable(lx::surface_id surface) const {
    const layer_record* r = find(surface);
    if (!r || !r->mapped || r->closed)
        return false;
    if (r->current.interactivity == static_cast<unsigned>(layer_interactivity::on_demand))
        return true;
    return r->current.interactivity == static_cast<unsigned>(layer_interactivity::exclusive) &&
           r->mapped_layer < static_cast<unsigned>(layer_level::top);
}

bool lx::compositor::layer_shell::focus_on_map(lx::surface_id surface) const {
    const layer_record* r = find(surface);
    return r && r->mapped && !r->closed &&
           r->current.interactivity == static_cast<unsigned>(layer_interactivity::on_demand) &&
           r->mapped_layer >= static_cast<unsigned>(layer_level::top);
}

int lx::compositor::layer_shell::mapped_level(lx::surface_id surface) const {
    const layer_record* r = find(surface);
    return r && r->mapped ? static_cast<int>(r->mapped_layer) : -1;
}

bool lx::compositor::layer_shell::hit(layer_level level, double gx, double gy, tree_hit& out,
                                      const layer_record*& rec) const {
    const unsigned lv = static_cast<unsigned>(level);
    if (lv >= k_layer_levels || !tree_)
        return false;
    for (unsigned i = mapped_count_[lv]; i-- > 0;) {
        const layer_record& r = records_[mapped_[lv][i]];
        if (!r.used || !r.mapped)
            continue;
        if (tree_->hit(r.surface, gx, gy, out)) {
            rec = &r;
            return true;
        }
    }
    return false;
}
