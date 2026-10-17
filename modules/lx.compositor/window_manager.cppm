module;

#include <bit>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.scene;
import lx.input;

export module lx.compositor:wm;

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
    /// Output 0's mode changed: layer surfaces are arranged again, and with them the usable
    /// area.
    void on_output_changed();
    /// The usable area changed (layer-shell exclusive zones, or the output). Windows in the
    /// normal state stay where they are (sway, KWin); maximized ones are configured to fill
    /// the new area.
    void on_usable_area_changed(lx::rect2i usable);
    /// xdg_toplevel.set_maximized / unset_maximized, zlm_policy_bridge_v1.request_maximize /
    /// request_unmaximize, a title-bar double click. Always answered by a configure (the
    /// spec: also when nothing changes). Maximized fills the usable area minus the frame;
    /// unmaximize restores the geometry it had. Position and state change with the commit
    /// that answers the configure, so the window never shows its new size at its old place.
    /// `source` is logged (`client`, `shell`, `decoration`).
    void set_maximized(lx::toplevel_id id, bool on, const char* source);
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
    bool begin_grab(lx::toplevel_id id, grab_mode mode, unsigned edges, grab_source source,
                    unsigned button);
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

    /// Output 0's geometry (single output until D5): what layer surfaces are arranged
    /// against and layer-surface popups constrained to.
    [[nodiscard]] lx::rect2i output_area() const;
    /// The output minus layer-shell exclusive zones: where windows are placed, how far a
    /// move may push them, what configure_bounds and maximize use, and what window popups
    /// are constrained to.
    [[nodiscard]] lx::rect2i usable_area() const;
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
    /// Keeps the frame of `rec` in line with it (after any geometry, mapping or mode change).
    void sync_decoration(const toplevel_record& rec);

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
    /// Last usable area logged (`usable area …` on change only).
    lx::rect2i usable_logged_{};
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

} // namespace lx::compositor


namespace {

constexpr const char* k_wm_category = "compositor.wm";

[[nodiscard]] bool wm_debug_enabled() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

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
    host.constraint_area = [](void* user, popup_parent_kind root) {
        const auto* wm = static_cast<window_manager*>(user);
        return root == popup_parent_kind::layer ? wm->output_area() : wm->usable_area();
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
    host.output_area = [](void* user) {
        return static_cast<window_manager*>(user)->output_area();
    };
    host.usable_changed = [](void* user, lx::rect2i usable) {
        static_cast<window_manager*>(user)->on_usable_area_changed(usable);
    };
    host.focus_changed = [](void* user) {
        auto* wm = static_cast<window_manager*>(user);
        // An on_demand surface that unmapped or lost its interactivity gives the keyboard
        // back.
        if (wm->layer_focus_ && !(wm->layers_ && wm->layers_->focusable(wm->layer_focus_))) {
            if (wm_debug_enabled()) {
                char line[128];
                std::snprintf(line, sizeof(line),
                              "layer focus surface=0 interactivity=none cause=unmap previous=%llu",
                              wm->layer_focus_.id());
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.layer", line);
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
        if (wm_debug_enabled()) {
            char line[128];
            std::snprintf(line, sizeof(line),
                          "layer focus surface=%llu interactivity=on_demand cause=map",
                          surface.id());
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.layer", line);
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
    return extents_for(rec.decoration_current, rec.state == lx::toplevel_state::maximized);
}

void lx::compositor::window_manager::sync_decoration(const toplevel_record& rec) {
    if (decorations_)
        decorations_->sync(rec);
}

lx::rect2i lx::compositor::window_manager::output_area() const {
    if (outputs_ && outputs_->count() > 0) {
        // Logical: windows, layers and popups are placed in layout coordinates.
        const lx::rect2i g = outputs_->logical_rect(outputs_->nth(0));
        if (!rect_empty(g))
            return g;
    }
    // Headless / no mode yet: a nominal desktop so placement still produces sane numbers.
    return {0, 0, 1920, 1080};
}

lx::rect2i lx::compositor::window_manager::usable_area() const {
    if (layers_ && layers_->arranged()) {
        const lx::rect2i u = layers_->usable();
        // Zones that ate the whole output leave nothing to place into; the output then.
        if (!rect_empty(u))
            return u;
    }
    return output_area();
}

void lx::compositor::window_manager::on_output_changed() {
    if (layers_)
        layers_->arrange(); // reports a changed usable area through the host
    else
        on_usable_area_changed(output_area());
}

void lx::compositor::window_manager::on_usable_area_changed(lx::rect2i usable) {
    if (rect_empty(usable))
        usable = output_area();
    if (!rect_equal(usable, usable_logged_)) {
        usable_logged_ = usable;
        if (wm_debug_enabled()) {
            char line[128];
            std::snprintf(line, sizeof(line), "usable area x=%d y=%d w=%d h=%d", usable.x,
                          usable.y, usable.width, usable.height);
            lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
        }
    }
    if (!toplevels_)
        return;
    rehit_pending_ = true;
    // Normal windows stay put (sway, KWin); maximized ones fill the new area.
    toplevels_->for_each_mut([&](toplevel_record& rec) {
        if (!(rec.desired.states & tl_maximized))
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
    if (wm_debug_enabled()) {
        char line[128];
        std::snprintf(line, sizeof(line), "maximize id=%llu on=%d source=%s", id.id(), on ? 1 : 0,
                      source ? source : "-");
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
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
    const bool is_max = (rec->desired.states & tl_maximized) != 0;
    if (on && !is_max) {
        if (rec->mapped && rec->state == lx::toplevel_state::normal) {
            rec->restore = rec->geometry;
            rec->restore_set = true;
        }
        const lx::rect2i area = usable_area();
        const frame_extents ext = extents_for(rec->desired.decoration, true);
        rec->desired.states |= tl_maximized;
        rec->desired.size = {area.width - ext.left - ext.right, area.height - ext.top - ext.bottom};
        rec->desired.place = true;
        rec->desired.place_at = {area.x + ext.left, area.y + ext.top};
        rec->desired.anchor_edges = 0;
    } else if (!on && is_max) {
        rec->desired.states &= ~static_cast<unsigned>(tl_maximized);
        if (rec->restore_set) {
            // Back where it was — kept reachable if the usable area changed meanwhile (a
            // panel appeared over the old title bar), as a move would be.
            const frame_extents ext = extents_for(rec->desired.decoration, false);
            const lx::point2i p = constrain_on_output(usable_area(),
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

void lx::compositor::window_manager::log_geometry(const toplevel_record& rec, const char* cause,
                                                  unsigned serial) const {
    if (!wm_debug_enabled())
        return;
    char line[192];
    std::snprintf(line, sizeof(line), "geometry id=%llu x=%d y=%d w=%d h=%d cause=%s serial=%u",
                  rec.id.id(), rec.geometry.x, rec.geometry.y, rec.geometry.width,
                  rec.geometry.height, cause, serial);
    lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
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
                break;
            }
        }
    }

    // configure_bounds only when it changed: it is a hint for the client's initial size,
    // and resending an unchanged one on every configure is noise. A server-side frame takes
    // its extents out of what the window geometry may use.
    const lx::rect2i area = usable_area();
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
        if (wm_debug_enabled()) {
            const char* req = rec.deco_requested == decoration_request::client   ? "client"
                              : rec.deco_requested == decoration_request::server ? "server"
                                                                                 : "unset";
            char line[160];
            std::snprintf(line, sizeof(line),
                          "decoration mode id=%llu requested=%s sent=%s serial=%u", rec.id.id(),
                          req, c.decoration == decoration_mode::server ? "server" : "client",
                          c.serial);
            lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco", line);
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

    if (wm_debug_enabled()) {
        char states[64];
        format_states(c.states, states, sizeof(states));
        char line[192];
        std::snprintf(line, sizeof(line),
                      "configure id=%llu serial=%u w=%d h=%d states=%s edges=%u watchdog=%d",
                      rec.id.id(), c.serial, c.size.width, c.size.height, states, c.anchor_edges,
                      watchdog ? 1 : 0);
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    }
}

void lx::compositor::window_manager::on_initial_commit(lx::toplevel_id id) {
    if (!toplevels_)
        return;
    auto* rec = toplevels_->find_mut(id);
    if (!rec)
        return;
    rec->initial_commit_seen = true;
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
        const lx::rect2i area = usable_area();
        const frame_extents ext = extents_for(rec->desired.decoration, true);
        rec->desired.states |= tl_maximized;
        rec->desired.size = {area.width - ext.left - ext.right, area.height - ext.top - ext.bottom};
        rec->desired.place = true;
        rec->desired.place_at = {area.x + ext.left, area.y + ext.top};
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
    if (toplevel_manager::consume_ack(*rec, applied)) {
        if (applied.place) {
            // Maximize / restore: the origin the configure named, with the size the client
            // drew — in this same snapshot as that content.
            rec->geometry = {applied.place_at.x, applied.place_at.y, eff.width, eff.height};
            placed = true;
        } else {
            rec->geometry = anchored_geometry(rec->geometry, applied, {eff.width, eff.height});
        }
        applied_serial = applied.serial;
        mode = applied.decoration;
        // The state the client now draws for. Only normal ↔ maximized is decided here.
        const lx::toplevel_state st = (applied.states & tl_maximized)
                                          ? lx::toplevel_state::maximized
                                          : lx::toplevel_state::normal;
        if (rec->state != st && (rec->state == lx::toplevel_state::normal ||
                                 rec->state == lx::toplevel_state::maximized)) {
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
        if (!mode_changed || !wm_debug_enabled())
            return;
        const lx::rect2i f = frame_rect(r.geometry, extents_of(r));
        char line[160];
        std::snprintf(line, sizeof(line), "decoration apply id=%llu mode=%s frame=%d,%d,%dx%d",
                      r.id.id(), r.decoration_current == decoration_mode::server ? "server"
                                                                                 : "client",
                      f.x, f.y, f.width, f.height);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco", line);
    };

    if (!rec->mapped) {
        if (info.has_buffer && !rect_empty(eff)) {
            map(*rec, placed);
            if (const auto* r = toplevels_->find(id))
                log_apply(*r);
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
    const lx::rect2i area = usable_area();

    // Stack array, not a heap one: mapping is rare, but it must not allocate either.
    // Placement is of the whole frame — a server-side title bar must land on the output too,
    // and the cascade compares frames, which is what the user sees.
    lx::rect2i existing[toplevel_manager::k_max];
    unsigned n = 0;
    toplevels_->for_each_bottom_up([&](const toplevel_record& other) {
        if (n < toplevel_manager::k_max)
            existing[n++] = frame_rect(other.geometry, extents_of(other));
    });

    const frame_extents ext = extents_of(rec);
    if (!placed) {
        const lx::rect2i frame0 = frame_rect(rec.geometry, ext);
        lx::point2i p = place_new_window(area, {frame0.width, frame0.height}, existing, n);
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
        lx::trace::logger::global().log(lx::trace::level::warn, k_wm_category,
                                        "map failed: no scene node for surface");
        return;
    }

    {
        char line[192];
        std::snprintf(line, sizeof(line), "map id=%llu app_id=%s x=%d y=%d w=%d h=%d",
                      rec.id.id(), rec.app_id ? rec.app_id : "", rec.geometry.x, rec.geometry.y,
                      rec.geometry.width, rec.geometry.height);
        lx::trace::logger::global().log(lx::trace::level::info, k_wm_category, line);
    }
    log_geometry(rec, "map", 0);
    sync_decoration(rec);

    const lx::toplevel_id id = rec.id;
    // An activation requested before the map is decided now; a refused one marks the window
    // (before the shell hears of it, so toplevel_created carries the flag).
    const bool take_focus = decide_activation_at_map(rec);
    if (shell_)
        shell_->emit_toplevel_created(rec);
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
    rehit_pending_ = true;
    stacking_dirty_ = true;
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

    {
        char line[128];
        std::snprintf(line, sizeof(line), "unmap id=%llu reason=%s", id.id(),
                      reason ? reason : "-");
        lx::trace::logger::global().log(lx::trace::level::info, k_wm_category, line);
    }

    // Focus falls to whatever is now on top — the stacking order is the only history kept.
    // With nothing left that is the null id, and the change must still go through
    // set_keyboard_focus: the router sends `leave` and the shell hears focus_changed(null),
    // or keys typed at an empty desktop keep reaching the hidden surface.
    if (keyboard_focus_ == id)
        set_keyboard_focus(toplevels_->top());
    stacking_dirty_ = true;
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
    flush_configures(now_ns);
    // O(1) unless a ping deadline passed; the focused client is pinged every 10 s.
    if (liveness_) {
        const auto* focused = toplevels_ ? toplevels_->find(keyboard_focus_) : nullptr;
        liveness_->tick(now_ns, focused ? focused->client : lx::client_id{});
    }
}

bool lx::compositor::window_manager::needs_tick() const {
    if (rehit_pending_ || stacking_dirty_ || (grab_.mode != grab_mode::none && grab_.motion_pending))
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
        if (watchdog && wm_debug_enabled()) {
            char line[128];
            std::snprintf(line, sizeof(line), "configure watchdog id=%llu serial=%u",
                          rec.id.id(), rec.last_sent.serial);
            lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
        }
        send_configure(rec, now_ns, false, watchdog);
        // A configure unanswered past the watchdog is the strongest hint of a hung client
        // there is: ask it.
        if (watchdog && liveness_)
            liveness_->request(rec.client, now_ns, ping_reason::watchdog);
    });
}

void lx::compositor::window_manager::set_keyboard_focus(lx::toplevel_id id) {
    if (!toplevels_ || id == keyboard_focus_)
        return;
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
    if (wm_debug_enabled()) {
        char line[64];
        std::snprintf(line, sizeof(line), "focus keyboard id=%llu", id.id());
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
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
    if (wm_debug_enabled()) {
        char line[96];
        std::snprintf(line, sizeof(line), "focus keyboard-surface kind=%s surface=%llu", kind,
                      surface.id());
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
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
    if (hit_layer(layer_level::overlay) || hit_layer(layer_level::top))
        return true;
    bool found = false;
    // Surface extents ∩ input region, not window geometry: a CSD client's resize margins
    // lie outside its geometry and must still receive the press (wlroots, Smithay alike).
    // Every surface of the tree counts, each clipped to its own extents — a subsurface is
    // not clipped to its parent.
    toplevels_->for_each_top_down([&](const toplevel_record& rec) {
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
    if (!wm_debug_enabled())
        return;
    char line[96];
    if (on_frame)
        // Deliberately not `focus pointer id=…`: no client has the pointer.
        std::snprintf(line, sizeof(line), "focus pointer decoration id=%llu zone=%s",
                      hit->id.id(), deco_zone_name(hit->zone));
    else if (hit && hit->owner == hit_owner::popup)
        // Deliberately not `focus pointer id=…`: the drag smoke counts those as windows.
        std::snprintf(line, sizeof(line), "focus pointer popup surface=%llu sx=%.1f sy=%.1f",
                      hit->surface.id(), hit->sx, hit->sy);
    else if (hit && hit->owner == hit_owner::layer)
        std::snprintf(line, sizeof(line), "focus pointer layer surface=%llu sx=%.1f sy=%.1f",
                      hit->surface.id(), hit->sx, hit->sy);
    else if (hit && hit->owner == hit_owner::ime_popup)
        std::snprintf(line, sizeof(line), "focus pointer ime-popup surface=%llu sx=%.1f sy=%.1f",
                      hit->surface.id(), hit->sx, hit->sy);
    else if (hit)
        std::snprintf(line, sizeof(line), "focus pointer id=%llu sx=%.1f sy=%.1f", hit->id.id(),
                      hit->sx, hit->sy);
    else
        std::snprintf(line, sizeof(line), "focus pointer none");
    lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    if (hit && !on_frame && hit->owner == hit_owner::toplevel && !(hit->surface == hit->root)) {
        std::snprintf(line, sizeof(line), "focus pointer-surface surface=%llu root=%llu",
                      hit->surface.id(), hit->root.id());
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    }
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
        // Nothing reaches a client during a grab, and an extra button does not end it.
        if (grab_.mode != grab_mode::none) {
            swallowed_mask_ |= bit;
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
        // TEMPORARY placeholder binding (Super+drag) — moves to shell policy over
        // zlm_policy_bridge_v1. Super+left moves, Super+right resizes from the edges
        // nearest the grab point. Neither press nor release reaches the client.
        if (bit && (event.evdev_code == k_btn_left || event.evdev_code == k_btn_right) &&
            seat_->keymap().named().super) {
            wm_hit h{};
            if (hit_test(seat_->pointer_x(), seat_->pointer_y(), h) && h.id) {
                swallowed_mask_ |= bit;
                set_keyboard_focus(h.id);
                (void)raise(h.id);
                const bool move = event.evdev_code == k_btn_left;
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
                    if (wm_debug_enabled()) {
                        const layer_record* l = layers_->find(layer_focus_);
                        char line[128];
                        std::snprintf(line, sizeof(line),
                                      "layer focus surface=%llu interactivity=%s cause=click",
                                      layer_focus_.id(),
                                      l && l->current.interactivity == 1 ? "exclusive"
                                                                         : "on_demand");
                        lx::trace::logger::global().log(lx::trace::level::debug,
                                                        "compositor.layer", line);
                    }
                    resolve_keyboard_focus();
                }
            } else if (pointer_owner_ != hit_owner::popup &&
                       pointer_owner_ != hit_owner::ime_popup && layer_focus_) {
                if (wm_debug_enabled()) {
                    char line[128];
                    std::snprintf(line, sizeof(line),
                                  "layer focus surface=0 interactivity=none cause=click "
                                  "previous=%llu",
                                  layer_focus_.id());
                    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.layer",
                                                    line);
                }
                layer_focus_ = {};
                resolve_keyboard_focus();
            }
        }
        // Placeholder policy until the shell decides: click-to-focus, raise-on-press, on
        // any button — focus before the button so the client sees keyboard `enter` first.
        if (pointer_focus_ && toplevels_->find(pointer_focus_)) {
            set_keyboard_focus(pointer_focus_);
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
                if (wm_debug_enabled()) {
                    char line[64];
                    std::snprintf(line, sizeof(line), "decoration close id=%llu", id.id());
                    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco",
                                                    line);
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
    if (wm_debug_enabled())
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category,
                                        locked ? "input locked (lock surfaces only)"
                                               : "input unlocked");
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
    toplevels_->for_each_mut([on](toplevel_record& rec) {
        if (on)
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
    if (wm_debug_enabled()) {
        char line[128];
        std::snprintf(line, sizeof(line), "decoration press id=%llu zone=%s button=%u", id.id(),
                      deco_zone_name(zone), event.evdev_code);
        lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco", line);
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
            if (wm_debug_enabled()) {
                char line[96];
                std::snprintf(line, sizeof(line), "decoration double-click id=%llu maximize=%d",
                              id.id(), maximized ? 0 : 1);
                lx::trace::logger::global().log(lx::trace::level::debug, "compositor.deco", line);
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
    if (!wm_debug_enabled())
        return;
    char line[128];
    std::snprintf(line, sizeof(line), "grab rejected id=%llu source=%s reason=%s", id.id(),
                  grab_source_name(source), reason);
    lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
}

bool lx::compositor::window_manager::begin_grab(lx::toplevel_id id, grab_mode mode,
                                                unsigned edges, grab_source source,
                                                unsigned button) {
    if (!toplevels_ || !surfaces_ || !seat_ || mode == grab_mode::none)
        return false;
    auto* rec = toplevels_->find_mut(id);
    const unsigned offset = button - k_button_base;
    const std::uint64_t bit =
        button >= k_button_base && offset < 64 ? (std::uint64_t{1} << offset) : 0;
    const char* reject = nullptr;
    if (grab_.mode != grab_mode::none)
        reject = "grab-active";
    else if (drag_active())
        reject = "dnd-active";
    else if (!rec || !rec->mapped)
        reject = "unmapped";
    else if (!bit || !(button_mask_ & bit))
        reject = "no-button";
    else if (rec->state != lx::toplevel_state::normal || (rec->desired.states & tl_maximized))
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

    grab_ = {};
    grab_.mode = mode;
    grab_.source = source;
    grab_.toplevel = id;
    grab_.edges = mode == grab_mode::resize ? edges : 0;
    grab_.button = button;
    grab_.start_x = grab_.now_x = seat_->pointer_x();
    grab_.start_y = grab_.now_y = seat_->pointer_y();
    grab_.start = rec->geometry;
    grab_.last_target = {rec->geometry.width, rec->geometry.height};

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

    if (wm_debug_enabled()) {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "grab begin id=%llu mode=%s edges=%u source=%s button=%u x=%d y=%d w=%d "
                      "h=%d",
                      id.id(), grab_mode_name(mode), grab_.edges, grab_source_name(source),
                      button, grab_.start.x, grab_.start.y, grab_.start.width,
                      grab_.start.height);
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    }
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
    const int dx = round_delta(grab_.now_x - grab_.start_x);
    const int dy = round_delta(grab_.now_y - grab_.start_y);
    if (grab_.mode == grab_mode::move) {
        // The frame is what must stay reachable: a server-side title bar stays on the output.
        const frame_extents ext = extents_of(*rec);
        const lx::rect2i f = frame_rect(
            {grab_.start.x + dx, grab_.start.y + dy, rec->geometry.width, rec->geometry.height},
            ext);
        const lx::point2i p = constrain_on_output(usable_area(), f);
        move_window(*rec, {p.x + ext.left, p.y + ext.top}, "move");
        return;
    }
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
    auto* rec = toplevels_ ? toplevels_->find_mut(g.toplevel) : nullptr;
    const bool live = rec && rec->mapped && (reason == grab_end_reason::release ||
                                             reason == grab_end_reason::escape);
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
    } else if (live && g.mode == grab_mode::move && reason == grab_end_reason::escape) {
        move_window(*rec, {g.start.x, g.start.y}, "cancel");
    }
    if (cursor_)
        cursor_->clear_override();

    if (wm_debug_enabled()) {
        const lx::rect2i r = rec ? rec->geometry : g.start;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "grab end id=%llu mode=%s reason=%s x=%d y=%d w=%d h=%d", g.toplevel.id(),
                      grab_mode_name(g.mode), grab_end_name(reason), r.x, r.y, r.width,
                      r.height);
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    }
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
    // The layer holds exactly the mapped toplevels' tree containers, in the same order, so
    // the toplevel's stacking index is its container's index.
    (void)windows_layer_->move_to(box, toplevels_->stacking_index(rec.id));
    // Everything that changed lies inside the restacked window's node rects: it now covers,
    // or uncovers, part of what overlapped it.
    tree_->damage_root(rec.surface);
    if (decorations_)
        decorations_->damage(rec.id);
    rehit_pending_ = true;
    stacking_dirty_ = true;
    if (wm_debug_enabled()) {
        char line[96];
        std::snprintf(line, sizeof(line), "%s id=%llu index=%u", verb, rec.id.id(),
                      toplevels_->stacking_index(rec.id));
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    }
}

bool lx::compositor::window_manager::raise(lx::toplevel_id id) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return false;
    const auto* rec = toplevels_->find(id);
    if (!rec || !rec->mapped || !toplevels_->raise(id))
        return false;
    sync_node_stacking(*rec, "raise");
    return true;
}

bool lx::compositor::window_manager::lower(lx::toplevel_id id) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return false;
    const auto* rec = toplevels_->find(id);
    if (!rec || !rec->mapped || !toplevels_->lower(id))
        return false;
    sync_node_stacking(*rec, "lower");
    return true;
}

bool lx::compositor::window_manager::restack(lx::toplevel_id id, unsigned index) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return false;
    const auto* rec = toplevels_->find(id);
    if (!rec || !rec->mapped || !toplevels_->move_to(id, index))
        return false;
    sync_node_stacking(*rec, "restack");
    return true;
}

void lx::compositor::window_manager::activate(lx::toplevel_id id) {
    const auto* rec = toplevels_ ? toplevels_->find(id) : nullptr;
    if (!rec || !rec->mapped)
        return;
    set_keyboard_focus(id);
    (void)raise(id);
}

// ── xdg-activation ─────────────────────────────────────────────────────────

namespace {

constexpr const char* k_activation_category = "compositor.activation";

void log_activation(const char* token_prefix, const lx::compositor::toplevel_record& rec,
                    lx::compositor::activation_result r, lx::compositor::activation_reason why,
                    const char* cause) {
    if (!wm_debug_enabled())
        return;
    char line[192];
    std::snprintf(line, sizeof(line), "activate token=%.8s surface=%llu id=%llu result=%s reason=%s%s",
                  token_prefix && token_prefix[0] ? token_prefix : "-", rec.surface.id(),
                  rec.id.id(), lx::compositor::activation_result_name(r),
                  lx::compositor::activation_reason_name(why), cause);
    lx::trace::logger::global().log(lx::trace::level::debug, k_activation_category, line);
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
    if (r == activation_result::granted)
        activate(id);
    else if (r == activation_result::urgent)
        set_demands_attention(*rec, true);
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
    if (wm_debug_enabled()) {
        // The token's own verdict too when the map overrode it.
        char cause[48];
        std::snprintf(cause, sizeof(cause), " cause=map%s%s",
                      why != token_reason ? " refused=" : "",
                      why != token_reason ? activation_reason_name(token_reason) : "");
        log_activation(grant ? grant->prefix : nullptr, rec, r, why, cause);
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
    if (wm_debug_enabled()) {
        char line[64];
        std::snprintf(line, sizeof(line), "urgent id=%llu on=%d", rec.id.id(), on ? 1 : 0);
        lx::trace::logger::global().log(lx::trace::level::debug, k_activation_category, line);
    }
    if (shell_)
        shell_->emit_toplevel_flags(rec);
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
