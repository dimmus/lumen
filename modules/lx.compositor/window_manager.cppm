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
import :shell_bridge;
import :output;
import :input_router;
import :xcursor;
import :cursor;

export namespace lx::compositor {

/// Interactive grab kinds. One grab at a time, seat-wide.
enum class grab_mode : unsigned char { none, move, resize };
/// Who started the grab. The executor is the same for all three; only validation differs.
enum class grab_source : unsigned char { client, shell, keybinding };
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
    /// is non-empty, v4+), `xdg_toplevel.configure` and `xdg_surface.configure(serial)`.
    void (*send_configure)(void* xdg_toplevel, void* xdg_surface, const toplevel_configure& c,
                           lx::size2i bounds, bool initial) = nullptr;
    /// Next serial in the seat's serial space, which input events share.
    unsigned (*next_serial)(void* user) = nullptr;
    /// `xdg_toplevel.close` — the shell's request_close.
    void (*send_close)(void* xdg_toplevel) = nullptr;
    void* user = nullptr;
};

/// The topmost mapped toplevel under a point, from `window_manager::hit_test`.
struct wm_hit {
    lx::toplevel_id id{};
    /// Its `wl_surface` resource (void*: no Wayland types in this partition).
    void* surface_resource = nullptr;
    /// Surface-local position of the point.
    double sx = 0.0;
    double sy = 0.0;
};

/// What a commit on a toplevel's surface carried, as far as the window manager cares.
struct toplevel_commit_info {
    /// The surface has content after this commit (false: never had any, or null attached).
    bool has_buffer = false;
    lx::size2i buffer_size{};
    /// Global node rect before the commit reached the scene, so a size change damages the
    /// rect the window used to cover.
    lx::rect2i old_node_rect{};
};

/// Window placement, configure flow and focus. UI affinity.
///
/// The executor of window-management *state*: `toplevel_record::geometry` changes only
/// here, the scene node follows it, and the shell is told. Policy (what to raise, where to
/// put things) arrives from the shell over zlm_policy_bridge_v1; until the shell drives it,
/// the defaults here are the placeholder policy.
class window_manager {
public:
    void bind(toplevel_manager& toplevels, surface_manager& surfaces, scene::scene_graph& scene,
              scene::container_node& windows_layer, input_router& router, lx::input::seat& seat,
              shell_bridge& shell, output_manager& outputs);
    void set_protocol_ops(wm_protocol_ops ops) { ops_ = ops; }

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

    /// Once per frame tick, before the scene is published.
    void on_tick(lx::runtime::clock_time now_ns);
    /// Sends every desired configure the single-in-flight rule allows.
    void flush_configures(lx::runtime::clock_time now_ns);

    void set_keyboard_focus(lx::toplevel_id id);
    [[nodiscard]] lx::toplevel_id keyboard_focus() const { return keyboard_focus_; }
    [[nodiscard]] lx::toplevel_id pointer_focus() const { return pointer_focus_; }

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
    /// during a grab cancels it, and both its press and release are withheld.
    [[nodiscard]] bool on_key(const lx::input::key_event& event);

    // ── Interactive move/resize: one executor, three entry points ──

    /// Cursor the grab overrides (move / resize shapes). Optional.
    void set_cursor(cursor_manager* cursor) { cursor_ = cursor; }
    [[nodiscard]] bool grab_active() const { return grab_.mode != grab_mode::none; }
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

    /// Topmost mapped toplevel whose surface extents and input region contain the global
    /// point. One top→bottom pass over the stacking order; stops at the first hit.
    [[nodiscard]] bool hit_test(double x, double y, wm_hit& out) const;

    /// Pointer buttons currently held (implicit grab active when non-zero).
    [[nodiscard]] unsigned buttons_down() const { return std::popcount(button_mask_); }
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

    /// Output the window manager places and constrains against (output 0 — single output
    /// until D5).
    [[nodiscard]] lx::rect2i work_area() const;

private:
    void send_configure(toplevel_record& rec, lx::runtime::clock_time now_ns, bool initial,
                        bool watchdog);
    void map(toplevel_record& rec);
    /// Moves pointer focus to `hit` (or to nothing when null), sending leave/enter.
    void set_pointer_focus(const wm_hit* hit);
    /// Re-runs the hit test at the current pointer position and moves focus if the window
    /// under it changed — "rebase", for when the scene changes under a still pointer.
    /// With `motion_if_same`, a focus that stays but whose local position moved (its
    /// window moved under the pointer) gets a `motion`.
    void refresh_pointer_focus(unsigned time_ms, bool motion_if_same);
    /// Restacks the scene node to match the toplevel order and damages it.
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

    toplevel_manager* toplevels_ = nullptr;
    surface_manager* surfaces_ = nullptr;
    scene::scene_graph* scene_ = nullptr;
    scene::container_node* windows_layer_ = nullptr;
    input_router* router_ = nullptr;
    lx::input::seat* seat_ = nullptr;
    shell_bridge* shell_ = nullptr;
    output_manager* outputs_ = nullptr;
    cursor_manager* cursor_ = nullptr;
    wm_protocol_ops ops_{};
    wm_grab grab_{};
    /// Buttons whose press never reached a client (the Super binding, presses during a
    /// grab): their release must not reach one either.
    std::uint64_t swallowed_mask_ = 0;
    /// Escape pressed to cancel a grab: its release is withheld too.
    bool swallowed_escape_ = false;
    lx::toplevel_id keyboard_focus_{};
    lx::toplevel_id pointer_focus_{};

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

/// The window geometry actually in effect: the requested rect intersected with the
/// surface extents, or the whole surface when the client never set one (xdg-shell).
[[nodiscard]] lx::rect2i effective_window_geometry(bool set, lx::rect2i requested,
                                                   lx::size2i buffer);

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
    return s == lx::compositor::grab_source::client  ? "client"
           : s == lx::compositor::grab_source::shell ? "shell"
                                                     : "keybinding";
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
                                                     lx::size2i buffer) {
    const lx::rect2i whole{0, 0, buffer.width > 0 ? buffer.width : 0,
                           buffer.height > 0 ? buffer.height : 0};
    if (!set || rect_empty(requested))
        return whole;
    const long long x0 = requested.x > 0 ? requested.x : 0;
    const long long y0 = requested.y > 0 ? requested.y : 0;
    long long x1 = static_cast<long long>(requested.x) + requested.width;
    long long y1 = static_cast<long long>(requested.y) + requested.height;
    if (x1 > whole.width)
        x1 = whole.width;
    if (y1 > whole.height)
        y1 = whole.height;
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
                                          scene::scene_graph& scene,
                                          scene::container_node& windows_layer,
                                          input_router& router, lx::input::seat& seat,
                                          shell_bridge& shell, output_manager& outputs) {
    toplevels_ = &toplevels;
    surfaces_ = &surfaces;
    scene_ = &scene;
    windows_layer_ = &windows_layer;
    router_ = &router;
    seat_ = &seat;
    shell_ = &shell;
    outputs_ = &outputs;
}

lx::rect2i lx::compositor::window_manager::work_area() const {
    if (outputs_ && outputs_->count() > 0) {
        const lx::rect2i g = outputs_->geometry(outputs_->nth(0));
        if (!rect_empty(g))
            return g;
    }
    // Headless / no mode yet: a nominal desktop so placement still produces sane numbers.
    return {0, 0, 1920, 1080};
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

    // configure_bounds only when it changed: it is a hint for the client's initial size,
    // and resending an unchanged one on every configure is noise.
    const lx::rect2i area = work_area();
    lx::size2i bounds{};
    if (rec.bounds_sent.width != area.width || rec.bounds_sent.height != area.height) {
        bounds = {area.width, area.height};
        rec.bounds_sent = bounds;
    }

    ops_.send_configure(rec.xdg_toplevel_resource, rec.xdg_surface_resource, c, bounds, initial);
    toplevel_manager::on_sent(rec, c, now_ns);
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
    rec->desired.states = tl_activated;
    send_configure(*rec, wm_now_ns(), true, false);
    rec->initial_configure_sent = true;
}

void lx::compositor::window_manager::on_toplevel_commit(lx::toplevel_id id,
                                                        const toplevel_commit_info& info) {
    if (!toplevels_ || !surfaces_ || !scene_)
        return;
    auto* rec = toplevels_->find_mut(id);
    if (!rec)
        return;

    const lx::rect2i previous = rec->geometry;
    if (info.has_buffer)
        rec->buffer_size = info.buffer_size;
    const lx::rect2i eff = effective_window_geometry(rec->window_geometry_set,
                                                     rec->window_geometry_requested,
                                                     rec->buffer_size);
    rec->geometry_offset = {eff.x, eff.y};

    // The commit that follows an ack carries the buffer for that configure, so this is
    // the one moment its anchoring can be applied without ever showing a frame whose size
    // and position disagree: origin and buffer reach the scene in the same UI call, hence
    // the same snapshot.
    toplevel_configure applied{};
    unsigned applied_serial = 0;
    if (toplevel_manager::consume_ack(*rec, applied)) {
        rec->geometry = anchored_geometry(rec->geometry, applied, {eff.width, eff.height});
        applied_serial = applied.serial;
    } else {
        rec->geometry.width = eff.width;
        rec->geometry.height = eff.height;
    }

    if (!rec->mapped) {
        if (info.has_buffer && !rect_empty(eff))
            map(*rec);
        return;
    }

    if (!info.has_buffer) {
        on_unmap(id, "null-buffer");
        // xdg-shell: the next commit is a new initial commit.
        if (auto* again = toplevels_->find_mut(id))
            toplevel_manager::reset_for_remap(*again);
        return;
    }

    const lx::point2i origin{rec->geometry.x - rec->geometry_offset.x,
                             rec->geometry.y - rec->geometry_offset.y};
    surfaces_->set_origin(rec->surface, origin);
    const lx::rect2i now_rect = surfaces_->node_rect(rec->surface);
    if (!rect_equal(now_rect, info.old_node_rect)) {
        // Separate clips: a window that moved far damages two small rects, not the bbox.
        scene_->note_damage(info.old_node_rect);
        scene_->note_damage(now_rect);
        // What is under a still pointer may have changed.
        rehit_pending_ = true;
    }
    if (!rect_equal(previous, rec->geometry)) {
        if (shell_)
            shell_->emit_toplevel_geometry(*rec);
        log_geometry(*rec, "commit", applied_serial);
    }
}

void lx::compositor::window_manager::map(toplevel_record& rec) {
    const lx::rect2i area = work_area();

    // Stack array, not a heap one: mapping is rare, but it must not allocate either.
    lx::rect2i existing[toplevel_manager::k_max];
    unsigned n = 0;
    toplevels_->for_each_bottom_up([&](const toplevel_record& other) {
        if (n < toplevel_manager::k_max)
            existing[n++] = other.geometry;
    });

    lx::point2i p = place_new_window(area, {rec.geometry.width, rec.geometry.height}, existing, n);
    p = constrain_on_output(area, {p.x, p.y, rec.geometry.width, rec.geometry.height});
    rec.geometry.x = p.x;
    rec.geometry.y = p.y;

    toplevels_->set_mapped(rec.id, true);
    if (!surfaces_->ensure_node(rec.surface)) {
        toplevels_->set_mapped(rec.id, false);
        lx::trace::logger::global().log(lx::trace::level::warn, k_wm_category,
                                        "map failed: no scene node for surface");
        return;
    }
    surfaces_->set_origin(rec.surface, {rec.geometry.x - rec.geometry_offset.x,
                                        rec.geometry.y - rec.geometry_offset.y});
    scene_->note_damage(surfaces_->node_rect(rec.surface));

    {
        char line[192];
        std::snprintf(line, sizeof(line), "map id=%llu app_id=%s x=%d y=%d w=%d h=%d",
                      rec.id.id(), rec.app_id ? rec.app_id : "", rec.geometry.x, rec.geometry.y,
                      rec.geometry.width, rec.geometry.height);
        lx::trace::logger::global().log(lx::trace::level::info, k_wm_category, line);
    }
    log_geometry(rec, "map", 0);

    const lx::toplevel_id id = rec.id;
    if (shell_)
        shell_->emit_toplevel_created(rec);
    // Placeholder policy until the shell drives focus: a newly mapped window takes the
    // keyboard. The pointer is a different matter — it enters the window only if the
    // window appeared under it, which the hit test at the next tick decides.
    set_keyboard_focus(id);
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

    scene_->note_damage(surfaces_->node_rect(rec->surface));
    surfaces_->drop_node(rec->surface);
    toplevels_->set_mapped(id, false);
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
    if (keyboard_focus_ == id)
        keyboard_focus_ = {};
    if (pointer_focus_ == id)
        pointer_focus_ = {};
    toplevels_->destroy(id);
}

void lx::compositor::window_manager::on_tick(lx::runtime::clock_time now_ns) {
    lx::trace::scoped_span span{"compositor.wm", "on_tick"};
    if (rehit_pending_) {
        rehit_pending_ = false;
        // With a button held the implicit grab owns focus; release re-hit-tests anyway.
        if (button_mask_ == 0)
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
    flush_configures(now_ns);
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
    });
}

void lx::compositor::window_manager::set_keyboard_focus(lx::toplevel_id id) {
    if (!toplevels_ || id == keyboard_focus_)
        return;
    if (auto* old = toplevels_->find_mut(keyboard_focus_))
        old->desired.states &= ~static_cast<unsigned>(tl_activated);
    keyboard_focus_ = id;
    auto* rec = toplevels_->find_mut(id);
    if (rec)
        rec->desired.states |= tl_activated;
    if (router_ && seat_)
        router_->set_keyboard_focus(rec ? rec->surface_resource : nullptr, *seat_);
    if (shell_)
        shell_->emit_focus_changed(rec);
    if (wm_debug_enabled()) {
        char line[64];
        std::snprintf(line, sizeof(line), "focus keyboard id=%llu", id.id());
        lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
    }
}

// ── Pointer focus and hit-testing ──────────────────────────────────────────

bool lx::compositor::window_manager::hit_test(double x, double y, wm_hit& out) const {
    if (!toplevels_ || !surfaces_)
        return false;
    bool found = false;
    // Surface extents ∩ input region, not window geometry: a CSD client's resize margins
    // lie outside its geometry and must still receive the press (wlroots, Smithay alike).
    toplevels_->for_each_top_down([&](const toplevel_record& rec) {
        double sx = 0.0;
        double sy = 0.0;
        if (!surfaces_->hit(rec.surface, x, y, sx, sy))
            return true; // keep looking further down
        out = {rec.id, rec.surface_resource, sx, sy};
        found = true;
        return false;
    });
    return found;
}

void lx::compositor::window_manager::set_pointer_focus(const wm_hit* hit) {
    pointer_focus_ = hit ? hit->id : lx::toplevel_id{};
    if (router_ && seat_) {
        if (hit)
            (void)router_->set_pointer_focus(hit->surface_resource, hit->sx, hit->sy, *seat_);
        else
            (void)router_->set_pointer_focus(nullptr, 0.0, 0.0, *seat_);
    }
    if (!wm_debug_enabled())
        return;
    char line[96];
    if (hit)
        std::snprintf(line, sizeof(line), "focus pointer id=%llu sx=%.1f sy=%.1f", hit->id.id(),
                      hit->sx, hit->sy);
    else
        std::snprintf(line, sizeof(line), "focus pointer none");
    lx::trace::logger::global().log(lx::trace::level::debug, k_wm_category, line);
}

void lx::compositor::window_manager::refresh_pointer_focus(unsigned time_ms,
                                                           bool motion_if_same) {
    if (!seat_ || !router_)
        return;
    wm_hit h{};
    const bool any = hit_test(seat_->pointer_x(), seat_->pointer_y(), h);
    if (!any) {
        if (pointer_focus_ || router_->pointer_focus())
            set_pointer_focus(nullptr);
        return;
    }
    if (h.id != pointer_focus_ || router_->pointer_focus() != h.surface_resource) {
        set_pointer_focus(&h);
        return;
    }
    if (motion_if_same)
        router_->send_pointer_motion(h.sx, h.sy, time_ms);
}

void lx::compositor::window_manager::on_pointer_motion(unsigned time_ms) {
    if (!seat_ || !router_ || !surfaces_ || !toplevels_)
        return;
    const double px = seat_->pointer_x();
    const double py = seat_->pointer_y();

    if (grab_.mode != grab_mode::none) {
        // O(1): the grab is applied at the tick. No client hears the pointer meanwhile.
        grab_.now_x = px;
        grab_.now_y = py;
        grab_.motion_pending = true;
        return;
    }

    if (button_mask_ == 0) {
        wm_hit h{};
        const bool any = hit_test(px, py, h);
        if (!any) {
            if (pointer_focus_ || router_->pointer_focus())
                set_pointer_focus(nullptr);
            return;
        }
        if (h.id != pointer_focus_ || router_->pointer_focus() != h.surface_resource) {
            // `enter` carries the position; a motion with the same one would be noise.
            set_pointer_focus(&h);
            return;
        }
        router_->send_pointer_motion(h.sx, h.sy, time_ms);
        return;
    }

    // Implicit grab: the surface that took the press keeps every motion until release,
    // wherever the pointer goes — coordinates outside its extents included, which is how
    // a drag-select keeps scrolling past the window edge.
    const auto* rec = toplevels_->find(pointer_focus_);
    if (!rec || !router_->pointer_focus())
        return;
    const lx::rect2i n = surfaces_->node_rect(rec->surface);
    router_->send_pointer_motion(px - n.x, py - n.y, time_ms);
}

void lx::compositor::window_manager::on_pointer_button(
    const lx::input::pointer_button_event& event) {
    if (!seat_ || !router_)
        return;
    const unsigned offset = event.evdev_code - k_button_base;
    const std::uint64_t bit =
        event.evdev_code >= k_button_base && offset < 64 ? (std::uint64_t{1} << offset) : 0;

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
        // TEMPORARY placeholder binding (Super+drag) — moves to shell policy over
        // zlm_policy_bridge_v1. Super+left moves, Super+right resizes from the edges
        // nearest the grab point. Neither press nor release reaches the client.
        if (bit && (event.evdev_code == k_btn_left || event.evdev_code == k_btn_right) &&
            seat_->keymap().named().super) {
            wm_hit h{};
            if (hit_test(seat_->pointer_x(), seat_->pointer_y(), h)) {
                swallowed_mask_ |= bit;
                set_keyboard_focus(h.id);
                (void)raise(h.id);
                const bool move = event.evdev_code == k_btn_left;
                unsigned edges = 0;
                if (const auto* rec = toplevels_->find(h.id); rec && !move)
                    edges = edges_for_point(rec->geometry, seat_->pointer_x(),
                                            seat_->pointer_y());
                (void)begin_grab(h.id, move ? grab_mode::move : grab_mode::resize, edges,
                                 grab_source::keybinding, event.evdev_code);
                return;
            }
        }
        // A press with no motion before it (a click right after a window mapped under a
        // still pointer) must still land on the window under the pointer.
        if (first)
            refresh_pointer_focus(event.time_ms, false);
        // Placeholder policy until the shell decides: click-to-focus, raise-on-press, on
        // any button — focus before the button so the client sees keyboard `enter` first.
        if (pointer_focus_) {
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
    // Pointer focus is empty during a grab anyway; say so rather than rely on it.
    if (router_ && grab_.mode == grab_mode::none)
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
    if (!event.pressed && swallowed_escape_) {
        swallowed_escape_ = false;
        return true;
    }
    return false;
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
    else if (!rec || !rec->mapped)
        reject = "unmapped";
    else if (!bit || !(button_mask_ & bit))
        reject = "no-button";
    else if (rec->state != lx::toplevel_state::normal)
        reject = "state";
    else if (mode == grab_mode::resize && (edges == edge_none || !valid_resize_edges(edges)))
        reject = "edges";
    if (reject) {
        log_grab_reject(id, source, reject);
        return false;
    }

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
    const lx::rect2i old_rect = surfaces_->node_rect(rec.surface);
    rec.geometry.x = p.x;
    rec.geometry.y = p.y;
    shift_anchors(rec, dx, dy);
    // An origin change only: the buffer, its texture and the node's bounds are untouched.
    surfaces_->set_origin(rec.surface, {rec.geometry.x - rec.geometry_offset.x,
                                        rec.geometry.y - rec.geometry_offset.y});
    // Two clips, not their bounding box: a fast drag repaints two window-sized rects.
    scene_->note_damage(old_rect);
    scene_->note_damage(surfaces_->node_rect(rec.surface));
    if (shell_)
        shell_->emit_toplevel_geometry(rec);
    log_geometry(rec, cause, 0);
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
        const lx::point2i p = constrain_on_output(
            work_area(),
            {grab_.start.x + dx, grab_.start.y + dy, rec->geometry.width, rec->geometry.height});
        move_window(*rec, p, "move");
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
    auto* node = surfaces_->node(rec.surface);
    if (!node || !windows_layer_)
        return;
    // The layer holds exactly the mapped toplevels' nodes, in the same order, so the
    // toplevel's stacking index is its node's index.
    (void)windows_layer_->move_to(node, toplevels_->stacking_index(rec.id));
    // Everything that changed lies inside the restacked window's rect: it now covers, or
    // uncovers, part of what overlapped it.
    scene_->note_damage(surfaces_->node_rect(rec.surface));
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
