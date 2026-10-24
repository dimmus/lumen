module;

// The wire objects shared by the protocol units (wire_*.cpp): every libwayland object
// record, the tables and helpers more than one unit uses, and the functions one unit
// defines and another calls. An implementation partition — not exported, not API. Each
// unit keeps its own helpers in an anonymous namespace, so only what is declared here has
// module linkage.

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif
#include <signal.h>

module lx.compositor:wire;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.gfx;
import lx.trace;
import :surface;
import :surface_tree;
import :toplevel;
import :positioner;
import :popup;
import :layer_shell;
import :protocols_p0;
import :data_transfer;
import :viewport;
import :text_input;
import :content_update;
import :capture;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

constexpr unsigned k_max_surfaces = 256;
/// wl_surface.frame callbacks one content update may carry; beyond, the oldest is answered
/// at once (a dropped callback stalls its client for good).
constexpr unsigned k_max_frame_cbs = 64;
constexpr unsigned k_max_regions = 128;

/// A frame-callback list (wire_surface.cpp): nodes in a shared pool, so a content update
/// carries three words instead of an inline array — what makes queued updates cheap.
struct frame_cb_list {
    unsigned head = 0; // pool index + 1; 0: empty
    unsigned tail = 0;
    unsigned count = 0;
};

/// linux-drm-syncobj-v1: a point on an imported timeline (`drm_timeline_table` id; 0: unset).
struct sync_point {
    unsigned timeline = 0;
    unsigned long long point = 0;
};

/// wp_content_type_v1 / wp_tearing_control_v1 values as the protocols number them.
enum : unsigned char { k_tearing_vsync = 0, k_tearing_async = 1 };

/// wp_viewport state (sticky: set once, it holds until changed or the viewport goes). The
/// source is kept as the raw 24.8 fixed values: the "unset" sentinel and the integer test
/// (bad_size) compare them exactly, never as doubles.
struct viewport_state {
    bool has_src = false;
    std::int32_t src_x = 0; // wl_fixed_t
    std::int32_t src_y = 0;
    std::int32_t src_w = 0;
    std::int32_t src_h = 0;
    bool has_dst = false;
    int dst_w = 0;
    int dst_h = 0;
};

/// One content update's worth of wl_surface state. Used three times per surface: the
/// pending state requests edit, the cached state a synchronized subsurface accumulates
/// until its parent's state is applied, and the current state.
struct surface_pending {
    wl_resource* buffer = nullptr;
    bool buffer_set = false;
    int dx = 0;
    int dy = 0;
    int scale = 1;
    int32_t transform = 0;
    /// Every wl_surface.damage since the last commit (surface-local, logical) and,
    /// separately, every damage_buffer (buffer pixels), as a few rects each. The two spaces differ as soon as
    /// a buffer scale, transform or viewport applies; each is converted into the other
    /// when the update is applied (`:viewport`).
    client_damage damage_surface{};
    client_damage damage_buffer{};
    /// wp_viewport state, merged like scale and transform.
    viewport_state viewport{};
    /// wp_alpha_modifier_v1 multiplier, wp_content_type_v1 type, wp_tearing_control_v1 hint:
    /// sticky like scale (set once, they hold until changed or their object goes).
    std::uint32_t alpha = 0xFFFFFFFFu;
    unsigned char content_type = 0;
    unsigned char tearing = k_tearing_vsync;
    /// fifo-v1 (per update): set_barrier / wait_barrier.
    bool fifo_set = false;
    bool fifo_wait = false;
    /// commit-timing-v1 (per update): the target, CLOCK_MONOTONIC ns (0: none).
    unsigned long long target_ns = 0;
    /// linux-drm-syncobj-v1 (per update): acquire and release points of the buffer attached
    /// with it. Each holds a timeline reference until used or dropped.
    sync_point acquire{};
    sync_point release{};
    /// wl_surface.frame callbacks of this update.
    frame_cb_list frame_cbs{};
    /// wp_presentation_feedback objects of this update: a list in the compositor's
    /// `presentation_feedback_table` (0: none). Moved with the update — merged into the
    /// cached state, queued when applied — like frame callbacks.
    unsigned feedback = 0;
    /// set_input_region / set_opaque_region: the region is copied when the request arrives
    /// (the wl_region may be destroyed right after) and applied with the rest.
    bool input_set = false;
    lx::compositor::input_region input{};
    bool opaque_set = false;
    lx::compositor::input_region opaque{};
    /// The wl_surface.commit that made this update (the newest one merged into it):
    /// double-buffered state another object stages for a commit (a pointer constraint's
    /// region) applies with the update that carries that commit.
    unsigned commit_seq = 0;
};

/// A wl_surface's role. Fixed for the surface's lifetime once assigned (wl_surface: a
/// surface has at most one role), which is what lets a second role request be rejected.
/// `dnd_icon`: wl_data_device.start_drag's icon (kept after the drag, as in wlroots).
/// `lock`: ext_session_lock_v1.get_lock_surface (wire_session_lock.cpp).
/// `input_popup`: zwp_input_method_v2.get_input_popup_surface (wire_input_method.cpp).
enum class surface_role : unsigned char {
    none,
    xdg_toplevel,
    cursor,
    subsurface,
    popup,
    layer,
    dnd_icon,
    lock,
    input_popup
};

struct xdg_surface_obj;
struct xdg_toplevel_obj;
struct subsurface_obj;
struct layer_surface_obj;
struct viewport_obj;
struct fractional_obj;
struct lock_surface_obj;
struct input_popup_obj;

/// A per-surface add-on object of the commit-path protocols (wp_fifo_v1, wp_commit_timer_v1,
/// wp_tearing_control_v1, wp_content_type_v1, wp_alpha_modifier_surface_v1,
/// wp_linux_drm_syncobj_surface_v1). Allocated per request, freed with its resource; inert
/// (`surface` null) once its wl_surface is gone. At most one of each kind per surface.
struct surface_addon {
    wl_resource* resource = nullptr;
    struct surface_obj* surface = nullptr;
    /// The surface pointer this addon occupies (`&surface->fifo`, …), nulled on destroy.
    surface_addon** slot = nullptr;
};

struct surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::surface_id id{};
    lx::client_id client{};
    surface_pending pending{};
    /// Every commit merges `pending` in here; `apply_cached` is the only way it becomes
    /// current. A root or desynchronized surface applies it in the same commit, a
    /// synchronized subsurface when its parent's state is applied.
    surface_pending cached{};
    bool has_cached = false;
    /// wl_surface.commit requests so far (`surface_pending::commit_seq`).
    unsigned commit_seq = 0;
    /// `buffer` is the live wl_buffer being shown, or null once the client destroyed that
    /// object — the content stays on screen regardless (`has_content`).
    surface_pending current{};
    bool used = false;
    /// The buffer whose content this surface is currently showing. Kept as an id rather
    /// than the `wl_resource*` because the client may destroy the buffer object while the
    /// content stays on screen — which is exactly the case that has to keep working.
    lx::buffer_id current_buffer_id{};
    /// Compositor texture of the current content (0 for an import-cache dma-buf), retired
    /// once nothing shows it.
    unsigned current_texture = 0;
    /// Damage of this surface's recent content updates, for buffer-age shm uploads.
    surface_damage_history damage_history{};
    /// The surface shows content: the last applied attach was non-null.
    bool has_content = false;
    /// The current buffer's format carries no alpha channel.
    bool format_opaque = false;
    surface_role role = surface_role::none;
    /// Back-pointers into the xdg tables. Every destructor nulls the others' pointers to
    /// it, because libwayland destroys a client's objects in id order — wl_surface first —
    /// and a pointer left behind addresses a slot the next surface will reuse.
    xdg_surface_obj* xdg = nullptr;
    lx::toplevel_id toplevel{};
    /// The live wl_subsurface for this surface, or null (none yet, or destroyed).
    subsurface_obj* sub = nullptr;
    /// The live zwlr_layer_surface_v1 for this surface, or null.
    layer_surface_obj* layer = nullptr;
    /// The live wp_viewport / wp_fractional_scale_v1 for this surface, or null (at most one
    /// of each: a second is `viewport_exists` / `fractional_scale_exists`).
    viewport_obj* viewport = nullptr;
    fractional_obj* fractional = nullptr;
    /// The live ext_session_lock_surface_v1 for this surface, or null (the role stays).
    lock_surface_obj* lock_surface = nullptr;
    /// The live zwp_input_popup_surface_v2 for this surface, or null (the role stays).
    input_popup_obj* input_popup = nullptr;
    /// Commit-path add-ons (wire_fifo.cpp, wire_tearing.cpp, wire_single_pixel.cpp,
    /// wire_syncobj.cpp): the live object of each kind, or null.
    surface_addon* fifo = nullptr;
    surface_addon* timer = nullptr;
    surface_addon* tearing_ctl = nullptr;
    surface_addon* content_type_ctl = nullptr;
    surface_addon* alpha_ctl = nullptr;
    surface_addon* syncobj = nullptr;
    /// fifo-v1: the barrier, set when an update with set_barrier applied, and the frame that
    /// first showed that update (cleared after the next latch of its pace output).
    bool fifo_barrier = false;
    unsigned fifo_barrier_frame = 0;
    /// Applied add-on state (what the scene and the output policy use).
    std::uint32_t alpha_current = 0xFFFFFFFFu;
    unsigned char content_type_current = 0;
    unsigned char tearing_current = k_tearing_vsync;
    /// Logical surface size of the current state (buffer / scale, transformed, or the
    /// viewport's): what roles, the tree and hit-testing use. 0x0 without content.
    lx::size2i surface_size{};
    /// Buffer size the last `surface geometry` debug line reported.
    lx::size2i logged_buffer{};
    /// wp_fractional_scale_v1.preferred_scale as last sent (120ths); 0 = never.
    std::uint32_t pref_scale120 = 0;
    /// invalid_size on a cursor or role-less surface is tolerated (wlroots; wayland#194)
    /// and logged once per surface.
    bool invalid_size_logged = false;
    /// Applied input region (infinite by default) and opaque region (empty by default).
    lx::compositor::input_region input_current{};
    lx::compositor::input_region opaque_current{};
    /// Output membership (wire_output.cpp): one bit per output slot this surface's node
    /// overlaps — what wl_surface.enter/leave have told the client.
    std::uint32_t outputs = 0;
    /// wl_surface.preferred_buffer_scale / _transform as last sent (v6), from the spec's
    /// defaults: nothing is sent while they stay 1 and normal.
    std::int32_t pref_scale = 1;
    std::uint32_t pref_transform = 0;
    /// In wl_surface's destructor: membership changes are recorded, never sent.
    bool destroying = false;
    /// CLOCK_MONOTONIC of the last wl_surface.frame callback answered for this surface:
    /// what the 995 ms throttle of a surface not on screen counts from (0: never).
    unsigned long long frame_done_ns = 0;
};

/// wl_subsurface. Inert once its surface is gone; a surface whose parent is gone keeps its
/// object (and its role) until the client destroys it.
struct subsurface_obj {
    wl_resource* resource = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
};

/// wp_viewport. Inert once its wl_surface is gone (`surface` null): every request but
/// destroy is then `no_surface`.
struct viewport_obj {
    wl_resource* resource = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
};

/// wp_fractional_scale_v1. Inert once its wl_surface is gone: nothing more is sent.
struct fractional_obj {
    wl_resource* resource = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
};

/// ext_session_lock_surface_v1 (wire_session_lock.cpp). Inert when `record` is 0: another
/// lock's, its lock ended, or its wl_surface went. Allocated per request.
struct lock_surface_obj {
    wl_resource* resource = nullptr;
    surface_obj* surface = nullptr;
    /// The `session_lock` record id (0: inert).
    unsigned record = 0;
    unsigned output = 0;
    /// The size of the last configure sent, and where the surface is placed (its output's
    /// logical origin).
    lx::size2i sent{};
    lx::point2i origin{};
};

/// zwp_input_popup_surface_v2 (wire_input_method.cpp): the relay's record and the wl_surface.
/// Inert once its surface or its input method is gone. Allocated per request.
struct input_popup_obj {
    lx::compositor::input_popup_record record{};
    surface_obj* surface = nullptr;
};

struct region_obj {
    wl_resource* resource = nullptr;
    lx::compositor::input_region region{};
    bool used = false;
};

struct buffer_obj {
    wl_resource* resource = nullptr;
    bool is_shm = false;
    bool is_dmabuf = false;
    /// wp_single_pixel_buffer_v1: 1x1, `solid` (premultiplied, encoded); never uploaded.
    bool is_single_pixel = false;
    lx::color solid = lx::color::rgb(0.f, 0.f, 0.f, 0.f);
    /// linux-drm-syncobj-v1 release point of the newest commit that applied this buffer,
    /// signalled when the buffer lifecycle retires it, after the newest composite fence. A
    /// re-commit of the buffer signals the older point at once: the spec lets a client reuse
    /// a buffer only once no commit holds it, so the newer one still guards it.
    sync_point release{};
    lx::buffer_id id{};
    unsigned width = 0;
    unsigned height = 0;
    lx::gfx::dmabuf_desc dmabuf{};
    /// What has been staged for this buffer's texture (GL path, buffer-age uploads).
    buffer_upload_state upload{};
    /// A screen capture imported it on the render thread (wire_capture.cpp): the import is
    /// dropped when the buffer goes.
    bool capture_imported = false;
    bool used = false;
};

/// Destroy listener for a wl_shm buffer, parallel to `buffers` by index. libwayland owns
/// those resources and their destructor — it frees the wl_shm_buffer and drops its pool
/// reference — so the compositor listens instead of replacing it. (Replacing it, as this
/// file once did, leaked every shm buffer struct and kept its pool mapped for good.)
struct shm_buffer_listener {
    wl_listener listener{}; // first member: the notify casts back to this struct
    struct buffer_obj* owner = nullptr;
};

/// xdg_wm_base, per bound resource: the xdg_surfaces created from it are counted, because
/// destroying it while any is alive is `defunct_surfaces`, and popup errors
/// (invalid_positioner, invalid_popup_parent, not_the_topmost_popup) are posted on it.
struct xdg_wm_base_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    /// The owning client, for liveness pings (wire_xdg_shell.cpp).
    lx::client_id client{};
    /// The last liveness ping sent on this object: when the object goes before its pong,
    /// the ping is cancelled (nothing can answer it any more).
    unsigned ping_serial = 0;
    unsigned surfaces = 0;
    bool used = false;
};

struct xdg_popup_obj;
struct decoration_obj;

struct xdg_surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    surface_obj* surface = nullptr;
    xdg_toplevel_obj* toplevel = nullptr;
    xdg_popup_obj* popup = nullptr;
    /// The xdg_wm_base it came from; null once that is gone (only at client teardown).
    xdg_wm_base_obj* wm_base = nullptr;
    bool used = false;
};

/// xdg_positioner: rules only, copied into a popup at get_popup / reposition.
struct positioner_obj {
    wl_resource* resource = nullptr;
    lx::compositor::positioner_rules rules{};
    bool used = false;
};

/// xdg_popup. The record lives in `popup_manager`, keyed by the surface id; this is the wire
/// half, inert once the record is gone.
struct xdg_popup_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_surface_obj* xdg_surface = nullptr;
    lx::surface_id surface{};
    bool used = false;
};

struct xdg_toplevel_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_surface_obj* xdg_surface = nullptr;
    /// The compositor-side record; null once any of the three objects is gone.
    lx::toplevel_id id{};
    /// The live zxdg_toplevel_decoration_v1 for this toplevel, or null.
    decoration_obj* deco = nullptr;
    char title[256]{};
    char app_id[256]{};
    bool used = false;
};

/// zwlr_layer_surface_v1. The record lives in `layer_shell`, keyed by the surface id; this
/// is the wire half, inert once its wl_surface is gone.
struct layer_surface_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
};

/// zxdg_toplevel_decoration_v1. Inert once its toplevel is gone (only at client teardown:
/// destroying the toplevel first is `orphaned`).
struct decoration_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    xdg_toplevel_obj* toplevel = nullptr;
    bool used = false;
};

struct dmabuf_params_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
    lx::gfx::dmabuf_desc desc{};
    bool used = false;
};

struct seat_obj {
    wl_resource* resource = nullptr;
    p0_protocol_context* ctx = nullptr;
};

// ── Data transfer: clipboard (wl_data_*) and primary selection (zwp_primary_selection_*) ──
// Both families share one model (wire_selection.cpp); each family's unit only marshals.

enum class xfer_family : unsigned char { clipboard, primary };
/// `control`: an offer made to a data-control device (ext/wlr data control) — it does not
/// go inert when its client loses keyboard focus, only when the selection moves on.
enum class offer_use : unsigned char { selection, dnd, control };
enum class source_role : unsigned char { none, selection, dnd };

constexpr unsigned k_xfer_families = 2;
constexpr unsigned k_max_xfer_offers = 512;
/// Live offers one client may hold: one that never destroys them would otherwise fill the
/// shared table and starve every other client of the selection.
constexpr unsigned k_max_offers_per_client = 32;

struct xfer_offer;

/// How a source is told to write (`send`) and that it lost the selection (`cancelled`):
/// what differs between a wl_data_source, a primary-selection source and a data-control
/// source, any of which may own either selection.
struct xfer_source_ops {
    void (*send_send)(wl_resource* source, const char* mime, int32_t fd) = nullptr;
    void (*send_cancelled)(wl_resource* source) = nullptr;
};

/// wl_data_source / zwp_primary_selection_source_v1 / ext_data_control_source_v1 /
/// zwlr_data_control_source_v1. Allocated per request, freed with its object
/// (wire_selection.cpp).
struct xfer_source {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    /// The selection it was set as (a data-control source: whichever set_* used it).
    xfer_family family{};
    /// Null: the family's own source (`xfer_ops[family]`).
    const xfer_source_ops* ops = nullptr;
    lx::compositor::mime_list mimes; // default-initialized: the arena is never zeroed
    /// wl_data_source.set_actions (v3), meaningful for drag-and-drop only.
    unsigned dnd_actions = 0;
    bool actions_set = false;
    /// Given to set_selection or start_drag: a source is usable once.
    bool used = false;
    bool mime_overflow_logged = false;
    source_role role = source_role::none;
    /// Drag-and-drop negotiation state (wire_dnd).
    bool accepted = false;
    unsigned action = 0;
    bool dropped = false;
    bool finished = false;
    /// Intrusive list of the live offers of this source.
    xfer_offer* offers = nullptr;
    bool live = false;
};

/// wl_data_offer / zwp_primary_selection_offer_v1. Inert once `source` is null: the
/// selection changed, its client lost keyboard focus, or the source is gone.
struct xfer_offer {
    wl_resource* resource = nullptr;
    xfer_source* source = nullptr;
    wl_client* client = nullptr;
    xfer_family family{};
    offer_use use{};
    /// wl_data_offer.set_actions (v3, drag-and-drop offers).
    unsigned actions = 0;
    unsigned preferred = 0;
    /// Last wl_data_offer.action sent (v3): the event goes out on change only.
    unsigned action = 0;
    bool in_ask = false;
    bool finished = false;
    xfer_offer* prev = nullptr;
    xfer_offer* next = nullptr;
    bool live = false;
};

/// wl_data_device / zwp_primary_selection_device_v1. Nothing holds one across calls:
/// delivery scans the live ones of the focused client. Allocated per request, freed with
/// its object, linked into one list (wire_selection.cpp).
struct xfer_device {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    xfer_family family{};
    bool live = false;
    xfer_device* prev = nullptr;
    xfer_device* next = nullptr;
};

/// What differs between the two families: the offer interface and the events. Filled by
/// wire_data_device.cpp and wire_primary_selection.cpp at install.
struct xfer_family_ops {
    const char* name = "";
    const wl_interface* offer_interface = nullptr;
    const void* offer_impl = nullptr;
    void (*send_data_offer)(wl_resource* device, wl_resource* offer) = nullptr;
    void (*send_offer)(wl_resource* offer, const char* mime) = nullptr;
    void (*send_selection)(wl_resource* device, wl_resource* offer) = nullptr;
    void (*send_cancelled)(wl_resource* source) = nullptr;
    void (*send_send)(wl_resource* source, const char* mime, int32_t fd) = nullptr;
};

// ── Shared helpers ─────────────────────────────────────────────────────────

/// The wl_surface's own record — its resource user data, O(1).
inline surface_obj* surface_from(wl_resource* res) {
    if (!res)
        return nullptr;
    auto* s = static_cast<surface_obj*>(wl_resource_get_user_data(res));
    return s && s->used && s->resource == res ? s : nullptr;
}

extern p0_protocol_context* g_p0_ctx;

/// The surface record of a surface id, through the surface manager's resource registry.
inline surface_obj* surface_by_id(lx::surface_id id) {
    if (!g_p0_ctx || !g_p0_ctx->surfaces || !id)
        return nullptr;
    return surface_from(static_cast<wl_resource*>(g_p0_ctx->surfaces->resource(id)));
}

/// X7: a client being torn down has already left the server's table (its destroy signal
/// fires before libwayland destroys its objects, in id order). Destructors must not post
/// protocol errors to it, nor do work on its behalf.
inline bool client_dying(wl_client* client) {
    return !g_p0_ctx || !g_p0_ctx->server || !client ||
           g_p0_ctx->server->find_client_by_native(client) == nullptr;
}

[[nodiscard]] inline bool surface_debug() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

inline void surface_log(const char* line) {
    lx::trace::logger::global().log(lx::trace::level::debug, "compositor.surface", line);
}

template <typename... Args>
void surface_log(std::format_string<Args...> fmt, Args&&... args) {
    lx::trace::logger::global().logf(lx::trace::level::debug, "compositor.surface", fmt,
                                     std::forward<Args>(args)...);
}

/// Every wl_surface record (wire_surface.cpp). Scanned by buffer and frame-callback
/// destructors, which must drop references to a dying object.
extern surface_obj surfaces[k_max_surfaces];

// ── wire_buffer.cpp ────────────────────────────────────────────────────────

buffer_obj* alloc_buffer();
void retire_texture(unsigned texture_id);
void destroy_buffer(buffer_obj* b);
lx::fourcc shm_format_to_fourcc(uint32_t shm_format);
lx::fourcc staged_shm_format(wl_shm_buffer* shm);
/// GL client-upload path: registers `region` of the buffer for the render thread to read
/// in place. Sets `pixels` (the region's first byte) and `stride`; returns the
/// `shm_direct_uploads` handle, or 0 to stage instead.
[[nodiscard]] unsigned direct_shm_upload(wl_resource* buffer, wl_shm_buffer* shm, buffer_obj* buf,
                                         lx::rect2i region, const unsigned char*& pixels,
                                         unsigned& stride);
/// Binds `ctx.shm_direct` to libwayland (access guards, release, pool unref).
void bind_shm_direct(p0_protocol_context& ctx);
/// Copies the commit's shm pixels into compositor-owned staging (see the definition).
/// `history`/`surface`: the committing surface's damage, with which the CPU path's
/// per-texture store rewrites only what changed in the slot it reuses.
const unsigned char* stage_shm_pixels(buffer_obj* buf, wl_shm_buffer* shm, unsigned texture_id,
                                      lx::rect2i region, unsigned long long& seq_out,
                                      unsigned long long& token_out,
                                      unsigned long long& pool_token_out,
                                      const surface_damage_history* history = nullptr,
                                      lx::surface_id surface = {});
lx::buffer_id next_buffer_id();
void buffer_destroy_resource(struct wl_resource* resource);
/// The dma-buf wl_buffer implementation. One definition: `find_buffer` tells the buffers
/// created here from libwayland's shm buffers by its address.
extern const struct wl_buffer_interface buffer_impl;
buffer_obj* find_buffer(wl_resource* res);
buffer_obj* track_shm_buffer(wl_resource* res, wl_shm_buffer* shm);
bool stage_image(void* user, unsigned texture_id, const uint32_t* argb, unsigned width,
                 unsigned height, unsigned stride_px, unsigned canvas_w, unsigned canvas_h);
void retire_image(void*, unsigned texture_id);

// ── wire_surface.cpp ───────────────────────────────────────────────────────

/// The surface's slot in `surfaces` + 1: its content queue key.
[[nodiscard]] inline unsigned surface_slot(const surface_obj* s) {
    return static_cast<unsigned>(s - surfaces) + 1u;
}
void apply_cached(surface_obj* s, const char* cause, unsigned depth);
/// Geometry inputs (`:viewport`) of the state a wl_surface.commit of `s` would produce now
/// (pending over cached over current), and of its current state.
[[nodiscard]] lx::compositor::surface_geometry_in pending_geometry(const surface_obj* s);
[[nodiscard]] lx::compositor::surface_geometry_in current_geometry(const surface_obj* s);
void flush_desync(surface_obj* s, const char* cause);
void discard_cached(surface_obj* s);
/// Answers every frame callback of `list` now and empties it (a dropped update's: "done"
/// rather than never, which would stall the client).
void frame_cbs_fire_now(frame_cb_list& list);
/// Drops the update-scoped references a state holds without applying it: its frame
/// callbacks (answered unless `dying`), presentation feedback (discarded), sync points
/// (released points signalled at once: the buffer was never sampled) and its buffer
/// (released unless it is what the surface shows, or `dying`).
void drop_state(surface_obj* s, surface_pending& st, bool dying);

// ── the content-update queue (wire_surface.cpp) ────────────────────────────

/// UI hooks into the compositor's clocks (`p0_protocol_context::cu_*`): kept here so the
/// queue code reads one place.
[[nodiscard]] content_queue& cu_queue();
/// The readiness poller's epoll fd (opened on first use; -1 when it cannot be).
[[nodiscard]] int cu_poll_fd();
/// Tries every surface with queued updates (a fence signalled, a barrier cleared, a tick
/// reached a target, visibility changed). Applies what is ready, in order.
void cu_try_all(const char* cause);
/// The poller's fd became readable: clears the signalled waits, then tries their surfaces.
void cu_poll();
/// Clears the fifo barriers output `slot` latched in frame `frame` (and every barrier of a
/// surface not on screen), then tries the queues.
void cu_after_latch(unsigned slot, unsigned frame);
/// Outputs whose clock must keep running for a queued update (a fifo wait of a shown
/// surface): bit per pace output.
[[nodiscard]] std::uint32_t cu_waiting_outputs();
/// The earliest commit-timing target waiting per output (0: none), for `request_at`.
[[nodiscard]] unsigned long long cu_earliest_target(unsigned slot);
/// A surface is going: its queued updates are discarded (`dying`: its client is being torn
/// down, nothing is sent).
void cu_surface_destroyed(surface_obj* s, bool dying);
/// A wl_buffer is going: queued updates attaching it keep their other state.
void cu_buffer_gone(wl_resource* buffer);
/// Explicit sync: the buffer lifecycle retired `buffer` — its release points are signalled
/// after the newest composite fence.
void syncobj_buffer_released(wl_resource* buffer);
/// Explicit sync: a release point held without its wl_buffer (`release_sync_point`) —
/// signalled after the newest composite fence when `signal`, else only dropped.
void syncobj_point_settle(unsigned timeline, unsigned long long point, bool signal);
/// Sends wl_buffer.release for a buffer that is given back unsampled (a refused import, a
/// role that keeps no content), signalling its explicit-sync release point first.
void buffer_release_now(wl_resource* buffer);
void bind_compositor(wayland::client_connection& client, unsigned id, int version);
/// `fire_frames`' output argument: every output.
inline constexpr unsigned k_any_output = ~0u;
/// Answers pending frame callbacks: all of them (`throttle` false), or those of surfaces
/// output `slot` paces (`k_any_output`: shown on any output) plus the throttled ones that
/// are due (see `fire_output_frame_callbacks`).
void fire_frames(unsigned frame_time_ms, bool throttle, unsigned slot, unsigned long long now_ns);
[[nodiscard]] bool frames_pending();
/// The outputs whose ticks the pending (not throttled) callbacks wait on: bit per pace
/// output; ~0 when one of them is not paced by a single output.
[[nodiscard]] std::uint32_t frames_pending_outputs();
[[nodiscard]] bool frames_throttled();
/// Whether `surface` was on screen on output `slot` in published frame `frame` (its scene
/// node's visibility-pass mark for that view).
[[nodiscard]] bool surface_visible(lx::surface_id surface, unsigned frame, unsigned slot);
/// The outputs `surface` is on screen on now: bit per view whose newest published frame
/// showed it.
[[nodiscard]] std::uint32_t surface_shown_mask(lx::surface_id surface);
/// The surface's pace output (ADR 008 M8, `pace_output` over its shown mask and node rect);
/// -1 when it is shown nowhere.
[[nodiscard]] int surface_pace_output(lx::surface_id surface);

// ── wire_presentation.cpp (wp_presentation) ────────────────────────────────

/// wp_presentation (a P1 protocol; defined only when its glue exists — wire_install.cpp
/// calls it under the same `LUMEN_PROTOCOL_*` guard).
void bind_presentation(wayland::client_connection& client, unsigned id, int version);
/// The content-update hooks of wl_surface (wire_surface.cpp calls them; no-ops without a
/// feedback table): cached → current, and the surface going away — destroyed
/// (`presentation_surface_destroyed`: its pending and cached feedback too) or unmapped
/// (`presentation_surface_unmapped`: what is in flight). Merges go through
/// `presentation_merge_lists`.
void presentation_apply(surface_obj* s);
void presentation_surface_destroyed(surface_obj* s);
void presentation_surface_unmapped(lx::surface_id surface);
/// A cached update is dropped unapplied (`discard_cached`): its feedback is discarded.
void presentation_drop_cached(surface_obj* s);

/// Discards a feedback list of a content update dropped unapplied (a queued one).
void presentation_drop_list(unsigned& list);
/// Merges update `from`'s feedback into `into`'s (whose own is superseded).
void presentation_merge_lists(unsigned& from, unsigned& into);

// ── commit-path protocols: wire_fifo.cpp (fifo-v1, commit-timing-v1), wire_tearing.cpp
//    (tearing-control-v1, content-type-v1), wire_single_pixel.cpp (single-pixel-buffer-v1,
//    alpha-modifier-v1), wire_syncobj.cpp (linux-drm-syncobj-v1) ───────────────

void bind_fifo_manager(wayland::client_connection& client, unsigned id, int version);
void bind_commit_timing_manager(wayland::client_connection& client, unsigned id, int version);
void bind_tearing_control_manager(wayland::client_connection& client, unsigned id, int version);
void bind_content_type_manager(wayland::client_connection& client, unsigned id, int version);
void bind_single_pixel_buffer_manager(wayland::client_connection& client, unsigned id,
                                      int version);
void bind_alpha_modifier(wayland::client_connection& client, unsigned id, int version);
void bind_syncobj_manager(wayland::client_connection& client, unsigned id, int version);
/// A wl_surface is going: every add-on object of it goes inert.
void surface_addons_destroyed(surface_obj* s);
/// Creates the add-on resource for `s` and links it into `*slot` (wire_fifo.cpp). Null
/// after posting no_memory. `destroy` must end in `addon_free`.
surface_addon* addon_create(wl_client* client, const wl_interface* iface, int version,
                            uint32_t id, surface_obj* s, surface_addon** slot, const void* impl,
                            void (*destroy)(wl_resource*));
/// The add-on of a resource created by `addon_create` (null for none).
[[nodiscard]] surface_addon* addon_from(wl_resource* resource);
/// Unlinks the add-on from its surface (if any) and frees it.
void addon_free(surface_addon* a);
/// wl_surface.commit precheck of explicit sync (wire_syncobj.cpp): unsupported_buffer,
/// no_buffer, no_acquire_point, no_release_point, conflicting_points, in wlroots' order.
/// False when an error was posted. True without a syncobj surface object.
bool syncobj_precheck(surface_obj* s);
/// The explicit-sync state: the timeline table (null when the global is not offered).
[[nodiscard]] lx::gfx::drm_timeline_table* syncobj_timelines();
/// Releases a sync point's timeline reference (and clears it).
void sync_point_drop(sync_point& p);
/// Signals a release point now (the buffer was never sampled, or the composite is done)
/// and drops it.
void sync_point_signal_now(sync_point& p);

// ── wire_viewporter.cpp, wire_fractional_scale.cpp ──────────────────────────

/// wp_viewporter / wp_fractional_scale_manager_v1 (P1 protocols; defined only when their
/// glue exists — wire_install.cpp calls them under the same `LUMEN_PROTOCOL_*` guard).
void bind_viewporter(wayland::client_connection& client, unsigned id, int version);
void bind_fractional_scale(wayland::client_connection& client, unsigned id, int version);
/// wl_surface commit precheck: `bad_size` / `out_of_buffer` on the state this commit
/// produces (wlroots checks at client commit too). False when an error was posted.
bool viewport_precheck(surface_obj* s);
/// A wl_surface is going: its viewport and fractional-scale objects go inert.
void viewport_surface_destroyed(surface_obj* s);
/// Sends wp_fractional_scale_v1.preferred_scale when `scale120` differs from what the
/// surface was last told (no-op without a fractional-scale object or its glue).
void fractional_scale_update(surface_obj* s, std::uint32_t scale120);

// ── wire_session_lock.cpp (ext-session-lock-v1) ────────────────────────────

/// ext_session_lock_manager_v1 (defined only when its glue exists — wire_install.cpp calls it
/// under the same `LUMEN_PROTOCOL_*` guard).
void bind_session_lock_manager(wayland::client_connection& client, unsigned id, int version);
/// wl_surface commit precheck of a lock surface: commit_before_first_ack, null_buffer,
/// dimensions_mismatch on the state the commit produces. False when an error was posted.
bool lock_surface_precheck(surface_obj* s);
/// A lock surface's commit was applied: it maps into the lock layer (at its output's
/// origin) and counts as content for the lock.
void lock_surface_committed(surface_obj* s);
/// The lock surface's content is drawn (its object lives and belongs to the lock in charge).
[[nodiscard]] bool lock_surface_drawable(const surface_obj* s);
/// A wl_surface with a live lock surface object is going (client teardown only — a
/// destroy request is defunct_role_object): the object goes inert.
void lock_surface_surface_destroyed(surface_obj* s);
/// Outputs changed: lock surfaces are configured to their output's new logical size and
/// placed at its origin (wire_output.cpp calls it; a no-op without the glue).
void session_lock_outputs_changed();

// ── wire_text_input.cpp, wire_input_method.cpp (text-input-v3, input-method-v2, virtual
//    keyboard v1) ─────────────────────────────────────────────────────────────

/// zwp_text_input_manager_v3 v2 / zwp_input_method_manager_v2 v1 /
/// zwp_virtual_keyboard_manager_v1 v1 (defined only when their glue exists — wire_install.cpp
/// calls them under the same `LUMEN_PROTOCOL_*` guards).
void bind_text_input_manager(wayland::client_connection& client, unsigned id, int version);
void bind_input_method_manager(wayland::client_connection& client, unsigned id, int version);
void bind_virtual_keyboard_manager(wayland::client_connection& client, unsigned id, int version);
/// The relay's sink (both halves), the router's focus hook and the seat keymap the router
/// restores clients to. A no-op without a relay.
void install_text_input(p0_protocol_context& ctx);
/// The zwp_text_input_v3 half of the relay's sink (wire_text_input.cpp; no-op without glue).
void text_input_sink_events(lx::compositor::text_input_sink& sink);
/// wl_surface hooks: an input popup's commit was applied (it shows or hides, and is placed);
/// whether its content is drawn; its surface is going (the object goes inert).
void input_popup_committed(surface_obj* s);
[[nodiscard]] bool input_popup_drawable(const surface_obj* s);
void input_popup_surface_destroyed(surface_obj* s);
/// A wl_surface's state was applied: v2 cursor rectangles staged for it become current.
void text_input_surface_applied(surface_obj* s);
/// `surface`'s node moved (the tree's rect hook) / the outputs changed: popups follow.
void text_input_surface_moved(lx::surface_id surface);
void text_input_outputs_changed();
/// Topmost shown input popup under the global point (the window manager's hit-test).
[[nodiscard]] bool input_popup_hit_impl(double x, double y, void*& surface_res,
                                        lx::surface_id& surface, double& sx, double& sy);

// ── wire_subsurface.cpp ────────────────────────────────────────────────────

void bind_subcompositor(wayland::client_connection& client, unsigned id, int version);

// ── wire_xdg_shell.cpp, wire_xdg_popup.cpp, wire_decoration.cpp, wire_layer_shell.cpp,
//    wire_dmabuf.cpp, wire_cursor_shape.cpp (protocol glue) ─────────────────

wl_resource* wm_base_resource(const xdg_surface_obj* xs);
#if defined(LUMEN_HAS_PROTOCOL_GLUE)
xdg_toplevel_obj* toplevel_of(wl_resource* resource);
lx::compositor::toplevel_record* record_of(xdg_toplevel_obj* t);
xdg_surface_obj* xdg_surface_from(wl_resource* resource);
positioner_obj* positioner_from(wl_resource* resource);
xdg_popup_obj* popup_from(wl_resource* resource);
void wm_send_configure(void* toplevel_res, void* xdg_surface_res,
                       const lx::compositor::toplevel_configure& c, lx::size2i bounds,
                       bool initial, void* decoration_res);
void wm_send_close(void* toplevel_res);
unsigned wm_next_serial(void* user);
void bind_xdg_wm_base(wayland::client_connection& client, unsigned id, int version);

void popup_send_configure(void* popup_res, void* xdg_surface_res, lx::rect2i rel,
                          unsigned serial, bool repositioned, unsigned token);
void popup_send_done(void* popup_res);
bool popup_client_alive(void* client);
void xdg_surface_get_popup(struct wl_client* client, struct wl_resource* resource, uint32_t id,
                           struct wl_resource* parent_res, struct wl_resource* positioner_res);

void bind_decoration_manager(wayland::client_connection& client, unsigned id, int version);

void layer_send_configure(void* layer_res, unsigned serial, int width, int height);
void layer_send_closed(void* layer_res);
void bind_layer_shell(wayland::client_connection& client, unsigned id, int version);

void bind_linux_dmabuf(wayland::client_connection& client, unsigned id, int version);

void shape_devices_drop_pointer(wl_resource* pointer);
void bind_cursor_shape(wayland::client_connection& client, unsigned id, int version);
#endif

// ── wire_seat.cpp, wire_shell.cpp, wire_output.cpp, wire_data_device.cpp ───

bool cursor_request_allowed(wl_client* client, uint32_t serial);
/// The seat's keymap as every wl_keyboard is sent it: a sealed memfd owned for the process
/// lifetime. False when there is none (no keyboard is advertised then).
bool seat_keymap(int& fd, unsigned& size);
/// The seat's keymap was replaced: the next `seat_keymap` serializes the new one.
void seat_keymap_invalidate();
void bind_seat(wayland::client_connection& client, unsigned id, int version);
void bind_shell(wayland::client_connection& client, unsigned id, int version);
/// Sends zlm_shell_v1.session_ending (v3) to every bound shell.
void shell_send_session_ending();
/// zlm_shell_v1 v7: see `p0_protocol_context::send_shell_config` / `send_shell_action`.
void shell_send_config(void* shell_resource, const lx::compositor::shell_config_report& r);
bool shell_send_action(const char* action, const char* argument, unsigned time_ms, bool input);
/// zlm_key_grab_v1.activated (v4): see `p0_protocol_context::fire_key_grab`.
void shell_fire_key_grab(void* grab_resource, unsigned time_ms);
/// zlm_shell_v1.held_action and zlm_held_action_v1 (v7): see
/// `p0_protocol_context::send_held_action` and `send_hold_*`.
void* shell_send_held_action(const char* action, const char* argument, unsigned time_ms);
void shell_send_hold_key(void* hold, unsigned keysym, unsigned mods, const char* binding,
                         unsigned time_ms);
void shell_send_hold_released(void* hold, unsigned time_ms);
void shell_send_hold_cancelled(void* hold);
/// zlm_shell_v1.lock_state / lock_requested (v5) to every bound shell of version 5.
void shell_send_lock_state(unsigned state);
/// True when one was told.
bool shell_send_lock_requested(unsigned reason);
/// zlm_shell_v1.capture_state (v6) to every bound shell, on change (sent on bind too).
void shell_send_capture_state(bool active);
void bind_output_ex(wayland::client_connection& client, unsigned id, int version, void* user);

// ── wire_output.cpp: wl_output, xdg-output, output membership ───────────────

/// zxdg_output_manager_v1 (a P0 protocol; needs the generated glue).
void bind_xdg_output_manager(wayland::client_connection& client, unsigned id, int version);
/// Installs the surface tree's rect hook and the cursor's surface hook.
void install_outputs(p0_protocol_context& ctx);
/// `surface_tree` rect hook: recomputes the surface's output membership from its node rect
/// (empty: unmapped) and sends enter/leave and the preferred scale/transform. Unmapped, the
/// surface's in-flight presentation feedback is discarded.
void output_node_rect(void* user, lx::surface_id surface, lx::rect2i rect);
/// The 120ths scale a surface not yet on any output is expected to show at (D7: the first
/// wp_fractional_scale_v1.preferred_scale): its current one when it is on outputs,
/// otherwise its parent's, otherwise output 0's (toplevels and layer surfaces map there).
[[nodiscard]] std::uint32_t expected_scale_120(const surface_obj* s);
/// Calls `fn(resource)` for every wl_output resource `client` bound for output `slot`.
void for_each_output_resource(wl_client* client, unsigned slot, void (*fn)(wl_resource*, void*),
                              void* user);

/// The output slot a wl_output resource stands for now, or -1 (inert, or no output left).
[[nodiscard]] int output_resource_slot(wl_resource* output);

// ── wire_idle.cpp, wire_output_power.cpp (ext-idle-notify, idle-inhibit, wlr output power) ──

/// ext_idle_notifier_v1 v2 / zwp_idle_inhibit_manager_v1 v1 / zwlr_output_power_manager_v1
/// v1 (P1 protocols; defined only when their glue exists — wire_install.cpp calls them under
/// the same `LUMEN_PROTOCOL_*` guards).
void bind_idle_notifier(wayland::client_connection& client, unsigned id, int version);
void bind_idle_inhibit_manager(wayland::client_connection& client, unsigned id, int version);
void bind_output_power_manager(wayland::client_connection& client, unsigned id, int version);
/// zwlr_output_manager_v1 v4 (wire_output_management.cpp; defined only with its glue).
void bind_output_manager(wayland::client_connection& client, unsigned id, int version);
/// Outputs changed: a zwlr_output_power_v1 whose wl_output stands for no output any more is
/// sent `failed` and goes inert (wire_output.cpp calls it; a no-op without the glue).
void output_power_outputs_changed();

// ── wire_xdg_shell.cpp: liveness ───────────────────────────────────────────

/// Installs the liveness tracker's wire ops (ping, the not-responding change).
void install_liveness(p0_protocol_context& ctx);
void bind_data_device_manager(wayland::client_connection& client, unsigned id, int version);

// ── wire_selection.cpp (both families), wire_data_device.cpp, wire_primary_selection.cpp ──

extern xfer_family_ops xfer_ops[k_xfer_families];
/// Allocates the slot and the resource (destructor installed). Null after posting
/// no_memory when the table is full.
xfer_source* xfer_create_source(wl_client* client, xfer_family family,
                                const wl_interface* iface, const void* impl, int version,
                                uint32_t id);
xfer_device* xfer_create_device(wl_client* client, xfer_family family,
                                const wl_interface* iface, const void* impl, int version,
                                uint32_t id);
xfer_source* xfer_source_from(wl_resource* resource);
xfer_device* xfer_device_from(wl_resource* resource);
xfer_offer* xfer_offer_from(wl_resource* resource);
void xfer_source_offer(xfer_source* source, const char* mime);
/// wl_data_device.set_selection / zwp_primary_selection_device_v1.set_selection.
void xfer_set_selection(xfer_device* device, wl_resource* source_resource, uint32_t serial);
/// wl_data_offer.receive / zwp_primary_selection_offer_v1.receive. Closes `fd`.
void xfer_offer_receive(xfer_offer* offer, const char* mime, int32_t fd);
/// input_router's focus-client hook.
void xfer_focus_changed(void* user, void* old_client, void* new_client);
/// The server's client-destroyed hook (every per-client table keyed by wl_client*).
void wire_client_destroyed(void* user, unsigned client_id, void* wl_client);
/// The lx client id of a wl_client, for logs; 0 when unknown (already gone).
unsigned wire_client_id(wl_client* client);
void install_data_device(p0_protocol_context& ctx);
/// A new offer of `source` for `device`, announced on it: data_offer, then one `offer` per
/// mime type. Null when no slot is left (the client holds too many live offers, or the
/// table is full); `no_memory` has been posted when the resource could not be created.
xfer_offer* xfer_create_offer(xfer_device* device, xfer_source* source, offer_use use);
/// The same for a data-control device (`device_res` of `client`), with that protocol's
/// marshalling in `ops` (offer interface, data_offer, offer). Use `offer_use::control`.
xfer_offer* xfer_create_control_offer(wl_client* client, wl_resource* device_res,
                                      const xfer_family_ops& ops, xfer_family family,
                                      xfer_source* source);
/// The source owning family `f`'s selection now (null: none, or its client is going).
[[nodiscard]] xfer_source* xfer_selection_source(xfer_family f);
/// ext/wlr data control set_selection / set_primary_selection: `source` (null: clear)
/// becomes the selection of `f` with no serial check — a clipboard manager acts for the
/// user, not on an input event. The source must be unused (the caller posts used_source).
void xfer_set_selection_control(xfer_family f, xfer_source* source);
/// Family `f`'s selection changed (wire_data_control.cpp): every data-control device is
/// sent it, whatever has the keyboard.
void data_control_selection_changed(xfer_family f);
/// Super+Escape and the router's keyboard-focus hook for keyboard-shortcuts-inhibit
/// (wire_shortcuts_inhibit.cpp; no-ops without its glue).
void shortcuts_inhibit_focus(void* surface);
void shortcuts_inhibit_surface_destroyed(surface_obj* s);
[[nodiscard]] bool shortcuts_inhibit_effective();
bool shortcuts_inhibit_user_toggle();
/// The offer goes inert (unlinked from its source); its resource lives on until destroyed.
void xfer_inert_offer(xfer_offer* offer);
/// The live devices of `client` of family `family`, up to `cap`, in table order. The
/// pointers stay valid until the next request is dispatched.
unsigned xfer_devices_of(wl_client* client, xfer_family family, xfer_device** out,
                         unsigned cap);

// ── wire_dnd.cpp (drag-and-drop; the clipboard family only) ─────────────────

/// wl_data_device.start_drag.
void dnd_start_drag(xfer_device* device, wl_resource* source, wl_resource* origin,
                    wl_resource* icon, uint32_t serial);
/// wl_data_offer.accept / set_actions / finish on a drag-and-drop offer.
void dnd_offer_accept(xfer_offer* offer, uint32_t serial, const char* mime);
void dnd_offer_set_actions(xfer_offer* offer, uint32_t actions, uint32_t preferred);
void dnd_offer_finish(xfer_offer* offer);
/// A drag-and-drop offer's resource is going (before it is unlinked from its source).
void dnd_offer_destroyed(xfer_offer* offer);
/// A source given to start_drag is going (before its offers go inert).
void dnd_source_destroyed(xfer_source* source);
/// A client is gone (the server's client-destroyed hook).
void dnd_client_destroyed(wl_client* client);
void install_dnd(p0_protocol_context& ctx);
/// Defined only when the primary-selection glue exists (a P1 protocol); wire_install.cpp
/// calls them under the same `LUMEN_PROTOCOL_*` guard.
void bind_primary_selection_manager(wayland::client_connection& client, unsigned id,
                                    int version);
void install_primary_selection(p0_protocol_context& ctx);

// ── wire_activation.cpp (xdg-activation) ──────────────────────────────────

/// wm_protocol_ops::last_user_serial: the newest key or button press delivered to the client
/// owning `surface` (the user-time rule).
unsigned wm_last_user_serial(void* user, void* surface);
/// A wl_surface is going: tokens naming it as their requester forget it.
void activation_surface_destroyed(lx::surface_id surface);
/// Defined only when the xdg-activation glue exists (a P1 protocol); wire_install.cpp calls
/// it under the same `LUMEN_PROTOCOL_*` guard.
void bind_activation(wayland::client_connection& client, unsigned id, int version);

// ── window and session protocols (design-app-compat-capture Part B) ─────────
//    wire_dialog.cpp, wire_toplevel_icon.cpp, wire_bell.cpp, wire_foreign.cpp,
//    wire_shortcuts_inhibit.cpp, wire_data_control.cpp, wire_security_context.cpp.
//    The bind functions exist only with their glue; wire_install.cpp calls them under the
//    same `LUMEN_PROTOCOL_*` guards.

void bind_wm_dialog(wayland::client_connection& client, unsigned id, int version);
void bind_toplevel_icon_manager(wayland::client_connection& client, unsigned id, int version);
void bind_system_bell(wayland::client_connection& client, unsigned id, int version);
void bind_exporter_v2(wayland::client_connection& client, unsigned id, int version);
void bind_importer_v2(wayland::client_connection& client, unsigned id, int version);
void bind_exporter_v1(wayland::client_connection& client, unsigned id, int version);
void bind_importer_v1(wayland::client_connection& client, unsigned id, int version);
void bind_shortcuts_inhibit_manager(wayland::client_connection& client, unsigned id, int version);
void bind_ext_data_control_manager(wayland::client_connection& client, unsigned id, int version);
void bind_wlr_data_control_manager(wayland::client_connection& client, unsigned id, int version);
void bind_security_context_manager(wayland::client_connection& client, unsigned id,
                                   int version);
/// wl_surface.commit of an xdg_toplevel: an icon staged by set_icon becomes current (and
/// the shell is told when the window is mapped).
void toplevel_icon_commit(surface_obj* s);
/// The toplevel record is going: its icon references are dropped.
void toplevel_icon_released(lx::compositor::toplevel_record& rec);
/// The toplevel is going: its xdg-foreign exports are withdrawn and every import told.
void foreign_toplevel_destroyed(lx::toplevel_id id);
/// The router's keyboard-focus hook of the text-input relay (wire_input_method.cpp).
void text_input_focus_hook(void* user, void* surface, void* client, void* destroyed);

// ── pointer protocols (design-app-compat-capture Part C) ───────────────────
//    wire_pointer_constraints.cpp, wire_relative_pointer.cpp, wire_gestures.cpp. The bind
//    functions exist only with their glue; wire_install.cpp calls them under the same
//    `LUMEN_PROTOCOL_*` guards. The hooks are no-ops without it.

void bind_pointer_constraints(wayland::client_connection& client, unsigned id, int version);
void bind_relative_pointer_manager(wayland::client_connection& client, unsigned id, int version);
void bind_pointer_gestures(wayland::client_connection& client, unsigned id, int version);
/// Which constraint holds is decided again (the router's pointer-focus hook, the
/// keyboard-focus hook, and `pointer_constraints_update`).
void pointer_constraints_reevaluate();
/// `s` committed (commit `seq`): a constraint's pending region and cursor hint are staged
/// for the content update that commit makes.
void pointer_constraints_surface_committed(surface_obj* s, unsigned seq);
/// A content update of `s` was applied (carrying commits up to `seq`): the regions and
/// cursor hints staged by those commits become current, the effective region follows the
/// new input region and size.
void pointer_constraints_surface_applied(surface_obj* s, unsigned seq);
/// Decides again if `pointer_constraints_surface_applied` changed anything (the end of a
/// wl_surface.commit request: the commit is complete, a warp is safe).
void pointer_constraints_flush();
/// `s` is being destroyed (before input focus drops it): its constraints go inert.
void pointer_constraints_surface_destroyed(surface_obj* s);
/// Super+Escape: releases the active constraint (false: none).
bool pointer_constraints_escape();
/// What the active constraint does to a motion between global points: 0 none, 1 locked (the
/// pointer stays), 2 confined (`to` clipped).
int pointer_constraints_filter(double from_x, double from_y, double& to_x, double& to_y);
/// 0 none, 1 a lock, 2 a confinement is active.
[[nodiscard]] int pointer_constraints_active_kind();
/// A client is gone (the server's client-destroyed hook): a gesture latched to it ends.
void pointer_gestures_client_destroyed(wl_client* client);

// ── screen capture (design-app-compat-capture Part D) ─────────────────────
//    wire_foreign_toplevel.cpp, wire_capture.cpp, wire_capture_cursor.cpp,
//    wire_screencopy.cpp. The bind functions exist only with their glue; wire_install.cpp
//    calls them under the same `LUMEN_PROTOCOL_*` guards. The hooks are no-ops without it.

void bind_foreign_toplevel_list(wayland::client_connection& client, unsigned id, int version);
void bind_output_capture_source_manager(wayland::client_connection& client, unsigned id,
                                        int version);
void bind_toplevel_capture_source_manager(wayland::client_connection& client, unsigned id,
                                          int version);
void bind_image_copy_capture_manager(wayland::client_connection& client, unsigned id, int version);
void bind_screencopy_manager(wayland::client_connection& client, unsigned id, int version);
/// The shell bridge's observer (maps, unmaps, title / app id changes): the foreign-toplevel
/// lists and window captures follow the windows through it.
void install_toplevel_observer(p0_protocol_context& ctx);
/// The window of an ext_foreign_toplevel_handle_v1 (empty: closed, or not a handle) and the
/// identifier of that mapping.
[[nodiscard]] lx::toplevel_id foreign_toplevel_of(wl_resource* handle, const char*& identifier);
/// What an ext_image_capture_source_v1 names (false: not a source).
bool capture_source_of(wl_resource* source, capture_source_kind& kind, unsigned& slot,
                       lx::output_id& output, lx::toplevel_id& toplevel, const char*& identifier);
/// Binds the capture shm guards to libwayland (access guards, pool unref).
void bind_capture_guards(p0_protocol_context& ctx);
/// A window unmapped: its captures stop.
void capture_toplevel_unmapped(lx::toplevel_id id);
/// A capture fence registered in the readiness poller (`cu_poll_add`) signalled.
void capture_fence_signalled(unsigned key, unsigned serial);
/// A wl_buffer is going: a capture import of it is dropped on the render thread.
void capture_buffer_gone(wl_resource* buffer, buffer_obj* b);
/// A window with a capture frame pending: its surfaces' frame callbacks are paced like a
/// shown window's (CC2: a cast window keeps painting while covered).
[[nodiscard]] bool capture_paces_surface(lx::surface_id surface);
/// ext_image_copy_capture_manager_v1.create_pointer_cursor_session (wire_capture_cursor.cpp).
void capture_cursor_session_create(wl_client* client, wl_resource* manager, uint32_t id,
                                   wl_resource* source, wl_resource* pointer);
/// Cursor sessions: per tick (enter / leave / position / hotspot, frames), and their window
/// unmapping.
void capture_cursor_tick();
void capture_cursor_toplevel_unmapped(lx::toplevel_id id);
/// Readiness-poller waits of other units than the content-update queue (keys above every
/// update index): `fd` is owned in every path; false when it could not be watched.
bool cu_poll_add(int fd, unsigned key, unsigned generation);
/// Drops (closes) every wait of `key`.
void cu_poll_drop(unsigned key);

// ── ext-workspace-v1 (wire_workspace.cpp; design-session-and-wm W.4) ─────

void bind_workspace_manager(wayland::client_connection& client, unsigned id, int version);
/// A wl_output was bound: the client's workspace groups of that output get `output_enter`.
void workspace_output_bound(wl_client* client, wl_resource* output, unsigned slot);
/// Every bound manager follows the model (one `done` each that changed).
void workspaces_sync_all();

// ── wire_install.cpp ───────────────────────────────────────────────────────

void install_core(p0_protocol_context& ctx);

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
