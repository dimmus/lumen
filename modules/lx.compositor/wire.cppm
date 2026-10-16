module;

// The wire objects shared by the protocol units (wire_*.cpp): every libwayland object
// record, the tables and helpers more than one unit uses, and the functions one unit
// defines and another calls. An implementation partition — not exported, not API. Each
// unit keeps its own helpers in an anonymous namespace, so only what is declared here has
// module linkage.

#include <cstdint>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#endif

module lx.compositor:wire;

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

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

constexpr unsigned k_max_surfaces = 256;
constexpr unsigned k_max_frame_cbs = 64;
constexpr unsigned k_max_regions = 128;

struct frame_callback {
    wl_resource* resource = nullptr;
    bool used = false;
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
    /// Union of every damage / damage_buffer since the last commit, surface-local. Both
    /// requests are treated alike: scale is 1 and the transform normal, so the two
    /// coordinate spaces coincide.
    lx::rect2i damage{};
    bool has_damage = false;
    frame_callback frame_cbs[k_max_frame_cbs]{};
    unsigned frame_cb_count = 0;
    /// set_input_region / set_opaque_region: the region is copied when the request arrives
    /// (the wl_region may be destroyed right after) and applied with the rest.
    bool input_set = false;
    lx::compositor::input_region input{};
    bool opaque_set = false;
    lx::compositor::input_region opaque{};
};

/// A wl_surface's role. Fixed for the surface's lifetime once assigned (wl_surface: a
/// surface has at most one role), which is what lets a second role request be rejected.
/// `dnd_icon`: wl_data_device.start_drag's icon (kept after the drag, as in wlroots).
enum class surface_role : unsigned char {
    none,
    xdg_toplevel,
    cursor,
    subsurface,
    popup,
    layer,
    dnd_icon
};

struct xdg_surface_obj;
struct xdg_toplevel_obj;
struct subsurface_obj;
struct layer_surface_obj;

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
};

/// wl_subsurface. Inert once its surface is gone; a surface whose parent is gone keeps its
/// object (and its role) until the client destroys it.
struct subsurface_obj {
    wl_resource* resource = nullptr;
    surface_obj* surface = nullptr;
    bool used = false;
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
    lx::buffer_id id{};
    unsigned width = 0;
    unsigned height = 0;
    lx::gfx::dmabuf_desc dmabuf{};
    /// What has been staged for this buffer's texture (GL path, buffer-age uploads).
    buffer_upload_state upload{};
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
enum class offer_use : unsigned char { selection, dnd };
enum class source_role : unsigned char { none, selection, dnd };

constexpr unsigned k_xfer_families = 2;
constexpr unsigned k_max_xfer_offers = 512;
/// Live offers one client may hold: one that never destroys them would otherwise fill the
/// shared table and starve every other client of the selection.
constexpr unsigned k_max_offers_per_client = 32;

struct xfer_offer;

/// wl_data_source / zwp_primary_selection_source_v1. Allocated per request, freed with its
/// object (wire_selection.cpp).
struct xfer_source {
    wl_resource* resource = nullptr;
    wl_client* client = nullptr;
    xfer_family family{};
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

/// Every wl_surface record (wire_surface.cpp). Scanned by buffer and frame-callback
/// destructors, which must drop references to a dying object.
extern surface_obj surfaces[k_max_surfaces];

// ── wire_buffer.cpp ────────────────────────────────────────────────────────

buffer_obj* alloc_buffer();
void retire_texture(unsigned texture_id);
void destroy_buffer(buffer_obj* b);
lx::fourcc shm_format_to_fourcc(uint32_t shm_format);
lx::fourcc staged_shm_format(wl_shm_buffer* shm);
const unsigned char* stage_shm_pixels(buffer_obj* buf, wl_shm_buffer* shm, unsigned texture_id,
                                      lx::rect2i region, unsigned long long& seq_out,
                                      unsigned long long& token_out,
                                      unsigned long long& pool_token_out);
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

void apply_cached(surface_obj* s, const char* cause, unsigned depth);
void flush_desync(surface_obj* s, const char* cause);
void discard_cached(surface_obj* s);
void bind_compositor(wayland::client_connection& client, unsigned id, int version);
void fire_frames(unsigned frame_time_ms);

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
void bind_seat(wayland::client_connection& client, unsigned id, int version);
void bind_shell(wayland::client_connection& client, unsigned id, int version);
void bind_output(wayland::client_connection& client, unsigned id, int version);

// ── wire_output.cpp: wl_output, xdg-output, output membership ───────────────

/// zxdg_output_manager_v1 (a P0 protocol; needs the generated glue).
void bind_xdg_output_manager(wayland::client_connection& client, unsigned id, int version);
/// Installs the surface tree's rect hook and the cursor's surface hook.
void install_outputs(p0_protocol_context& ctx);
/// `surface_tree` rect hook: recomputes the surface's output membership from its node rect
/// (empty: unmapped) and sends enter/leave and the preferred scale/transform.
void output_node_rect(void* user, lx::surface_id surface, lx::rect2i rect);

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
/// calls them under the same `__has_include` guard.
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
/// it under the same `__has_include` guard.
void bind_activation(wayland::client_connection& client, unsigned id, int version);

// ── wire_install.cpp ───────────────────────────────────────────────────────

void install_core(p0_protocol_context& ctx);

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND
