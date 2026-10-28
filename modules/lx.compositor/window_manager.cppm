module;

#include <signal.h>

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.scene;
import lx.input;

export module lx.compositor:wm;

import std;

import :toplevel;
import :surface;
import :surface_tree;
import :shell_bridge;
import :output;
import :input_router;
import :xcursor;
import :cursor;
import :popup;
import :decoration;
import :layer_shell;
import :dnd;
import :liveness;
import :activation;
import :workspaces;
import :tiling;

namespace {

constexpr lx::trace::category log_deco{"compositor.deco"};
constexpr lx::trace::category log_layer{"compositor.layer"};

} // namespace

export namespace lx::compositor {

/// Interactive grab kinds. One grab at a time, seat-wide.
enum class grab_mode : unsigned char { none, move, resize };
/// Who started the grab. The executor is the same for all four; only validation differs.
/// `decoration` is a press on a server-side frame — compositor-originated, like a keybinding.
enum class grab_source : unsigned char { client, shell, keybinding, decoration };
enum class grab_end_reason : unsigned char { release, escape, destroyed, unmapped };

/// State of the interactive move/resize in progress. UI affinity.
struct wm_grab {
    grab_mode mode = grab_mode::none;
    grab_source source = grab_source::client;
    lx::toplevel_id toplevel{};
    /// `resize_edge_bits`; 0 for a move.
    unsigned edges = 0;
    /// evdev code of the button whose release ends the grab.
    unsigned button = 0;
    /// Pointer at begin, global.
    double start_x = 0.0;
    double start_y = 0.0;
    /// Window geometry at begin: the move origin, the resize anchor, the Escape restore.
    lx::rect2i start{};
    /// Newest pointer position; applied once per tick, not per event.
    double now_x = 0.0;
    double now_y = 0.0;
    bool motion_pending = false;
    /// Last size asked of the client, so an unchanged target writes nothing.
    lx::size2i last_target{};
    /// window.move-keyboard / window.resize-keyboard (design-session-and-wm K.6): no button
    /// holds it. The arrow keys move the pointer, Return or any button press ends it.
    bool keyboard = false;
    /// A keyboard resize: the axes whose edge an arrow key has picked (`edge_*` bits).
    unsigned picked = 0;
    /// A move of a maximized or snapped window that has not travelled 8 px yet (K.5).
    bool restore_pending = false;
    /// Drag-to-restore sent its configure; until the commit answering it, motion moves where
    /// the restored window lands instead of the window.
    bool awaiting_restore = false;
    /// What drag-to-restore undid, put back by Escape: snap bits, or maximized.
    unsigned restored_snap = 0;
    bool restored_max = false;
    /// The window's normal geometry: what a snap made at the release goes back to.
    lx::rect2i normal{};
    /// The edge zone under the pointer (K.4: snap bits, or `k_snap_maximize`) and the output
    /// it is on (-1: none).
    unsigned zone = 0;
    int zone_output = -1;
};

/// config.toml [windows] (design-session-and-wm C.2, K.3–K.5).
struct wm_window_config {
    /// A move dropped at an outer edge of the layout snaps (halves, quarters, maximize).
    bool edge_snap = true;
    /// Logical px from an outer edge that make the zone, and along an edge that make a corner.
    int edge_snap_distance = 8;
    int edge_snap_corner = 64;
    /// The translucent rect showing where the window will go.
    bool snap_preview = true;
    /// A move of a maximized or snapped window restores its size under the pointer.
    bool drag_restore = true;
};

/// `next_snap`'s and `edge_snap_zone`'s "maximize", beside the `edge_*` bits of a snap.
inline constexpr unsigned k_snap_maximize = 0x10;
/// The Super+arrow keys of snapping.
enum class snap_key : unsigned char { left, right, up, down };
/// What a snap key does to a window (`next_snap`).
enum class snap_move : unsigned char { none, snap, maximize, restore, minimize };
struct snap_step {
    snap_move move = snap_move::none;
    /// snap_move::snap: the half or quarter (`edge_*` bits).
    unsigned snap = 0;
    /// -1 / +1: on the output to the left / right (nothing when there is none).
    int output_dx = 0;
};

/// Wire operations the window manager needs, installed by the protocol layer so that no
/// Wayland type appears in this partition.
struct wm_protocol_ops {
    /// Sends `wm_capabilities` (when `initial`, v5+), `configure_bounds` (when `bounds`
    /// is non-empty, v4+), `xdg_toplevel.configure`, `zxdg_toplevel_decoration_v1.configure`
    /// (when `decoration` is non-null) and `xdg_surface.configure(serial)` — in that order:
    /// the decoration mode is part of the configure sequence the serial closes.
    void (*send_configure)(void* xdg_toplevel, void* xdg_surface, const toplevel_configure& c,
                           lx::size2i bounds, bool initial, void* decoration) = nullptr;
    /// Next serial in the seat's serial space, which input events share.
    unsigned (*next_serial)(void* user) = nullptr;
    /// `xdg_toplevel.close` — the shell's request_close.
    void (*send_close)(void* xdg_toplevel) = nullptr;
    /// The newest key or button press serial delivered to the client owning `surface` (a
    /// `wl_surface` resource); 0 when none. xdg-activation's user-time rule reads it.
    unsigned (*last_user_serial)(void* user, void* surface) = nullptr;
    /// The record of `id` is about to go (any of its three objects was destroyed): what the
    /// wire keyed by it lets go — its icon references, its xdg-foreign exports.
    void (*toplevel_destroyed)(void* user, lx::toplevel_id id) = nullptr;
    void* user = nullptr;
};

/// What kind of root the hit surface's tree has. `ime_popup`: an input method's popup
/// (zwp_input_popup_surface_v2) — it takes the pointer, never the keyboard.
enum class hit_owner : unsigned char { toplevel, popup, layer, lock, ime_popup };
/// What was hit: a client surface, or the compositor's frame around a window.
enum class hit_target : unsigned char { surface, decoration };

/// The topmost mapped surface under a point, from `window_manager::hit_test`.
struct wm_hit {
    /// The window the point is in (the owner, for click-to-focus): for a popup, the toplevel
    /// at the root of its chain; null for a layer surface and a chain rooted at one.
    lx::toplevel_id id{};
    /// The `wl_surface` resource of the surface actually hit — the toplevel's own or one of
    /// its subsurfaces (void*: no Wayland types in this partition).
    void* surface_resource = nullptr;
    /// Surface-local position of the point, in the hit surface.
    double sx = 0.0;
    double sy = 0.0;
    /// The surface hit, and the root of its tree (the toplevel's, popup's or layer
    /// surface's surface).
    lx::surface_id surface{};
    lx::surface_id root{};
    hit_owner owner = hit_owner::toplevel;
    /// The client owning the hit surface — what a popup grab's owner-events test compares.
    lx::client_id client{};
    /// `decoration`: the point is on window `id`'s server-side frame, in `zone`;
    /// `surface_resource` and `surface` are null then.
    hit_target target = hit_target::surface;
    deco_zone zone = deco_zone::none;
};

struct wm_hit;

/// Told after every operation that changed workspaces or a window's membership (design
/// Part W): ext-workspace and the control socket follow the model from here.
struct workspace_observer {
    void (*changed)(void* user) = nullptr;
    void* user = nullptr;
};

/// How focus follows the pointer (config.toml [focus]).
struct wm_focus_policy {
    /// Focus follows the pointer into a window (kept over the desktop); else click to focus.
    bool sloppy = false;
    bool raise_on_click = true;
    bool raise_on_sloppy = false;
};

/// What a configured pointer-button binding asks of a press (`set_button_binding`).
enum class wm_button_binding : unsigned char {
    none,
    /// window.move-interactive / window.resize-interactive on the window under the pointer.
    move,
    resize,
    /// Another action, already executed by the hook: the press (and release) is consumed.
    consumed,
};

/// The session lock's side of input (ext-session-lock-v1), installed by the compositor so
/// the lock records stay out of this partition. Consulted only while the session is locked.
struct wm_lock_ops {
    /// The topmost lock surface under the global point (`out.owner` = lock). False when none
    /// is shown there (no lock surface on that output yet: the solid colour takes nothing).
    bool (*hit)(void* user, double x, double y, wm_hit& out) = nullptr;
    /// The lock surface with the keyboard (its `wl_surface` resource), and its id; null when
    /// none has content yet.
    void* (*keyboard)(void* user, lx::surface_id& surface) = nullptr;
    /// A press landed on lock surface `surface` (click-to-focus among lock surfaces).
    void (*clicked)(void* user, lx::surface_id surface) = nullptr;
    void* user = nullptr;
};

/// Input-method popups (text-input / input-method-v2), installed by the compositor: they are
/// drawn above everything a client draws — the lock layer included, for a lock surface's text
/// input — so they are hit first. A press on one moves neither keyboard focus nor stacking.
struct wm_ime_ops {
    /// The topmost shown input popup under the global point (`out.owner` = ime_popup).
    bool (*hit)(void* user, double x, double y, wm_hit& out) = nullptr;
    void* user = nullptr;
};

/// What a commit on a toplevel's surface carried, as far as the window manager cares.
struct toplevel_commit_info {
    /// The surface has content after this commit (false: never had any, or null attached).
    bool has_buffer = false;
    /// Logical surface size (buffer scale, transform and viewport applied).
    lx::size2i surface_size{};
    /// Surface-local bounding box of the surface and its mapped subsurfaces after this
    /// commit (`surface_tree::subtree_bbox`) — what the window geometry is clamped to.
    lx::rect2i tree_bbox{};
};

/// The black under a fullscreen window (M11): covers its output's logical rect, so nothing
/// else shows on that output whatever the window draws (xdg-shell: a non-opaque or smaller
/// fullscreen surface is shown over black). One per output, in the fullscreen layer.
class fullscreen_backdrop final : public scene::scene_node {
public:
    ~fullscreen_backdrop() override;
    [[nodiscard]] scene::node_kind kind() const override;
    using scene::scene_node::emit_draws;
    void emit_draws(scene::draw_list& out, const scene::emit_context& ctx) const override;
    /// Texture id of the 1×1 opaque black image it draws scaled. Its own: the decoration
    /// atlas is 0x7FFF'FE00 (the two shared it, so a fullscreen window's backdrop replaced the
    /// server-side frames' atlas).
    static constexpr unsigned k_texture_id = 0x7FFF'FD00u;
    void show(lx::rect2i logical);
    void hide();
    [[nodiscard]] bool shown() const { return shown_; }
    [[nodiscard]] lx::rect2i rect() const { return rect_; }

private:
    lx::rect2i rect_{};
    bool shown_ = false;
};

/// The edge-snap preview (design-session-and-wm K.4): where a dragged window will go if it is
/// dropped now — a translucent accent rect with a 1 px border, in its own layer between the
/// windows and the top layer-shell layer (so in output captures, never in a window's). Two
/// 1×1 premultiplied textures stretched: five draws while shown, none otherwise; damaged
/// only when it appears, moves or goes.
class snap_preview final : public scene::scene_node {
public:
    ~snap_preview() override;
    [[nodiscard]] scene::node_kind kind() const override;
    using scene::scene_node::emit_draws;
    void emit_draws(scene::draw_list& out, const scene::emit_context& ctx) const override;
    static constexpr unsigned k_fill_texture = 0x7FFF'FD01u;
    static constexpr unsigned k_border_texture = 0x7FFF'FD02u;
    void show(lx::rect2i logical);
    void hide();
    [[nodiscard]] bool shown() const { return shown_; }
    [[nodiscard]] lx::rect2i rect() const { return rect_; }

private:
    lx::rect2i rect_{};
    bool shown_ = false;
};

/// Window placement, configure flow and focus. UI affinity.
///
/// The executor of window-management *state*: `toplevel_record::geometry` changes only
/// here, the scene node follows it, and the shell is told. Policy (what to raise, where to
/// put things) arrives from the shell over zlm_policy_bridge_v1; until the shell drives it,
/// the defaults here are the placeholder policy.
class window_manager {
public:
    /// `windows_layer` holds exactly the mapped toplevels' tree containers, in stacking
    /// order — nothing else may be added to it.
    void bind(toplevel_manager& toplevels, surface_manager& surfaces, surface_tree& tree,
              scene::scene_graph& scene, scene::container_node& windows_layer,
              input_router& router, lx::input::seat& seat, shell_bridge& shell,
              output_manager& outputs);
    /// Popups (optional): their layer is hit before the windows, their grab filters pointer
    /// focus (owner events) and takes the keyboard, and their parents' moves reach them.
    void set_popups(popup_manager* popups);
    /// Server-side frames (optional): placement and constraints use the frame, presses on it
    /// move, resize, close and lower the window.
    void set_decorations(decoration_manager* decorations) { decorations_ = decorations; }
    /// Layer-shell (optional): its exclusive zones make the usable area, its surfaces are hit
    /// above (overlay, top) and below (bottom, background) the windows, and its keyboard
    /// interactivity takes part in keyboard-focus resolution.
    void set_layers(layer_shell* layers);
    void set_focus_policy(const wm_focus_policy& p) { focus_ = p; }
    [[nodiscard]] const wm_focus_policy& focus_policy() const { return focus_; }
    /// The configured pointer-button bindings (config.toml, e.g. Super+BTN_LEFT): asked on
    /// every press outside a grab, a drag, a popup grab and the lock. Null: none.
    using button_binding_fn = wm_button_binding (*)(void* user, unsigned button, unsigned time_ms);
    void set_button_binding(button_binding_fn fn, void* user) {
        button_binding_ = fn;
        button_binding_user_ = user;
    }
    /// window.move-to-output-{left,right,up,down}: the window goes to the nearest output in
    /// that direction (dx, dy one of ±1, 0), keeping its place relative to the usable area
    /// as hotplug evacuation does; maximized and fullscreen windows fill the new output.
    /// False when there is none that way.
    bool move_to_output(lx::toplevel_id id, int dx, int dy);
    /// The nearest live output from `slot` in direction (dx, dy), or -1.
    [[nodiscard]] int neighbour_output(int slot, int dx, int dy) const;

    // ── Snapping, edge snapping, keyboard move/resize (design-session-and-wm Part K) ──

    /// config.toml [windows]. A drag in progress keeps the values it started with only for
    /// what it already decided (a restore, a zone shown).
    void set_window_config(const wm_window_config& c);
    [[nodiscard]] const wm_window_config& window_config() const { return window_cfg_; }
    /// window.snap-left/right/up/down: Windows 11's Super+arrows (`next_snap`), applied like
    /// maximize: one configure, the window placed by the commit answering it. False when it
    /// changes nothing (fullscreen, no output beyond the edge, an arrow that leads nowhere).
    bool snap_by_key(lx::toplevel_id id, snap_key key, const char* source);
    /// Snaps `id` to `snap` (a half or a quarter, `edge_*` bits) of output `slot`'s usable
    /// area, keeping its normal geometry (`restore`) to go back to; 0 restores that geometry.
    /// Unmaximizes it first when it was maximized. Tiled states go with the configure.
    void set_snap(lx::toplevel_id id, unsigned snap, int slot, const char* source);
    /// The edge-snap preview's layer (between the windows and the top layer-shell layer).
    void set_snap_preview_layer(scene::container_node* layer);
    /// window.move-keyboard / window.resize-keyboard: an interactive move or resize of `id`
    /// with no button held (K.6). The pointer is warped to the window's centre (move) or the
    /// frame's bottom-right corner (resize) and drives it as in a button grab; the arrow keys
    /// move the pointer by 10 logical px (Ctrl: 1), and for a resize the first arrow of an
    /// axis picks that edge; Return or a button press applies, Escape cancels.
    bool begin_keyboard_grab(lx::toplevel_id id, grab_mode mode);
    /// How a keyboard grab moves the pointer (the compositor's warp: seat, cursor, then
    /// `on_pointer_motion`). Without it there are no keyboard grabs.
    void set_warp(void (*fn)(void* user, double x, double y), void* user) {
        warp_ = fn;
        warp_user_ = user;
    }
    /// switcher.next with no shell to run it: the topmost shown window that does not have the
    /// keyboard is activated (a usable quick switch while the shell restarts). False when
    /// there is none.
    bool quick_switch();

    // ── Tiling: master-stack per workspace (ADR 012) ──

    /// config.toml [tiling]. The first call, and one that changes the layout, the ratio or
    /// the count, applies them to every workspace (T8); `gaps` applies at once.
    void set_tiling_config(const tiling_config& c);
    [[nodiscard]] const tiling_config& tiling() const { return tiling_; }
    /// layout.toggle: output `slot`'s active workspace (-1: the focused output's) between
    /// floating and master-stack. False without one.
    bool toggle_layout(int slot, const char* source);
    /// window.float-toggle: a tiled window leaves the layout, a floated one on a master-stack
    /// workspace joins it again (T3). False when it changes nothing.
    bool toggle_floating(lx::toplevel_id id);
    /// layout.focus-next / -previous (`dir` ±1): the next window in the focused workspace's
    /// layout order, wrapping; the first master when the keyboard is elsewhere.
    bool focus_tiled(int dir);
    /// layout.swap-next / -previous: `id` changes places with its neighbour in the order.
    bool swap_tiled(lx::toplevel_id id, int dir);
    /// layout.promote: `id` changes places with the first master (the first master, with the
    /// window after it).
    bool promote_tiled(lx::toplevel_id id);
    /// layout.master-grow / -shrink (`ratio_delta`) and layout.master-more / -fewer
    /// (`count_delta`) on the focused output's active workspace. False when nothing changed.
    bool adjust_master(double ratio_delta, int count_delta);
    /// The tiled windows of workspace `ws` in layout order, into `out`; returns how many.
    unsigned tiled_windows(lx::workspace_id ws, std::span<lx::toplevel_id> out) const;
    /// Lays out the master-stack workspaces again when membership, order, parameters or an
    /// area changed since the last call (T5): one configure per window whose tile changed
    /// size, a move for one whose tile only moved. From the end of every operation
    /// (`flush_workspaces`) and the tick; nothing to do costs one test.
    void flush_tiling();

    // ── Workspaces, minimize, sticky (design-session-and-wm Part W) ──

    /// config.toml [workspaces] (mode, count, names, switch-on-activate, move-follows).
    void set_workspace_config(const workspace_config& c);
    [[nodiscard]] const workspace_model& workspaces() const { return workspaces_; }
    void set_workspace_observer(workspace_observer o) { ws_observer_ = o; }
    /// Workspaces for every live output that has none (`add`: start-up, an ext-workspace
    /// bind, a map; outputs added later get theirs in `on_output_added`) and none for an
    /// output that went without `evacuate_output`.
    void sync_workspace_outputs(bool add);
    /// mapped ∧ ¬minimized ∧ (sticky ∨ its workspace is active) — or in an interactive grab.
    [[nodiscard]] bool shown(const toplevel_record& rec) const;
    /// The output keyboard actions act on: the focused window's, else the pointer's.
    [[nodiscard]] int focused_output() const;
    /// Makes `ws` its output's active workspace: windows hide and show, the keyboard goes
    /// to the window last focused there (else its topmost window, else nowhere) unless it
    /// is on another output. `source` is logged. False when nothing changed.
    bool switch_workspace(lx::workspace_id ws, const char* source);
    /// workspace.switch N on the focused output (dynamic: past the end, the empty last).
    bool switch_to_index(unsigned index_1based);
    /// workspace.next / workspace.previous (no wrap).
    bool switch_relative(int dir);
    /// Moves `id` (and its children) to workspace `ws` — of another output too, placed
    /// there as evacuation places windows; with `follow` the workspace becomes active and
    /// the window keeps the keyboard.
    bool move_to_workspace(lx::toplevel_id id, lx::workspace_id ws, bool follow,
                           const char* source);
    /// workspace.move-window N / move-window-next / -previous (follows with move-follows).
    bool move_to_index(lx::toplevel_id id, unsigned index_1based);
    bool move_relative(lx::toplevel_id id, int dir);
    /// ext-workspace assign: workspace `ws` goes to output `slot`, its windows with it.
    bool assign_workspace(lx::workspace_id ws, unsigned slot);
    /// xdg_toplevel.set_minimized, zlm request_minimize, window.minimize (`on`), and an
    /// activation (`off`). The keyboard leaves a minimized window.
    void set_minimized(lx::toplevel_id id, bool on, const char* source);
    /// window.sticky-toggle: shown on every workspace of its output.
    void set_sticky(lx::toplevel_id id, bool on);
    /// Tells the observer (ext-workspace, the control socket), then the shell (zlm
    /// toplevel_workspace), of what changed since the last flush. Every operation above
    /// ends with it; the compositor also calls it after each dispatch.
    void flush_workspaces();
    /// Several operations that clients must see as one change (an ext-workspace commit):
    /// flushes wait for the outermost `end_workspace_batch`.
    void begin_workspace_batch() { ++ws_batch_; }
    void end_workspace_batch() {
        if (ws_batch_ > 0 && --ws_batch_ == 0)
            flush_workspaces();
    }
    void set_protocol_ops(wm_protocol_ops ops) { ops_ = ops; }
    /// Ping/pong liveness (optional): activation and the configure watchdog ping the
    /// client, and the client with the keyboard is pinged periodically from `on_tick`.
    void set_liveness(liveness_tracker* liveness) { liveness_ = liveness; }
    /// The liveness tracker's verdict on `client` changed: each of its toplevels is marked
    /// (`toplevel_record::not_responding`), its frame title gets the suffix, its tree is
    /// dimmed and the shell is told (`toplevel_flags`). Returns the number of toplevels.
    unsigned set_client_not_responding(lx::client_id client, bool on);

    // ── xdg-activation: focus-stealing prevention (the wire owns the tokens) ──

    /// xdg_activation_v1.activate of toplevel `id`, presenting a token whose grant is
    /// `grant` (null: an unknown token), at `now`. Decided by `decide_activation`: granted →
    /// focus and raise; urgent → `demands_attention`, which the shell shows; before the
    /// window maps, the request is kept and decided at map (`decide_activation_at_map`:
    /// only a token the user moved on from, or a stale one, keeps a new window from taking
    /// the keyboard).
    activation_result request_activation(lx::toplevel_id id, const activation_grant* grant,
                                         lx::runtime::clock_time now, activation_reason& why);
    /// Sets or clears the window's `demands_attention` and tells the shell (on change). The
    /// window with the keyboard is never marked.
    void set_demands_attention(toplevel_record& rec, bool on);

    /// First commit after get_toplevel (or after an unmap): sends the initial configure
    /// right away rather than at the next tick, so the client is not held up a frame.
    void on_initial_commit(lx::toplevel_id id);
    /// Every later commit of a toplevel's surface, after the content reached the surface
    /// manager. Maps, unmaps, applies acked configures and keeps the node in place.
    void on_toplevel_commit(lx::toplevel_id id, const toplevel_commit_info& info);
    /// Takes a mapped toplevel off screen. `reason` is logged (`null-buffer`, `destroyed`).
    void on_unmap(lx::toplevel_id id, const char* reason);
    /// One of the toplevel's protocol objects is being destroyed; the record goes with it.
    void on_destroyed(lx::toplevel_id id);
    /// xdg_toplevel.set_title (the frame's title is redrawn at the tick).
    void on_title_changed(lx::toplevel_id id);
    /// A decoration object was created, or set_mode / unset_mode changed what the client
    /// wants. The policy's answer goes into the desired configure and is sent now when no
    /// configure is in flight; with `immediate` (a new object after the initial configure)
    /// regardless, so a version 1 client is never left between creation and its first
    /// decoration configure.
    void on_decoration_request(lx::toplevel_id id, bool immediate);

    /// Once per frame tick, before the scene is published.
    void on_tick(lx::runtime::clock_time now_ns);
    /// The layout changed (an output's mode, position or scale; an output added or gone):
    /// every output's layer surfaces are arranged again, and with them the usable areas;
    /// fullscreen windows follow their outputs.
    void on_output_changed();
    /// Output `slot` is about to go (ADR 008 §B.5): every window on it moves to `fallback`,
    /// keeping its relative position (`evacuate_rect`; shrunk with a configure when it does
    /// not fit), maximized and fullscreen ones are configured for `fallback`, and each
    /// remembers the output's `identifier` and its place there (its home). Unmapped windows
    /// placed on it are placed on `fallback`. Returns how many mapped windows moved.
    /// `fallback` -1 (no output is left, one is on its way): the windows are stranded
    /// (`toplevel_record::stranded`) until `on_output_added`, unmapped ones unplaced.
    unsigned evacuate_output(unsigned slot, int fallback, const char* identifier);
    /// Output `slot` was added: windows whose home it is (and that the user did not move or
    /// resize since they left it) go back to their place there, maximized or fullscreen
    /// again if they were; stranded windows are evacuated onto it. Returns how many went
    /// back or were taken in.
    unsigned on_output_added(unsigned slot);
    /// Output `slot`'s usable area changed (layer-shell exclusive zones, or the output).
    /// Windows in the normal state stay where they are (sway, KWin); maximized ones on that
    /// output are configured to fill the new area.
    void on_usable_area_changed(unsigned slot, lx::rect2i usable);
    /// xdg_toplevel.set_maximized / unset_maximized, zlm_policy_bridge_v1.request_maximize /
    /// request_unmaximize, a title-bar double click. Always answered by a configure (the
    /// spec: also when nothing changes). Maximized fills the usable area minus the frame;
    /// unmaximize restores the geometry it had. Position and state change with the commit
    /// that answers the configure, so the window never shows its new size at its old place.
    /// `source` is logged (`client`, `shell`, `decoration`).
    void set_maximized(lx::toplevel_id id, bool on, const char* source);
    /// xdg_toplevel.set_fullscreen / unset_fullscreen, zlm_policy_bridge_v1.request_fullscreen
    /// (M11). `output`: the slot asked for, -1 for none (the window's output). Always
    /// answered by a configure carrying `fullscreen` and the output's logical size; the
    /// commit answering it moves the window to the fullscreen layer at the output's origin,
    /// over a black backdrop, frame hidden. Unset restores the geometry and the maximized
    /// state it had.
    void set_fullscreen(lx::toplevel_id id, bool on, int output, const char* source);
    /// xdg_toplevel.set_parent, and xdg-foreign's set_parent_of across clients: the child is
    /// placed on its parent's output, centred on it at its first map, and the shell is told.
    void set_parent(lx::toplevel_id id, lx::toplevel_id parent);
    /// xdg_dialog_v1.set_modal / unset_modal (and the dialog object going), effective at once
    /// (Mutter). A modal child of a mapped parent stays above it, and the parent never takes
    /// the keyboard while it has one: focus, activation and clicks go to the dialog.
    void set_modal(lx::toplevel_id id, bool modal);
    /// The topmost mapped window that blocks `id` — a modal child, or a modal child of one
    /// (to any depth ≤ 8) — or a null id.
    [[nodiscard]] lx::toplevel_id modal_blocker(lx::toplevel_id id) const;
    /// Sends every desired configure the single-in-flight rule allows.
    void flush_configures(lx::runtime::clock_time now_ns);

    /// The window manager's toplevel focus decision (activation, the shell's focus_changed).
    /// The surface that actually receives keys follows from `resolve_keyboard_focus`.
    void set_keyboard_focus(lx::toplevel_id id);
    /// Decides which surface gets keys: an `exclusive` layer surface on top/overlay first
    /// (only its own popups can take them from it), then the topmost mapped grabbing popup
    /// (unless its layer root has interactivity `none`), then a clicked `on_demand` layer
    /// surface, then the focused toplevel. The toplevel owning the result is the activated
    /// one. Run after every change to any of them.
    void resolve_keyboard_focus();
    [[nodiscard]] lx::toplevel_id keyboard_focus() const { return keyboard_focus_; }
    [[nodiscard]] lx::toplevel_id pointer_focus() const { return pointer_focus_; }
    /// The surface with pointer focus — the toplevel's own or a subsurface of it.
    [[nodiscard]] lx::surface_id pointer_surface() const { return pointer_surface_; }

    /// The surface tree dropped `surface`'s node. If it had the pointer, focus moves now:
    /// to nothing while a button is held (no `enter` mid-press), else to what is under the
    /// pointer.
    void on_surface_unmapped(lx::surface_id surface);

    // ── Pointer input (UI affinity; once per libinput event, after the seat position and
    //    the cursor have been updated). No allocation, no logging above debug. ──

    /// Hit-tests (no button held) or follows the implicit grab (button held), keeps
    /// enter/leave paired, and sends `wl_pointer.motion` in surface-local coordinates.
    /// During a grab it only records the position: clients get nothing, and the window
    /// follows once per tick.
    void on_pointer_motion(unsigned time_ms);
    /// Press: click-to-focus and raise-on-press (placeholder policy), then the button goes
    /// to the surface under the pointer, which keeps pointer focus until every button is
    /// released (implicit grab). Release: forwarded to that surface, then focus is
    /// re-evaluated at the pointer.
    void on_pointer_button(const lx::input::pointer_button_event& event);
    void on_pointer_axis(const lx::input::pointer_axis_event& event);
    /// True when the key is the window manager's and must not reach the client: Escape
    /// during a grab or a drag cancels it, and both its press and release are withheld.
    [[nodiscard]] bool on_key(const lx::input::key_event& event);
    /// The seat's modifiers changed (after the router sent them): during a drag they choose
    /// the user's action (Shift move, Ctrl copy, Alt ask).
    void on_modifiers();

    // ── Drag-and-drop: the third pointer mode (`:dnd`). The grab follows the pointer path
    //    here; the wire owns the objects and the negotiation. ──

    void set_dnd(dnd_grab* dnd) { dnd_ = dnd; }
    [[nodiscard]] bool drag_active() const { return dnd_ && dnd_->active(); }
    /// wl_data_device.start_drag's serial rule (wlroots'
    /// `wlr_seat_validate_pointer_grab_serial`, as for move/resize): the serial of the last
    /// button press, delivered to `client` (`wl_client*`), that button still held, and no
    /// move/resize or drag already running. Null when valid, else the reason (`serial`,
    /// `no-button`, `grab-active`, `dnd-active`). A touch serial never matches: touch input
    /// is not routed.
    [[nodiscard]] const char* validate_drag_serial(void* client, unsigned serial) const;
    /// After `dnd_grab::begin`: popup grabs end, every held button is withheld from clients
    /// until release, the origin client gets `wl_pointer.leave`, and the surface under the
    /// pointer becomes the first drag target without waiting for motion.
    void begin_drag();

    // ── Interactive move/resize: one executor, three entry points ──

    /// Cursor the grab overrides (move / resize shapes). Optional.
    void set_cursor(cursor_manager* cursor) { cursor_ = cursor; }
    [[nodiscard]] bool grab_active() const { return grab_.mode != grab_mode::none; }
    /// Work `on_tick` has yet to do, or a deadline only a tick notices: a re-hit, a
    /// stacking change, grab motion, or a configure held back until the client acks (or
    /// the 200 ms watchdog fires). The frame clock keeps running while this is true.
    [[nodiscard]] bool needs_tick() const;
    [[nodiscard]] const wm_grab& grab() const { return grab_; }
    /// The executor. Requires: no grab active, a mapped toplevel in the normal state,
    /// `button` (an evdev code) still held, and for a resize valid non-zero `edges`.
    /// Takes pointer focus away from the client (it gets `leave`), sets the cursor
    /// override and, for a resize, the `resizing` state. Rejections log at debug.
    /// A move of a maximized or snapped window restores it once the pointer has travelled
    /// 8 px (drag-to-restore, [windows] drag-restore); a resize of a snapped window ends its
    /// snap. With `keyboard` no button is held (`begin_keyboard_grab`).
    bool begin_grab(lx::toplevel_id id, grab_mode mode, unsigned edges, grab_source source,
                    unsigned button, bool keyboard = false);
    /// xdg_toplevel.move / .resize. `serial` must be the press serial delivered to
    /// `client` (`wl_client*`) and that button must still be held — the rule
    /// `wlr_seat_validate_pointer_grab_serial` applies. Otherwise ignored.
    bool request_client_grab(lx::toplevel_id id, void* client, unsigned serial, grab_mode mode,
                             unsigned edges);
    /// zlm_policy_bridge_v1.request_move / request_resize. The shell is trusted to name the
    /// toplevel, but the request must still answer a live press (`serial` = the last press
    /// serial, from whichever client got it, button still held).
    bool request_shell_grab(lx::toplevel_id id, unsigned serial, grab_mode mode, unsigned edges);
    void end_grab(grab_end_reason reason);

    /// xdg_popup.grab: the serial must be the last button press or key press (or that key's
    /// release) this client (`wl_client*`) received — stricter than wlroots, as Mutter and
    /// Smithay are — and no move/resize or drag may be running.
    [[nodiscard]] popup_manager::grab_result request_popup_grab(lx::surface_id popup,
                                                                void* client, unsigned serial);

    /// Topmost surface whose extents and input region contain the global point: popups
    /// top→bottom first, then the overlay and top layer surfaces, every mapped toplevel's
    /// tree in stacking order (reverse paint order within each; the frame after the
    /// client), then the bottom and background layer surfaces; stops at the first hit.
    [[nodiscard]] bool hit_test(double x, double y, wm_hit& out) const;

    /// Pointer buttons currently held (implicit grab active when non-zero).
    [[nodiscard]] unsigned buttons_down() const { return std::popcount(button_mask_); }

    // ── Session pause (VT switch away) ──
    /// Ends what a pause must not leave half-done: an interactive move or resize ends as
    /// Escape would, a drag is cancelled (so no release can become a drop). Popups stay —
    /// they are client state. Also for a button released by its device going away.
    void cancel_for_pause();
    /// Releases, stamped `time_ms`, of every held button whose press reached a client;
    /// afterwards no button is held. For what libinput's own releases did not cover.
    void release_held_buttons(unsigned time_ms);
    /// Why windows are `suspended` (xdg-shell: "outputs switched off due to screen
    /// locking" is one of the spec's own cases). Any reason held suspends them.
    enum suspend_reason : unsigned {
        /// The VT is away (session pause).
        suspend_vt = 1u << 0,
        /// The display is powered off (zwlr_output_power_v1, idle blanking).
        suspend_power = 1u << 1,
        /// The session is locked (ext_session_lock_v1).
        suspend_lock = 1u << 2,
    };
    // ── Session lock (ext-session-lock-v1) ──
    void set_lock_ops(wm_lock_ops ops) { lock_ops_ = ops; }
    // ── Input-method popups ──
    void set_ime_ops(wm_ime_ops ops) { ime_ops_ = ops; }
    /// The session is locked (from the lock request on, design L1): normal clients lose the
    /// pointer and the keyboard at once — a move or resize ends as Escape would, a drag is
    /// cancelled, popups are dismissed, held buttons' releases reach nobody — and from then
    /// on only lock surfaces are hit-tested and given the keyboard (`wm_lock_ops`). Unlocked,
    /// focus resolves as before; the window decision taken meanwhile is kept.
    void set_session_locked(bool locked);
    [[nodiscard]] bool session_locked() const { return locked_; }
    /// The lock surfaces changed (one mapped, went, or took the keyboard): keyboard focus is
    /// resolved again and the pointer re-hit-tested.
    void lock_focus_changed();
    /// Holds or releases one reason. While any is held, every toplevel's desired configure
    /// carries `suspended` (windows created meanwhile too); the configures go out through
    /// the normal flush.
    void set_suspend_reason(unsigned reason, bool on);
    [[nodiscard]] bool session_suspended() const { return suspend_reasons_ != 0; }
    /// A button press that woke the display (I5): consumed — it reaches no client and starts
    /// nothing — and its release is withheld like that of any swallowed press.
    void swallow_button_press(const lx::input::pointer_button_event& event);
    /// Serial of the last button press delivered to a client, and that client
    /// (`wl_client*`). What xdg_toplevel.move/resize are validated against.
    [[nodiscard]] unsigned last_press_serial() const { return last_press_serial_; }
    [[nodiscard]] void* last_press_client() const { return last_press_client_; }
    [[nodiscard]] unsigned last_press_button() const { return last_press_button_; }

    // ── Stacking and activation (placeholder policy defaults; the shell's requests over
    //    zlm_policy_bridge_v1 land here too) ──

    /// Keyboard focus + raise.
    void activate(lx::toplevel_id id);
    bool raise(lx::toplevel_id id);
    bool lower(lx::toplevel_id id);
    /// 0 = bottom; clamped to the top.
    bool restack(lx::toplevel_id id, unsigned index);
    void request_close(lx::toplevel_id id);
    /// A mapped window in the normal state whose frame reaches outside its output's usable
    /// area is moved (the least distance) to lie inside it, or to its top-left corner when
    /// it is larger. For windows that mapped before the session's shell had reserved its
    /// panel strip (the compositor's placement hold, design-session-and-wm S.2). True when
    /// it moved.
    bool fit_into_usable(lx::toplevel_id id);

    /// Output `slot`'s logical rect: what its layer surfaces are arranged against, what a
    /// fullscreen window there fills, and what layer-surface popups are constrained to. A
    /// dead or negative slot means the first output; a nominal 1920x1080 without any.
    [[nodiscard]] lx::rect2i output_area(int slot) const;
    /// Output `slot` minus its layer-shell exclusive zones: where windows are placed, how far
    /// a move may push them, what configure_bounds and maximize use, and what window popups
    /// are constrained to.
    [[nodiscard]] lx::rect2i usable_area(int slot) const;
    /// The output a window is on (§A.9): mapped, the one its frame overlaps most (else the
    /// one it was on); not yet mapped, its placement output.
    [[nodiscard]] int window_output(const toplevel_record& rec) const;
    /// Moves a mapped window off an output whose usable area was `from` onto output
    /// `fallback` (`evacuate_output`, and a stranded window taken in by a new output).
    void relocate_window(toplevel_record& rec, lx::rect2i from, int fallback);
    /// The output under the pointer (the nearest one in a gap), -1 without outputs.
    [[nodiscard]] int pointer_output() const;
    /// The fullscreen layer (between the top layer-shell layer and the overlay one) and the
    /// staging of its backdrop texture. Optional: without it fullscreen windows stay in the
    /// window layer.
    void set_fullscreen_layer(scene::container_node* layer) {
        fullscreen_layer_ = layer;
        // The backdrops first: every fullscreen window's tree goes above them.
        if (layer)
            for (auto& b : backdrops_)
                (void)layer->add(&b);
    }
    void set_texture_ops(compositor_texture_ops ops) { tex_ops_ = ops; }
    /// The edge-snap preview's rect while it is shown (none: empty).
    [[nodiscard]] lx::rect2i snap_preview_rect() const;
    /// The frame the compositor draws around `rec` in its current state (none: client-side).
    [[nodiscard]] static frame_extents extents_of(const toplevel_record& rec);

private:
    void send_configure(toplevel_record& rec, lx::runtime::clock_time now_ns, bool initial,
                        bool watchdog);
    /// `placed`: the commit answered a configure that placed the window (maximized before
    /// it ever mapped); placement is skipped.
    void map(toplevel_record& rec, bool placed);
    /// What `decide_activation` needs about `rec` (null: no toplevel) and the focus now.
    [[nodiscard]] activation_context activation_context_for(const toplevel_record* rec,
                                                            const activation_grant* grant,
                                                            lx::runtime::clock_time now) const;
    /// The pending activation of a window being mapped (`decide_activation_at_map` in
    /// :activation): true when it may take the keyboard; a refused one is marked.
    [[nodiscard]] bool decide_activation_at_map(toplevel_record& rec);
    /// Moves pointer focus to `hit` (or to nothing when null), sending leave/enter.
    void set_pointer_focus(const wm_hit* hit);
    /// Re-runs the hit test at the current pointer position and moves focus if the window
    /// under it changed — "rebase", for when the scene changes under a still pointer.
    /// With `motion_if_same`, a focus that stays but whose local position moved (its
    /// window moved under the pointer) gets a `motion`.
    void refresh_pointer_focus(unsigned time_ms, bool motion_if_same);
    /// Restacks the window's tree container to match the toplevel order and damages it.
    void sync_node_stacking(const toplevel_record& rec, const char* verb);
    void emit_stacking_order();
    void log_geometry(const toplevel_record& rec, const char* cause, unsigned serial) const;
    /// Applies the coalesced grab motion: a move is an origin change (no texture work), a
    /// resize updates the desired configure size, which the single-in-flight rule throttles.
    void apply_grab();
    /// Moves a mapped window to global geometry origin `p`, damaging old and new rects
    /// separately and telling the shell.
    void move_window(toplevel_record& rec, lx::point2i p, const char* cause);
    void log_grab_reject(lx::toplevel_id id, grab_source source, const char* reason) const;
    /// `hit_test` filtered by an active popup grab: surfaces of other clients do not get
    /// the pointer (owner events).
    [[nodiscard]] bool pointer_hit(double x, double y, wm_hit& out) const;
    /// The drag's target at the pointer (a client surface; a frame is not one). Without
    /// `send_motion` only a change of target is reported (the scene changed, not the pointer).
    void drag_hit(unsigned time_ms, bool send_motion);
    /// The pointer is already where `h` says (same window, surface or frame zone).
    [[nodiscard]] bool same_pointer_focus(const wm_hit& h) const;
    /// A press on window `id`'s frame. Never reaches a client.
    void decoration_press(lx::toplevel_id id, deco_zone zone,
                          const lx::input::pointer_button_event& event);
    /// The frame hover (close-button look, pointer shape) follows pointer focus.
    void set_decoration_hover(lx::toplevel_id id, deco_zone zone);
    /// Keeps the frame of `rec` in line with it (after any geometry, mapping or mode change),
    /// its title rasterized at the largest scale among its outputs.
    void sync_decoration(const toplevel_record& rec);
    /// The window entered or left the fullscreen state (its committed state): its tree moves
    /// between the window layer and the fullscreen layer, and its output's backdrop follows.
    void sync_fullscreen(toplevel_record& rec, bool was_fullscreen);
    /// The backdrops follow the fullscreen windows (after any of them changed).
    void sync_backdrops();
    /// The placement output of a window about to get its initial configure.
    [[nodiscard]] int placement_output(const toplevel_record& rec) const;
    /// Whether `rec` is modal over `ancestor` through a chain of effectively modal links.
    [[nodiscard]] bool blocks(const toplevel_record& rec, lx::toplevel_id ancestor) const;
    /// After `id` was restacked: the windows modal over it go right above it, in their order.
    void keep_modals_above(lx::toplevel_id id);
    /// The modal state the shell sees of `id`'s children changed (its mapping, or theirs).
    void emit_children_state(lx::toplevel_id id);
    // ── workspaces ──
    /// `rec` joins workspace `ws` (leaving its previous one): counts, the shell's dirty bit.
    void join_workspace(toplevel_record& rec, lx::workspace_id ws);
    void leave_workspace(toplevel_record& rec);
    /// Every mapped window's visibility follows `shown` (O(windows)); a hidden window with
    /// the keyboard gives it to its output's workspace focus target.
    void update_visibility();
    void apply_hidden(toplevel_record& rec, bool hide);
    /// The window the keyboard goes to on output `slot`'s active workspace: the one last
    /// focused there if still shown, else the topmost shown window there; null for none.
    [[nodiscard]] lx::toplevel_id workspace_focus_target(int slot) const;
    /// After `slot`'s active workspace changed: the keyboard follows unless it is on a shown
    /// window of another output.
    void focus_after_switch(int slot);
    /// Brings a hidden window into view for an activation: unminimized, its workspace
    /// active (with `switch_ok`). False when it stays hidden.
    bool reveal(toplevel_record& rec, bool switch_ok, const char* source);
    void recompute_urgent();
    /// The configures of windows whose visibility changed (`suspended`), with the focus
    /// decided since: sent at the end of the operation.
    void send_visibility_configures();
    [[nodiscard]] workspace_sink sink();
    /// The topmost shown window (on `slot`, or anywhere for -1).
    [[nodiscard]] lx::toplevel_id top_shown(int slot) const;
    // ── snapping ──
    /// Puts `rec`'s desired configure at `snap` of `slot`'s usable area (tiled states, the
    /// place, the outer edges anchored); no restore bookkeeping, no send.
    void aim_snap(toplevel_record& rec, unsigned snap, int slot);
    /// Maximizes `rec` (desired) on `slot`; no restore bookkeeping, no send.
    void aim_maximized(toplevel_record& rec, int slot);
    /// Sends the configure `rec` now wants when none is in flight (else the flush does).
    void configure_now(toplevel_record& rec);
    /// Drag-to-restore: the move has travelled far enough — the window is asked for its
    /// normal size, placed so the pointer keeps its spot on the title bar.
    void drag_restore(toplevel_record& rec);
    /// The edge zone under the pointer during a move, and the preview of it.
    void update_edge_zone();
    void show_snap_preview(unsigned zone, int slot);
    void hide_snap_preview();
    /// `edge_*` bits of `slot`'s edges at (x, y) that no other output continues.
    [[nodiscard]] unsigned open_edges(int slot, double x, double y) const;
    /// A keyboard grab's arrow key: the pointer moves `dx`, `dy` (logical px).
    void keyboard_grab_step(int dx, int dy);
    // ── tiling ──
    /// `rec` belongs in workspace `ws`'s layout (T3), mapped or not.
    [[nodiscard]] bool tiles_on(const toplevel_record& rec, lx::workspace_id ws) const;
    /// Puts `rec`'s desired configure at frame rect `frame` (tiled states, the place);
    /// moves it without a configure when only the place changed. With `send`, the configure
    /// goes out now when none is in flight (once the initial one went).
    void aim_tile(toplevel_record& rec, lx::rect2i frame, bool send);
    /// `rec` joins the layout: its floating geometry kept, a snap cleared.
    void join_tiling(toplevel_record& rec);
    /// `rec` leaves the layout: tiled states cleared, its floating geometry back.
    void leave_tiling(toplevel_record& rec);
    /// The workspace the layout actions act on: the focused output's active one.
    [[nodiscard]] lx::workspace_id focused_workspace() const;

    toplevel_manager* toplevels_ = nullptr;
    surface_manager* surfaces_ = nullptr;
    surface_tree* tree_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    scene::container_node* windows_layer_ = nullptr;
    input_router* router_ = nullptr;
    lx::input::seat* seat_ = nullptr;
    shell_bridge* shell_ = nullptr;
    output_manager* outputs_ = nullptr;
    cursor_manager* cursor_ = nullptr;
    popup_manager* popups_ = nullptr;
    decoration_manager* decorations_ = nullptr;
    layer_shell* layers_ = nullptr;
    dnd_grab* dnd_ = nullptr;
    liveness_tracker* liveness_ = nullptr;
    wm_protocol_ops ops_{};
    wm_lock_ops lock_ops_{};
    wm_ime_ops ime_ops_{};
    /// The session is locked: input goes to lock surfaces only.
    bool locked_ = false;
    wm_focus_policy focus_{};
    button_binding_fn button_binding_ = nullptr;
    void* button_binding_user_ = nullptr;
    wm_grab grab_{};
    /// Buttons whose press never reached a client (the Super binding, presses during a
    /// grab): their release must not reach one either.
    std::uint64_t swallowed_mask_ = 0;
    /// Escape pressed to cancel a grab: its release is withheld too.
    bool swallowed_escape_ = false;
    /// `suspend_reason` bits held: while any is, windows carry `tl_suspended`.
    unsigned suspend_reasons_ = 0;
    lx::toplevel_id keyboard_focus_{};
    /// The window owning the pointer focus (click-to-focus target) and the surface that
    /// actually has it, which may be one of the window's subsurfaces.
    lx::toplevel_id pointer_focus_{};
    lx::surface_id pointer_surface_{};
    /// What the pointer focus belongs to, and the root of its tree.
    hit_owner pointer_owner_ = hit_owner::toplevel;
    lx::surface_id pointer_root_{};
    /// An `on_demand` layer surface given the keyboard by a click (taken back by a click on
    /// a window or on nothing).
    lx::surface_id layer_focus_{};
    /// The toplevel whose desired configure carries `activated`: the owner of the surface
    /// resolve_keyboard_focus gave the keyboard to (none while a layer surface has it).
    lx::toplevel_id activated_{};
    /// Last usable area logged per output (`usable area …` on change only).
    lx::rect2i usable_logged_[output_manager::k_max]{};
    scene::container_node* fullscreen_layer_ = nullptr;
    compositor_texture_ops tex_ops_{};
    bool backdrop_staged_ = false;
    fullscreen_backdrop backdrops_[output_manager::k_max]{};
    /// The frame zone with pointer focus (none: the focus, if any, is a client surface).
    deco_zone pointer_zone_ = deco_zone::none;
    /// A close-button press waiting for its release (the click happens on release over it).
    struct deco_press {
        lx::toplevel_id id{};
        unsigned button = 0;
    } deco_press_{};
    /// The last left press on a title bar, for double-click detection.
    struct title_click {
        lx::toplevel_id id{};
        unsigned time_ms = 0;
        double x = 0.0;
        double y = 0.0;
    } last_title_click_{};
    /// The window whose frame shows the active colours (the effective keyboard owner).
    lx::toplevel_id deco_active_{};

    /// Held pointer buttons, bit (code - k_button_base). Codes outside the window are
    /// forwarded but do not hold the implicit grab.
    static constexpr unsigned k_button_base = 0x110; // BTN_LEFT
    std::uint64_t button_mask_ = 0;
    unsigned last_press_serial_ = 0;
    void* last_press_client_ = nullptr;
    unsigned last_press_button_ = 0;
    /// The scene changed under the pointer (map, unmap, restack, a window moved or
    /// resized): re-hit-test once at the next tick rather than per change.
    bool rehit_pending_ = false;
    /// Stacking changed this tick: one stacking_order_changed batch goes to the shell at
    /// the tick (the protocol's "done=1 after the last index in a frame").
    bool stacking_dirty_ = false;
    workspace_model workspaces_{};
    workspace_observer ws_observer_{};
    /// A window's membership changed since the last flush (its `workspace_dirty` is set).
    bool ws_dirty_ = false;
    unsigned ws_generation_seen_ = 0;
    /// Bumped by every change of a window's shown/sticky/minimized state (the observer runs).
    bool ws_windows_changed_ = false;
    unsigned ws_batch_ = 0;
    /// A window's visibility changed: its configure goes out at the end of the operation.
    bool ws_configures_due_ = false;
    // ── snapping (Part K) ──
    wm_window_config window_cfg_{};
    scene::container_node* snap_preview_layer_ = nullptr;
    snap_preview preview_{};
    bool preview_staged_ = false;
    void (*warp_)(void* user, double x, double y) = nullptr;
    void* warp_user_ = nullptr;
    // ── tiling (ADR 012) ──
    tiling_config tiling_{};
    bool tiling_set_ = false;
    /// Membership, order, parameters or an area changed: `flush_tiling` has work.
    bool tiling_dirty_ = false;
    /// The last `tile_order` handed out (a window joining goes after every other).
    long long tile_seq_ = 0;
};

// ── Pure helpers (exported for tests) ──────────────────────────────────────

/// The window geometry actually in effect (xdg-shell): the requested rect clamped to the
/// bounding box of the surface and its subsurfaces, or that bounding box when the client
/// never set one. `tree_bbox` is surface-local and may extend left of or above the surface
/// (a CSD title bar is a subsurface at y < 0), so the result may have a negative origin.
[[nodiscard]] lx::rect2i effective_window_geometry(bool set, lx::rect2i requested,
                                                   lx::rect2i tree_bbox);

/// Global window geometry after a commit of size `committed` that answers `applied`.
/// For each anchor edge the *opposite* edge stays where it was in `applied.anchor` — so a
/// client that rounds the size (to character cells, say) still keeps that edge exactly
/// fixed, because the committed size is used, not the configured one.
[[nodiscard]] lx::rect2i anchored_geometry(lx::rect2i current, const toplevel_configure& applied,
                                           lx::size2i committed);

/// Centred on `output`, cascading by 32 px while another window's top-left sits within
/// 16 px of the candidate; wraps back to the output's corner when the cascade runs off it.
[[nodiscard]] lx::point2i place_new_window(lx::rect2i output, lx::size2i size,
                                           const lx::rect2i* existing, unsigned count);

/// Keeps a window reachable: the top edge stays on the output (a CSD title bar must remain
/// grabbable, as in Mutter), and at least 64x32 px of it stays inside horizontally and
/// vertically.
[[nodiscard]] lx::point2i constrain_on_output(lx::rect2i output, lx::rect2i geometry);
/// Where frame `f`, in an area `from` that goes away, lands in `to` (ADR 008 §B.5): its
/// relative position kept — x' = to.x + (f.x − from.x)·(to.w − w)/(from.w − f.w), likewise
/// for y — its size kept when it fits, else shrunk to `to`, and the result clamped inside
/// `to`. Pure; exported for tests.
[[nodiscard]] lx::rect2i evacuate_rect(lx::rect2i f, lx::rect2i from, lx::rect2i to);

/// One of the nine `resize_edge` values (none, four sides, four corners).
[[nodiscard]] bool valid_resize_edges(unsigned edges);

/// Window-geometry size for a resize grab that started at `start` with the pointer moved by
/// (dx, dy). Only the axes named by `edges` change; each changed axis is clamped to
/// [max(min, 1), max] (a 0 max is unbounded). Left/top edges grow as the pointer moves
/// left/up.
[[nodiscard]] lx::size2i resize_target(lx::rect2i start, unsigned edges, double dx, double dy,
                                       lx::size2i min, lx::size2i max);

/// Edges a keybinding resize takes from where the window was grabbed: per axis, the outer
/// thirds pick that edge and the middle third none; the very centre picks the corner of
/// the quadrant (KWin's Meta+right-drag behaviour).
[[nodiscard]] unsigned edges_for_point(lx::rect2i geometry, double px, double py);

/// Moves every anchor the record still has to honour by (dx, dy): a window moved while a
/// resize configure is outstanding must not snap back to where the resize began when that
/// configure is finally answered.
void shift_anchors(toplevel_record& rec, int dx, int dy);

// ── Snapping (design-session-and-wm K.3–K.5), pure ──

/// Windows 11's Super+arrow table, made total: from a window snapped to `snap` (0: none) or
/// `maximized`, what `key` does. Left half + Left is the right half of the output to the
/// left (`output_dx` -1), a top-left quarter + Down the left half, and so on (K.3).
[[nodiscard]] snap_step next_snap(unsigned snap, bool maximized, snap_key key);
/// The frame rect of `snap` (a half or a quarter, `edge_*` bits) in `usable`: halves split
/// the width with the odd pixel to the left one, quarters likewise the height (to the top).
[[nodiscard]] lx::rect2i snap_frame(lx::rect2i usable, unsigned snap);
/// The window geometry of a window snapped to `snap`: `snap_frame` minus the frame `ext`. A
/// window whose `min` (0: none) exceeds that keeps its minimum and is anchored at the snap's
/// outer corner (KWin); one whose `max` is smaller, likewise.
[[nodiscard]] lx::rect2i snap_geometry(lx::rect2i usable, unsigned snap, frame_extents ext,
                                       lx::size2i min, lx::size2i max);
/// The xdg tiled states of a window snapped to `snap`: each edge lies against the usable
/// area's edge or the other half / quarter, so all four for any snap; 0 for none.
[[nodiscard]] unsigned snap_tiled_states(unsigned snap);
/// The edge zone (K.4) at the pointer (px, py) on output `output` (logical rect): within
/// `distance` of an edge in `open_edges` (no output continues it there) — the left or right
/// edge is that half, or the quarter within `corner` of its ends; the top edge maximizes
/// (`k_snap_maximize`), or near its ends the top quarters; the bottom edge's ends the bottom
/// quarters. 0 elsewhere.
[[nodiscard]] unsigned edge_snap_zone(lx::rect2i output, double px, double py,
                                      unsigned open_edges, int distance, int corner);
/// Drag-to-restore (K.5): the origin of a frame of size `restored` replacing `frame` under
/// the pointer (px, py) — the pointer keeps its fraction of the width and its distance from
/// the top (the title bar it holds), at most the new height.
[[nodiscard]] lx::point2i drag_restore_origin(lx::rect2i frame, lx::size2i restored, double px,
                                              double py);

} // namespace lx::compositor


namespace {

constexpr lx::trace::category log_wm{"compositor.wm"};

[[nodiscard]] lx::runtime::clock_time wm_now_ns() {
    return static_cast<lx::runtime::clock_time>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

[[nodiscard]] bool rect_equal(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] bool rect_empty(lx::rect2i r) { return r.width <= 0 || r.height <= 0; }
[[nodiscard]] bool rect_contains(lx::rect2i r, double x, double y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

/// Comma list of the state bits, for the configure log line.
void format_states(unsigned states, char* out, unsigned cap) {
    unsigned n = 0;
    const auto put = [&](const char* name) {
        const int w = std::snprintf(out + n, cap - n, "%s%s", n ? "," : "", name);
        if (w > 0 && n + static_cast<unsigned>(w) < cap)
            n += static_cast<unsigned>(w);
    };
    out[0] = '\0';
    if (states & lx::compositor::tl_activated)
        put("activated");
    if (states & lx::compositor::tl_resizing)
        put("resizing");
    if (states & lx::compositor::tl_maximized)
        put("maximized");
    if (states & lx::compositor::tl_fullscreen)
        put("fullscreen");
    if (states & lx::compositor::tl_suspended)
        put("suspended");
    if ((states & lx::compositor::tl_tiled_all) == lx::compositor::tl_tiled_all)
        put("tiled");
    if (n == 0)
        std::snprintf(out, cap, "-");
}

constexpr unsigned k_btn_left = 0x110;
constexpr unsigned k_btn_right = 0x111;
constexpr unsigned k_key_esc = 1;

[[nodiscard]] const char* grab_mode_name(lx::compositor::grab_mode m) {
    return m == lx::compositor::grab_mode::move     ? "move"
           : m == lx::compositor::grab_mode::resize ? "resize"
                                                    : "none";
}

[[nodiscard]] const char* grab_source_name(lx::compositor::grab_source s) {
    switch (s) {
    case lx::compositor::grab_source::client:
        return "client";
    case lx::compositor::grab_source::shell:
        return "shell";
    case lx::compositor::grab_source::keybinding:
        return "keybinding";
    case lx::compositor::grab_source::decoration:
        return "decoration";
    }
    return "-";
}

[[nodiscard]] const char* grab_end_name(lx::compositor::grab_end_reason r) {
    switch (r) {
    case lx::compositor::grab_end_reason::release:
        return "release";
    case lx::compositor::grab_end_reason::escape:
        return "escape";
    case lx::compositor::grab_end_reason::destroyed:
        return "destroyed";
    case lx::compositor::grab_end_reason::unmapped:
        return "unmapped";
    }
    return "-";
}

/// Rounds a pointer delta to whole pixels, saturating instead of overflowing.
[[nodiscard]] int round_delta(double d) {
    if (!(d == d))
        return 0;
    if (d > 1e9)
        return 1'000'000'000;
    if (d < -1e9)
        return -1'000'000'000;
    return static_cast<int>(std::lround(d));
}

/// The point is on the close button of a mapped, server-side decorated window.
[[nodiscard]] bool in_close(const lx::compositor::toplevel_record& rec, double x, double y) {
    return rec.mapped && rec.decoration_current == lx::compositor::decoration_mode::server &&
           lx::compositor::deco_hit(rec.geometry,
                                    lx::compositor::window_manager::extents_of(rec), x, y) ==
               lx::compositor::deco_zone::close;
}

/// Double click on a title bar: within 400 ms and 4 px of the previous press.
constexpr unsigned k_double_click_ms = 400;
constexpr double k_double_click_px = 4.0;

} // namespace

// ── Pure helpers ───────────────────────────────────────────────────────────

bool lx::compositor::valid_resize_edges(unsigned edges) {
    switch (edges) {
    case edge_none:
    case edge_top:
    case edge_bottom:
    case edge_left:
    case edge_right:
    case edge_top | edge_left:
    case edge_top | edge_right:
    case edge_bottom | edge_left:
    case edge_bottom | edge_right:
        return true;
    default:
        return false;
    }
}

lx::size2i lx::compositor::resize_target(lx::rect2i start, unsigned edges, double dx, double dy,
                                         lx::size2i min, lx::size2i max) {
    const auto axis = [](int size, bool grow_pos, bool grow_neg, int delta, int lo_limit,
                         int hi_limit) {
        if (!grow_pos && !grow_neg)
            return size;
        long long v = static_cast<long long>(size) + (grow_pos ? delta : -static_cast<long long>(delta));
        const long long lo = lo_limit > 1 ? lo_limit : 1;
        const long long hi = hi_limit > 0 ? hi_limit : 1'000'000'000;
        if (v > hi)
            v = hi;
        if (v < lo)
            v = lo; // min wins over a contradictory max, as the client's own clamp would
        return static_cast<int>(v);
    };
    return {axis(start.width, (edges & edge_right) != 0, (edges & edge_left) != 0,
                 round_delta(dx), min.width, max.width),
            axis(start.height, (edges & edge_bottom) != 0, (edges & edge_top) != 0,
                 round_delta(dy), min.height, max.height)};
}

unsigned lx::compositor::edges_for_point(lx::rect2i geometry, double px, double py) {
    const double lx_ = px - geometry.x;
    const double ly = py - geometry.y;
    unsigned edges = 0;
    if (lx_ < geometry.width / 3.0)
        edges |= edge_left;
    else if (lx_ >= geometry.width * 2.0 / 3.0)
        edges |= edge_right;
    if (ly < geometry.height / 3.0)
        edges |= edge_top;
    else if (ly >= geometry.height * 2.0 / 3.0)
        edges |= edge_bottom;
    if (edges == 0) {
        // Dead centre: the corner of the quadrant the pointer is in.
        edges = (lx_ < geometry.width / 2.0 ? edge_left : edge_right) |
                (ly < geometry.height / 2.0 ? edge_top : edge_bottom);
    }
    return edges;
}

void lx::compositor::shift_anchors(toplevel_record& rec, int dx, int dy) {
    const auto shift = [&](toplevel_configure& c) {
        c.anchor.x += dx;
        c.anchor.y += dy;
    };
    shift(rec.desired);
    shift(rec.last_sent);
    shift(rec.acked);
    for (unsigned i = 0; i < rec.sent_count; ++i)
        shift(rec.sent[i]);
}

lx::compositor::snap_step lx::compositor::next_snap(unsigned snap, bool maximized,
                                                      snap_key key) {
    using enum snap_key;
    const auto to = [](unsigned s, int dx = 0) { return snap_step{snap_move::snap, s, dx}; };
    constexpr snap_step none{};
    constexpr snap_step restore{snap_move::restore, 0, 0};
    const unsigned l = edge_left, r = edge_right, t = edge_top, b = edge_bottom;
    if (maximized) {
        switch (key) {
        case left: return to(l);
        case right: return to(r);
        case up: return none;
        case down: return restore;
        }
        return none;
    }
    switch (snap) {
    case 0:
        switch (key) {
        case left: return to(l);
        case right: return to(r);
        case up: return {snap_move::maximize, 0, 0};
        case down: return {snap_move::minimize, 0, 0};
        }
        break;
    case edge_left:
        switch (key) {
        case left: return to(r, -1);
        case right: return restore;
        case up: return to(t | l);
        case down: return to(b | l);
        }
        break;
    case edge_right:
        switch (key) {
        case left: return restore;
        case right: return to(l, 1);
        case up: return to(t | r);
        case down: return to(b | r);
        }
        break;
    case edge_top | edge_left:
        switch (key) {
        case left: return to(t | r, -1);
        case right: return to(t | r);
        case up: return none;
        case down: return to(l);
        }
        break;
    case edge_bottom | edge_left:
        switch (key) {
        case left: return to(b | r, -1);
        case right: return to(b | r);
        case up: return to(l);
        case down: return none;
        }
        break;
    case edge_top | edge_right:
        switch (key) {
        case left: return to(t | l);
        case right: return to(t | l, 1);
        case up: return none;
        case down: return to(r);
        }
        break;
    case edge_bottom | edge_right:
        switch (key) {
        case left: return to(b | l);
        case right: return to(b | l, 1);
        case up: return to(r);
        case down: return none;
        }
        break;
    default: break;
    }
    return none;
}

lx::rect2i lx::compositor::snap_frame(lx::rect2i usable, unsigned snap) {
    lx::rect2i f = usable;
    // The odd pixel goes to the left (top) part, so the two halves always tile exactly.
    const int left_w = (usable.width + 1) / 2;
    const int top_h = (usable.height + 1) / 2;
    if (snap & edge_left) {
        f.width = left_w;
    } else if (snap & edge_right) {
        f.x = usable.x + left_w;
        f.width = usable.width - left_w;
    }
    if (snap & edge_top) {
        f.height = top_h;
    } else if (snap & edge_bottom) {
        f.y = usable.y + top_h;
        f.height = usable.height - top_h;
    }
    return f;
}

lx::rect2i lx::compositor::snap_geometry(lx::rect2i usable, unsigned snap, frame_extents ext,
                                         lx::size2i min, lx::size2i max) {
    const lx::rect2i f = snap_frame(usable, snap);
    lx::rect2i g{f.x + ext.left, f.y + ext.top, f.width - ext.left - ext.right,
                 f.height - ext.top - ext.bottom};
    if (g.width < 1)
        g.width = 1;
    if (g.height < 1)
        g.height = 1;
    // Limits the client set win: the window keeps the outer corner of its place (a right
    // half too narrow for its minimum still touches the right edge).
    const auto clamp_axis = [](int& pos, int& len, int lo, int hi, bool far_edge) {
        const int want = len;
        if (lo > 0 && len < lo)
            len = lo;
        if (hi > 0 && len > hi && hi >= lo)
            len = hi;
        if (far_edge)
            pos += want - len;
    };
    clamp_axis(g.x, g.width, min.width, max.width, (snap & edge_right) != 0);
    clamp_axis(g.y, g.height, min.height, max.height, (snap & edge_bottom) != 0);
    return g;
}

unsigned lx::compositor::snap_tiled_states(unsigned snap) {
    // Every edge of a half or a quarter lies against the usable area's edge or the
    // neighbouring part: GTK then draws no shadow or rounded corner anywhere (the tiled
    // look), foot fills the cell grid. Mutter tiles halves the same way.
    return (snap & (edge_left | edge_right | edge_top | edge_bottom)) ? tl_tiled_all : 0u;
}

unsigned lx::compositor::edge_snap_zone(lx::rect2i o, double px, double py, unsigned open,
                                        int distance, int corner) {
    if (o.width <= 0 || o.height <= 0 || distance <= 0)
        return 0;
    const double x0 = o.x, y0 = o.y;
    const double x1 = static_cast<double>(o.x) + o.width;
    const double y1 = static_cast<double>(o.y) + o.height;
    if (px < x0 || py < y0 || px >= x1 || py >= y1)
        return 0;
    const bool near_top = py < y0 + corner;
    const bool near_bottom = py >= y1 - corner;
    if ((open & edge_left) && px < x0 + distance)
        return near_top ? edge_top | edge_left : near_bottom ? edge_bottom | edge_left : edge_left;
    if ((open & edge_right) && px >= x1 - distance)
        return near_top ? edge_top | edge_right
                        : near_bottom ? edge_bottom | edge_right : edge_right;
    const bool near_left = px < x0 + corner;
    const bool near_right = px >= x1 - corner;
    if ((open & edge_top) && py < y0 + distance)
        return near_left ? edge_top | edge_left
                         : near_right ? edge_top | edge_right : k_snap_maximize;
    if ((open & edge_bottom) && py >= y1 - distance)
        return near_left ? edge_bottom | edge_left : near_right ? edge_bottom | edge_right : 0u;
    return 0;
}

lx::point2i lx::compositor::drag_restore_origin(lx::rect2i frame, lx::size2i restored, double px,
                                                double py) {
    double frac = frame.width > 0 ? (px - frame.x) / frame.width : 0.5;
    frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
    double dy = py - frame.y;
    const double max_dy = restored.height > 1 ? restored.height - 1.0 : 0.0;
    dy = dy < 0.0 ? 0.0 : (dy > max_dy ? max_dy : dy);
    return {static_cast<int>(std::lround(px - frac * restored.width)),
            static_cast<int>(std::lround(py - dy))};
}

lx::rect2i lx::compositor::effective_window_geometry(bool set, lx::rect2i requested,
                                                     lx::rect2i tree_bbox) {
    const lx::rect2i whole = rect_empty(tree_bbox) ? lx::rect2i{} : tree_bbox;
    if (!set || rect_empty(requested) || rect_empty(whole))
        return whole;
    const long long bx1 = static_cast<long long>(whole.x) + whole.width;
    const long long by1 = static_cast<long long>(whole.y) + whole.height;
    const long long x0 = requested.x > whole.x ? requested.x : whole.x;
    const long long y0 = requested.y > whole.y ? requested.y : whole.y;
    long long x1 = static_cast<long long>(requested.x) + requested.width;
    long long y1 = static_cast<long long>(requested.y) + requested.height;
    if (x1 > bx1)
        x1 = bx1;
    if (y1 > by1)
        y1 = by1;
    // A geometry entirely outside the surface is meaningless; fall back to the surface.
    if (x1 <= x0 || y1 <= y0)
        return whole;
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

lx::rect2i lx::compositor::anchored_geometry(lx::rect2i current, const toplevel_configure& applied,
                                             lx::size2i committed) {
    lx::rect2i out{current.x, current.y, committed.width, committed.height};
    if (applied.anchor_edges & edge_left)
        out.x = applied.anchor.x + applied.anchor.width - committed.width;
    if (applied.anchor_edges & edge_top)
        out.y = applied.anchor.y + applied.anchor.height - committed.height;
    return out;
}

lx::point2i lx::compositor::place_new_window(lx::rect2i output, lx::size2i size,
                                             const lx::rect2i* existing, unsigned count) {
    constexpr int k_cascade = 32;
    constexpr int k_near = 16;
    const auto centred = [&](int extent, int origin, int length) {
        const int free = extent - length;
        return origin + (free > 0 ? free / 2 : 0);
    };
    lx::point2i p{centred(output.width, output.x, size.width),
                  centred(output.height, output.y, size.height)};

    // Bounded: every step either moves past a window or wraps, and there are `count` of
    // them, so `count + 1` steps always find a free spot or give up on a crowded output.
    for (unsigned step = 0; step <= count; ++step) {
        bool crowded = false;
        for (unsigned i = 0; i < count && existing; ++i) {
            const int dx = existing[i].x - p.x;
            const int dy = existing[i].y - p.y;
            if (dx <= k_near && dx >= -k_near && dy <= k_near && dy >= -k_near) {
                crowded = true;
                break;
            }
        }
        if (!crowded)
            break;
        p.x += k_cascade;
        p.y += k_cascade;
        if (p.x + size.width > output.x + output.width ||
            p.y + size.height > output.y + output.height) {
            p = {output.x + k_cascade, output.y + k_cascade};
        }
    }
    return p;
}

lx::rect2i lx::compositor::evacuate_rect(lx::rect2i f, lx::rect2i from, lx::rect2i to) {
    lx::rect2i r{0, 0, f.width < to.width ? f.width : to.width,
                 f.height < to.height ? f.height : to.height};
    if (r.width < 1)
        r.width = 1;
    if (r.height < 1)
        r.height = 1;
    // The fraction of the free room on the left (top) of the old area, applied to the free
    // room of the new one: a window at the right edge stays at the right edge.
    const auto place = [](int pos, int size, int from_pos, int from_size, int to_pos, int to_size) {
        const long long room_from = static_cast<long long>(from_size) - size;
        const long long room_to = static_cast<long long>(to_size) - size;
        long long p = to_pos;
        if (room_from > 0 && room_to > 0) {
            long long off = static_cast<long long>(pos) - from_pos;
            off = off < 0 ? 0 : (off > room_from ? room_from : off);
            p = to_pos + (off * room_to + room_from / 2) / room_from;
        }
        const long long hi = static_cast<long long>(to_pos) + (room_to > 0 ? room_to : 0);
        return static_cast<int>(p < to_pos ? to_pos : (p > hi ? hi : p));
    };
    r.x = place(f.x, r.width, from.x, from.width, to.x, to.width);
    r.y = place(f.y, r.height, from.y, from.height, to.y, to.height);
    return r;
}

lx::point2i lx::compositor::constrain_on_output(lx::rect2i output, lx::rect2i geometry) {
    const int keep_w = geometry.width < 64 ? geometry.width : 64;
    const int keep_h = geometry.height < 32 ? geometry.height : 32;
    int x = geometry.x;
    int y = geometry.y;
    const int min_x = output.x - geometry.width + keep_w;
    const int max_x = output.x + output.width - keep_w;
    const int min_y = output.y;
    const int max_y = output.y + output.height - keep_h;
    if (x > max_x)
        x = max_x;
    if (x < min_x)
        x = min_x;
    if (y > max_y)
        y = max_y;
    if (y < min_y)
        y = min_y;
    return {x, y};
}

// ── fullscreen_backdrop ────────────────────────────────────────────────────

lx::compositor::fullscreen_backdrop::~fullscreen_backdrop() = default;

lx::scene::node_kind lx::compositor::fullscreen_backdrop::kind() const {
    // Not a container, so `scene_graph::emit_node` asks it for draws.
    return scene::node_kind::surface;
}

void lx::compositor::fullscreen_backdrop::emit_draws(scene::draw_list& out,
                                                    const scene::emit_context& ctx) const {
    if (!shown_ || rect_.width <= 0 || rect_.height <= 0)
        return;
    scene::draw_command cmd{};
    cmd.texture = lx::texture_id{k_texture_id};
    cmd.dst = scene::project_node(ctx.projection, {rect_.x, rect_.y}, {rect_.x, rect_.y},
                                  {rect_.width, rect_.height});
    if (ctx.projection.size.width > 0 && ctx.projection.size.height > 0 &&
        (cmd.dst.x >= ctx.projection.size.width || cmd.dst.y >= ctx.projection.size.height ||
         cmd.dst.x + cmd.dst.width <= 0 || cmd.dst.y + cmd.dst.height <= 0))
        return; // another output's
    cmd.src = lx::rect2f{0.f, 0.f, 1.f, 1.f};
    cmd.scaled = false; // one texel: nearest is exact
    cmd.opacity = 1.f;
    cmd.tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
    // Opaque: whatever is under it on this output is occluded and never composited.
    cmd.blend = lx::blend_mode::opaque;
    cmd.sort_key = lx::draw_sort_key::make(0, out.size());
    out.push(cmd);
}

void lx::compositor::fullscreen_backdrop::show(lx::rect2i logical) {
    rect_ = logical;
    shown_ = true;
    mark_dirty({});
}

void lx::compositor::fullscreen_backdrop::hide() {
    shown_ = false;
    mark_dirty({});
}

// ── snap_preview ───────────────────────────────────────────────────────────

lx::compositor::snap_preview::~snap_preview() = default;

lx::scene::node_kind lx::compositor::snap_preview::kind() const {
    return scene::node_kind::surface;
}

void lx::compositor::snap_preview::emit_draws(scene::draw_list& out,
                                              const scene::emit_context& ctx) const {
    if (!shown_ || rect_.width <= 2 || rect_.height <= 2)
        return;
    const auto push = [&](unsigned texture, lx::rect2i r) {
        scene::draw_command cmd{};
        cmd.texture = lx::texture_id{texture};
        cmd.dst = scene::project_node(ctx.projection, {r.x, r.y}, {r.x, r.y}, {r.width, r.height});
        if (cmd.dst.width <= 0 || cmd.dst.height <= 0)
            return;
        if (ctx.projection.size.width > 0 && ctx.projection.size.height > 0 &&
            (cmd.dst.x >= ctx.projection.size.width || cmd.dst.y >= ctx.projection.size.height ||
             cmd.dst.x + cmd.dst.width <= 0 || cmd.dst.y + cmd.dst.height <= 0))
            return; // another output's
        cmd.src = lx::rect2f{0.f, 0.f, 1.f, 1.f};
        cmd.scaled = false; // one texel: nearest is exact
        cmd.opacity = 1.f;
        cmd.tint = lx::color::rgb(1.f, 1.f, 1.f, 1.f);
        cmd.blend = lx::blend_mode::premultiplied;
        cmd.sort_key = lx::draw_sort_key::make(0, out.size());
        out.push(cmd);
    };
    const lx::rect2i r = rect_;
    push(k_fill_texture, {r.x + 1, r.y + 1, r.width - 2, r.height - 2});
    push(k_border_texture, {r.x, r.y, r.width, 1});
    push(k_border_texture, {r.x, r.y + r.height - 1, r.width, 1});
    push(k_border_texture, {r.x, r.y + 1, 1, r.height - 2});
    push(k_border_texture, {r.x + r.width - 1, r.y + 1, 1, r.height - 2});
}

void lx::compositor::snap_preview::show(lx::rect2i logical) {
    rect_ = logical;
    shown_ = true;
    mark_dirty({});
}

void lx::compositor::snap_preview::hide() {
    shown_ = false;
    mark_dirty({});
}

// ── window_manager ─────────────────────────────────────────────────────────

void lx::compositor::window_manager::bind(toplevel_manager& toplevels, surface_manager& surfaces,
                                          surface_tree& tree, scene::scene_graph& scene,
                                          scene::container_node& windows_layer,
                                          input_router& router, lx::input::seat& seat,
                                          shell_bridge& shell, output_manager& outputs) {
    toplevels_ = &toplevels;
    surfaces_ = &surfaces;
    tree_ = &tree;
    tree.set_unmap_hook(
        [](void* user, lx::surface_id surface) {
            static_cast<window_manager*>(user)->on_surface_unmapped(surface);
        },
        this);
    scene_ = &scene;
    windows_layer_ = &windows_layer;
    router_ = &router;
    seat_ = &seat;
    shell_ = &shell;
    outputs_ = &outputs;
}

void lx::compositor::window_manager::set_popups(popup_manager* popups) {
    popups_ = popups;
    if (!popups)
        return;
    popup_host host{};
    host.user = this;
    // Window popups stay inside the usable area (Mutter, KWin: a menu opened under a top
    // panel opens below it), layer-surface popups inside the whole output (sway: a panel's
    // own menus may cover its exclusive zone).
    host.constraint_area = [](void* user, popup_parent_kind root, lx::surface_id root_surface) {
        const auto* wm = static_cast<window_manager*>(user);
        // The output of the chain's root: the layer surface's own, the window's.
        int slot = -1;
        if (root == popup_parent_kind::layer && wm->layers_)
            slot = wm->layers_->output_of(root_surface);
        else if (wm->toplevels_)
            if (const auto* rec = wm->toplevels_->find_by_surface(root_surface))
                slot = wm->window_output(*rec);
        return root == popup_parent_kind::layer ? wm->output_area(slot) : wm->usable_area(slot);
    };
    host.focus_changed = [](void* user) {
        static_cast<window_manager*>(user)->resolve_keyboard_focus();
    };
    popups->set_host(host);
}

void lx::compositor::window_manager::set_layers(layer_shell* layers) {
    layers_ = layers;
    if (popups_)
        popups_->set_layers(layers);
    if (!layers)
        return;
    layer_host host{};
    host.user = this;
    host.output_area = [](void* user, unsigned slot) {
        const auto* wm = static_cast<window_manager*>(user);
        // A gone output has no area: its surfaces are closed by the arrangement.
        if (!wm->outputs_ || !wm->outputs_->slot_live(slot))
            return wm->outputs_ && wm->outputs_->count() > 0 ? lx::rect2i{}
                                                             : lx::rect2i{0, 0, 1920, 1080};
        return wm->output_area(static_cast<int>(slot));
    };
    host.usable_changed = [](void* user, unsigned slot, lx::rect2i usable) {
        static_cast<window_manager*>(user)->on_usable_area_changed(slot, usable);
    };
    host.focus_changed = [](void* user) {
        auto* wm = static_cast<window_manager*>(user);
        // An on_demand surface that unmapped or lost its interactivity gives the keyboard
        // back.
        if (wm->layer_focus_ && !(wm->layers_ && wm->layers_->focusable(wm->layer_focus_))) {
            if (log_layer.enabled(lx::trace::level::debug)) {
                log_layer.debug("layer focus surface=0 interactivity=none "
                                "cause=unmap previous={}",
                                wm->layer_focus_.id());
            }
            wm->layer_focus_ = {};
        }
        wm->resolve_keyboard_focus();
        wm->rehit_pending_ = true;
    };
    host.surface_mapped = [](void* user, lx::surface_id surface) {
        auto* wm = static_cast<window_manager*>(user);
        // sway's handle_map: an on_demand surface on the top or overlay layer takes the
        // keyboard as it maps — unless an interactive move, resize or drag is running, or
        // a surface on a higher layer already has it. focus_changed resolves it next.
        if (!wm->layers_ || !wm->layers_->focus_on_map(surface) || wm->grab_active() ||
            wm->drag_active())
            return;
        if (wm->layer_focus_ && wm->layers_->focusable(wm->layer_focus_) &&
            wm->layers_->mapped_level(wm->layer_focus_) > wm->layers_->mapped_level(surface))
            return;
        wm->layer_focus_ = surface;
        if (log_layer.enabled(lx::trace::level::debug)) {
            log_layer.debug("layer focus surface={} interactivity=on_demand "
                            "cause=map",
                            surface.id());
        }
    };
    host.surface_moved = [](void* user, lx::surface_id surface) {
        auto* wm = static_cast<window_manager*>(user);
        if (wm->popups_)
            wm->popups_->on_parent_moved(surface);
        wm->rehit_pending_ = true;
    };
    host.surface_unmapped = [](void* user, lx::surface_id surface, const char* reason) {
        auto* wm = static_cast<window_manager*>(user);
        if (!wm->popups_)
            return;
        if (reason && std::strcmp(reason, "destroyed") == 0)
            wm->popups_->on_parent_destroyed(surface);
        else
            wm->popups_->on_parent_unmapped(surface, "parent-unmapped");
    };
    layers->set_host(host);
}

lx::compositor::frame_extents
lx::compositor::window_manager::extents_of(const toplevel_record& rec) {
    // No frame around a fullscreen window.
    if (rec.state == lx::toplevel_state::fullscreen)
        return {};
    return extents_for(rec.decoration_current, rec.state == lx::toplevel_state::maximized);
}

void lx::compositor::window_manager::sync_decoration(const toplevel_record& rec) {
    if (!decorations_)
        return;
    decorations_->sync(rec);
    // The title at the scale of every output the frame is on: 1:1 on each (ADR-010 §4.11).
    if (rec.mapped && outputs_ && rec.decoration_current == decoration_mode::server) {
        const std::uint32_t mask = outputs_for_rect(*outputs_, frame_rect(rec.geometry, extents_of(rec)));
        double scales[output_manager::k_max]{};
        unsigned n = 0;
        for (unsigned s = 0; s < output_manager::k_max; ++s)
            if ((mask & (1u << s)) && outputs_->slot_state(s).scale > 0.0)
                scales[n++] = outputs_->slot_state(s).scale;
        if (n > 0)
            decorations_->set_window_scales(rec.id, {scales, n});
    }
}

void lx::compositor::window_manager::sync_backdrops() {
    // One backdrop per output with a fullscreen window on it (committed state).
    bool want[output_manager::k_max]{};
    if (toplevels_)
        toplevels_->for_each([&](const toplevel_record& rec) {
            if (rec.mapped && !rec.hidden && rec.state == lx::toplevel_state::fullscreen &&
                rec.fullscreen_output >= 0 &&
                static_cast<unsigned>(rec.fullscreen_output) < output_manager::k_max)
                want[rec.fullscreen_output] = true;
        });
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        fullscreen_backdrop& b = backdrops_[s];
        const bool live = outputs_ && outputs_->slot_live(s);
        if (want[s] && live && fullscreen_layer_) {
            const lx::rect2i area = output_area(static_cast<int>(s));
            if (b.shown() && rect_equal(b.rect(), area))
                continue;
            if (!backdrop_staged_ && tex_ops_.stage) {
                const std::uint32_t black = 0xFF000000u;
                backdrop_staged_ =
                    tex_ops_.stage(tex_ops_.user, fullscreen_backdrop::k_texture_id, &black, 1, 1, 1,
                                   1, 1);
            }
            if (b.shown() && scene_)
                scene_->note_damage(b.rect());
            b.show(area);
            if (scene_)
                scene_->note_damage(area);
        } else if (b.shown()) {
            if (scene_)
                scene_->note_damage(b.rect());
            b.hide();
        }
    }
}

void lx::compositor::window_manager::sync_fullscreen(toplevel_record& rec, bool was_fullscreen) {
    const bool full = rec.state == lx::toplevel_state::fullscreen;
    if (full == was_fullscreen || !fullscreen_layer_ || !tree_ || !windows_layer_)
        return;
    if (full) {
        // Above the top layer-shell layer (a panel) and every window, under the overlay
        // layer and popups (sway).
        (void)tree_->move_root(rec.surface, *fullscreen_layer_);
    } else {
        (void)tree_->move_root(rec.surface, *windows_layer_);
        sync_node_stacking(rec, "unfullscreen");
    }
    tree_->damage_root(rec.surface);
    sync_backdrops();
    rehit_pending_ = true;
}

lx::rect2i lx::compositor::window_manager::output_area(int slot) const {
    if (outputs_ && outputs_->count() > 0) {
        if (slot < 0 || !outputs_->slot_live(static_cast<unsigned>(slot)))
            slot = outputs_->slot_of(outputs_->nth(0));
        // Logical: windows, layers and popups are placed in layout coordinates.
        const lx::rect2i g = outputs_->slot_state(static_cast<unsigned>(slot)).logical_rect();
        if (!rect_empty(g))
            return g;
    }
    // Headless / no mode yet: a nominal desktop so placement still produces sane numbers.
    return {0, 0, 1920, 1080};
}

lx::rect2i lx::compositor::window_manager::usable_area(int slot) const {
    if (outputs_ && outputs_->count() > 0 &&
        (slot < 0 || !outputs_->slot_live(static_cast<unsigned>(slot))))
        slot = outputs_->slot_of(outputs_->nth(0));
    if (layers_ && slot >= 0 && layers_->arranged(static_cast<unsigned>(slot))) {
        const lx::rect2i u = layers_->usable(static_cast<unsigned>(slot));
        // Zones that ate the whole output leave nothing to place into; the output then.
        if (!rect_empty(u))
            return u;
    }
    return output_area(slot);
}

int lx::compositor::window_manager::pointer_output() const {
    if (!outputs_ || outputs_->count() == 0)
        return -1;
    double x = seat_ ? seat_->pointer_x() : 0.0;
    double y = seat_ ? seat_->pointer_y() : 0.0;
    (void)closest_point(*outputs_, x, y);
    const int at = output_at(*outputs_, x, y);
    return at >= 0 ? at : outputs_->slot_of(outputs_->nth(0));
}

int lx::compositor::window_manager::window_output(const toplevel_record& rec) const {
    if (!outputs_ || outputs_->count() == 0)
        return -1;
    if (rec.state == lx::toplevel_state::fullscreen && rec.fullscreen_output >= 0 &&
        outputs_->slot_live(static_cast<unsigned>(rec.fullscreen_output)))
        return rec.fullscreen_output;
    if (rec.mapped) {
        const int primary = primary_output_for_rect(*outputs_, frame_rect(rec.geometry, extents_of(rec)));
        if (primary >= 0)
            return primary;
    }
    if (rec.output >= 0 && outputs_->slot_live(static_cast<unsigned>(rec.output)))
        return rec.output;
    return pointer_output();
}

int lx::compositor::window_manager::placement_output(const toplevel_record& rec) const {
    // M10: a child on its parent's output; anything else on the output under the pointer —
    // in a stacking desktop the pointer marks where the user works (Mutter).
    for (unsigned depth = 0; rec.parent && depth < 8; ++depth) {
        if (const auto* parent = toplevels_ ? toplevels_->find(rec.parent) : nullptr) {
            if (parent->mapped || parent->output >= 0)
                return window_output(*parent);
        }
        break;
    }
    return pointer_output();
}

void lx::compositor::window_manager::set_parent(lx::toplevel_id id, lx::toplevel_id parent) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || rec->parent == parent)
        return;
    const bool was_modal = effective_modal(*toplevels_, *rec);
    rec->parent = parent;
    tiling_dirty_ = true;
    const bool modal = effective_modal(*toplevels_, *rec);
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("parent id={} parent={} modal={}", id.id(), parent.id(), modal ? 1 : 0);
    }
    if (!rec->mapped)
        return;
    if (shell_) {
        shell_->emit_toplevel_parent(*rec);
        if (was_modal != modal)
            shell_->emit_toplevel_state(*rec);
    }
    if (modal) {
        keep_modals_above(parent);
        if (const lx::toplevel_id b = modal_blocker(keyboard_focus_); b)
            set_keyboard_focus(b);
    }
}

void lx::compositor::window_manager::set_modal(lx::toplevel_id id, bool modal) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || rec->modal == modal)
        return;
    const bool was = effective_modal(*toplevels_, *rec);
    rec->modal = modal;
    const bool now = effective_modal(*toplevels_, *rec);
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("modal id={} on={} effective={}", id.id(), modal ? 1 : 0, now ? 1 : 0);
    }
    if (was == now)
        return;
    if (shell_)
        shell_->emit_toplevel_state(*rec);
    if (now) {
        // Mutter: a dialog that becomes modal over the focused window takes the keyboard.
        keep_modals_above(rec->parent);
        if (const lx::toplevel_id b = modal_blocker(keyboard_focus_); b)
            set_keyboard_focus(b);
    }
}

bool lx::compositor::window_manager::blocks(const toplevel_record& rec,
                                            lx::toplevel_id ancestor) const {
    const toplevel_record* at = &rec;
    for (unsigned depth = 0; at && depth < 8; ++depth) {
        if (!effective_modal(*toplevels_, *at))
            return false;
        if (at->parent == ancestor)
            return true;
        at = toplevels_->find(at->parent);
    }
    return false;
}

lx::toplevel_id lx::compositor::window_manager::modal_blocker(lx::toplevel_id id) const {
    lx::toplevel_id found{};
    if (!toplevels_ || !id)
        return found;
    toplevels_->for_each_top_down([&](const toplevel_record& r) {
        if (r.id != id && r.modal && blocks(r, id)) {
            found = r.id;
            return false;
        }
        return true;
    });
    return found;
}

void lx::compositor::window_manager::keep_modals_above(lx::toplevel_id id) {
    if (!toplevels_ || !id || toplevels_->stacking_index(id) == ~0u)
        return;
    // Bottom-up, so the dialogs keep their order among themselves; fixed storage.
    lx::toplevel_id above[16];
    unsigned n = 0;
    toplevels_->for_each_bottom_up([&](const toplevel_record& r) {
        if (n < 16 && r.id != id && r.modal && blocks(r, id))
            above[n++] = r.id;
    });
    for (unsigned j = 0; j < n; ++j) {
        const unsigned at = toplevels_->stacking_index(id);
        const unsigned cur = toplevels_->stacking_index(above[j]);
        // Taking a window from below `id` shifts `id` (and those already placed) down one.
        const unsigned want = cur < at ? at + j : at + 1 + j;
        if (cur != want && toplevels_->move_to(above[j], want))
            if (const auto* r = toplevels_->find(above[j]))
                sync_node_stacking(*r, "modal-above");
    }
}

void lx::compositor::window_manager::emit_children_state(lx::toplevel_id id) {
    if (!shell_ || !toplevels_ || !id)
        return;
    toplevels_->for_each_bottom_up([&](const toplevel_record& r) {
        if (r.parent != id)
            return;
        shell_->emit_toplevel_parent(r);
        if (r.modal)
            shell_->emit_toplevel_state(r);
    });
}

void lx::compositor::window_manager::on_output_changed() {
    // An output that went without `evacuate_output` (a path that never had windows to
    // move) takes its workspaces with it; outputs that came get theirs in on_output_added.
    sync_workspace_outputs(false);
    tiling_dirty_ = true;
    if (layers_)
        layers_->arrange(); // reports each changed usable area through the host
    else if (outputs_)
        for (unsigned s = 0; s < output_manager::k_max; ++s)
            if (outputs_->slot_live(s))
                on_usable_area_changed(s, output_area(static_cast<int>(s)));
    if (!toplevels_)
        return;
    // Fullscreen windows follow their output's rect (a mode or scale change); one whose
    // output went is fullscreen on the window's output now.
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        // A stranded window waits for the output on its way (`on_output_added`).
        if (!(rec.desired.states & tl_fullscreen) || rec.stranded)
            return;
        if (rec.fullscreen_output < 0 || !outputs_ ||
            !outputs_->slot_live(static_cast<unsigned>(rec.fullscreen_output)))
            rec.fullscreen_output = pointer_output();
        const lx::rect2i area = output_area(rec.fullscreen_output);
        if (rec.desired.size.width != area.width || rec.desired.size.height != area.height ||
            rec.geometry.x != area.x || rec.geometry.y != area.y) {
            rec.desired.size = {area.width, area.height};
            rec.desired.place = true;
            rec.desired.place_at = {area.x, area.y};
            rec.configure_due = true;
        }
    });
    sync_backdrops();
    // Decorations follow the scales of the outputs their windows are on.
    toplevels_->for_each([&](const toplevel_record& rec) {
        if (rec.mapped)
            sync_decoration(rec);
    });
}

unsigned lx::compositor::window_manager::evacuate_output(unsigned slot, int fallback,
                                                         const char* identifier) {
    if (!toplevels_ || !outputs_ || static_cast<unsigned>(fallback) == slot)
        return 0;
    const lx::rect2i from = usable_area(static_cast<int>(slot));
    const lx::rect2i out = output_area(static_cast<int>(slot));
    unsigned moved = 0;
    tiling_dirty_ = true;
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!rec.mapped) {
            // Placed there, not shown yet: placed on the fallback instead (none: placed
            // again when it maps).
            if (rec.output == static_cast<int>(slot))
                rec.output = fallback;
            if (rec.want_fullscreen_output == static_cast<int>(slot))
                rec.want_fullscreen_output = fallback;
            return;
        }
        if (window_output(rec) != static_cast<int>(slot))
            return;
        ++moved;
        // Home: the first output it was moved off (moved again before returning, it still
        // goes back to where it started).
        if (!rec.home_identifier[0] && identifier && *identifier) {
            std::snprintf(rec.home_identifier, sizeof(rec.home_identifier), "%s", identifier);
            rec.home_rect = {rec.geometry.x - out.x, rec.geometry.y - out.y, rec.geometry.width,
                             rec.geometry.height};
            rec.home_maximized = (rec.desired.states & tl_maximized) != 0;
            rec.home_fullscreen = (rec.desired.states & tl_fullscreen) != 0;
            rec.home_snap = rec.snap;
            // Its workspace's place there, for its return (W.6).
            const workspace_record* w = workspaces_.find(rec.workspace);
            rec.home_workspace = w && w->output == static_cast<int>(slot) ? w->index + 1 : 0;
        }
        if (fallback < 0) {
            // Nowhere to go yet: the output on its way takes it in.
            rec.stranded = true;
            rec.stranded_from = from;
            return;
        }
        relocate_window(rec, from, fallback);
    });
    if (moved > 0) {
        rehit_pending_ = true;
        if (log_wm.enabled(lx::trace::level::debug)) {
            log_wm.debug("evacuate output={} to={} windows={:d}{}", slot, fallback, moved,
                         fallback < 0 ? " (stranded until an output comes)" : "");
        }
    }
    // Its workspaces (W.6): the active one's windows join what the fallback shows, the
    // others are appended there (static: merged by position), or all wait for an output.
    if (fallback >= 0 && !workspaces_.has_group(static_cast<unsigned>(fallback)) &&
        outputs_->slot_live(static_cast<unsigned>(fallback))) {
        const output_state& f = outputs_->slot_state(static_cast<unsigned>(fallback));
        workspaces_.output_added(static_cast<unsigned>(fallback), f.id.id(), f.identifier, f.name,
                                 sink());
        workspaces_.settle(static_cast<unsigned>(fallback));
    }
    workspaces_.output_removed(slot, fallback, sink());
    update_visibility();
    flush_workspaces();
    return moved;
}

void lx::compositor::window_manager::relocate_window(toplevel_record& rec, lx::rect2i from,
                                                     int fallback) {
    const lx::rect2i to = usable_area(fallback);
    const bool fullscreen = (rec.desired.states & tl_fullscreen) != 0;
    const bool maximized = (rec.desired.states & tl_maximized) != 0;
    rec.stranded = false;
    rec.output = fallback;
    if (fullscreen) {
        rec.fullscreen_output = fallback;
        const lx::rect2i area = output_area(fallback);
        rec.desired.size = {area.width, area.height};
        rec.desired.place = true;
        rec.desired.place_at = {area.x, area.y};
        rec.configure_due = true;
        // What unset_fullscreen goes back to moves along.
        if (rec.fullscreen_restore_set)
            rec.fullscreen_restore = evacuate_rect(rec.fullscreen_restore, from, to);
        return;
    }
    if (maximized) {
        const frame_extents ext = extents_for(rec.desired.decoration, true);
        rec.desired.size = {to.width - ext.left - ext.right, to.height - ext.top - ext.bottom};
        rec.desired.place = true;
        rec.desired.place_at = {to.x + ext.left, to.y + ext.top};
        rec.configure_due = true;
        if (rec.restore_set)
            rec.restore = evacuate_rect(rec.restore, from, to);
        return;
    }
    if (rec.snap) {
        // The same half or quarter of the new output (K.3); its normal geometry moves along.
        aim_snap(rec, rec.snap, fallback);
        if (rec.restore_set)
            rec.restore = evacuate_rect(rec.restore, from, to);
        return;
    }
    const frame_extents ext = extents_of(rec);
    const lx::rect2i f = frame_rect(rec.geometry, ext);
    const lx::rect2i nf = evacuate_rect(f, from, to);
    const lx::point2i at{nf.x + ext.left, nf.y + ext.top};
    if (nf.width == f.width && nf.height == f.height) {
        move_window(rec, at, "evacuate");
        return;
    }
    // Too large for the fallback: shrunk by a configure, placed by its answer.
    rec.desired.size = {nf.width - ext.left - ext.right, nf.height - ext.top - ext.bottom};
    rec.desired.place = true;
    rec.desired.place_at = at;
    rec.configure_due = true;
}

bool lx::compositor::window_manager::move_to_output(lx::toplevel_id id, int dx, int dy) {
    toplevel_record* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || !rec->mapped || !outputs_)
        return false;
    const int from_slot = window_output(*rec);
    if (from_slot < 0)
        return false;
    const int best = neighbour_output(from_slot, dx, dy);
    if (best < 0)
        return false;
    // The user moved it: it no longer goes back to an output it was evacuated from.
    rec->home_identifier[0] = '\0';
    relocate_window(*rec, usable_area(from_slot), best);
    // It joins what that output shows (W.6); its dialogs come along.
    if (!rec->sticky) {
        sync_workspace_outputs(true);
        const lx::workspace_id to = workspaces_.active(static_cast<unsigned>(best));
        join_workspace(*rec, to);
        toplevels_->for_each_mut([&](toplevel_record& child) {
            if (child.parent == id && child.mapped && !child.sticky) {
                relocate_window(child, usable_area(from_slot), best);
                join_workspace(child, to);
            }
        });
    }
    update_visibility();
    flush_workspaces();
    rehit_pending_ = true;
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("move to output id={} from={} to={}", id.id(), from_slot, best);
    }
    return true;
}

int lx::compositor::window_manager::neighbour_output(int slot, int dx, int dy) const {
    if (!outputs_ || slot < 0)
        return -1;
    const lx::rect2i from = output_area(slot);
    const double cx = from.x + from.width / 2.0;
    const double cy = from.y + from.height / 2.0;
    // The nearest output whose centre lies that way (more along the direction than across).
    int best = -1;
    double best_d = 0.0;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (static_cast<int>(s) == slot || !outputs_->slot_live(s))
            continue;
        const lx::rect2i o = output_area(static_cast<int>(s));
        const double ox = o.x + o.width / 2.0 - cx;
        const double oy = o.y + o.height / 2.0 - cy;
        const double along = ox * dx + oy * dy;
        const double across = std::abs(dx ? oy : ox);
        if (along <= 0.0 || across > along)
            continue;
        const double d = ox * ox + oy * oy;
        if (best < 0 || d < best_d) {
            best = static_cast<int>(s);
            best_d = d;
        }
    }
    return best;
}

unsigned lx::compositor::window_manager::on_output_added(unsigned slot) {
    if (!toplevels_ || !outputs_ || !outputs_->slot_live(slot))
        return 0;
    const output_state& o = outputs_->slot_state(slot);
    const lx::rect2i out = output_area(static_cast<int>(slot));
    const lx::rect2i usable = usable_area(static_cast<int>(slot));
    unsigned back = 0;
    tiling_dirty_ = true;
    // Its workspaces (W.6): those that waited with no output left, else its own coming back
    // from where they were appended (their windows move with them) — then, below, windows
    // that were merged go to a workspace at their old position, and `settle` makes the
    // workspace that was active there active again.
    workspaces_.output_added(slot, o.id.id(), o.identifier, o.name, sink());
    // Stranded windows (their output went with none left): this one takes them in. One whose
    // home this is goes back to its place below instead.
    unsigned adopted = 0;
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!rec.stranded)
            return;
        if (o.identifier[0] && rec.home_identifier[0] &&
            std::strcmp(rec.home_identifier, o.identifier) == 0) {
            rec.stranded = false;
            return;
        }
        if (!rec.mapped) {
            rec.stranded = false;
            return;
        }
        ++adopted;
        relocate_window(rec, rec.stranded_from, static_cast<int>(slot));
    });
    if (adopted > 0) {
        rehit_pending_ = true;
        if (log_wm.enabled(lx::trace::level::debug)) {
            log_wm.debug("stranded windows taken in by output={} windows={}", slot, adopted);
        }
    }
    if (!o.identifier[0]) {
        workspaces_.settle(slot);
        update_visibility();
        flush_workspaces();
        return adopted;
    }
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!rec.home_identifier[0] || std::strcmp(rec.home_identifier, o.identifier) != 0)
            return;
        rec.home_identifier[0] = '\0';
        rec.stranded = false;
        if (!rec.mapped)
            return;
        ++back;
        rec.output = static_cast<int>(slot);
        if (!rec.sticky)
            join_workspace(rec, workspaces_.home_workspace(slot, rec.home_workspace));
        rec.home_workspace = 0;
        const bool fullscreen = (rec.desired.states & tl_fullscreen) != 0;
        const bool maximized = (rec.desired.states & tl_maximized) != 0;
        if (fullscreen) {
            rec.fullscreen_output = static_cast<int>(slot);
            rec.desired.size = {out.width, out.height};
            rec.desired.place = true;
            rec.desired.place_at = {out.x, out.y};
            rec.configure_due = true;
            return;
        }
        if (maximized) {
            const frame_extents ext = extents_for(rec.desired.decoration, true);
            rec.desired.size = {usable.width - ext.left - ext.right,
                                usable.height - ext.top - ext.bottom};
            rec.desired.place = true;
            rec.desired.place_at = {usable.x + ext.left, usable.y + ext.top};
            rec.configure_due = true;
            if (rec.restore_set)
                rec.restore = {out.x + rec.home_rect.x, out.y + rec.home_rect.y, rec.restore.width,
                               rec.restore.height};
            return;
        }
        if (rec.snap && rec.home_snap) {
            // Snapped there: the same half or quarter again (its normal geometry came along
            // with every relocation).
            aim_snap(rec, rec.home_snap, static_cast<int>(slot));
            rec.snap = rec.home_snap;
            rec.home_snap = 0;
            return;
        }
        rec.home_snap = 0;
        // Its old place on the output, kept reachable (the output may have come back
        // smaller, or with a panel over the old title bar).
        const frame_extents ext = extents_of(rec);
        const lx::rect2i want{out.x + rec.home_rect.x, out.y + rec.home_rect.y,
                              rec.home_rect.width, rec.home_rect.height};
        const lx::point2i p = constrain_on_output(usable, frame_rect(want, ext));
        const lx::point2i at{p.x + ext.left, p.y + ext.top};
        if (want.width == rec.geometry.width && want.height == rec.geometry.height) {
            move_window(rec, at, "home");
            return;
        }
        // It was shrunk to fit where it went: its size back with a configure.
        rec.desired.size = {want.width, want.height};
        rec.desired.place = true;
        rec.desired.place_at = at;
        rec.configure_due = true;
    });
    if (back > 0) {
        rehit_pending_ = true;
        if (log_wm.enabled(lx::trace::level::debug)) {
            log_wm.debug("home output={} identifier=\"{}\" windows={}", slot, o.identifier, back);
        }
    }
    workspaces_.settle(slot);
    update_visibility();
    flush_workspaces();
    return back + adopted;
}

void lx::compositor::window_manager::on_usable_area_changed(unsigned slot, lx::rect2i usable) {
    if (rect_empty(usable))
        usable = output_area(static_cast<int>(slot));
    if (slot < output_manager::k_max && !rect_equal(usable, usable_logged_[slot])) {
        usable_logged_[slot] = usable;
        if (log_wm.enabled(lx::trace::level::debug)) {
            log_wm.debug("usable area x={} y={} w={} h={} output={}", usable.x, usable.y,
                         usable.width, usable.height, slot);
        }
    }
    if (!toplevels_)
        return;
    rehit_pending_ = true;
    tiling_dirty_ = true;
    // The output a window belongs to here: where a placing configure not yet answered takes
    // it (a window sent home by a hotplug moments ago, before the new output's panel maps),
    // else where it is drawn.
    const auto headed_output = [&](const toplevel_record& rec) {
        const lx::point2i* to = rec.desired.place ? &rec.desired.place_at : nullptr;
        for (unsigned i = rec.sent_count; !to && i-- > 0;)
            if (rec.sent[i].place)
                to = &rec.sent[i].place_at;
        if (to && outputs_) {
            const int at = output_at(*outputs_, to->x, to->y);
            if (at >= 0)
                return at;
        }
        return window_output(rec);
    };
    // Normal windows stay put (sway, KWin); maximized ones on this output fill the new area,
    // and snapped ones their half or quarter of it (K.3).
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (rec.snap && !(rec.desired.states & (tl_maximized | tl_fullscreen))) {
            if (headed_output(rec) != static_cast<int>(slot))
                return;
            const lx::rect2i g = snap_geometry(usable, rec.snap,
                                               extents_for(rec.desired.decoration, false),
                                               rec.min_size, rec.max_size);
            if (g.width != rec.desired.size.width || g.height != rec.desired.size.height ||
                rec.desired.place) {
                aim_snap(rec, rec.snap, static_cast<int>(slot));
                return;
            }
            // Same size, new place: moved without a configure, the far edges kept.
            const int x = (rec.snap & edge_right) ? g.x + g.width - rec.geometry.width : g.x;
            const int y = (rec.snap & edge_bottom) ? g.y + g.height - rec.geometry.height : g.y;
            if (rec.mapped && (rec.geometry.x != x || rec.geometry.y != y))
                move_window(rec, {x, y}, "usable");
            return;
        }
        if (!(rec.desired.states & tl_maximized) || (rec.desired.states & tl_fullscreen))
            return;
        if (headed_output(rec) != static_cast<int>(slot))
            return;
        const frame_extents ext = extents_for(rec.desired.decoration, true);
        const lx::size2i size{usable.width - ext.left - ext.right,
                              usable.height - ext.top - ext.bottom};
        const lx::point2i at{usable.x + ext.left, usable.y + ext.top};
        // A placing configure still unanswered names the old place: a new configure must
        // carry the new one, or the answer to the old would land the window there.
        bool stale_inflight = false;
        for (unsigned i = 0; i < rec.sent_count; ++i)
            if (rec.sent[i].place &&
                (rec.sent[i].place_at.x != at.x || rec.sent[i].place_at.y != at.y))
                stale_inflight = true;
        if (size.width != rec.desired.size.width || size.height != rec.desired.size.height) {
            rec.desired.size = size;
            rec.desired.place = true;
            rec.desired.place_at = at;
        } else if (rec.desired.place || stale_inflight) {
            rec.desired.place = true;
            rec.desired.place_at = at;
            if (stale_inflight)
                rec.configure_due = true;
        } else if (rec.mapped && rec.state == lx::toplevel_state::maximized &&
                   (rec.geometry.x != at.x || rec.geometry.y != at.y)) {
            // Same size, new place (a panel moved edges): no configure needed.
            move_window(rec, at, "usable");
        }
    });
}

void lx::compositor::window_manager::set_maximized(lx::toplevel_id id, bool on,
                                                   const char* source) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec)
        return;
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("maximize id={} on={} source={}", id.id(), on ? 1 : 0, source ? source : "-");
    }
    // Before the initial configure, that configure answers it.
    if (!rec->initial_configure_sent) {
        rec->want_maximized = on;
        return;
    }
    // A move or resize of this window does not survive it changing size under the pointer.
    if (grab_.mode != grab_mode::none && grab_.toplevel == id) {
        end_grab(grab_end_reason::release);
        rec = toplevels_->find_mut(id);
        if (!rec)
            return;
    }
    if (rec->desired.states & tl_fullscreen) {
        // Fullscreen: what unset_fullscreen goes back to; the configure still answers it.
        rec->fullscreen_was_maximized = on;
        rec->configure_due = true;
        if (!rec->inflight)
            send_configure(*rec, wm_now_ns(), false, false);
        return;
    }
    const bool is_max = (rec->desired.states & tl_maximized) != 0;
    const int slot = window_output(*rec);
    if (on && !is_max) {
        // A snapped window's normal geometry is already kept (K.3): maximize keeps it.
        if (rec->mapped && rec->state == lx::toplevel_state::normal && !rec->snap) {
            rec->restore = rec->geometry;
            rec->restore_set = true;
        }
        aim_maximized(*rec, slot);
    } else if (!on && is_max) {
        rec->desired.states &= ~static_cast<unsigned>(tl_maximized | tl_tiled_all);
        rec->snap = 0;
        if (rec->tiled) {
            // Back in the slot it kept (ADR 012 T3).
            rec->restore_set = false;
            aim_tile(*rec, rec->tile, false);
        } else if (rec->restore_set) {
            // Back where it was — kept reachable if the usable area changed meanwhile (a
            // panel appeared over the old title bar), as a move would be.
            const frame_extents ext = extents_for(rec->desired.decoration, false);
            const lx::point2i p = constrain_on_output(usable_area(slot),
                                                      frame_rect(rec->restore, ext));
            rec->desired.size = {rec->restore.width, rec->restore.height};
            rec->desired.place = true;
            rec->desired.place_at = {p.x + ext.left, p.y + ext.top};
            rec->restore_set = false;
        } else {
            // Maximized before it ever mapped: nothing to restore to; the client picks.
            rec->desired.size = {};
            rec->desired.place = false;
        }
    }
    // xdg-shell: answered by a configure even when the state does not change.
    rec->configure_due = true;
    if (!rec->inflight)
        send_configure(*rec, wm_now_ns(), false, false);
}

void lx::compositor::window_manager::set_fullscreen(lx::toplevel_id id, bool on, int output,
                                                    const char* source) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec)
        return;
    if (output >= 0 && !(outputs_ && outputs_->slot_live(static_cast<unsigned>(output))))
        output = -1; // a wl_output whose output is gone: the window's own
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("fullscreen id={} on={} output={} source={}", id.id(), on ? 1 : 0, output,
                     source ? source : "-");
    }
    // Before the initial configure, that configure answers it.
    if (!rec->initial_configure_sent) {
        rec->want_fullscreen = on;
        rec->want_fullscreen_output = output;
        return;
    }
    if (grab_.mode != grab_mode::none && grab_.toplevel == id) {
        end_grab(grab_end_reason::release);
        rec = toplevels_->find_mut(id);
        if (!rec)
            return;
    }
    const bool is_full = (rec->desired.states & tl_fullscreen) != 0;
    if (on) {
        const int slot = output >= 0 ? output : window_output(*rec);
        if (!is_full) {
            rec->fullscreen_was_maximized = (rec->desired.states & tl_maximized) != 0;
            rec->fullscreen_restore_set = rec->mapped && rec->state == lx::toplevel_state::normal;
            if (rec->fullscreen_restore_set)
                rec->fullscreen_restore = rec->geometry;
            else if (rec->restore_set)
                rec->fullscreen_restore = rec->restore; // maximized: the normal geometry
            rec->fullscreen_restore_set = rec->fullscreen_restore_set || rec->restore_set;
            // Snapped: unset_fullscreen snaps it there again (its normal geometry stays in
            // `restore`).
            rec->fullscreen_snap = rec->snap;
            rec->snap = 0;
        }
        rec->fullscreen_output = slot;
        // The whole output, no frame: xdg-shell's fullscreen size is the output's.
        const lx::rect2i area = output_area(slot);
        rec->desired.states |= tl_fullscreen;
        rec->desired.states &= ~static_cast<unsigned>(tl_maximized | tl_tiled_all);
        rec->desired.size = {area.width, area.height};
        rec->desired.place = true;
        rec->desired.place_at = {area.x, area.y};
        rec->desired.anchor_edges = 0;
    } else if (is_full) {
        rec->desired.states &= ~static_cast<unsigned>(tl_fullscreen);
        const int slot = window_output(*rec);
        if (rec->fullscreen_was_maximized) {
            const lx::rect2i area = usable_area(slot);
            const frame_extents ext = extents_for(rec->desired.decoration, true);
            rec->desired.states |= tl_maximized;
            rec->desired.size = {area.width - ext.left - ext.right, area.height - ext.top - ext.bottom};
            rec->desired.place = true;
            rec->desired.place_at = {area.x + ext.left, area.y + ext.top};
        } else if (rec->tiled) {
            aim_tile(*rec, rec->tile, false);
        } else if (rec->fullscreen_snap) {
            aim_snap(*rec, rec->fullscreen_snap, slot);
            rec->snap = rec->fullscreen_snap;
        } else if (rec->fullscreen_restore_set) {
            const frame_extents ext = extents_for(rec->desired.decoration, false);
            const lx::point2i p = constrain_on_output(usable_area(slot),
                                                      frame_rect(rec->fullscreen_restore, ext));
            rec->desired.size = {rec->fullscreen_restore.width, rec->fullscreen_restore.height};
            rec->desired.place = true;
            rec->desired.place_at = {p.x + ext.left, p.y + ext.top};
        } else {
            // Fullscreen before it ever mapped: nothing to restore to; the client picks.
            rec->desired.size = {};
            rec->desired.place = false;
        }
        rec->fullscreen_restore_set = false;
        rec->fullscreen_snap = 0;
    }
    // xdg-shell: answered by a configure even when the state does not change.
    rec->configure_due = true;
    if (!rec->inflight)
        send_configure(*rec, wm_now_ns(), false, false);
}

void lx::compositor::window_manager::log_geometry(const toplevel_record& rec, const char* cause,
                                                  unsigned serial) const {
    if (!log_wm.enabled(lx::trace::level::debug))
        return;
    log_wm.debug("geometry id={} x={} y={} w={} h={} cause={} serial={}", rec.id.id(),
                 rec.geometry.x, rec.geometry.y, rec.geometry.width, rec.geometry.height, cause,
                 serial);
}

void lx::compositor::window_manager::send_configure(toplevel_record& rec,
                                                    lx::runtime::clock_time now_ns, bool initial,
                                                    bool watchdog) {
    if (!ops_.send_configure || !rec.xdg_toplevel_resource || !rec.xdg_surface_resource)
        return;
    toplevel_configure c = rec.desired;
    c.serial = ops_.next_serial ? ops_.next_serial(ops_.user) : rec.last_sent.serial + 1;
    // A placing configure (maximize, restore) still unanswered hands its placement on: the
    // client may answer only the newest configure, and the window must still land there.
    if (!c.place) {
        for (unsigned i = rec.sent_count; i-- > 0;) {
            if (rec.sent[i].place) {
                c.place = true;
                c.place_at = rec.sent[i].place_at;
                // A snap's far edges with it (a right half stays against the right edge).
                if (c.anchor_edges == 0) {
                    c.anchor_edges = rec.sent[i].anchor_edges;
                    c.anchor = rec.sent[i].anchor;
                }
                break;
            }
        }
    }

    // configure_bounds only when it changed: it is a hint for the client's initial size,
    // and resending an unchanged one on every configure is noise. A server-side frame takes
    // its extents out of what the window geometry may use. The window's own output's.
    const lx::rect2i area = usable_area(window_output(rec));
    const frame_extents ext =
        extents_for(c.decoration, (c.states & tl_maximized) != 0);
    const lx::size2i room{area.width - ext.left - ext.right, area.height - ext.top - ext.bottom};
    lx::size2i bounds{};
    if (rec.bounds_sent.width != room.width || rec.bounds_sent.height != room.height) {
        bounds = room;
        rec.bounds_sent = bounds;
    }

    // The decoration mode goes with every configure that changes it, the first one, and the
    // one answering a new decoration object or a set_mode (the spec: "the compositor will
    // respond by emitting an xdg_surface.configure event").
    const bool send_deco =
        rec.deco_resource && (initial || c.decoration != rec.last_sent.decoration ||
                              rec.deco_configure_due || rec.deco_awaiting_configure);
    ops_.send_configure(rec.xdg_toplevel_resource, rec.xdg_surface_resource, c, bounds, initial,
                        send_deco ? rec.deco_resource : nullptr);
    if (send_deco) {
        rec.deco_configure_due = false;
        rec.deco_awaiting_configure = false;
        if (log_deco.enabled(lx::trace::level::debug)) {
            const char* req = rec.deco_requested == decoration_request::client   ? "client"
                              : rec.deco_requested == decoration_request::server ? "server"
                                                                                 : "unset";
            log_deco.debug("decoration mode id={} requested={} sent={} serial={}", rec.id.id(), req,
                           c.decoration == decoration_mode::server ? "server" : "client", c.serial);
        }
    }
    toplevel_manager::on_sent(rec, c, now_ns);
    // The placement travels with the configures now on the wire; the answering commit
    // applies it.
    rec.desired.place = false;
    rec.configure_due = false;
    // The anchor travels with each sent configure. Once the last one a grab needs (its
    // final or Escape configure) is on the wire, later configures — an activation change,
    // say — must not anchor to a rect the window may since have left.
    if (!(grab_.mode != grab_mode::none && grab_.toplevel == rec.id)) {
        rec.desired.anchor_edges = 0;
        rec.desired.anchor = {};
    }

    if (log_wm.enabled(lx::trace::level::debug)) {
        char states[64];
        format_states(c.states, states, sizeof(states));
        log_wm.debug("configure id={} serial={} w={} h={} states={} edges={} "
                     "watchdog={}",
                     rec.id.id(), c.serial, c.size.width, c.size.height, states, c.anchor_edges,
                     watchdog ? 1 : 0);
    }
}

void lx::compositor::window_manager::on_initial_commit(lx::toplevel_id id) {
    if (!toplevels_)
        return;
    auto* rec = toplevels_->find_mut(id);
    if (!rec)
        return;
    rec->initial_commit_seen = true;
    // Where it will map (M10): its parent's output, else the one under the pointer. The
    // initial configure's bounds, a pre-map maximize and the fractional scale it is told
    // all follow from it.
    rec->output = placement_output(*rec);
    // 0x0: the client picks its own size. Activated, because it takes focus when it maps
    // (placeholder policy) — saying so now saves a second configure right after.
    rec->desired = {};
    // An exclusive layer surface keeps the keyboard: the new window will not be activated.
    rec->desired.states = layers_ && layers_->exclusive_focus() ? 0u : unsigned{tl_activated};
    if (suspend_reasons_ != 0)
        rec->desired.states |= tl_suspended;
    rec->desired.decoration = toplevel_manager::wanted_decoration(*rec);
    if (rec->want_maximized) {
        // set_maximized before the initial commit: the first configure maximizes, and the
        // commit answering it maps the window in place.
        rec->want_maximized = false;
        const lx::rect2i area = usable_area(rec->output);
        const frame_extents ext = extents_for(rec->desired.decoration, true);
        rec->desired.states |= tl_maximized;
        rec->desired.size = {area.width - ext.left - ext.right, area.height - ext.top - ext.bottom};
        rec->desired.place = true;
        rec->desired.place_at = {area.x + ext.left, area.y + ext.top};
    }
    if (rec->want_fullscreen) {
        // set_fullscreen before the initial commit: the first configure is fullscreen on the
        // output asked for (else the placement output).
        rec->want_fullscreen = false;
        const int slot = rec->want_fullscreen_output >= 0 && outputs_ &&
                                 outputs_->slot_live(static_cast<unsigned>(rec->want_fullscreen_output))
                             ? rec->want_fullscreen_output
                             : rec->output;
        const lx::rect2i area = output_area(slot);
        rec->fullscreen_was_maximized = (rec->desired.states & tl_maximized) != 0;
        rec->fullscreen_output = slot;
        rec->desired.states |= tl_fullscreen;
        rec->desired.states &= ~static_cast<unsigned>(tl_maximized);
        rec->desired.size = {area.width, area.height};
        rec->desired.place = true;
        rec->desired.place_at = {area.x, area.y};
    }
    // ADR 012 T4: a window that will tile is counted in its workspace's layout now, so this
    // configure carries its tile and the commit answering it maps it there.
    if (rec->output >= 0) {
        // Workspaces made at start-up only matter here when they would start tiled.
        const auto slot = static_cast<unsigned>(rec->output);
        if (!workspaces_.active(slot) && tiling_.layout == workspace_layout::master_stack)
            sync_workspace_outputs(true);
        if (const lx::workspace_id ws = workspaces_.active(slot); ws && tiles_on(*rec, ws)) {
            rec->tile_workspace = ws;
            rec->tile_order = ++tile_seq_;
            tiling_dirty_ = true;
            flush_tiling();
            rec = toplevels_->find_mut(id);
            if (!rec)
                return;
        }
    }
    send_configure(*rec, wm_now_ns(), true, false);
    rec->initial_configure_sent = true;
}

void lx::compositor::window_manager::on_toplevel_commit(lx::toplevel_id id,
                                                        const toplevel_commit_info& info) {
    if (!toplevels_ || !surfaces_ || !scene_ || !tree_)
        return;
    auto* rec = toplevels_->find_mut(id);
    if (!rec)
        return;

    const lx::rect2i previous = rec->geometry;
    if (info.has_buffer)
        rec->surface_size = info.surface_size;
    // Clamped to the whole tree, not the root buffer: a CSD title bar drawn as a subsurface
    // above the main surface is part of the window.
    const lx::rect2i eff = effective_window_geometry(rec->window_geometry_set,
                                                     rec->window_geometry_requested,
                                                     info.tree_bbox);
    rec->geometry_offset = {eff.x, eff.y};

    // The commit that follows an ack carries the buffer for that configure, so this is
    // the one moment its anchoring can be applied without ever showing a frame whose size
    // and position disagree: origin and buffer reach the scene in the same UI call, hence
    // the same snapshot.
    toplevel_configure applied{};
    unsigned applied_serial = 0;
    decoration_mode mode = rec->decoration_current;
    bool placed = false;
    bool state_changed = false;
    bool fullscreen_changed = false;
    const bool was_fullscreen = rec->state == lx::toplevel_state::fullscreen;
    if (toplevel_manager::consume_ack(*rec, applied)) {
        if (applied.place) {
            // Maximize / restore / snap: the origin the configure named, with the size the
            // client drew — in this same snapshot as that content. A snap to the right or the
            // bottom keeps that edge where it was asked to be, whatever size was drawn.
            rec->geometry = {applied.place_at.x, applied.place_at.y, eff.width, eff.height};
            if (applied.anchor_edges & edge_left)
                rec->geometry.x = applied.anchor.x + applied.anchor.width - eff.width;
            if (applied.anchor_edges & edge_top)
                rec->geometry.y = applied.anchor.y + applied.anchor.height - eff.height;
            placed = true;
            // Drag-to-restore's configure is answered: the move goes on with the window itself.
            if (grab_.awaiting_restore && grab_.toplevel == id &&
                !(applied.states & (tl_maximized | tl_fullscreen | tl_tiled_all))) {
                grab_.awaiting_restore = false;
                grab_.motion_pending = true;
            }
        } else {
            rec->geometry = anchored_geometry(rec->geometry, applied, {eff.width, eff.height});
        }
        applied_serial = applied.serial;
        mode = applied.decoration;
        // The state the client now draws for: normal, maximized or fullscreen.
        const lx::toplevel_state st = (applied.states & tl_fullscreen)
                                          ? lx::toplevel_state::fullscreen
                                          : (applied.states & tl_maximized)
                                                ? lx::toplevel_state::maximized
                                                : lx::toplevel_state::normal;
        if (rec->state != st && (rec->state == lx::toplevel_state::normal ||
                                 rec->state == lx::toplevel_state::maximized ||
                                 rec->state == lx::toplevel_state::fullscreen)) {
            if (rec->state == lx::toplevel_state::fullscreen ||
                st == lx::toplevel_state::fullscreen)
                fullscreen_changed = true;
            rec->state = st;
            state_changed = true;
        }
    } else {
        rec->geometry.width = eff.width;
        rec->geometry.height = eff.height;
    }
    // The decoration object went away: "no server-side decoration at the next commit" —
    // and at every commit after it until a new object is created. Server-side mode only
    // exists through a decoration object, so a configure that carried `server` and is
    // acked after the object's destruction (a resize keeps several in flight) must not
    // turn the frame back on around a client that now draws its own.
    if (!rec->deco_resource)
        mode = decoration_mode::client;
    // The mode answering a configure takes effect with the content drawn for it: frame and
    // client buffer reach the scene in this same UI call, so the same snapshot.
    const bool mode_changed = mode != rec->decoration_current;
    rec->decoration_current = mode;
    // Logged once the window is where this commit puts it (a first map places it).
    const auto log_apply = [&](const toplevel_record& r) {
        if (!mode_changed || !log_deco.enabled(lx::trace::level::debug))
            return;
        const lx::rect2i f = frame_rect(r.geometry, extents_of(r));
        log_deco.debug("decoration apply id={} mode={} frame={},{},{}x{}", r.id.id(),
                       r.decoration_current == decoration_mode::server ? "server" : "client", f.x,
                       f.y, f.width, f.height);
    };

    if (!rec->mapped) {
        if (info.has_buffer && !rect_empty(eff)) {
            map(*rec, placed);
            if (auto* r = toplevels_->find_mut(id)) {
                log_apply(*r);
                // Mapped fullscreen (asked before its first buffer): straight into the
                // fullscreen layer.
                if (r->mapped && r->state == lx::toplevel_state::fullscreen)
                    sync_fullscreen(*r, false);
            }
        }
        return;
    }

    if (!info.has_buffer) {
        on_unmap(id, "null-buffer");
        // xdg-shell: the next commit is a new initial commit.
        if (auto* again = toplevels_->find_mut(id))
            toplevel_manager::reset_for_remap(*again);
        return;
    }

    // The whole tree follows in this same UI call (and so the same snapshot): every node is
    // moved here, and the caller syncs the tree's scene right after. Damage per node, old
    // and new rects apart; a size change was damaged where the content landed.
    tree_->set_root_origin(rec->surface, {rec->geometry.x - rec->geometry_offset.x,
                                          rec->geometry.y - rec->geometry_offset.y});
    if (fullscreen_changed)
        sync_fullscreen(*rec, was_fullscreen);
    else if (rec->state == lx::toplevel_state::fullscreen && !rect_equal(previous, rec->geometry))
        sync_backdrops();
    sync_decoration(*rec);
    log_apply(*rec);
    if (state_changed && shell_)
        shell_->emit_toplevel_state(*rec);
    if (!rect_equal(previous, rec->geometry)) {
        if (shell_)
            shell_->emit_toplevel_geometry(*rec);
        log_geometry(*rec, "commit", applied_serial);
        // Popups are placed relative to the window geometry: an anchored resize moves its
        // origin, and reactive popups are placed again for the new size.
        if (popups_)
            popups_->on_parent_moved(rec->surface);
    }
}

void lx::compositor::window_manager::map(toplevel_record& rec, bool placed) {
    // Placed on its placement output (M10), cascading among the windows there.
    if (rec.output < 0)
        rec.output = placement_output(rec);
    const lx::rect2i area = usable_area(rec.output);

    // Stack array, not a heap one: mapping is rare, but it must not allocate either.
    // Placement is of the whole frame — a server-side title bar must land on the output too,
    // and the cascade compares frames, which is what the user sees.
    lx::rect2i existing[toplevel_manager::k_max];
    unsigned n = 0;
    toplevels_->for_each_bottom_up([&](const toplevel_record& other) {
        // What the user sees: windows of other workspaces do not push the cascade.
        if (n < toplevel_manager::k_max && !other.hidden)
            existing[n++] = frame_rect(other.geometry, extents_of(other));
    });

    const frame_extents ext = extents_of(rec);
    if (!placed) {
        const lx::rect2i frame0 = frame_rect(rec.geometry, ext);
        // A child (a dialog, a transient) is centred on its parent's frame, clamped into the
        // usable area of the output it maps on (the parent's): Mutter and KWin place
        // transients so. Anything else cascades.
        const toplevel_record* parent = rec.parent ? toplevels_->find(rec.parent) : nullptr;
        lx::point2i p{};
        if (parent && parent->mapped) {
            const lx::rect2i pf = frame_rect(parent->geometry, extents_of(*parent));
            p = {pf.x + (pf.width - frame0.width) / 2, pf.y + (pf.height - frame0.height) / 2};
        } else {
            p = place_new_window(area, {frame0.width, frame0.height}, existing, n);
        }
        p = constrain_on_output(area, {p.x, p.y, frame0.width, frame0.height});
        rec.geometry.x = p.x + ext.left;
        rec.geometry.y = p.y + ext.top;
    }

    toplevels_->set_mapped(rec.id, true);
    // A new window of a client already known not to answer pings is shown as such too.
    rec.not_responding = liveness_ && liveness_->not_responding(rec.client);
    tree_->set_root_dim(rec.surface, rec.not_responding);
    // The tree container goes on top of the window layer, matching the stacking order the
    // record was just pushed onto; mapping it creates (and damages) every node.
    tree_->set_root_tag(rec.surface, rec.id.id());
    tree_->set_root_origin(rec.surface, {rec.geometry.x - rec.geometry_offset.x,
                                         rec.geometry.y - rec.geometry_offset.y});
    if (!tree_->map_root(rec.surface, *windows_layer_, ~0u) || !surfaces_->node(rec.surface)) {
        tree_->unmap_root(rec.surface);
        toplevels_->set_mapped(rec.id, false);
        log_wm.warn("map failed: no scene node for surface");
        return;
    }

    {
        log_wm.info("map id={} app_id={} x={} y={} w={} h={}", rec.id.id(),
                    rec.app_id ? rec.app_id : "", rec.geometry.x, rec.geometry.y,
                    rec.geometry.width, rec.geometry.height);
    }
    log_geometry(rec, "map", 0);
    sync_decoration(rec);

    // Its workspace (W.1): a child's parent's, else the active one of the output it maps
    // on. A child of a window on another workspace maps there, hidden.
    {
        sync_workspace_outputs(true);
        lx::workspace_id ws{};
        const toplevel_record* parent = rec.parent ? toplevels_->find(rec.parent) : nullptr;
        if (parent && parent->mapped && parent->workspace)
            ws = parent->workspace;
        else if (const int slot = window_output(rec); slot >= 0)
            ws = workspaces_.active(static_cast<unsigned>(slot));
        rec.workspace_dirty = true;
        ws_dirty_ = true;
        if (ws) {
            rec.workspace = ws;
            workspaces_.window_added(ws);
        }
        if (!shown(rec))
            apply_hidden(rec, true);
        tiling_dirty_ = true;
    }

    const lx::toplevel_id id = rec.id;
    // An activation requested before the map is decided now; a refused one marks the window
    // (before the shell hears of it, so toplevel_created carries the flag). A window that
    // maps hidden (its parent's workspace is not shown) does not take the keyboard.
    bool take_focus = decide_activation_at_map(rec);
    if (take_focus && rec.hidden) {
        take_focus = false;
        set_demands_attention(rec, true);
    }
    if (shell_)
        shell_->emit_toplevel_created(rec);
    // Children mapped before it now have a mapped parent: the shell hears of the relation,
    // and a modal one starts blocking.
    emit_children_state(id);
    if (take_focus) {
        // Placeholder policy until the shell drives focus: a newly mapped window takes the
        // keyboard. The pointer is a different matter — it enters the window only if the
        // window appeared under it, which the hit test at the next tick decides.
        set_keyboard_focus(id);
    } else if (const unsigned below = toplevels_->stacking_index(keyboard_focus_);
               below != ~0u) {
        // Refused focus: it opens just below the window that has the keyboard (Mutter,
        // KWin), not on top of what the user is working in.
        (void)restack(id, below);
    }
    // A modal dialog stays above its parent and has the keyboard whenever the parent would
    // (also when focus-stealing prevention refused it the keyboard as a new window).
    if (const auto* now = toplevels_->find(id); now && effective_modal(*toplevels_, *now))
        keep_modals_above(now->parent);
    if (const lx::toplevel_id b = modal_blocker(keyboard_focus_); b)
        set_keyboard_focus(b);
    rehit_pending_ = true;
    stacking_dirty_ = true;
    flush_workspaces();
}

void lx::compositor::window_manager::on_unmap(lx::toplevel_id id, const char* reason) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return;
    auto* rec = toplevels_->find_mut(id);
    if (!rec || !rec->mapped)
        return;

    // Client gone or window withdrawn mid-grab: the grab ends with it, and nothing of it is
    // applied to a record that is going away.
    if (grab_.mode != grab_mode::none && grab_.toplevel == id) {
        end_grab(reason && std::strcmp(reason, "destroyed") == 0 ? grab_end_reason::destroyed
                                                                 : grab_end_reason::unmapped);
        rec = toplevels_->find_mut(id);
        if (!rec)
            return;
    }

    // Its popups go first (popup_done, top-down): none may stay on screen without it.
    if (popups_)
        popups_->on_parent_unmapped(rec->surface, "parent-unmapped");
    // Every node of the window goes (each damaged); the unmap hook moves pointer focus off
    // any of them that had it.
    tree_->unmap_root(rec->surface);
    toplevels_->set_mapped(id, false);
    // Off its workspace (dynamic: an inactive workspace left empty goes); a remap joins
    // one again.
    leave_workspace(*rec);
    rec->workspace_dirty = false;
    workspaces_.forget_window(id);
    // Out of its layout (the others close the gap); nothing is sent to a window that went.
    rec->tiled = false;
    rec->tile_workspace = {};
    tiling_dirty_ = true;
    if (rec->hidden) {
        rec->hidden = false;
        if (auto* box = tree_->container(rec->surface))
            box->set_visible(true);
    }
    if (rec->state == lx::toplevel_state::fullscreen)
        sync_backdrops();
    // The shell forgets the window (toplevel_destroyed below): its attention goes with it.
    rec->demands_attention = false;
    sync_decoration(*rec); // not mapped: the frame goes (damaged) with the window
    if (deco_press_.id == id) {
        deco_press_ = {};
        if (decorations_)
            decorations_->set_button(id, deco_button::normal);
    }
    if (shell_)
        shell_->emit_toplevel_destroyed(*rec);
    // xdg-shell: "If the parent is unmapped then its children are managed as though the
    // parent of the now-unmapped parent has become the parent of this surface" (wlroots
    // reparents for good, as here). A modal child of it blocks nothing now.
    {
        const lx::toplevel_id grandparent = rec->parent;
        toplevels_->for_each_mut([&](toplevel_record& child) {
            if (child.parent != id)
                return;
            child.parent = grandparent;
            if (shell_ && child.mapped) {
                shell_->emit_toplevel_parent(child);
                if (child.modal)
                    shell_->emit_toplevel_state(child);
            }
        });
        rec = toplevels_->find_mut(id);
        if (!rec)
            return;
    }

    {
        log_wm.info("unmap id={} reason={}", id.id(), reason ? reason : "-");
    }

    // Focus falls to whatever is now on top — the stacking order is the only history kept.
    // With nothing left that is the null id, and the change must still go through
    // set_keyboard_focus: the router sends `leave` and the shell hears focus_changed(null),
    // or keys typed at an empty desktop keep reaching the hidden surface.
    // Shown windows only: the next window of the output it was on, else any.
    if (keyboard_focus_ == id) {
        lx::toplevel_id next = top_shown(window_output(*rec));
        if (!next)
            next = top_shown(-1);
        set_keyboard_focus(next);
    }
    stacking_dirty_ = true;
    flush_workspaces();
    if (pointer_focus_ == id) {
        // Rebase now, not at the tick: focus must not name an unmapped window for even one
        // more event. With a button held the implicit grab's target is gone, so nothing
        // has the pointer until release (the window beneath gets no `enter` mid-press).
        if (button_mask_ != 0)
            set_pointer_focus(nullptr);
        else
            refresh_pointer_focus(0, false);
    } else {
        rehit_pending_ = true;
    }
}

void lx::compositor::window_manager::on_destroyed(lx::toplevel_id id) {
    if (!toplevels_ || !id)
        return;
    if (const auto* rec = toplevels_->find(id); rec && rec->mapped)
        on_unmap(id, "destroyed");
    if (const auto* rec = toplevels_->find(id); rec && popups_)
        popups_->on_parent_destroyed(rec->surface);
    if (const auto* rec = toplevels_->find(id); rec && decorations_)
        decorations_->on_destroyed(id, rec->surface);
    if (deco_press_.id == id)
        deco_press_ = {};
    if (last_title_click_.id == id)
        last_title_click_ = {};
    if (deco_active_ == id)
        deco_active_ = {};
    if (activated_ == id)
        activated_ = {};
    if (keyboard_focus_ == id)
        keyboard_focus_ = {};
    if (pointer_focus_ == id) {
        pointer_focus_ = {};
        pointer_surface_ = {};
        if (pointer_zone_ != deco_zone::none) {
            pointer_zone_ = deco_zone::none;
            if (cursor_)
                cursor_->clear_hover_shape();
        }
    }
    if (ops_.toplevel_destroyed)
        ops_.toplevel_destroyed(ops_.user, id);
    if (const auto* rec = toplevels_->find(id); rec && rec->tile_workspace)
        tiling_dirty_ = true;
    toplevels_->destroy(id);
}

void lx::compositor::window_manager::on_tick(lx::runtime::clock_time now_ns) {
    lx::trace::scoped_span span{"compositor.wm", "on_tick"};
    // A node appeared, went, moved or changed size since the last tick.
    if (surfaces_ && surfaces_->consume_layout_changed())
        rehit_pending_ = true;
    if (rehit_pending_) {
        rehit_pending_ = false;
        // With a button held the implicit grab owns focus; release re-hit-tests anyway. A
        // drag's target is re-hit instead: a window mapped or moved under a still pointer
        // becomes the target without waiting for motion.
        if (drag_active())
            drag_hit(static_cast<unsigned>(now_ns / 1'000'000), false);
        else if (button_mask_ == 0)
            refresh_pointer_focus(static_cast<unsigned>(now_ns / 1'000'000), true);
    }
    if (stacking_dirty_) {
        stacking_dirty_ = false;
        emit_stacking_order();
    }
    // Once per tick however many motion events arrived: the window lands in the same
    // snapshot (and, with the cursor plane, the same atomic commit) as the pointer.
    if (grab_.mode != grab_mode::none && grab_.motion_pending)
        apply_grab();
    // The drag icon likewise: once per tick, in the snapshot that carries the pointer.
    if (dnd_)
        dnd_->on_tick();
    // A change no operation flushed (an area, an output): laid out before the configures go.
    flush_tiling();
    flush_configures(now_ns);
    // O(1) unless a ping deadline passed; the focused client is pinged every 10 s.
    if (liveness_) {
        const auto* focused = toplevels_ ? toplevels_->find(keyboard_focus_) : nullptr;
        liveness_->tick(now_ns, focused ? focused->client : lx::client_id{});
    }
}

bool lx::compositor::window_manager::needs_tick() const {
    if (rehit_pending_ || stacking_dirty_ || tiling_dirty_ ||
        (grab_.mode != grab_mode::none && grab_.motion_pending))
        return true;
    if (surfaces_ && surfaces_->layout_changed())
        return true;
    bool waiting = false;
    if (toplevels_) {
        toplevels_->for_each([&](const toplevel_record& rec) {
            if (rec.initial_configure_sent && rec.inflight &&
                (!rec.desired.same_request(rec.last_sent) || rec.deco_configure_due ||
                 rec.configure_due))
                waiting = true;
        });
    }
    return waiting;
}

void lx::compositor::window_manager::flush_configures(lx::runtime::clock_time now_ns) {
    if (!toplevels_)
        return;
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!rec.initial_configure_sent || !toplevel_manager::want_send(rec, now_ns))
            return;
        const bool watchdog = toplevel_manager::timed_out(rec, now_ns);
        if (watchdog && log_wm.enabled(lx::trace::level::debug)) {
            log_wm.debug("configure watchdog id={} serial={}", rec.id.id(), rec.last_sent.serial);
        }
        send_configure(rec, now_ns, false, watchdog);
        // A configure unanswered past the watchdog is the strongest hint of a hung client
        // there is: ask it.
        if (watchdog && liveness_)
            liveness_->request(rec.client, now_ns, ping_reason::watchdog);
    });
}

void lx::compositor::window_manager::set_keyboard_focus(lx::toplevel_id id) {
    if (!toplevels_)
        return;
    // xdg-dialog-v1: a window with a modal dialog never takes the keyboard — the dialog does
    // (activation, the shell's request_activate, a click, a window mapping).
    if (const lx::toplevel_id b = modal_blocker(id); b) {
        if (log_wm.enabled(lx::trace::level::debug)) {
            log_wm.debug("focus redirect id={} modal={}", id.id(), b.id());
        }
        id = b;
    }
    if (id == keyboard_focus_)
        return;
    // A keyboard move or resize belongs to the focused window: it ends where it is when the
    // keyboard goes elsewhere.
    if (grab_.mode != grab_mode::none && grab_.keyboard && !(grab_.toplevel == id))
        end_grab(grab_end_reason::release);
    // Activating another client's window (a new window mapping, the shell's
    // request_activate) ends a popup grab: its menu must not keep the keyboard of a window
    // that is no longer active. A click there already ended it as an outside click.
    if (popups_ && popups_->grab_active()) {
        const auto* next = toplevels_->find(id);
        if (!next || !(next->client == popups_->grab_client()))
            popups_->dismiss_all("focus-change");
    }
    keyboard_focus_ = id;
    auto* rec = toplevels_->find_mut(id);
    // Its workspace remembers it: switching back focuses it again (W.1).
    if (rec && rec->workspace)
        workspaces_.set_focus_memory(rec->workspace, id);
    // The window the user wanted is the one they now have.
    if (rec && rec->demands_attention)
        set_demands_attention(*rec, false);
    // Activation follows the surface that actually gets the keys (resolve_keyboard_focus).
    resolve_keyboard_focus();
    // Mutter pings on activation: a window given focus that never answers is the one the
    // user is about to wait on.
    if (rec && liveness_)
        liveness_->request(rec->client,
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           ping_reason::activate);
    if (shell_)
        shell_->emit_focus_changed(rec);
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("focus keyboard id={}", id.id());
    }
}

void lx::compositor::window_manager::resolve_keyboard_focus() {
    if (!toplevels_)
        return;
    void* target = nullptr;
    const char* kind = "none";
    lx::surface_id surface{};
    lx::toplevel_id owner{};
    // "For the top and overlay layers, the seat will always give exclusive keyboard focus to
    // the top-most layer which has keyboard interactivity set to exclusive."
    lx::surface_id exclusive{};
    // Locked: the lock surface with the keyboard, or nothing — no layer surface, popup or
    // window, and no window is activated meanwhile.
    if (locked_) {
        if (lock_ops_.keyboard)
            target = lock_ops_.keyboard(lock_ops_.user, surface);
        kind = target ? "lock" : "none";
    } else if (layers_) {
        if (const layer_record* l = layers_->exclusive_focus()) {
            target = l->surface_res;
            kind = "layer";
            surface = l->surface;
            exclusive = l->surface;
        }
    }
    // A popup grab gives the keyboard to its topmost mapped popup; its toplevel stays
    // activated meanwhile (the window decision is unchanged), which is what GTK expects. A
    // layer surface's popups inherit its interactivity (`none`: pointer grab only), and only
    // an exclusive surface's own popups can take the keyboard from it.
    if (popups_ && !locked_) {
        if (const popup_record* p = popups_->keyboard_target()) {
            const lx::surface_id root_layer = popups_->root_layer(*p);
            const bool allowed = root_layer
                                     ? layers_ && layers_->focusable(root_layer) &&
                                           (!exclusive || root_layer == exclusive)
                                     : !exclusive;
            if (allowed) {
                target = p->surface_res;
                kind = "popup";
                surface = p->surface;
                owner = p->owner;
            }
        }
    }
    // A clicked on_demand layer surface, until a click elsewhere.
    if (!target && !locked_ && layer_focus_ && layers_ && layers_->focusable(layer_focus_)) {
        if (const layer_record* l = layers_->find(layer_focus_)) {
            target = l->surface_res;
            kind = "layer";
            surface = l->surface;
        }
    }
    if (!target && !locked_) {
        if (const auto* rec = toplevels_->find(keyboard_focus_)) {
            target = rec->surface_resource;
            kind = "toplevel";
            surface = rec->surface;
            owner = rec->id;
        }
    }
    // The window owning the keys is the activated one; while a layer surface has them, none
    // is (sway). The configure carrying it goes out with the next flush.
    if (owner != activated_) {
        if (auto* old = toplevels_->find_mut(activated_))
            old->desired.states &= ~static_cast<unsigned>(tl_activated);
        if (auto* now = toplevels_->find_mut(owner))
            now->desired.states |= tl_activated;
        activated_ = owner;
    }
    // The frame of the window that owns the keyboard is drawn active, at once (KWin): not
    // tied to a configure, since the frame is the compositor's own.
    if (owner != deco_active_) {
        deco_active_ = owner;
        if (decorations_)
            decorations_->set_active(owner);
    }
    // Compared with what the router will deliver, not what it delivers now: while the
    // keyboard is suspended (VT away) the focus is null, and a lock deciding "nobody" then
    // must still replace the surface the resume would give the keys to.
    if (!router_ || !seat_ || router_->chosen_keyboard_focus() == target)
        return;
    router_->set_keyboard_focus(target, *seat_);
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("focus keyboard-surface kind={} surface={}", kind, surface.id());
    }
}

// ── Pointer focus and hit-testing ──────────────────────────────────────────

bool lx::compositor::window_manager::pointer_hit(double x, double y, wm_hit& out) const {
    if (!hit_test(x, y, out))
        return false;
    // Owner events: during a popup grab only the grabbing client's surfaces get the pointer
    // (not its frame either: that is the compositor's).
    // An input method's popup serves the text input inside the grabbing client's menu: it
    // stays reachable (its candidates are clicked).
    return !(popups_ && popups_->grab_active() && out.owner != hit_owner::ime_popup &&
             (out.target != hit_target::surface || !(out.client == popups_->grab_client())));
}

bool lx::compositor::window_manager::hit_test(double x, double y, wm_hit& out) const {
    if (!toplevels_ || !surfaces_ || !tree_)
        return false;
    // Input-method popups are above everything (only ever shown for the focused text input,
    // which while locked is a lock surface's).
    if (ime_ops_.hit && ime_ops_.hit(ime_ops_.user, x, y, out))
        return true;
    // Locked: lock surfaces only. Nothing else is drawn, and nothing else may be reached.
    if (locked_)
        return lock_ops_.hit && lock_ops_.hit(lock_ops_.user, x, y, out);
    // Popups sit above every window (the popup layer), newest on top.
    if (popups_ && popups_->mapped_count() > 0) {
        tree_hit th{};
        const popup_record* pr = nullptr;
        if (popups_->hit(x, y, th, pr)) {
            out = {};
            out.id = pr->owner;
            out.surface_resource = surfaces_->resource(th.surface);
            out.sx = th.sx;
            out.sy = th.sy;
            out.surface = th.surface;
            out.root = pr->surface;
            out.owner = hit_owner::popup;
            out.client = pr->client;
            return out.surface_resource != nullptr;
        }
    }
    const auto hit_layer = [&](layer_level level) {
        if (!layers_ || layers_->mapped_count() == 0)
            return false;
        tree_hit th{};
        const layer_record* lr = nullptr;
        if (!layers_->hit(level, x, y, th, lr))
            return false;
        out = {};
        out.surface_resource = surfaces_->resource(th.surface);
        out.sx = th.sx;
        out.sy = th.sy;
        out.surface = th.surface;
        out.root = lr->surface;
        out.owner = hit_owner::layer;
        out.client = lr->client;
        return out.surface_resource != nullptr;
    };
    if (hit_layer(layer_level::overlay))
        return true;
    // A fullscreen window covers its whole output above the top layer (its backdrop takes
    // what the window leaves): only it is reachable there (M11).
    if (fullscreen_layer_) {
        bool covered = false;
        bool hit = false;
        toplevels_->for_each_top_down([&](const toplevel_record& rec) {
            if (rec.hidden || rec.state != lx::toplevel_state::fullscreen ||
                rec.fullscreen_output < 0)
                return true;
            const lx::rect2i area = output_area(rec.fullscreen_output);
            if (x < area.x || y < area.y || x >= area.x + area.width || y >= area.y + area.height)
                return true;
            covered = true;
            tree_hit th{};
            if (tree_->hit(rec.surface, x, y, th)) {
                out = {};
                out.id = rec.id;
                out.surface_resource =
                    th.surface == rec.surface ? rec.surface_resource : surfaces_->resource(th.surface);
                out.sx = th.sx;
                out.sy = th.sy;
                out.surface = th.surface;
                out.root = rec.surface;
                out.client = rec.client;
                hit = out.surface_resource != nullptr;
            }
            return false;
        });
        if (covered)
            return hit;
    }
    if (hit_layer(layer_level::top))
        return true;
    bool found = false;
    // Surface extents ∩ input region, not window geometry: a CSD client's resize margins
    // lie outside its geometry and must still receive the press (wlroots, Smithay alike).
    // Every surface of the tree counts, each clipped to its own extents — a subsurface is
    // not clipped to its parent.
    toplevels_->for_each_top_down([&](const toplevel_record& rec) {
        // Another workspace's, or minimized: not there for input.
        if (rec.hidden)
            return true;
        tree_hit th{};
        if (!tree_->hit(rec.surface, x, y, th)) {
            // Not on a surface of this window: perhaps on its frame. The client is on top
            // inside the window, so the frame's bands only ever catch what it leaves.
            if (rec.decoration_current != decoration_mode::server || !decorations_)
                return true; // keep looking further down
            const deco_zone zone = deco_hit(rec.geometry, extents_of(rec), x, y);
            if (zone == deco_zone::none)
                return true;
            out = {};
            out.id = rec.id;
            out.root = rec.surface;
            out.client = rec.client;
            out.target = hit_target::decoration;
            out.zone = zone;
            found = true;
            return false;
        }
        out = {};
        out.id = rec.id;
        out.surface_resource =
            th.surface == rec.surface ? rec.surface_resource : surfaces_->resource(th.surface);
        out.sx = th.sx;
        out.sy = th.sy;
        out.surface = th.surface;
        out.root = rec.surface;
        out.client = rec.client;
        found = out.surface_resource != nullptr;
        return !found;
    });
    if (found)
        return true;
    return hit_layer(layer_level::bottom) || hit_layer(layer_level::background);
}

void lx::compositor::window_manager::set_decoration_hover(lx::toplevel_id id, deco_zone zone) {
    const lx::toplevel_id old_id = pointer_zone_ != deco_zone::none ? pointer_focus_
                                                                    : lx::toplevel_id{};
    if (decorations_) {
        if (old_id && !(old_id == id && zone != deco_zone::none))
            decorations_->set_button(old_id, deco_button::normal);
        if (zone != deco_zone::none)
            decorations_->set_button(id, zone == deco_zone::close ? deco_button::hover
                                                                  : deco_button::normal);
    }
    if (cursor_) {
        if (zone != deco_zone::none)
            cursor_->set_hover_shape(cursor_for_zone(zone));
        else if (pointer_zone_ != deco_zone::none)
            cursor_->clear_hover_shape();
    }
    pointer_zone_ = zone;
}

bool lx::compositor::window_manager::same_pointer_focus(const wm_hit& h) const {
    if (h.target == hit_target::decoration)
        return h.id == pointer_focus_ && h.zone == pointer_zone_ && !router_->pointer_focus();
    return pointer_zone_ == deco_zone::none && h.id == pointer_focus_ &&
           router_->pointer_focus() == h.surface_resource;
}

void lx::compositor::window_manager::set_pointer_focus(const wm_hit* hit) {
    const bool on_frame = hit && hit->target == hit_target::decoration;
    set_decoration_hover(on_frame ? hit->id : lx::toplevel_id{},
                         on_frame ? hit->zone : deco_zone::none);
    pointer_focus_ = hit ? hit->id : lx::toplevel_id{};
    pointer_surface_ = hit && !on_frame ? hit->surface : lx::surface_id{};
    pointer_owner_ = hit ? hit->owner : hit_owner::toplevel;
    pointer_root_ = hit && !on_frame ? hit->root : lx::surface_id{};
    if (router_ && seat_) {
        // On the frame no client has the pointer: the one it came from gets `leave`.
        if (hit && !on_frame)
            (void)router_->set_pointer_focus(hit->surface_resource, hit->sx, hit->sy, *seat_);
        else
            (void)router_->set_pointer_focus(nullptr, 0.0, 0.0, *seat_);
    }
    if (!log_wm.enabled(lx::trace::level::debug))
        return;
    constexpr auto lvl = lx::trace::level::debug;
    if (on_frame)
        // Deliberately not `focus pointer id=…`: no client has the pointer.
        log_wm.log(lvl, "focus pointer decoration id={} zone={}", hit->id.id(),
                   deco_zone_name(hit->zone));
    else if (hit && hit->owner == hit_owner::popup)
        // Deliberately not `focus pointer id=…`: the drag smoke counts those as windows.
        log_wm.log(lvl, "focus pointer popup surface={} sx={:.1f} sy={:.1f}", hit->surface.id(),
                   hit->sx, hit->sy);
    else if (hit && hit->owner == hit_owner::layer)
        log_wm.log(lvl, "focus pointer layer surface={} sx={:.1f} sy={:.1f}", hit->surface.id(),
                   hit->sx, hit->sy);
    else if (hit && hit->owner == hit_owner::ime_popup)
        log_wm.log(lvl, "focus pointer ime-popup surface={} sx={:.1f} sy={:.1f}", hit->surface.id(),
                   hit->sx, hit->sy);
    else if (hit)
        log_wm.log(lvl, "focus pointer id={} sx={:.1f} sy={:.1f}", hit->id.id(), hit->sx, hit->sy);
    else
        log_wm.log(lvl, "focus pointer none");
    if (hit && !on_frame && hit->owner == hit_owner::toplevel && !(hit->surface == hit->root))
        log_wm.log(lvl, "focus pointer-surface surface={} root={}", hit->surface.id(),
                   hit->root.id());
}

void lx::compositor::window_manager::on_surface_unmapped(lx::surface_id surface) {
    // A drag target that stops being drawn is left at once (the next motion finds another).
    if (dnd_)
        dnd_->on_surface_unmapped(surface);
    if (!surface || !(surface == pointer_surface_))
        return;
    // As for a focused window unmapping: focus must not name a surface that is not drawn
    // for even one more event.
    if (button_mask_ != 0)
        set_pointer_focus(nullptr);
    else
        refresh_pointer_focus(0, false);
}

void lx::compositor::window_manager::refresh_pointer_focus(unsigned time_ms,
                                                           bool motion_if_same) {
    if (!seat_ || !router_)
        return;
    wm_hit h{};
    const bool any = pointer_hit(seat_->pointer_x(), seat_->pointer_y(), h);
    if (!any) {
        if (pointer_focus_ || router_->pointer_focus())
            set_pointer_focus(nullptr);
        return;
    }
    if (!same_pointer_focus(h)) {
        set_pointer_focus(&h);
        return;
    }
    if (motion_if_same && h.target == hit_target::surface)
        router_->send_pointer_motion(h.sx, h.sy, time_ms);
}

void lx::compositor::window_manager::on_pointer_motion(unsigned time_ms) {
    if (!seat_ || !router_ || !surfaces_ || !toplevels_)
        return;
    const double px = seat_->pointer_x();
    const double py = seat_->pointer_y();

    // A drag: one hit test (the cost of any motion), then `wl_data_device.motion` to the
    // target or a focus change. No client has pointer focus meanwhile.
    if (drag_active()) {
        drag_hit(time_ms, true);
        return;
    }

    if (grab_.mode != grab_mode::none) {
        // O(1): the grab is applied at the tick. No client hears the pointer meanwhile.
        grab_.now_x = px;
        grab_.now_y = py;
        grab_.motion_pending = true;
        return;
    }

    if (button_mask_ == 0) {
        wm_hit h{};
        const bool any = pointer_hit(px, py, h);
        if (!any) {
            if (pointer_focus_ || router_->pointer_focus())
                set_pointer_focus(nullptr);
            return;
        }
        if (!same_pointer_focus(h)) {
            // `enter` carries the position; a motion with the same one would be noise.
            set_pointer_focus(&h);
            // Focus follows the mouse: into a window (never out of one onto the desktop,
            // never from a popup's or a layer surface's motion), only on real motion — a
            // window mapping or moving under a still pointer takes nothing.
            if (focus_.sloppy && !locked_ && h.id && h.owner == hit_owner::toplevel &&
                !(popups_ && popups_->grab_active()) && !(h.id == keyboard_focus_)) {
                const lx::toplevel_id b = modal_blocker(h.id);
                set_keyboard_focus(b ? b : h.id);
                if (focus_.raise_on_sloppy)
                    (void)raise(h.id);
            }
            return;
        }
        if (h.target == hit_target::surface)
            router_->send_pointer_motion(h.sx, h.sy, time_ms);
        return;
    }

    // A close-button press shows pressed only while the pointer is still over the button —
    // releasing elsewhere cancels, and the look says so beforehand.
    if (deco_press_.id && decorations_) {
        const auto* rec = toplevels_->find(deco_press_.id);
        const bool over = rec && in_close(*rec, px, py);
        decorations_->set_button(deco_press_.id, over ? deco_button::pressed : deco_button::normal);
        return;
    }

    // Implicit grab: the surface that took the press keeps every motion until release,
    // wherever the pointer goes — coordinates outside its extents included, which is how
    // a drag-select keeps scrolling past the window edge. In that surface's coordinates: a
    // subsurface that took the press (a CSD title bar) is not the window's main surface.
    if (!pointer_surface_ || !router_->pointer_focus() || !surfaces_->node(pointer_surface_))
        return;
    const lx::rect2i n = surfaces_->node_rect(pointer_surface_);
    router_->send_pointer_motion(px - n.x, py - n.y, time_ms);
}

void lx::compositor::window_manager::on_pointer_button(
    const lx::input::pointer_button_event& event) {
    if (!seat_ || !router_)
        return;
    const unsigned offset = event.evdev_code - k_button_base;
    const std::uint64_t bit =
        event.evdev_code >= k_button_base && offset < 64 ? (std::uint64_t{1} << offset) : 0;

    // A drag: no button reaches a client. Every press is withheld (and so is its release);
    // the release that lets go of the last button drops or cancels, then the pointer goes
    // back to whatever is under it with a normal `enter`.
    if (drag_active()) {
        if (!bit)
            return;
        if (event.pressed) {
            if (!(button_mask_ & bit)) {
                button_mask_ |= bit;
                swallowed_mask_ |= bit;
            }
            return;
        }
        if (!(button_mask_ & bit))
            return;
        button_mask_ &= ~bit;
        swallowed_mask_ &= ~bit;
        if (button_mask_ == 0) {
            dnd_->on_released();
            refresh_pointer_focus(event.time_ms, false);
        }
        return;
    }

    if (event.pressed) {
        // A second press of a held button (two mice on one seat) would desynchronise the
        // client's button state; libinput's seat count is per device, so filter here.
        if (bit && (button_mask_ & bit))
            return;
        const bool first = button_mask_ == 0;
        button_mask_ |= bit;
        // Nothing reaches a client during a grab, and an extra button does not end it — but
        // any press ends a keyboard move or resize where it is (K.6).
        if (grab_.mode != grab_mode::none) {
            swallowed_mask_ |= bit;
            if (grab_.keyboard)
                end_grab(grab_end_reason::release);
            return;
        }
        // Locked: no binding, frame, layer or window focus — the press goes to the lock
        // surface under the pointer (which takes the keyboard), or to nobody.
        if (locked_) {
            if (first)
                refresh_pointer_focus(event.time_ms, false);
            if (pointer_owner_ == hit_owner::lock && pointer_root_ && router_->pointer_focus() &&
                lock_ops_.clicked)
                lock_ops_.clicked(lock_ops_.user, pointer_root_);
            if (const unsigned serial = router_->send_pointer_button(event, *seat_); serial) {
                last_press_serial_ = serial;
                last_press_client_ = router_->pointer_focus_client();
                last_press_button_ = event.evdev_code;
            }
            return;
        }
        // A popup grab ends with a press outside the grabbing client's surfaces. The press
        // is consumed (wlroots and Mutter alike): the menu closes and nothing under it is
        // clicked. Checked before the Super binding and click-to-focus.
        if (popups_ && popups_->grab_active()) {
            wm_hit h{};
            const bool inside = hit_test(seat_->pointer_x(), seat_->pointer_y(), h) &&
                                h.target == hit_target::surface &&
                                (h.client == popups_->grab_client() ||
                                 h.owner == hit_owner::ime_popup);
            if (!inside) {
                swallowed_mask_ |= bit;
                popups_->dismiss_all("outside-click");
                return;
            }
        }
        // Configured pointer bindings (config.toml: Super+BTN_LEFT moves, Super+BTN_RIGHT
        // resizes from the edges nearest the grab point, by default). Neither press nor
        // release reaches the client.
        const wm_button_binding bound =
            bit && button_binding_ ? button_binding_(button_binding_user_, event.evdev_code,
                                                     event.time_ms)
                                   : wm_button_binding::none;
        if (bound == wm_button_binding::consumed) {
            swallowed_mask_ |= bit;
            return;
        }
        if (bound == wm_button_binding::move || bound == wm_button_binding::resize) {
            wm_hit h{};
            if (hit_test(seat_->pointer_x(), seat_->pointer_y(), h) && h.id) {
                swallowed_mask_ |= bit;
                set_keyboard_focus(h.id);
                (void)raise(h.id);
                const bool move = bound == wm_button_binding::move;
                unsigned edges = 0;
                // The thirds of what the user sees: the frame, title bar included.
                if (const auto* rec = toplevels_->find(h.id); rec && !move)
                    edges = edges_for_point(frame_rect(rec->geometry, extents_of(*rec)),
                                            seat_->pointer_x(), seat_->pointer_y());
                (void)begin_grab(h.id, move ? grab_mode::move : grab_mode::resize, edges,
                                 grab_source::keybinding, event.evdev_code);
                return;
            }
        }
        // A press with no motion before it (a click right after a window mapped under a
        // still pointer) must still land on the window under the pointer.
        if (first)
            refresh_pointer_focus(event.time_ms, false);
        // The compositor's frame: the press is the window manager's, never a client's.
        if (pointer_zone_ != deco_zone::none && pointer_focus_ &&
            toplevels_->find(pointer_focus_)) {
            swallowed_mask_ |= bit;
            decoration_press(pointer_focus_, pointer_zone_, event);
            return;
        }
        // Layer-shell keyboard: a press on an on_demand surface (or an exclusive one below
        // the windows) gives it the keyboard; a press on a window or on nothing takes it
        // back. Popups leave it as it is. Before the button, as for click-to-focus.
        if (layers_) {
            if (pointer_owner_ == hit_owner::layer && pointer_root_ && router_->pointer_focus()) {
                if (!(layer_focus_ == pointer_root_) && layers_->click_focusable(pointer_root_)) {
                    layer_focus_ = pointer_root_;
                    if (log_layer.enabled(lx::trace::level::debug)) {
                        const layer_record* l = layers_->find(layer_focus_);
                        log_layer.debug("layer focus surface={} interactivity={} "
                                        "cause=click",
                                        layer_focus_.id(),
                                        l && l->current.interactivity == 1 ? "exclusive"
                                                                           : "on_demand");
                    }
                    resolve_keyboard_focus();
                }
            } else if (pointer_owner_ != hit_owner::popup &&
                       pointer_owner_ != hit_owner::ime_popup && layer_focus_) {
                if (log_layer.enabled(lx::trace::level::debug)) {
                    log_layer.debug("layer focus surface=0 interactivity=none "
                                    "cause=click previous={}",
                                    layer_focus_.id());
                }
                layer_focus_ = {};
                resolve_keyboard_focus();
            }
        }
        // Placeholder policy until the shell decides: click-to-focus, raise-on-press, on
        // any button — focus before the button so the client sees keyboard `enter` first.
        if (pointer_focus_ && toplevels_->find(pointer_focus_)) {
            // A window blocked by a modal dialog: the press is consumed (its release with it)
            // and the dialog is focused and raised with its parent — the parent stays
            // visible and still gets motion and scroll (Mutter, KWin).
            if (const lx::toplevel_id b = modal_blocker(pointer_focus_); b) {
                swallowed_mask_ |= bit;
                set_keyboard_focus(b);
                (void)raise(pointer_focus_);
                if (log_wm.enabled(lx::trace::level::debug)) {
                    log_wm.debug("modal press id={} dialog={}", pointer_focus_.id(), b.id());
                }
                return;
            }
            set_keyboard_focus(pointer_focus_);
            if (focus_.raise_on_click)
                (void)raise(pointer_focus_);
        }
        if (const unsigned serial = router_->send_pointer_button(event, *seat_); serial) {
            last_press_serial_ = serial;
            last_press_client_ = router_->pointer_focus_client();
            last_press_button_ = event.evdev_code;
        }
        return;
    }

    if (bit) {
        // A release whose press was never seen (held across startup) has no receiver
        // that expects it.
        if (!(button_mask_ & bit))
            return;
        button_mask_ &= ~bit;
        const bool swallowed = (swallowed_mask_ & bit) != 0;
        swallowed_mask_ &= ~bit;
        if (grab_.mode != grab_mode::none && event.evdev_code == grab_.button) {
            end_grab(grab_end_reason::release); // re-hit-tests once no button is held
            return;
        }
        // The close button clicks on release, and only over the button it was pressed on.
        if (deco_press_.id && event.evdev_code == deco_press_.button) {
            const lx::toplevel_id id = deco_press_.id;
            deco_press_ = {};
            const auto* rec = toplevels_->find(id);
            const bool over = rec && in_close(*rec, seat_->pointer_x(), seat_->pointer_y());
            if (decorations_)
                decorations_->set_button(id, over ? deco_button::hover : deco_button::normal);
            if (over) {
                if (log_deco.enabled(lx::trace::level::debug)) {
                    log_deco.debug("decoration close id={}", id.id());
                }
                request_close(id);
            }
        }
        if (swallowed) {
            if (button_mask_ == 0 && grab_.mode == grab_mode::none)
                refresh_pointer_focus(event.time_ms, false);
            return;
        }
    }
    (void)router_->send_pointer_button(event, *seat_);
    // Implicit grab over: whatever is under the pointer now gets it.
    if (button_mask_ == 0)
        refresh_pointer_focus(event.time_ms, false);
}

void lx::compositor::window_manager::on_pointer_axis(const lx::input::pointer_axis_event& event) {
    // Pointer focus is empty during a grab or a drag anyway; say so rather than rely on it.
    if (router_ && grab_.mode == grab_mode::none && !drag_active())
        router_->send_pointer_axis(event);
}

bool lx::compositor::window_manager::on_key(const lx::input::key_event& event) {
    // A keyboard move or resize (K.6): every key press is the window manager's while it
    // lasts — arrows move the pointer, Return applies, Escape cancels. Releases go on: the
    // router passes only those of keys whose press reached a client (pressed before).
    if (grab_.mode != grab_mode::none && grab_.keyboard) {
        if (!event.pressed)
            return false;
        constexpr unsigned k_key_enter = 28, k_key_kpenter = 96, k_key_space = 57;
        constexpr unsigned k_key_up = 103, k_key_left = 105, k_key_right = 106, k_key_down = 108;
        const unsigned code = event.evdev_keycode;
        const int step = seat_ && seat_->keymap().named().ctrl ? 1 : 10;
        switch (code) {
        case k_key_esc: end_grab(grab_end_reason::escape); break;
        case k_key_enter:
        case k_key_kpenter:
        case k_key_space: end_grab(grab_end_reason::release); break;
        case k_key_left: keyboard_grab_step(-step, 0); break;
        case k_key_right: keyboard_grab_step(step, 0); break;
        case k_key_up: keyboard_grab_step(0, -step); break;
        case k_key_down: keyboard_grab_step(0, step); break;
        default: break;
        }
        return true;
    }
    if (event.evdev_keycode != k_key_esc)
        return false;
    if (event.pressed && grab_.mode != grab_mode::none) {
        swallowed_escape_ = true;
        end_grab(grab_end_reason::escape);
        return true;
    }
    // Escape cancels a drag (Mutter, sway). Held buttons stay withheld: their releases reach
    // no client, and the last one gives the pointer back.
    if (event.pressed && drag_active()) {
        swallowed_escape_ = true;
        dnd_->on_escape();
        return true;
    }
    if (!event.pressed && swallowed_escape_) {
        swallowed_escape_ = false;
        return true;
    }
    return false;
}

void lx::compositor::window_manager::cancel_for_pause() {
    if (grab_.mode != grab_mode::none)
        end_grab(grab_end_reason::escape);
    if (drag_active())
        dnd_->on_escape();
    swallowed_escape_ = false;
}

void lx::compositor::window_manager::release_held_buttons(unsigned time_ms) {
    if (!router_ || !seat_) {
        button_mask_ = 0;
        swallowed_mask_ = 0;
        return;
    }
    for (std::uint64_t b = button_mask_; b; b &= b - 1) {
        const unsigned offset = static_cast<unsigned>(__builtin_ctzll(b));
        const std::uint64_t bit = std::uint64_t{1} << offset;
        if (swallowed_mask_ & bit)
            continue;
        lx::input::pointer_button_event release{};
        release.evdev_code = k_button_base + offset;
        release.pressed = false;
        release.time_ms = time_ms;
        (void)router_->send_pointer_button(release, *seat_);
    }
    button_mask_ = 0;
    swallowed_mask_ = 0;
}

void lx::compositor::window_manager::swallow_button_press(
    const lx::input::pointer_button_event& event) {
    if (!event.pressed || event.evdev_code < k_button_base ||
        event.evdev_code - k_button_base >= 64)
        return;
    const std::uint64_t bit = std::uint64_t{1} << (event.evdev_code - k_button_base);
    if (button_mask_ & bit)
        return; // already held: its release belongs to that press
    button_mask_ |= bit;
    swallowed_mask_ |= bit;
}

void lx::compositor::window_manager::set_session_locked(bool locked) {
    if (locked == locked_)
        return;
    if (locked) {
        // What a lock must not leave half-done or reachable: a move or resize ends as Escape
        // would, a drag is cancelled (no release may become a drop), every popup is
        // dismissed (popup_done), and the releases of buttons held now reach nobody.
        if (grab_.mode != grab_mode::none)
            end_grab(grab_end_reason::escape);
        if (drag_active())
            dnd_->on_escape();
        if (popups_)
            popups_->dismiss_all("session-lock");
        swallowed_mask_ |= button_mask_;
        deco_press_ = {};
        locked_ = true;
        // The pointer leaves whatever it was on (a lock surface takes it once shown).
        set_pointer_focus(nullptr);
        resolve_keyboard_focus();
        if (button_mask_ == 0)
            refresh_pointer_focus(0, false);
    } else {
        locked_ = false;
        resolve_keyboard_focus();
        if (button_mask_ == 0)
            refresh_pointer_focus(0, false);
        else
            set_pointer_focus(nullptr);
    }
    if (log_wm.enabled(lx::trace::level::debug))
        log_wm.debug("{}", locked ? "input locked (lock surfaces only)" : "input unlocked");
}

void lx::compositor::window_manager::lock_focus_changed() {
    if (!locked_)
        return;
    resolve_keyboard_focus();
    if (button_mask_ == 0)
        refresh_pointer_focus(0, false);
}

void lx::compositor::window_manager::set_suspend_reason(unsigned reason, bool on) {
    const unsigned before = suspend_reasons_;
    if (on)
        suspend_reasons_ |= reason;
    else
        suspend_reasons_ &= ~reason;
    if ((before != 0) == (suspend_reasons_ != 0) || !toplevels_)
        return;
    on = suspend_reasons_ != 0;
    // A hidden window stays suspended whatever the session does.
    toplevels_->for_each_mut([on](toplevel_record& rec) {
        if (on || rec.hidden)
            rec.desired.states |= tl_suspended;
        else
            rec.desired.states &= ~static_cast<unsigned>(tl_suspended);
    });
}

void lx::compositor::window_manager::on_modifiers() {
    if (!drag_active() || !seat_)
        return;
    const lx::input::named_modifiers m = seat_->keymap().named();
    dnd_->on_modifiers(m.shift, m.ctrl, m.alt);
}

// ── Drag-and-drop ──────────────────────────────────────────────────────────

const char* lx::compositor::window_manager::validate_drag_serial(void* client,
                                                                 unsigned serial) const {
    if (drag_active())
        return "dnd-active";
    if (grab_.mode != grab_mode::none)
        return "grab-active";
    if (serial == 0 || client == nullptr || serial != last_press_serial_ ||
        client != last_press_client_)
        return "serial";
    const unsigned offset = last_press_button_ - k_button_base;
    const std::uint64_t bit = last_press_button_ >= k_button_base && offset < 64
                                  ? (std::uint64_t{1} << offset)
                                  : 0;
    if (!bit || !(button_mask_ & bit))
        return "no-button";
    return nullptr;
}

void lx::compositor::window_manager::begin_drag() {
    if (!drag_active() || !seat_)
        return;
    // Menus do not travel with a drag (as for move/resize).
    if (popups_ && popups_->grab_active())
        popups_->dismiss_all("dnd");
    // The releases of the buttons held now (the one that started the drag included) reach
    // no client, wherever the drop lands.
    swallowed_mask_ |= button_mask_;
    // The origin client's implicit grab ends with `leave` (wlroots, Weston, Mutter): from
    // here on it hears the drag through its data device, like any other target.
    set_pointer_focus(nullptr);
    on_modifiers();
    drag_hit(0, false);
}

void lx::compositor::window_manager::drag_hit(unsigned time_ms, bool send_motion) {
    if (!seat_)
        return;
    const double px = seat_->pointer_x();
    const double py = seat_->pointer_y();
    // Not `pointer_hit`: popup owner-events ended with the drag. The icon layer is never
    // hit ("the input region is ignored").
    wm_hit h{};
    if (hit_test(px, py, h) && h.target == hit_target::surface && h.surface_resource) {
        dnd_target t{};
        t.surface_resource = h.surface_resource;
        t.surface = h.surface;
        t.client = h.client;
        t.sx = h.sx;
        t.sy = h.sy;
        dnd_->on_motion(&t, px, py, time_ms, send_motion);
        return;
    }
    dnd_->on_motion(nullptr, px, py, time_ms, send_motion);
}

// ── Server-side frame ──────────────────────────────────────────────────────

void lx::compositor::window_manager::decoration_press(
    lx::toplevel_id id, deco_zone zone, const lx::input::pointer_button_event& event) {
    if (log_deco.enabled(lx::trace::level::debug)) {
        log_deco.debug("decoration press id={} zone={} button={}", id.id(), deco_zone_name(zone),
                       event.evdev_code);
    }
    constexpr unsigned k_btn_middle = 0x112;
    // Middle on the title lowers (the KWin and Mutter default). Without click-to-focus: a
    // window just sent to the back does not take the keyboard on its way down.
    if (event.evdev_code == k_btn_middle && zone == deco_zone::title) {
        (void)lower(id);
        return;
    }
    // Any other press on the frame focuses and raises, as a press on the window does.
    set_keyboard_focus(id);
    (void)raise(id);
    if (event.evdev_code != k_btn_left)
        return; // right on the title: no window menu yet
    const double px = seat_->pointer_x();
    const double py = seat_->pointer_y();
    switch (zone) {
    case deco_zone::title: {
        const title_click prev = last_title_click_;
        const double dx = px - prev.x;
        const double dy = py - prev.y;
        if (prev.id == id && event.time_ms - prev.time_ms <= k_double_click_ms &&
            dx * dx + dy * dy <= k_double_click_px * k_double_click_px) {
            last_title_click_ = {};
            // Double click on the title toggles maximize (every desktop's default).
            const auto* rec = toplevels_->find(id);
            const bool maximized = rec && (rec->desired.states & tl_maximized) != 0;
            if (log_deco.enabled(lx::trace::level::debug)) {
                log_deco.debug("decoration double-click id={} maximize={}", id.id(),
                               maximized ? 0 : 1);
            }
            set_maximized(id, !maximized, "decoration");
            return;
        }
        last_title_click_ = {id, event.time_ms, px, py};
        (void)begin_grab(id, grab_mode::move, 0, grab_source::decoration, event.evdev_code);
        return;
    }
    case deco_zone::close:
        deco_press_ = {id, event.evdev_code};
        if (decorations_)
            decorations_->set_button(id, deco_button::pressed);
        return;
    case deco_zone::none:
        return;
    default:
        (void)begin_grab(id, grab_mode::resize, edges_for_zone(zone), grab_source::decoration,
                         event.evdev_code);
        return;
    }
}

void lx::compositor::window_manager::on_title_changed(lx::toplevel_id id) {
    if (!decorations_ || !toplevels_)
        return;
    if (const auto* rec = toplevels_->find(id))
        decorations_->on_title_changed(*rec);
}

void lx::compositor::window_manager::on_decoration_request(lx::toplevel_id id, bool immediate) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec)
        return;
    rec->desired.decoration = toplevel_manager::wanted_decoration(*rec);
    if (!rec->deco_resource)
        return;
    rec->deco_configure_due = true;
    // Before the initial commit the initial configure carries it. Afterwards the answer
    // goes out now when nothing is in flight (or for a new object, which a version 1 client
    // may not attach a buffer before); else the flush sends it once the one in flight is
    // answered, as for any configure.
    if (rec->initial_configure_sent && (immediate || !rec->inflight))
        send_configure(*rec, wm_now_ns(), false, false);
}

// ── Interactive grabs ──────────────────────────────────────────────────────

void lx::compositor::window_manager::log_grab_reject(lx::toplevel_id id, grab_source source,
                                                     const char* reason) const {
    if (!log_wm.enabled(lx::trace::level::debug))
        return;
    log_wm.debug("grab rejected id={} source={} reason={}", id.id(), grab_source_name(source),
                 reason);
}

bool lx::compositor::window_manager::begin_grab(lx::toplevel_id id, grab_mode mode,
                                                unsigned edges, grab_source source,
                                                unsigned button, bool keyboard) {
    if (!toplevels_ || !surfaces_ || !seat_ || mode == grab_mode::none)
        return false;
    auto* rec = toplevels_->find_mut(id);
    const unsigned offset = button - k_button_base;
    const std::uint64_t bit =
        button >= k_button_base && offset < 64 ? (std::uint64_t{1} << offset) : 0;
    const bool maximized = rec && (rec->state == lx::toplevel_state::maximized ||
                                   (rec->desired.states & tl_maximized));
    const bool fullscreen = rec && (rec->state == lx::toplevel_state::fullscreen ||
                                    (rec->desired.states & tl_fullscreen));
    const char* reject = nullptr;
    if (grab_.mode != grab_mode::none)
        reject = "grab-active";
    else if (drag_active())
        reject = "dnd-active";
    else if (!rec || !rec->mapped)
        reject = "unmapped";
    else if (keyboard ? (button_mask_ != 0 || !warp_ || locked_) : (!bit || !(button_mask_ & bit)))
        reject = keyboard ? "button-held" : "no-button";
    else if (fullscreen || (rec->state != lx::toplevel_state::normal && !maximized))
        reject = "state";
    // A maximized window moves only by being restored under the pointer (K.5), and never
    // resizes; a snapped one may be dragged out (restored) or resized (no longer snapped).
    else if (maximized && (mode == grab_mode::resize || !window_cfg_.drag_restore))
        reject = "state";
    else if (mode == grab_mode::resize && (edges == edge_none || !valid_resize_edges(edges)))
        reject = "edges";
    if (reject) {
        log_grab_reject(id, source, reject);
        return false;
    }
    // A move or resize ends any popup grab: menus do not travel with an interactive grab.
    if (popups_ && popups_->grab_active())
        popups_->dismiss_all("wm-grab");
    // A resize of a tiled window floats it where it is (dwm's Mod+right-drag, ADR 012 T6);
    // the others close the gap once the grab has begun.
    const bool floated = mode == grab_mode::resize && rec->tiled;
    if (floated) {
        rec->floating = true;
        rec->float_set = false;
        leave_tiling(*rec);
        tiling_dirty_ = true;
    }

    grab_ = {};
    grab_.mode = mode;
    grab_.source = source;
    grab_.toplevel = id;
    grab_.edges = mode == grab_mode::resize ? edges : 0;
    grab_.button = keyboard ? 0u : button;
    grab_.keyboard = keyboard;
    grab_.start_x = grab_.now_x = seat_->pointer_x();
    grab_.start_y = grab_.now_y = seat_->pointer_y();
    grab_.start = rec->geometry;
    grab_.last_target = {rec->geometry.width, rec->geometry.height};
    grab_.normal = rec->snap || maximized ? (rec->restore_set ? rec->restore : lx::rect2i{})
                                          : rec->geometry;
    if (mode == grab_mode::move && (maximized || rec->snap) && window_cfg_.drag_restore) {
        // Drag-to-restore: nothing moves until the pointer has travelled 8 px.
        grab_.restore_pending = true;
        grab_.restored_max = maximized;
        grab_.restored_snap = rec->snap;
    } else if (rec->snap) {
        // Resized, or moved with drag-restore off: an ordinary window from now on.
        rec->snap = 0;
        rec->restore_set = false;
        rec->desired.states &= ~static_cast<unsigned>(tl_tiled_all);
        rec->configure_due = true;
        if (shell_)
            shell_->emit_toplevel_state(*rec);
    }

    // The client's implicit grab is broken (it gets `leave`), which is what GTK and Qt
    // expect after xdg_toplevel.move/resize; no client hears the pointer until the end.
    set_pointer_focus(nullptr);
    if (cursor_)
        cursor_->set_override(mode == grab_mode::move ? cursor_shape::move
                                                      : shape_for_edges(grab_.edges));
    if (mode == grab_mode::resize) {
        // Announce `resizing` at the current size: the first configure costs the client
        // no relayout, and every later one is a pure size change.
        rec->desired.size = grab_.last_target;
        rec->desired.states |= tl_resizing;
        rec->desired.anchor_edges = grab_.edges;
        rec->desired.anchor = grab_.start;
    }

    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("grab begin id={} mode={} edges={} source={} button={} "
                     "x={} y={} w={} h={}{}{}",
                     id.id(), grab_mode_name(mode), grab_.edges, grab_source_name(source), button,
                     grab_.start.x, grab_.start.y, grab_.start.width, grab_.start.height,
                     keyboard ? " keyboard" : "", grab_.restore_pending ? " restore-pending" : "");
    }
    if (floated)
        flush_tiling();
    return true;
}

bool lx::compositor::window_manager::request_client_grab(lx::toplevel_id id, void* client,
                                                         unsigned serial, grab_mode mode,
                                                         unsigned edges) {
    // A move/resize must answer a press this client received, with that button still
    // down. A stale serial, another client's serial, or a request after release would
    // otherwise let any client start dragging on its own.
    if (serial == 0 || serial != last_press_serial_ || client != last_press_client_) {
        log_grab_reject(id, grab_source::client, "serial");
        return false;
    }
    return begin_grab(id, mode, edges, grab_source::client, last_press_button_);
}

lx::compositor::popup_manager::grab_result
lx::compositor::window_manager::request_popup_grab(lx::surface_id popup, void* client,
                                                   unsigned serial) {
    if (!popups_)
        return popup_manager::grab_result::denied;
    const bool serial_ok =
        serial != 0 && client != nullptr &&
        ((serial == last_press_serial_ && client == last_press_client_) ||
         (router_ && router_->is_key_grab_serial(serial, client)));
    // No menu grab while a move/resize or a drag owns the pointer.
    return popups_->grab(popup, serial_ok, grab_.mode != grab_mode::none || drag_active());
}

bool lx::compositor::window_manager::request_shell_grab(lx::toplevel_id id, unsigned serial,
                                                        grab_mode mode, unsigned edges) {
    if (serial == 0 || serial != last_press_serial_) {
        log_grab_reject(id, grab_source::shell, "serial");
        return false;
    }
    return begin_grab(id, mode, edges, grab_source::shell, last_press_button_);
}

void lx::compositor::window_manager::move_window(toplevel_record& rec, lx::point2i p,
                                                 const char* cause) {
    if (p.x == rec.geometry.x && p.y == rec.geometry.y)
        return;
    const int dx = p.x - rec.geometry.x;
    const int dy = p.y - rec.geometry.y;
    rec.geometry.x = p.x;
    rec.geometry.y = p.y;
    shift_anchors(rec, dx, dy);
    // An origin change only: buffers, textures and node bounds are untouched. Every node of
    // the tree moves; old and new rects are damaged as separate clips per node, never one
    // window-sized bounding box.
    // The geometry line first: the tree's dump of the moved nodes follows it, as it follows
    // the geometry line of a commit.
    log_geometry(rec, cause, 0);
    tree_->set_root_origin(rec.surface, {rec.geometry.x - rec.geometry_offset.x,
                                         rec.geometry.y - rec.geometry_offset.y});
    tree_->sync_scene(rec.surface, "move");
    sync_decoration(rec);
    // Popups follow their window in the same UI call, so in the same snapshot.
    if (popups_)
        popups_->on_parent_moved(rec.surface);
    if (shell_)
        shell_->emit_toplevel_geometry(rec);
}

void lx::compositor::window_manager::apply_grab() {
    lx::trace::scoped_span span{"compositor.wm", "apply_grab"};
    grab_.motion_pending = false;
    auto* rec = toplevels_ ? toplevels_->find_mut(grab_.toplevel) : nullptr;
    if (!rec || !rec->mapped)
        return;
    if (grab_.mode == grab_mode::move) {
        if (grab_.restore_pending) {
            // KWin's and GNOME's threshold: a click on a maximized title bar is not a drag.
            const double mx = grab_.now_x - grab_.start_x;
            const double my = grab_.now_y - grab_.start_y;
            if (mx * mx + my * my < 64.0)
                return;
            drag_restore(*rec);
        }
        const int dx = round_delta(grab_.now_x - grab_.start_x);
        const int dy = round_delta(grab_.now_y - grab_.start_y);
        // The frame is what must stay reachable: a server-side title bar stays on the output.
        // Restoring, the frame it is restored with (the commit has not changed its state yet).
        const frame_extents ext = grab_.awaiting_restore
                                      ? extents_for(rec->desired.decoration, false)
                                      : extents_of(*rec);
        const lx::rect2i f = frame_rect({grab_.start.x + dx, grab_.start.y + dy,
                                         grab_.start.width, grab_.start.height},
                                        ext);
        // The title bar stays reachable on *some* output: the one the pointer drags it over
        // (the nearest one in a gap between outputs, §A.9) — so a window crosses outputs.
        const lx::point2i p = constrain_on_output(usable_area(pointer_output()), f);
        const lx::point2i at{p.x + ext.left, p.y + ext.top};
        if (grab_.awaiting_restore) {
            // The restored window lands where the pointer is when its commit comes.
            if (rec->desired.place)
                rec->desired.place_at = at;
            for (unsigned i = 0; i < rec->sent_count; ++i)
                if (rec->sent[i].place && !(rec->sent[i].states & (tl_maximized | tl_fullscreen)))
                    rec->sent[i].place_at = at;
            if (rec->acked_unapplied && rec->acked.place)
                rec->acked.place_at = at;
        } else {
            move_window(*rec, at, "move");
        }
        update_edge_zone();
        return;
    }
    const int dx = round_delta(grab_.now_x - grab_.start_x);
    const int dy = round_delta(grab_.now_y - grab_.start_y);
    // Resize: only the request changes here. Geometry follows the client's commit of the
    // acked size (on_toplevel_commit), so no frame ever shows a stretched or cropped
    // window, and the single-in-flight rule coalesces every position in between.
    const lx::size2i target = resize_target(grab_.start, grab_.edges, dx, dy, rec->min_size,
                                            rec->max_size);
    if (target.width == grab_.last_target.width && target.height == grab_.last_target.height)
        return;
    grab_.last_target = target;
    rec->desired.size = target;
}

void lx::compositor::window_manager::end_grab(grab_end_reason reason) {
    if (grab_.mode == grab_mode::none)
        return;
    // Motion since the last tick is still only recorded; a release lands the window (or
    // the final configure's size) where the pointer let go, not one frame short of it.
    if (reason == grab_end_reason::release && grab_.motion_pending)
        apply_grab();
    const wm_grab g = grab_;
    grab_ = {};
    hide_snap_preview();
    auto* rec = toplevels_ ? toplevels_->find_mut(g.toplevel) : nullptr;
    const bool live = rec && rec->mapped && (reason == grab_end_reason::release ||
                                             reason == grab_end_reason::escape);
    // Drag-to-restore happened (the window left its maximized or snapped state).
    const bool restored = (g.restored_max || g.restored_snap) && !g.restore_pending;
    // The user placed it: it no longer goes back to an output it was moved off (H5).
    if (rec && reason == grab_end_reason::release && !g.restore_pending)
        rec->home_identifier[0] = '\0';
    // Dropped where it is now (W.1): on the output its frame overlaps most, it joins that
    // output's active workspace — also when the workspace was switched during the drag.
    if (rec && rec->mapped && g.mode == grab_mode::move && reason == grab_end_reason::release)
        rec->output = window_output(*rec); // the output it was dropped on
    if (rec && rec->tiled && rec->mapped && g.mode == grab_mode::move) {
        // ADR 012 T6: dropped on another tile of its workspace, the two change places;
        // anywhere else it goes back to its tile (or, on another output's floating
        // workspace, stays where it was dropped).
        if (reason == grab_end_reason::release) {
            toplevel_record* target = nullptr;
            toplevels_->for_each_mut([&](toplevel_record& t) {
                if (!target && t.tiled && t.id != rec->id && t.mapped && !t.minimized &&
                    t.tile_workspace == rec->tile_workspace &&
                    rect_contains(t.tile, g.now_x, g.now_y))
                    target = &t;
            });
            if (target) {
                std::swap(target->tile_order, rec->tile_order);
                log_wm.info("tile swap id={} with={}", rec->id.id(), target->id.id());
            }
            const int slot = window_output(*rec);
            const lx::workspace_id ws = slot >= 0
                                            ? workspaces_.active(static_cast<unsigned>(slot))
                                            : lx::workspace_id{};
            const workspace_record* w = workspaces_.find(ws);
            if (!target && ws != rec->workspace &&
                !(w && w->layout == workspace_layout::master_stack))
                rec->float_set = false;
        }
        tiling_dirty_ = true;
    }
    if (rec && rec->mapped && !rec->sticky && g.mode == grab_mode::move &&
        reason == grab_end_reason::release) {
        const int slot = window_output(*rec);
        const lx::workspace_id ws = slot >= 0 ? workspaces_.active(static_cast<unsigned>(slot))
                                              : lx::workspace_id{};
        if (ws && ws != rec->workspace) {
            const lx::toplevel_id moved = rec->id;
            join_workspace(*rec, ws);
            toplevels_->for_each_mut([&](toplevel_record& child) {
                if (child.parent == moved && child.mapped && !child.sticky)
                    join_workspace(child, ws);
            });
        }
    }
    if (live && g.mode == grab_mode::resize) {
        // The final configure, without `resizing`, goes out through the normal flush once
        // the one in flight is answered. Escape asks for the start size, anchored to the
        // start rect, so the answering commit restores it exactly.
        if (reason == grab_end_reason::escape)
            rec->desired.size = {g.start.width, g.start.height};
        rec->desired.states &= ~static_cast<unsigned>(tl_resizing);
        rec->desired.anchor_edges = g.edges;
        rec->desired.anchor = g.start;
        // Nothing left to send (the grab ended before its first configure went out): no
        // configure will carry the anchor, so it must not linger for an unrelated one.
        if (rec->desired.same_request(rec->last_sent)) {
            rec->desired.anchor_edges = 0;
            rec->desired.anchor = {};
        }
    } else if (live && g.mode == grab_mode::move && reason == grab_end_reason::release &&
               g.zone && g.zone_output >= 0) {
        // Dropped in an edge zone (K.4): snapped or maximized there; its normal geometry —
        // where it was before the drag, or the size it was restored to — is what it goes back
        // to.
        const int slot = g.zone_output;
        if (!rect_empty(g.normal)) {
            rec->restore = g.normal;
            rec->restore_set = true;
        }
        if (g.zone == k_snap_maximize) {
            aim_maximized(*rec, slot);
        } else {
            rec->snap = g.zone;
            aim_snap(*rec, g.zone, slot);
        }
        configure_now(*rec);
        if (shell_)
            shell_->emit_toplevel_state(*rec);
        log_wm.info("edge snap id={} zone={} output={}", g.toplevel.id(),
                    g.zone == k_snap_maximize ? 0x10u : g.zone, slot);
    } else if (live && g.mode == grab_mode::move && reason == grab_end_reason::escape &&
               restored) {
        // Escape after drag-to-restore puts the window back as it was (maximized or snapped,
        // its normal geometry kept).
        const int slot = window_output(*rec);
        if (!rect_empty(g.normal) && g.normal.width > 0) {
            rec->restore = g.normal;
            rec->restore_set = true;
        }
        if (g.restored_max) {
            aim_maximized(*rec, slot);
        } else {
            rec->snap = g.restored_snap;
            aim_snap(*rec, g.restored_snap, slot);
        }
        configure_now(*rec);
    } else if (live && g.mode == grab_mode::move && reason == grab_end_reason::escape &&
               !g.restore_pending) {
        move_window(*rec, {g.start.x, g.start.y}, "cancel");
    }
    if (cursor_)
        cursor_->clear_override();

    if (log_wm.enabled(lx::trace::level::debug)) {
        const lx::rect2i r = rec ? rec->geometry : g.start;
        log_wm.debug("grab end id={} mode={} reason={} x={} y={} w={} h={}", g.toplevel.id(),
                     grab_mode_name(g.mode), grab_end_name(reason), r.x, r.y, r.width, r.height);
    }
    // The grab kept its window shown (`shown`); now its workspace decides again.
    update_visibility();
    flush_workspaces();
    // The pointer goes back to whatever is under it — unless a button is still held, in
    // which case nothing has it until release (no `enter` mid-press).
    if (button_mask_ == 0)
        refresh_pointer_focus(0, false);
}

// ── Stacking ───────────────────────────────────────────────────────────────

void lx::compositor::window_manager::sync_node_stacking(const toplevel_record& rec,
                                                        const char* verb) {
    auto* box = tree_ ? tree_->container(rec.surface) : nullptr;
    if (!box || !windows_layer_)
        return;
    if (rec.state == lx::toplevel_state::fullscreen && fullscreen_layer_) {
        // In the fullscreen layer, above the backdrops: a raise there is to its top.
        (void)fullscreen_layer_->raise_to_top(box);
    } else {
        // The window layer holds the mapped toplevels' tree containers that are not
        // fullscreen, in stacking order: the container's index is the number of those below.
        unsigned below = 0;
        const unsigned at = toplevels_->stacking_index(rec.id);
        unsigned i = 0;
        toplevels_->for_each_bottom_up([&](const toplevel_record& other) {
            if (i++ < at && !(other.state == lx::toplevel_state::fullscreen && fullscreen_layer_))
                ++below;
        });
        (void)windows_layer_->move_to(box, below);
    }
    // Everything that changed lies inside the restacked window's node rects: it now covers,
    // or uncovers, part of what overlapped it.
    tree_->damage_root(rec.surface);
    if (decorations_)
        decorations_->damage(rec.id);
    rehit_pending_ = true;
    stacking_dirty_ = true;
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("{} id={} index={}", verb, rec.id.id(), toplevels_->stacking_index(rec.id));
    }
}

bool lx::compositor::window_manager::raise(lx::toplevel_id id) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return false;
    const auto* rec = toplevels_->find(id);
    if (!rec || !rec->mapped)
        return false;
    const bool moved = toplevels_->raise(id);
    if (moved)
        sync_node_stacking(*rec, "raise");
    // Its modal dialogs come along, above it (also when it already was on top of them).
    keep_modals_above(id);
    return moved;
}

namespace {
/// The window a modal chain hangs from: what lowering or restacking a modal dialog moves.
lx::toplevel_id modal_root(const lx::compositor::toplevel_manager& m, lx::toplevel_id id) {
    for (unsigned depth = 0; depth < 8; ++depth) {
        const auto* rec = m.find(id);
        if (!rec || !lx::compositor::effective_modal(m, *rec))
            break;
        id = rec->parent;
    }
    return id;
}
} // namespace

bool lx::compositor::window_manager::lower(lx::toplevel_id id) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return false;
    // A modal dialog never goes below its parent: the whole family is lowered.
    id = modal_root(*toplevels_, id);
    const auto* rec = toplevels_->find(id);
    if (!rec || !rec->mapped || !toplevels_->lower(id))
        return false;
    sync_node_stacking(*rec, "lower");
    keep_modals_above(id);
    return true;
}

bool lx::compositor::window_manager::restack(lx::toplevel_id id, unsigned index) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return false;
    id = modal_root(*toplevels_, id);
    const auto* rec = toplevels_->find(id);
    if (!rec || !rec->mapped || !toplevels_->move_to(id, index))
        return false;
    sync_node_stacking(*rec, "restack");
    keep_modals_above(id);
    return true;
}

void lx::compositor::window_manager::activate(lx::toplevel_id id) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || !rec->mapped)
        return;
    // The shell's request_activate (a task-list click, the switcher): an explicit choice,
    // so a minimized window comes back and its workspace is switched to.
    (void)reveal(*rec, true, "activate");
    set_keyboard_focus(id);
    (void)raise(id);
    flush_workspaces();
}

// ── xdg-activation ─────────────────────────────────────────────────────────

namespace {

constexpr lx::trace::category log_activation{"compositor.activation"};

void log_activation_result(const char* token_prefix, const lx::compositor::toplevel_record& rec,
                           lx::compositor::activation_result r,
                           lx::compositor::activation_reason why, const char* cause) {
    if (!log_activation.enabled(lx::trace::level::debug))
        return;
    log_activation.debug("activate token={:.8} surface={} id={} result={} reason={}{}",
                         token_prefix && token_prefix[0] ? token_prefix : "-", rec.surface.id(),
                         rec.id.id(), lx::compositor::activation_result_name(r),
                         lx::compositor::activation_reason_name(why), cause);
}

} // namespace

lx::compositor::activation_context
lx::compositor::window_manager::activation_context_for(const toplevel_record* rec,
                                                       const activation_grant* grant,
                                                       lx::runtime::clock_time now) const {
    activation_context c{};
    c.token = grant;
    c.now = now;
    if (rec) {
        c.target_client = rec->client;
        c.target_is_toplevel = true;
        c.target_is_mapped = rec->mapped;
        c.target_already_focused = rec->mapped && keyboard_focus_ == rec->id;
    }
    const toplevel_record* focused = toplevels_ ? toplevels_->find(keyboard_focus_) : nullptr;
    if (focused && focused->mapped && focused != rec) {
        c.focused_exists = true;
        c.focused_client = focused->client;
        if (ops_.last_user_serial)
            c.focused_last_user_serial = ops_.last_user_serial(ops_.user, focused->surface_resource);
    }
    return c;
}

lx::compositor::activation_result
lx::compositor::window_manager::request_activation(lx::toplevel_id id,
                                                   const activation_grant* grant,
                                                   lx::runtime::clock_time now,
                                                   activation_reason& why) {
    toplevel_record* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    activation_result r = decide_activation(activation_context_for(rec, grant, now), why);
    if (!rec)
        return r;
    // Locked: no window takes the keyboard behind the lock; it is marked instead, which the
    // shell shows once the session is unlocked.
    if (locked_ && rec->mapped && r == activation_result::granted && keyboard_focus_ != rec->id) {
        r = activation_result::urgent;
        why = activation_reason::session_locked;
    }
    if (!rec->mapped) {
        if (r == activation_result::pending || r == activation_result::urgent) {
            rec->activation_pending = true;
            rec->activation_token_known = grant != nullptr;
            rec->pending_grant = grant ? *grant : activation_grant{};
            rec->activation_requested_ns = now;
        }
        return r;
    }
    if (r == activation_result::granted) {
        // A window out of view: minimized ones come back; one on an inactive workspace is
        // switched to with switch-on-activate (GNOME, KDE), else it asks for attention.
        if (rec->hidden && !reveal(*rec, workspaces_.config().switch_on_activate, "activation")) {
            r = activation_result::urgent;
            set_demands_attention(*rec, true);
            flush_workspaces();
            return r;
        }
        activate(id);
    } else if (r == activation_result::urgent) {
        set_demands_attention(*rec, true);
    }
    return r;
}

bool lx::compositor::window_manager::decide_activation_at_map(toplevel_record& rec) {
    // A window mapping while the session is locked does not take the keyboard decision from
    // the window the user left: it opens marked, below the focused one.
    if (locked_ && toplevels_ && toplevels_->find(keyboard_focus_)) {
        rec.activation_pending = false;
        set_demands_attention(rec, true);
        return false;
    }
    if (!rec.activation_pending)
        return true;
    rec.activation_pending = false;
    const activation_grant* grant = rec.activation_token_known ? &rec.pending_grant : nullptr;
    activation_reason why{};
    activation_reason token_reason{};
    const activation_result r = lx::compositor::decide_activation_at_map(
        activation_context_for(&rec, grant, rec.activation_requested_ns), why, token_reason);
    if (log_activation.enabled(lx::trace::level::debug)) {
        // The token's own verdict too when the map overrode it.
        char cause[48];
        std::snprintf(cause, sizeof(cause), " cause=map%s%s",
                      why != token_reason ? " refused=" : "",
                      why != token_reason ? activation_reason_name(token_reason) : "");
        log_activation_result(grant ? grant->prefix : nullptr, rec, r, why, cause);
    }
    if (r != activation_result::urgent)
        return true;
    set_demands_attention(rec, true);
    return false;
}

void lx::compositor::window_manager::set_demands_attention(toplevel_record& rec, bool on) {
    // The window the user is typing into needs no attention drawn to it (Mutter does not
    // mark a window that is in plain view either).
    if (rec.demands_attention == on || (on && rec.id == keyboard_focus_))
        return;
    rec.demands_attention = on;
    if (log_activation.enabled(lx::trace::level::debug)) {
        log_activation.debug("urgent id={} on={}", rec.id.id(), on ? 1 : 0);
    }
    if (shell_)
        shell_->emit_toplevel_flags(rec);
    // Its workspace is urgent while a member asks for attention (ext-workspace).
    if (rec.workspace)
        flush_workspaces();
}

unsigned lx::compositor::window_manager::set_client_not_responding(lx::client_id client,
                                                                   bool on) {
    if (!toplevels_ || !client)
        return 0;
    unsigned n = 0;
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!(rec.client == client) || rec.not_responding == on)
            return;
        rec.not_responding = on;
        ++n;
        if (decorations_)
            decorations_->set_not_responding(rec.id, on);
        if (tree_)
            tree_->set_root_dim(rec.surface, on);
        if (shell_)
            shell_->emit_toplevel_flags(rec);
    });
    return n;
}

bool lx::compositor::window_manager::fit_into_usable(lx::toplevel_id id) {
    toplevel_record* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || !rec->mapped || rec->state != lx::toplevel_state::normal || grab_active())
        return false;
    const lx::rect2i area = usable_area(window_output(*rec));
    const frame_extents ext = extents_of(*rec);
    const lx::rect2i f = frame_rect(rec->geometry, ext);
    const auto fit = [](int pos, int len, int lo, int span) {
        if (len >= span)
            return lo;
        if (pos < lo)
            return lo;
        if (pos + len > lo + span)
            return lo + span - len;
        return pos;
    };
    const int x = fit(f.x, f.width, area.x, area.width);
    const int y = fit(f.y, f.height, area.y, area.height);
    if (x == f.x && y == f.y)
        return false;
    move_window(*rec, {x + ext.left, y + ext.top}, "placement-hold");
    return true;
}

void lx::compositor::window_manager::request_close(lx::toplevel_id id) {
    const auto* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->xdg_toplevel_resource || !ops_.send_close)
        return;
    ops_.send_close(rec->xdg_toplevel_resource);
}

void lx::compositor::window_manager::emit_stacking_order() {
    if (!shell_ || !shell_->has_bridge() || !toplevels_)
        return;
    const unsigned n = toplevels_->mapped_count();
    unsigned i = 0;
    toplevels_->for_each_bottom_up([&](const toplevel_record& rec) {
        shell_->emit_stacking_order_changed(rec, i, i + 1 == n);
        ++i;
    });
}

// ── Workspaces, minimize, sticky (design-session-and-wm Part W) ────────────

namespace {

constexpr lx::trace::category log_workspace{"compositor.workspace"};

} // namespace

lx::compositor::workspace_sink lx::compositor::window_manager::sink() {
    workspace_sink s{};
    s.user = this;
    s.windows_moved = [](void* u, lx::workspace_id from, lx::workspace_id to) {
        auto* wm = static_cast<window_manager*>(u);
        if (!wm->toplevels_)
            return;
        // The model already moved the count; only the records follow.
        wm->toplevels_->for_each_mut([&](toplevel_record& r) {
            if (r.workspace != from)
                return;
            r.workspace = to;
            r.workspace_dirty = true;
            wm->ws_dirty_ = true;
        });
        wm->tiling_dirty_ = true;
    };
    s.workspace_moved = [](void* u, lx::workspace_id ws, int from, int to) {
        auto* wm = static_cast<window_manager*>(u);
        if (!wm->toplevels_ || !wm->outputs_ || from < 0 || to < 0 || from == to ||
            !wm->outputs_->slot_live(static_cast<unsigned>(to)))
            return;
        // Its windows go with it, placed on the new output as an evacuation places them.
        const lx::rect2i area = wm->usable_area(from);
        wm->toplevels_->for_each_mut([&](toplevel_record& r) {
            if (r.mapped && r.workspace == ws && !r.sticky)
                wm->relocate_window(r, area, to);
        });
        wm->rehit_pending_ = true;
        wm->tiling_dirty_ = true;
    };
    return s;
}

void lx::compositor::window_manager::set_workspace_config(const workspace_config& c) {
    workspaces_.configure(c, sink());
    update_visibility();
    flush_workspaces();
}

void lx::compositor::window_manager::sync_workspace_outputs(bool add) {
    if (!outputs_)
        return;
    if (add) {
        for (unsigned s = 0; s < output_manager::k_max && s < k_workspace_groups; ++s) {
            if (!outputs_->slot_live(s))
                continue;
            const output_state& o = outputs_->slot_state(s);
            if (workspaces_.has_group(s) && workspaces_.group(s).key == o.id.id())
                continue;
            workspaces_.output_added(s, o.id.id(), o.identifier, o.name, sink());
            workspaces_.settle(s);
        }
    }
    for (unsigned s = 0; s < k_workspace_groups; ++s) {
        if (!workspaces_.has_group(s))
            continue;
        if (outputs_->slot_live(s) && outputs_->slot_state(s).id.id() == workspaces_.group(s).key)
            continue;
        int fallback = -1;
        for (unsigned f = 0; f < k_workspace_groups && fallback < 0; ++f)
            if (f != s && workspaces_.has_group(f) && outputs_->slot_live(f))
                fallback = static_cast<int>(f);
        workspaces_.output_removed(s, fallback, sink());
    }
    if (workspaces_.generation() != ws_generation_seen_)
        update_visibility();
}

bool lx::compositor::window_manager::shown(const toplevel_record& rec) const {
    if (!rec.mapped)
        return false;
    // The window being dragged comes along wherever the user goes.
    if (grab_.mode != grab_mode::none && grab_.toplevel == rec.id)
        return true;
    if (rec.minimized)
        return false;
    if (rec.sticky || !rec.workspace)
        return true;
    return workspaces_.is_active(rec.workspace);
}

int lx::compositor::window_manager::focused_output() const {
    if (const auto* rec = toplevels_ ? toplevels_->find(keyboard_focus_) : nullptr;
        rec && rec->mapped && !rec->hidden)
        return window_output(*rec);
    return pointer_output();
}

lx::toplevel_id lx::compositor::window_manager::top_shown(int slot) const {
    lx::toplevel_id found{};
    if (!toplevels_)
        return found;
    toplevels_->for_each_top_down([&](const toplevel_record& r) {
        if (r.hidden || (slot >= 0 && window_output(r) != slot))
            return true;
        found = r.id;
        return false;
    });
    return found;
}

lx::toplevel_id lx::compositor::window_manager::workspace_focus_target(int slot) const {
    if (slot < 0)
        return {};
    const lx::workspace_id ws = workspaces_.active(static_cast<unsigned>(slot));
    if (const auto* rec = toplevels_->find(workspaces_.focus_memory(ws));
        rec && rec->mapped && !rec->hidden && rec->workspace == ws)
        return rec->id;
    return top_shown(slot);
}

void lx::compositor::window_manager::focus_after_switch(int slot) {
    const auto* f = toplevels_->find(keyboard_focus_);
    // The keyboard stays on a shown window of another output (a switch there from its
    // panel, ext-workspace).
    if (f && f->mapped && !f->hidden && window_output(*f) != slot)
        return;
    set_keyboard_focus(workspace_focus_target(slot));
}

void lx::compositor::window_manager::join_workspace(toplevel_record& rec, lx::workspace_id ws) {
    if (rec.workspace == ws)
        return;
    leave_workspace(rec);
    rec.workspace = ws;
    if (ws)
        workspaces_.window_added(ws);
    rec.workspace_dirty = true;
    ws_dirty_ = true;
    tiling_dirty_ = true;
}

void lx::compositor::window_manager::leave_workspace(toplevel_record& rec) {
    if (!rec.workspace)
        return;
    const lx::workspace_id was = rec.workspace;
    rec.workspace = {};
    rec.workspace_dirty = true;
    ws_dirty_ = true;
    tiling_dirty_ = true;
    workspaces_.window_removed(was);
}

void lx::compositor::window_manager::apply_hidden(toplevel_record& rec, bool hide) {
    rec.hidden = hide;
    ws_windows_changed_ = true;
    ws_configures_due_ = true;
    if (tree_) {
        if (auto* box = tree_->container(rec.surface))
            box->set_visible(!hide);
        // Its rects repaint: uncovered (what is under it shows) or covered again. A hidden
        // window's commits damage capture views only (surface_manager), so nothing of it
        // repaints an output meanwhile; frame callbacks fall to the 995 ms throttle.
        tree_->damage_root(rec.surface);
    }
    if (decorations_)
        decorations_->damage(rec.id);
    if (hide) {
        // Its menus close: nothing of it stays on screen.
        if (popups_)
            popups_->on_parent_unmapped(rec.surface, "parent-hidden");
        if (deco_press_.id == rec.id) {
            deco_press_ = {};
            if (decorations_)
                decorations_->set_button(rec.id, deco_button::normal);
        }
        if (pointer_focus_ == rec.id) {
            if (button_mask_ != 0)
                set_pointer_focus(nullptr);
            else
                refresh_pointer_focus(0, false);
        }
    }
    // Mutter sends `suspended` for minimized and other-workspace windows: the client may
    // stop rendering altogether. Out with the next flush.
    if (hide || suspend_reasons_ != 0)
        rec.desired.states |= tl_suspended;
    else
        rec.desired.states &= ~static_cast<unsigned>(tl_suspended);
    rehit_pending_ = true;
    if (log_workspace.enabled(lx::trace::level::debug)) {
        log_workspace.debug("window id={} {} workspace={} minimized={} sticky={}", rec.id.id(),
                            hide ? "hidden" : "shown", rec.workspace.id(), rec.minimized ? 1 : 0,
                            rec.sticky ? 1 : 0);
    }
}

void lx::compositor::window_manager::update_visibility() {
    if (!toplevels_)
        return;
    bool fullscreen = false;
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!rec.mapped)
            return;
        const bool hide = !shown(rec);
        if (hide == rec.hidden)
            return;
        apply_hidden(rec, hide);
        if (rec.state == lx::toplevel_state::fullscreen)
            fullscreen = true;
    });
    // A fullscreen window's backdrop hides and shows with it.
    if (fullscreen)
        sync_backdrops();
    // The keyboard never stays on a window that is not shown.
    if (const auto* f = toplevels_->find(keyboard_focus_); f && f->hidden) {
        const int slot = window_output(*f);
        lx::toplevel_id next = workspace_focus_target(slot);
        if (next == keyboard_focus_)
            next = {};
        set_keyboard_focus(next);
    }
}

void lx::compositor::window_manager::send_visibility_configures() {
    // Out now, each one carrying the focus decided after the switch too (`activated` and
    // `suspended` in one configure): a switch is an event, not a frame, and a window coming
    // back into view should draw at once. The single-in-flight rule still holds.
    if (!ws_configures_due_ || !toplevels_)
        return;
    ws_configures_due_ = false;
    flush_configures(wm_now_ns());
}

bool lx::compositor::window_manager::reveal(toplevel_record& rec, bool switch_ok,
                                            const char* source) {
    if (!rec.mapped)
        return false;
    if (rec.minimized) {
        rec.minimized = false;
        ws_windows_changed_ = true;
        if (shell_)
            shell_->emit_toplevel_state(rec);
    }
    if (!rec.sticky && rec.workspace && !workspaces_.is_active(rec.workspace)) {
        if (!switch_ok) {
            update_visibility();
            return false;
        }
        (void)switch_workspace(rec.workspace, source);
    }
    update_visibility();
    return !rec.hidden;
}

void lx::compositor::window_manager::recompute_urgent() {
    if (!toplevels_)
        return;
    bool urgent[k_max_workspaces]{};
    lx::workspace_id ids[k_max_workspaces]{};
    unsigned n = 0;
    workspaces_.for_each([&](const workspace_record& w) {
        if (n < k_max_workspaces)
            ids[n++] = w.id;
    });
    toplevels_->for_each([&](const toplevel_record& r) {
        if (!r.mapped || !r.demands_attention || !r.workspace)
            return;
        for (unsigned i = 0; i < n; ++i)
            if (ids[i] == r.workspace)
                urgent[i] = true;
    });
    for (unsigned i = 0; i < n; ++i)
        workspaces_.set_urgent(ids[i], urgent[i]);
}

void lx::compositor::window_manager::flush_workspaces() {
    if (ws_batch_ > 0)
        return;
    flush_tiling();
    send_visibility_configures();
    recompute_urgent();
    const bool model_changed = workspaces_.generation() != ws_generation_seen_;
    if (!model_changed && !ws_dirty_ && !ws_windows_changed_)
        return;
    ws_generation_seen_ = workspaces_.generation();
    ws_windows_changed_ = false;
    // ext-workspace first: a workspace just made has its handles before the shell is told a
    // window is on it.
    if (ws_observer_.changed)
        ws_observer_.changed(ws_observer_.user);
    if (!ws_dirty_ || !toplevels_)
        return;
    ws_dirty_ = false;
    toplevels_->for_each_mut([&](toplevel_record& r) {
        if (!r.workspace_dirty)
            return;
        r.workspace_dirty = false;
        if (r.mapped && shell_)
            shell_->emit_toplevel_workspace(r);
    });
}

bool lx::compositor::window_manager::switch_workspace(lx::workspace_id ws, const char* source) {
    const workspace_record* w = workspaces_.find(ws);
    if (!w || w->output < 0)
        return false;
    const int slot = w->output;
    char name[k_workspace_name_bytes];
    std::snprintf(name, sizeof(name), "%s", w->name);
    if (!workspaces_.activate(ws)) {
        flush_workspaces();
        return false;
    }
    update_visibility();
    focus_after_switch(slot);
    {
        const char* out = outputs_ && outputs_->slot_live(static_cast<unsigned>(slot))
                              ? outputs_->slot_state(static_cast<unsigned>(slot)).name
                              : "-";
        log_workspace.info("workspace activate output={} name={} id={} source={}", out, name,
                           ws.id(), source ? source : "-");
    }
    rehit_pending_ = true;
    flush_workspaces();
    return true;
}

bool lx::compositor::window_manager::switch_to_index(unsigned index_1based) {
    sync_workspace_outputs(true);
    const int slot = focused_output();
    if (slot < 0)
        return false;
    const lx::workspace_id ws = workspaces_.by_index(static_cast<unsigned>(slot), index_1based);
    return ws && switch_workspace(ws, "binding");
}

bool lx::compositor::window_manager::switch_relative(int dir) {
    sync_workspace_outputs(true);
    const int slot = focused_output();
    if (slot < 0)
        return false;
    const lx::workspace_id ws =
        workspaces_.neighbour(workspaces_.active(static_cast<unsigned>(slot)), dir);
    return ws && switch_workspace(ws, "binding");
}

bool lx::compositor::window_manager::move_to_workspace(lx::toplevel_id id, lx::workspace_id ws,
                                                       bool follow, const char* source) {
    toplevel_record* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    const workspace_record* w = workspaces_.find(ws);
    if (!rec || !rec->mapped || !w || w->output < 0)
        return false;
    // A modal dialog moves with the window it blocks (W.6): the family moves.
    for (unsigned depth = 0; depth < 8; ++depth) {
        const toplevel_record* r = toplevels_->find(id);
        if (!r || !effective_modal(*toplevels_, *r))
            break;
        id = r->parent;
    }
    rec = toplevels_->find_mut(id);
    if (!rec || !rec->mapped)
        return false;
    if (rec->workspace == ws && !rec->sticky)
        return false;
    const int to = w->output;
    const int from = window_output(*rec);
    // The user placed it.
    rec->home_identifier[0] = '\0';
    rec->home_workspace = 0;
    const auto move_one = [&](toplevel_record& r) {
        if (r.sticky) {
            r.sticky = false;
            if (shell_)
                shell_->emit_toplevel_flags(r);
        }
        if (from >= 0 && from != to)
            relocate_window(r, usable_area(from), to);
        join_workspace(r, ws);
    };
    move_one(*rec);
    // Its children (dialogs, transients) go along, to any depth.
    lx::toplevel_id family[16]{id};
    unsigned count = 1;
    for (unsigned i = 0; i < count; ++i)
        toplevels_->for_each_mut([&](toplevel_record& child) {
            if (count < 16 && child.parent == family[i] && child.mapped) {
                family[count++] = child.id;
                move_one(child);
            }
        });
    log_workspace.info("workspace move window id={} to={} id={} follow={} "
                       "source={}",
                       id.id(), w->name, ws.id(), follow ? 1 : 0, source ? source : "-");
    if (follow) {
        (void)switch_workspace(ws, source);
        set_keyboard_focus(id);
    } else {
        update_visibility();
    }
    rehit_pending_ = true;
    flush_workspaces();
    return true;
}

bool lx::compositor::window_manager::move_to_index(lx::toplevel_id id, unsigned index_1based) {
    sync_workspace_outputs(true);
    const toplevel_record* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->mapped)
        return false;
    const int slot = window_output(*rec);
    if (slot < 0)
        return false;
    const lx::workspace_id ws = workspaces_.by_index(static_cast<unsigned>(slot), index_1based);
    return ws && move_to_workspace(id, ws, workspaces_.config().move_follows, "binding");
}

bool lx::compositor::window_manager::move_relative(lx::toplevel_id id, int dir) {
    sync_workspace_outputs(true);
    const toplevel_record* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->mapped)
        return false;
    const int slot = window_output(*rec);
    if (slot < 0)
        return false;
    // From the window's own workspace (a sticky window: from the active one).
    const lx::workspace_id here =
        rec->workspace && !rec->sticky ? rec->workspace
                                       : workspaces_.active(static_cast<unsigned>(slot));
    const lx::workspace_id ws = workspaces_.neighbour(here, dir);
    return ws && move_to_workspace(id, ws, workspaces_.config().move_follows, "binding");
}

bool lx::compositor::window_manager::assign_workspace(lx::workspace_id ws, unsigned slot) {
    if (!workspaces_.assign(ws, slot, sink())) {
        flush_workspaces();
        return false;
    }
    log_workspace.info("workspace assign id={} output={}", ws.id(), slot);
    update_visibility();
    flush_workspaces();
    return true;
}

void lx::compositor::window_manager::set_minimized(lx::toplevel_id id, bool on,
                                                   const char* source) {
    toplevel_record* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    // Before the map there is nothing to minimize (xdg-shell leaves it to the compositor).
    if (!rec || !rec->mapped || rec->minimized == on)
        return;
    rec->minimized = on;
    ws_windows_changed_ = true;
    tiling_dirty_ = true;
    if (log_workspace.enabled(lx::trace::level::debug)) {
        log_workspace.debug("minimize id={} on={} source={}", id.id(), on ? 1 : 0,
                            source ? source : "-");
    }
    // A move or resize of it ends (it leaves the screen under the pointer).
    if (on && grab_.mode != grab_mode::none && grab_.toplevel == id) {
        end_grab(grab_end_reason::escape);
        rec = toplevels_->find_mut(id);
        if (!rec)
            return;
    }
    if (shell_)
        shell_->emit_toplevel_state(*rec);
    update_visibility();
    flush_workspaces();
}

void lx::compositor::window_manager::set_sticky(lx::toplevel_id id, bool on) {
    toplevel_record* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || !rec->mapped || rec->sticky == on)
        return;
    if (on) {
        leave_workspace(*rec);
        rec->sticky = true;
    } else {
        rec->sticky = false;
        const int slot = window_output(*rec);
        join_workspace(*rec, slot >= 0 ? workspaces_.active(static_cast<unsigned>(slot))
                                       : lx::workspace_id{});
    }
    rec->workspace_dirty = true;
    ws_dirty_ = true;
    ws_windows_changed_ = true;
    tiling_dirty_ = true;
    if (shell_)
        shell_->emit_toplevel_flags(*rec);
    if (log_workspace.enabled(lx::trace::level::debug)) {
        log_workspace.debug("sticky id={} on={}", id.id(), on ? 1 : 0);
    }
    update_visibility();
    flush_workspaces();
}

// ── Snapping, edge snapping, keyboard move/resize (design-session-and-wm Part K) ──

namespace {

constexpr lx::trace::category log_snap{"compositor.snap"};

[[nodiscard]] const char* snap_name(unsigned snap) {
    using namespace lx::compositor;
    switch (snap) {
    case 0: return "none";
    case edge_left: return "left";
    case edge_right: return "right";
    case edge_top | edge_left: return "top-left";
    case edge_top | edge_right: return "top-right";
    case edge_bottom | edge_left: return "bottom-left";
    case edge_bottom | edge_right: return "bottom-right";
    case k_snap_maximize: return "maximize";
    default: return "?";
    }
}

} // namespace

void lx::compositor::window_manager::set_window_config(const wm_window_config& c) {
    window_cfg_ = c;
    if (!c.snap_preview || !c.edge_snap)
        hide_snap_preview();
}

void lx::compositor::window_manager::configure_now(toplevel_record& rec) {
    rec.configure_due = true;
    if (!rec.inflight)
        send_configure(rec, wm_now_ns(), false, false);
}

void lx::compositor::window_manager::aim_maximized(toplevel_record& rec, int slot) {
    const lx::rect2i area = usable_area(slot);
    const frame_extents ext = extents_for(rec.desired.decoration, true);
    rec.snap = 0;
    rec.desired.states &= ~static_cast<unsigned>(tl_tiled_all);
    rec.desired.states |= tl_maximized;
    rec.desired.size = {area.width - ext.left - ext.right, area.height - ext.top - ext.bottom};
    rec.desired.place = true;
    rec.desired.place_at = {area.x + ext.left, area.y + ext.top};
    rec.desired.anchor_edges = 0;
    rec.configure_due = true;
}

void lx::compositor::window_manager::aim_snap(toplevel_record& rec, unsigned snap, int slot) {
    const lx::rect2i g = snap_geometry(usable_area(slot), snap,
                                       extents_for(rec.desired.decoration, false), rec.min_size,
                                       rec.max_size);
    rec.desired.states &= ~static_cast<unsigned>(tl_maximized | tl_tiled_all);
    rec.desired.states |= snap_tiled_states(snap);
    rec.desired.size = {g.width, g.height};
    rec.desired.place = true;
    rec.desired.place_at = {g.x, g.y};
    // The far edges stay where they belong whatever size the client draws (a terminal that
    // rounds to its cell grid, a minimum the half is narrower than).
    rec.desired.anchor_edges = ((snap & edge_right) ? unsigned{edge_left} : 0u) |
                               ((snap & edge_bottom) ? unsigned{edge_top} : 0u);
    rec.desired.anchor = g;
    rec.configure_due = true;
}

void lx::compositor::window_manager::set_snap(lx::toplevel_id id, unsigned snap, int slot,
                                              const char* source) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    // A tiled window's place is its layout's (ADR 012 T6).
    if (!rec || !rec->mapped || !rec->initial_configure_sent ||
        (rec->desired.states & tl_fullscreen) || rec->tiled)
        return;
    if (grab_.mode != grab_mode::none && grab_.toplevel == id) {
        end_grab(grab_end_reason::release);
        rec = toplevels_->find_mut(id);
        if (!rec)
            return;
    }
    const bool was_max = (rec->desired.states & tl_maximized) != 0;
    if (slot < 0 || !(outputs_ && outputs_->slot_live(static_cast<unsigned>(slot))))
        slot = window_output(*rec);
    if (snap == 0) {
        // Back to its normal geometry, kept reachable as an unmaximize keeps it.
        rec->snap = 0;
        rec->desired.states &= ~static_cast<unsigned>(tl_tiled_all | tl_maximized);
        rec->desired.anchor_edges = 0;
        if (rec->restore_set) {
            const frame_extents ext = extents_for(rec->desired.decoration, false);
            const lx::point2i p =
                constrain_on_output(usable_area(slot), frame_rect(rec->restore, ext));
            rec->desired.size = {rec->restore.width, rec->restore.height};
            rec->desired.place = true;
            rec->desired.place_at = {p.x + ext.left, p.y + ext.top};
            rec->restore_set = false;
        } else {
            rec->desired.size = {};
            rec->desired.place = false;
        }
    } else {
        // Its normal geometry, the first time it leaves the normal state (a snapped or
        // maximized window keeps the one it had).
        if (!rec->snap && !was_max && rec->state == lx::toplevel_state::normal) {
            rec->restore = rec->geometry;
            rec->restore_set = true;
        }
        rec->snap = snap;
        aim_snap(*rec, snap, slot);
    }
    configure_now(*rec);
    if (shell_)
        shell_->emit_toplevel_state(*rec);
    log_snap.info("snap id={} to={} output={} source={}", id.id(), snap_name(snap), slot,
                  source ? source : "-");
}

bool lx::compositor::window_manager::snap_by_key(lx::toplevel_id id, snap_key key,
                                               const char* source) {
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || !rec->mapped || rec->hidden || (rec->desired.states & tl_fullscreen))
        return false;
    const bool max = (rec->desired.states & tl_maximized) != 0;
    const snap_step st = next_snap(rec->snap, max, key);
    // Tiled: Up maximizes and Down minimizes as for any window; a half is the layout's.
    if (rec->tiled && st.move == snap_move::snap)
        return false;
    switch (st.move) {
    case snap_move::none: return false;
    case snap_move::minimize: set_minimized(id, true, source); return true;
    case snap_move::maximize: set_maximized(id, true, source); return true;
    case snap_move::restore:
        if (max)
            set_maximized(id, false, source);
        else
            set_snap(id, 0, -1, source);
        return true;
    case snap_move::snap: break;
    }
    int slot = window_output(*rec);
    if (st.output_dx != 0) {
        // Across to the neighbouring output (Windows 11): the window joins what it shows, as
        // window.move-to-output does; nothing beyond the edge, nothing happens.
        const int to = neighbour_output(slot, st.output_dx, 0);
        if (to < 0 || !move_to_output(id, st.output_dx, 0))
            return false;
        slot = to;
    }
    set_snap(id, st.snap, slot, source);
    return true;
}

void lx::compositor::window_manager::set_snap_preview_layer(scene::container_node* layer) {
    snap_preview_layer_ = layer;
    if (layer)
        (void)layer->add(&preview_);
}

lx::rect2i lx::compositor::window_manager::snap_preview_rect() const {
    return preview_.shown() ? preview_.rect() : lx::rect2i{};
}

unsigned lx::compositor::window_manager::open_edges(int slot, double x, double y) const {
    if (!outputs_ || slot < 0)
        return 0;
    const lx::rect2i o = output_area(slot);
    const auto covered = [&](double px, double py) { return output_at(*outputs_, px, py) >= 0; };
    unsigned open = 0;
    if (!covered(o.x - 0.5, y))
        open |= edge_left;
    if (!covered(o.x + o.width + 0.5, y))
        open |= edge_right;
    if (!covered(x, o.y - 0.5))
        open |= edge_top;
    if (!covered(x, o.y + o.height + 0.5))
        open |= edge_bottom;
    return open;
}

void lx::compositor::window_manager::show_snap_preview(unsigned zone, int slot) {
    if (!snap_preview_layer_ || !scene_)
        return;
    if (!preview_staged_ && tex_ops_.stage) {
        // The panel's accent (#3d6fb4): a quarter of it over what is beneath, the border
        // at 80 % — premultiplied.
        const std::uint32_t fill = 0x400F1C2Du;
        const std::uint32_t border = 0xCC315990u;
        preview_staged_ =
            tex_ops_.stage(tex_ops_.user, snap_preview::k_fill_texture, &fill, 1, 1, 1, 1, 1) &&
            tex_ops_.stage(tex_ops_.user, snap_preview::k_border_texture, &border, 1, 1, 1, 1, 1);
    }
    const lx::rect2i area = usable_area(slot);
    const lx::rect2i r = zone == k_snap_maximize ? area : snap_frame(area, zone);
    if (preview_.shown() && rect_equal(preview_.rect(), r))
        return;
    if (preview_.shown())
        scene_->note_damage(preview_.rect());
    preview_.show(r);
    scene_->note_damage(r);
    if (log_snap.enabled(lx::trace::level::debug)) {
        log_snap.debug("snap preview zone={} output={} x={} y={} w={} h={}", snap_name(zone), slot,
                       r.x, r.y, r.width, r.height);
    }
}

void lx::compositor::window_manager::hide_snap_preview() {
    if (!preview_.shown())
        return;
    if (scene_)
        scene_->note_damage(preview_.rect());
    preview_.hide();
    if (log_snap.enabled(lx::trace::level::debug))
        log_snap.debug("snap preview hidden");
}

void lx::compositor::window_manager::update_edge_zone() {
    unsigned zone = 0;
    int slot = -1;
    const auto* moving = toplevels_ ? toplevels_->find(grab_.toplevel) : nullptr;
    if (window_cfg_.edge_snap && grab_.mode == grab_mode::move && !grab_.restore_pending &&
        !(moving && moving->tiled)) {
        slot = pointer_output();
        if (slot >= 0)
            zone = edge_snap_zone(output_area(slot), grab_.now_x, grab_.now_y,
                                  open_edges(slot, grab_.now_x, grab_.now_y),
                                  window_cfg_.edge_snap_distance, window_cfg_.edge_snap_corner);
    }
    if (zone == grab_.zone && (zone == 0 || slot == grab_.zone_output))
        return;
    grab_.zone = zone;
    grab_.zone_output = zone ? slot : -1;
    if (zone && window_cfg_.snap_preview)
        show_snap_preview(zone, slot);
    else
        hide_snap_preview();
    if (log_snap.enabled(lx::trace::level::debug)) {
        log_snap.debug("edge zone id={} zone={} output={}", grab_.toplevel.id(), snap_name(zone),
                       slot);
    }
}

void lx::compositor::window_manager::drag_restore(toplevel_record& rec) {
    const int slot = pointer_output();
    const lx::rect2i area = usable_area(slot);
    // Its normal size; a window maximized before it ever mapped has none: two thirds of the
    // usable area (the client may still pick another size in its answer).
    const lx::size2i size = rec.restore_set && rec.restore.width > 0 && rec.restore.height > 0
                                ? lx::size2i{rec.restore.width, rec.restore.height}
                                : lx::size2i{area.width * 2 / 3, area.height * 2 / 3};
    const frame_extents ext = extents_for(rec.desired.decoration, false);
    const lx::rect2i frame = frame_rect(rec.geometry, extents_of(rec));
    const lx::point2i o =
        drag_restore_origin(frame, {size.width + ext.left + ext.right,
                                    size.height + ext.top + ext.bottom},
                            grab_.now_x, grab_.now_y);
    const lx::point2i at{o.x + ext.left, o.y + ext.top};
    rec.snap = 0;
    rec.restore_set = false;
    rec.desired.states &= ~static_cast<unsigned>(tl_maximized | tl_tiled_all);
    rec.desired.size = size;
    rec.desired.place = true;
    rec.desired.place_at = at;
    rec.desired.anchor_edges = 0;
    configure_now(rec);
    // The move goes on from the restored rect, the pointer where it is now.
    grab_.restore_pending = false;
    grab_.awaiting_restore = true;
    grab_.start = {at.x, at.y, size.width, size.height};
    grab_.normal = grab_.start;
    grab_.start_x = grab_.now_x;
    grab_.start_y = grab_.now_y;
    if (shell_)
        shell_->emit_toplevel_state(rec);
    log_snap.info("drag restore id={} from={} x={} y={} w={} h={}", rec.id.id(),
                  grab_.restored_max ? "maximized" : snap_name(grab_.restored_snap), at.x, at.y,
                  size.width, size.height);
}

bool lx::compositor::window_manager::begin_keyboard_grab(lx::toplevel_id id, grab_mode mode) {
    const auto* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->mapped || rec->hidden || !seat_ || !warp_)
        return false;
    const lx::rect2i f = frame_rect(rec->geometry, extents_of(*rec));
    const unsigned edges = mode == grab_mode::resize ? unsigned{edge_bottom | edge_right} : 0u;
    if (!begin_grab(id, mode, edges, grab_source::keybinding, 0, true))
        return false;
    // The pointer goes where the grab holds the window: its centre, or the corner a resize
    // starts from; motion from there drives it as a button grab's does.
    const double x = mode == grab_mode::move ? f.x + f.width / 2.0 : f.x + f.width - 1.0;
    const double y = mode == grab_mode::move ? f.y + f.height / 2.0 : f.y + f.height - 1.0;
    grab_.start_x = grab_.now_x = x;
    grab_.start_y = grab_.now_y = y;
    warp_(warp_user_, x, y);
    // Where the warp actually put it (clamped to the layout): the origin of every delta.
    grab_.start_x = grab_.now_x = seat_->pointer_x();
    grab_.start_y = grab_.now_y = seat_->pointer_y();
    grab_.motion_pending = false;
    log_snap.info("keyboard {} id={}", grab_mode_name(mode), id.id());
    return true;
}

void lx::compositor::window_manager::keyboard_grab_step(int dx, int dy) {
    auto* rec = toplevels_ ? toplevels_->find_mut(grab_.toplevel) : nullptr;
    if (!rec || !warp_ || !seat_)
        return;
    double x = grab_.now_x;
    double y = grab_.now_y;
    if (grab_.mode == grab_mode::resize) {
        // GNOME's rule: the first arrow of an axis picks that axis' edge (Left the left
        // edge…); the resize goes on from the window as it is now, the pointer on that edge.
        bool repick = false;
        if (dx != 0 && !(grab_.picked & (edge_left | edge_right))) {
            grab_.picked |= edge_left | edge_right;
            grab_.edges = (grab_.edges & ~unsigned{edge_left | edge_right}) |
                          (dx < 0 ? unsigned{edge_left} : unsigned{edge_right});
            repick = true;
        }
        if (dy != 0 && !(grab_.picked & (edge_top | edge_bottom))) {
            grab_.picked |= edge_top | edge_bottom;
            grab_.edges = (grab_.edges & ~unsigned{edge_top | edge_bottom}) |
                          (dy < 0 ? unsigned{edge_top} : unsigned{edge_bottom});
            repick = true;
        }
        if (repick) {
            if (grab_.motion_pending)
                apply_grab();
            // From the size last asked for (its configure may not be answered yet), at the
            // window's place: a resize from the right or the bottom keeps the origin.
            const lx::rect2i now{rec->geometry.x, rec->geometry.y, grab_.last_target.width,
                                 grab_.last_target.height};
            const lx::rect2i f = frame_rect(now, extents_of(*rec));
            x = (grab_.edges & edge_left) ? f.x : (grab_.edges & edge_right) ? f.x + f.width - 1.0
                                                                             : f.x + f.width / 2.0;
            y = (grab_.edges & edge_top) ? f.y : (grab_.edges & edge_bottom) ? f.y + f.height - 1.0
                                                                             : f.y + f.height / 2.0;
            grab_.start = now;
            grab_.start_x = x;
            grab_.start_y = y;
            rec->desired.anchor_edges = grab_.edges;
            rec->desired.anchor = grab_.start;
            if (cursor_)
                cursor_->set_override(shape_for_edges(grab_.edges));
        }
    }
    warp_(warp_user_, x + dx, y + dy);
}

bool lx::compositor::window_manager::quick_switch() {
    if (!toplevels_ || locked_)
        return false;
    lx::toplevel_id found{};
    toplevels_->for_each_top_down([&](const toplevel_record& r) {
        if (r.hidden || r.minimized || r.id == keyboard_focus_ || effective_modal(*toplevels_, r))
            return true;
        found = r.id;
        return false;
    });
    if (!found)
        return false;
    activate(found);
    if (log_wm.enabled(lx::trace::level::debug)) {
        log_wm.debug("quick switch id={} (no shell)", found.id());
    }
    return true;
}

// ── Tiling: master-stack per workspace (ADR 012) ───────────────────────────────

namespace {

constexpr lx::trace::category log_tile{"compositor.tile"};

} // namespace

void lx::compositor::window_manager::set_tiling_config(const tiling_config& c) {
    const bool params = !tiling_set_ || c.layout != tiling_.layout ||
                        c.master_count != tiling_.master_count ||
                        c.master_ratio != tiling_.master_ratio;
    tiling_ = c;
    tiling_.gaps = std::clamp(c.gaps, 0, 64);
    tiling_set_ = true;
    workspaces_.set_layout_defaults(tiling_, params);
    tiling_dirty_ = true;
    flush_workspaces();
}

bool lx::compositor::window_manager::tiles_on(const toplevel_record& rec,
                                              lx::workspace_id ws) const {
    const workspace_record* w = workspaces_.find(ws);
    if (!w || w->layout != workspace_layout::master_stack || w->output < 0)
        return false;
    // Dialogs, transients and fixed-size windows float (dwm, sway, Hyprland), and so does
    // what the user floated or pinned to every workspace.
    const bool fixed = rec.min_size.width > 0 && rec.min_size.height > 0 &&
                       rec.min_size.width == rec.max_size.width &&
                       rec.min_size.height == rec.max_size.height;
    return !rec.floating && !rec.parent && !rec.sticky && !rec.minimized && !fixed &&
           !rec.override_redirect && !rec.stranded;
}

lx::workspace_id lx::compositor::window_manager::focused_workspace() const {
    const int slot = focused_output();
    return slot >= 0 ? workspaces_.active(static_cast<unsigned>(slot)) : lx::workspace_id{};
}

void lx::compositor::window_manager::join_tiling(toplevel_record& rec) {
    // What it goes back to when it leaves: its normal geometry (a snapped or maximized
    // window's is kept in `restore`).
    rec.float_set = false;
    if (rec.mapped) {
        if (rec.snap || (rec.desired.states & tl_maximized)) {
            rec.float_geometry = rec.restore;
            rec.float_set = rec.restore_set;
        } else if (rec.state == lx::toplevel_state::normal) {
            rec.float_geometry = rec.geometry;
            rec.float_set = true;
        }
    }
    if (rec.snap) {
        rec.snap = 0;
        rec.restore_set = false;
    }
    rec.tiled = true;
    if (rec.mapped && shell_)
        shell_->emit_toplevel_state(rec);
}

void lx::compositor::window_manager::leave_tiling(toplevel_record& rec) {
    rec.tiled = false;
    rec.desired.states &= ~static_cast<unsigned>(tl_tiled_all);
    if (rec.desired.states & (tl_maximized | tl_fullscreen)) {
        // Unmaximized later, it goes where it floated.
        rec.restore = rec.float_geometry;
        rec.restore_set = rec.float_set;
    } else if (rec.mapped && rec.float_set) {
        const frame_extents ext = extents_for(rec.desired.decoration, false);
        const lx::point2i p = constrain_on_output(usable_area(window_output(rec)),
                                                  frame_rect(rec.float_geometry, ext));
        rec.desired.size = {rec.float_geometry.width, rec.float_geometry.height};
        rec.desired.place = true;
        rec.desired.place_at = {p.x + ext.left, p.y + ext.top};
        rec.desired.anchor_edges = 0;
    }
    rec.float_set = false;
    rec.configure_due = true;
    if (rec.initial_configure_sent)
        configure_now(rec);
    if (rec.mapped && shell_)
        shell_->emit_toplevel_state(rec);
}

void lx::compositor::window_manager::aim_tile(toplevel_record& rec, lx::rect2i frame,
                                              bool send) {
    rec.tile = frame;
    // Maximized or fullscreen over the layout: the slot is kept, the state wins (T3).
    if (rec.desired.states & (tl_maximized | tl_fullscreen))
        return;
    // Moved by the user right now: its tile waits for the drop.
    if (grab_.mode != grab_mode::none && grab_.toplevel == rec.id)
        return;
    const frame_extents ext = extents_for(rec.desired.decoration, false);
    const lx::rect2i g{frame.x + ext.left, frame.y + ext.top,
                       std::max(frame.width - ext.left - ext.right, 1),
                       std::max(frame.height - ext.top - ext.bottom, 1)};
    const bool tiled_states = (rec.desired.states & tl_tiled_all) == tl_tiled_all;
    // A placing configure still unanswered: does it take the window here or elsewhere?
    bool inflight_here = false;
    bool inflight_elsewhere = false;
    for (unsigned i = 0; i < rec.sent_count; ++i) {
        if (!rec.sent[i].place)
            continue;
        if (rec.sent[i].place_at.x == g.x && rec.sent[i].place_at.y == g.y)
            inflight_here = true;
        else
            inflight_elsewhere = true;
    }
    if (tiled_states && rec.desired.size.width == g.width &&
        rec.desired.size.height == g.height && !inflight_elsewhere) {
        // The size it has, or has been asked for: at most a move (T5).
        if (rec.desired.place) {
            rec.desired.place_at = {g.x, g.y};
            return;
        }
        if (rec.mapped && !inflight_here && (rec.geometry.x != g.x || rec.geometry.y != g.y))
            move_window(rec, {g.x, g.y}, "tile");
        return;
    }
    rec.desired.states |= tl_tiled_all;
    rec.desired.size = {g.width, g.height};
    rec.desired.place = true;
    rec.desired.place_at = {g.x, g.y};
    rec.desired.anchor_edges = 0;
    rec.desired.anchor = {};
    rec.configure_due = true;
    if (send && rec.initial_configure_sent)
        configure_now(rec);
    if (log_tile.enabled(lx::trace::level::debug)) {
        log_tile.debug("tile id={} x={} y={} w={} h={}", rec.id.id(), g.x, g.y, g.width,
                       g.height);
    }
}

void lx::compositor::window_manager::flush_tiling() {
    if (!tiling_dirty_ || !toplevels_)
        return;
    tiling_dirty_ = false;
    lx::trace::scoped_span span{"compositor.wm", "flush_tiling"};
    struct entry {
        lx::workspace_id ws{};
        long long order = 0;
        lx::toplevel_id id{};
    };
    // Fixed arrays: a flush runs on changes only, and never allocates.
    entry items[toplevel_manager::k_max];
    unsigned n = 0;
    lx::toplevel_id leaving[toplevel_manager::k_max];
    unsigned left = 0;
    toplevels_->for_each_mut([&](toplevel_record& r) {
        // Mapped, its workspace; before the map, the one its initial configure tiled it on.
        const lx::workspace_id ws = r.mapped ? r.workspace : r.tile_workspace;
        if (!ws || !tiles_on(r, ws)) {
            // Minimized: it keeps its state and gives up its place; back, it goes last.
            if (r.tiled && !r.minimized && left < toplevel_manager::k_max)
                leaving[left++] = r.id;
            r.tile_workspace = {};
            return;
        }
        if (r.tile_workspace != ws) {
            r.tile_workspace = ws;
            r.tile_order = ++tile_seq_;
        }
        if (!r.tiled)
            join_tiling(r);
        if (n < toplevel_manager::k_max)
            items[n++] = {ws, r.tile_order, r.id};
    });
    for (unsigned i = 0; i < left; ++i)
        if (auto* r = toplevels_->find_mut(leaving[i]))
            leave_tiling(*r);
    std::sort(items, items + n, [](const entry& a, const entry& b) {
        return a.ws.id() != b.ws.id() ? a.ws.id() < b.ws.id() : a.order < b.order;
    });
    lx::rect2i tiles[toplevel_manager::k_max];
    for (unsigned i = 0; i < n;) {
        unsigned j = i;
        while (j < n && items[j].ws == items[i].ws)
            ++j;
        const workspace_record* w = workspaces_.find(items[i].ws);
        if (w && w->output >= 0) {
            const unsigned count = j - i;
            master_stack_layout(usable_area(w->output), w->master_count, w->master_ratio,
                                tiling_.gaps, std::span<lx::rect2i>(tiles, count));
            for (unsigned k = 0; k < count; ++k)
                if (auto* r = toplevels_->find_mut(items[i + k].id))
                    aim_tile(*r, tiles[k], true);
        }
        i = j;
    }
    rehit_pending_ = true;
}

unsigned lx::compositor::window_manager::tiled_windows(lx::workspace_id ws,
                                                       std::span<lx::toplevel_id> out) const {
    if (!toplevels_ || !ws)
        return 0;
    struct entry {
        long long order = 0;
        lx::toplevel_id id{};
    };
    entry items[toplevel_manager::k_max];
    unsigned n = 0;
    toplevels_->for_each([&](const toplevel_record& r) {
        if (r.tiled && r.mapped && !r.minimized && r.tile_workspace == ws &&
            n < toplevel_manager::k_max)
            items[n++] = {r.tile_order, r.id};
    });
    std::sort(items, items + n, [](const entry& a, const entry& b) { return a.order < b.order; });
    const auto count = std::min<unsigned>(n, static_cast<unsigned>(out.size()));
    for (unsigned i = 0; i < count; ++i)
        out[i] = items[i].id;
    return count;
}

bool lx::compositor::window_manager::toggle_layout(int slot, const char* source) {
    if (slot < 0)
        slot = focused_output();
    if (slot < 0)
        return false;
    const lx::workspace_id ws = workspaces_.active(static_cast<unsigned>(slot));
    const workspace_record* w = workspaces_.find(ws);
    if (!w)
        return false;
    const workspace_layout to = w->layout == workspace_layout::master_stack
                                    ? workspace_layout::floating
                                    : workspace_layout::master_stack;
    (void)workspaces_.set_layout(ws, to, w->master_count, w->master_ratio);
    log_tile.info("layout workspace={} output={} layout={} source={}", ws.id(), slot,
                  layout_name(to), source ? source : "-");
    tiling_dirty_ = true;
    flush_workspaces();
    return true;
}

bool lx::compositor::window_manager::toggle_floating(lx::toplevel_id id) {
    flush_tiling();
    auto* rec = toplevels_ ? toplevels_->find_mut(id) : nullptr;
    if (!rec || !rec->mapped)
        return false;
    const workspace_record* w = workspaces_.find(rec->workspace);
    if (!w || w->layout != workspace_layout::master_stack)
        return false;
    if (rec->tiled) {
        rec->floating = true;
    } else if (rec->floating) {
        rec->floating = false;
    } else {
        return false; // a dialog, a fixed-size window: it floats by its nature
    }
    const bool floated = rec->floating;
    log_tile.info("float id={} on={}", id.id(), floated ? 1 : 0);
    tiling_dirty_ = true;
    flush_workspaces();
    if (floated)
        (void)raise(id);
    return true;
}

bool lx::compositor::window_manager::focus_tiled(int dir) {
    flush_tiling();
    lx::toplevel_id order[toplevel_manager::k_max];
    const unsigned n = tiled_windows(focused_workspace(), order);
    if (n == 0)
        return false;
    unsigned at = n;
    for (unsigned i = 0; i < n; ++i)
        if (order[i] == keyboard_focus_)
            at = i;
    const unsigned to = at == n ? 0u
                                : static_cast<unsigned>((static_cast<int>(at) + dir +
                                                         static_cast<int>(n)) %
                                                        static_cast<int>(n));
    if (order[to] == keyboard_focus_)
        return false;
    activate(order[to]);
    return true;
}

bool lx::compositor::window_manager::swap_tiled(lx::toplevel_id id, int dir) {
    flush_tiling();
    const auto* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->tiled)
        return false;
    lx::toplevel_id order[toplevel_manager::k_max];
    const unsigned n = tiled_windows(rec->tile_workspace, order);
    unsigned at = n;
    for (unsigned i = 0; i < n; ++i)
        if (order[i] == id)
            at = i;
    if (n < 2 || at == n)
        return false;
    const unsigned to = static_cast<unsigned>((static_cast<int>(at) + dir + static_cast<int>(n)) %
                                              static_cast<int>(n));
    auto* a = toplevels_->find_mut(order[at]);
    auto* b = toplevels_->find_mut(order[to]);
    std::swap(a->tile_order, b->tile_order);
    tiling_dirty_ = true;
    flush_workspaces();
    return true;
}

bool lx::compositor::window_manager::promote_tiled(lx::toplevel_id id) {
    flush_tiling();
    const auto* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->tiled)
        return false;
    lx::toplevel_id order[toplevel_manager::k_max];
    const unsigned n = tiled_windows(rec->tile_workspace, order);
    if (n < 2)
        return false;
    // dwm's zoom: the first master changes places with the focused window, or, when that is
    // the first master, with the window after it.
    const lx::toplevel_id other = order[0] == id ? order[1] : order[0];
    auto* a = toplevels_->find_mut(id);
    auto* b = toplevels_->find_mut(other);
    std::swap(a->tile_order, b->tile_order);
    tiling_dirty_ = true;
    flush_workspaces();
    return true;
}

bool lx::compositor::window_manager::adjust_master(double ratio_delta, int count_delta) {
    const lx::workspace_id ws = focused_workspace();
    const workspace_record* w = workspaces_.find(ws);
    if (!w || w->layout != workspace_layout::master_stack)
        return false;
    const double ratio =
        std::clamp(w->master_ratio + ratio_delta, k_master_ratio_min, k_master_ratio_max);
    const int count = std::clamp(static_cast<int>(w->master_count) + count_delta, 0,
                                 static_cast<int>(k_master_count_max));
    // Within a rounding step of the old ratio is no change (0.1 + 0.05 − 0.05).
    if (std::abs(ratio - w->master_ratio) < 1e-9 && count == static_cast<int>(w->master_count))
        return false;
    (void)workspaces_.set_layout(ws, w->layout, static_cast<unsigned>(count), ratio);
    if (log_tile.enabled(lx::trace::level::debug)) {
        log_tile.debug("master workspace={} ratio={} count={}", ws.id(), ratio, count);
    }
    tiling_dirty_ = true;
    flush_workspaces();
    return true;
}
