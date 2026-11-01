module;

// lx.ui.host:surface — one wl_surface drawn with the ADR-010 engine, whatever its role
// (Phase 5; moved from lx.app:window): an lx.ui `ui_root` with its own `ui_context`, the
// client pixel path of §4.10 (a `client_swapchain` on the context's shared `client_gpu`,
// whose images become wl_buffers here) and the frame loop of §4.9. The role — xdg_toplevel,
// zwlr_layer_surface_v1, xdg_popup, wl_subsurface (`:roles`) — feeds it configures and acks
// them at the commit that shows them; nothing here knows which role it is.
//
// Frame loop. Input, a reducer, a timer or a configure leaves work in the root's ledger; the
// surface's frame source sees it in the same loop iteration. With no frame callback
// outstanding the frame is drawn at once (no round trip to the compositor first: input that
// arrives while the surface is idle is on screen at the next refresh); with one outstanding it
// waits for it, so a surface never draws faster than the compositor presents. A frame:
// acquire a free image (none free: wait for a release, then draw once — never a busy loop),
// take its age as repaint (`ui_root::add_repaint`), tick the root with `emit_scope::damage`,
// render inside the repaint region (each rect scissored), present it (sync exported), ack the
// configure it answers, attach, damage_buffer × what changed (≤ 16 rects), set the syncobj
// points, request the next frame callback and presentation feedback, commit. The ledger is
// cleared only by that commit: a frame that fails puts its damage back. A configure that finds
// no frame callback outstanding (and a preferred-scale change, always) is answered at once;
// one that arrives while a callback is outstanding waits for it, the newest replacing the
// older, so a compositor configuring per pointer motion gets one resize per refresh (GTK 4's
// frozen frame clock). The commit right after an ack has its size (frame-perfect resize).
// A bounded configure's size (maximized, tiled, a popup's) is the surface's, a fullscreen or
// resizing one's its maximum; `min_size` holds only where the role leaves the size to the
// client.
//
// Idle (G1, per surface): an empty ledger, nothing animating, no timer due, no key held → no
// frame callback is requested after the last frame's, nothing is submitted, and the context
// arms no timer for it.
//
// Scale. With wp_fractional_scale_v1 and wp_viewporter the buffer is ceil(logical × scale)
// device px and the viewport's destination the logical size (buffer_scale 1); without them the
// integer wl_surface.preferred_buffer_scale (v6), else the outputs' scale, else 1, as
// buffer_scale. The UI always lays out in logical px; `ui_root::set_scale` projects at
// emission and rasterises text and icons at device resolution.
//
// Shared with the other surfaces of the context: the client_gpu (one render node, one EGL
// context or VkDevice), the renderer, the atlas, the glyph and icon caches. Per surface: the
// wl_surface, the ui_root with its ui_context and arena, the draw list, the swapchain and its
// wl_buffers, the input router, and — opt-in (`surface_options::frame_records`, ≈ 30 KiB) —
// the frame ring.
//
// UI affinity. A steady frame allocates nothing (G5); allocation happens at create, at a
// swapchain (re)creation and when the tree's shape changes.

export module lx.ui.host:surface;

import std;

import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.ui;
import lx.wayland.client;

import :input;

export namespace lx::ui::host {

class context;
class surface;

/// One committed frame's wp_presentation feedback, for the bench (§8.3 S9). CLOCK_MONOTONIC
/// ns (the compositor's presentation clock when it is that, which Lumen's is).
struct frame_record {
    std::uint64_t commit_id = 0;
    std::uint64_t commit_ns = 0;
    /// When the frame started (its event or callback seen): commit − start is the UI
    /// thread's frame time.
    std::uint64_t start_ns = 0;
    std::uint64_t presented_ns = 0;
    std::uint64_t sequence = 0;
    std::uint32_t refresh_ns = 0;
    std::uint32_t flags = 0;
    std::uint16_t damage_rects = 0;
    bool presented = false;
    bool discarded = false;
    /// The buffer's size in device px.
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    /// Where the UI thread's time went (ns, §8.1's stages): geometry (a configure's size,
    /// the scale, a swapchain resize and its new images and wl_buffers); acquire; the root's
    /// tick and, inside it, describe / reconcile / layout / emit; `batch_for_render`
    /// (order); the renderer's draw into the image (render: CPU raster, or GL / Vulkan
    /// record and submit); the swapchain's present (sync export, flush). Commit − start
    /// minus these is the input flush, the age and the wire requests.
    /// The pointer motion this frame consumed: its hit test and the hover / leave events
    /// (`input_router::flush_motion`), before anything else.
    std::uint32_t input_ns = 0;
    /// Device px reported as damage (the area of the damage_buffer rects).
    std::uint32_t damage_px = 0;
    std::uint32_t geometry_ns = 0;
    /// The UI thread's CPU time from the frame's start to its commit
    /// (CLOCK_THREAD_CPUTIME_ID): commit − start much above it is time the thread was not
    /// running (preempted, or its vCPU descheduled by the host).
    std::uint32_t cpu_ns = 0;
    std::uint32_t acquire_ns = 0;
    std::uint32_t tick_ns = 0;
    std::uint32_t describe_ns = 0;
    std::uint32_t reconcile_ns = 0;
    std::uint32_t layout_ns = 0;
    std::uint32_t emit_ns = 0;
    std::uint32_t order_ns = 0;
    std::uint32_t render_ns = 0;
    std::uint32_t present_ns = 0;
    /// Layout work in the tick: measure and arrange calls (`ui_layout_counters`).
    std::uint32_t measures = 0;
    std::uint32_t arranges = 0;
};
inline constexpr unsigned k_frame_ring = 256;

struct surface_options {
    /// The UI. Required.
    lx::ui::describe_fn describe = nullptr;
    /// Logical px, used where the role's configure leaves an axis to the client (0).
    lx::size2i size{640, 480};
    /// The floor of a size the client picks (never above a bounded configure's size).
    lx::size2i min_size{};
    /// An opaque surface (XRGB buffers, an opaque region over its size): the compositor
    /// blends nothing.
    bool opaque = true;
    /// The UI arena (descriptors, per frame); mmap'd, resident as touched.
    std::size_t arena_bytes = 512 * 1024;
    /// wp_presentation feedback for every frame. Off: only until the first frame is
    /// presented (the timeline's `first_presented_ns`).
    bool presentation_feedback = false;
    /// Keep the frame ring (`frame_at`, `k_frame_ring` records ≈ 30 KiB). Off: none is
    /// allocated and `frame_count` is 0.
    bool frame_records = true;
    /// The ring's storage, the caller's (exactly `k_frame_ring` records, outliving the
    /// surface: an lx.app window keeps it in itself); empty: allocated with the surface.
    std::span<frame_record> frame_ring{};
    /// Use wp_fractional_scale_v1 with a viewport when the compositor offers both. Off: the
    /// integer buffer scale (a surface whose viewport is someone else's).
    bool fractional_scale = true;
};

/// What a role's configure asks of the surface.
struct surface_configure {
    /// What the role acks (`surface_role::role_ack`) at the commit that answers it.
    std::uint32_t serial = 0;
    /// Logical px; 0 on an axis: the client picks.
    lx::size2i size{};
    /// The size is the surface's, or at most it (maximized, tiled, fullscreen, resizing,
    /// a popup's geometry): `min_size` does not apply on that axis.
    bool bounded = false;
};

/// The role's side of a surface: acks a configure right before the commit that shows it,
/// and destroys its object when the surface goes first (the role object must go before the
/// wl_surface: xdg-shell's defunct_role_object).
class surface_role {
public:
    virtual void role_ack(std::uint32_t serial) = 0;
    virtual void role_surface_destroyed() = 0;

protected:
    ~surface_role() = default;
};


struct surface_stats {
    std::uint64_t frame_requests = 0;  ///< wl_surface.frame requested
    std::uint64_t frame_callbacks = 0; ///< wl_callback.done received
    std::uint64_t frames = 0;          ///< frames drawn and committed
    std::uint64_t commits = 0;         ///< wl_surface.commit (frames, and acks without a frame)
    std::uint64_t empty_ticks = 0;     ///< ticks that produced nothing (the image went back)
    std::uint64_t no_image = 0;        ///< frames that waited for a release
    std::uint64_t configures = 0;
    std::uint64_t acks = 0;
    std::uint64_t reallocations = 0;   ///< swapchain allocations (creation and new sizes)
    std::uint64_t scale_changes = 0;
    std::uint64_t failures = 0;        ///< frames that failed (their damage put back)
    std::uint64_t presented = 0;
    std::uint64_t discarded = 0;
    /// GL: the time this surface's draws waited for the shared instance ring's slot (the
    /// fence of the frame that used it last, any surface's).
    std::uint64_t slot_wait_ns = 0;
    /// The last frame: what changed (device rects sent as damage_buffer), what it repainted.
    unsigned last_damage_rects = 0;
    unsigned last_repaint_rects = 0;
    std::uint64_t last_damage_px = 0;
    lx::rect2i last_damage_bounds{};
};

/// When the surface got where (CLOCK_MONOTONIC ns; 0: not yet).
struct surface_timeline {
    std::uint64_t opened_ns = 0;
    std::uint64_t first_configure_ns = 0;
    std::uint64_t first_commit_ns = 0;
    std::uint64_t first_presented_ns = 0;
};

class surface final : public lx::wayland::surface_handler,
                      public lx::wayland::buffer_handler,
                      public lx::gfx::swapchain_listener {
public:
    surface();
    ~surface() override;
    surface(const surface&) = delete;
    surface& operator=(const surface&) = delete;

    /// Creates the wl_surface (with fractional scale and a viewport when offered), the root
    /// on its own `ui_context`, binds input and adds the frame source to the context's loop.
    /// No role yet and nothing committed: create the role, then `show`. Fails
    /// (invalid_argument) without a describe function or with the context's surface table
    /// full; wayland errors from the connection.
    [[nodiscard]] lx::result<void> create(context& ctx, const surface_options& options);
    /// The role's initial commit (no buffer) goes out, then the pixel path is made: the
    /// context's client_gpu on first use (its tier initialises while the compositor's first
    /// configure travels), and this surface's buffer format. The first frame is drawn when
    /// the role's first configure arrives.
    [[nodiscard]] lx::result<void> show();
    /// Destroys the role's object (if the role is still set), the swapchain (its wl_buffers
    /// with it: the compositor drops them), the wl_surface and the root. A no-op when not
    /// created.
    void destroy();
    [[nodiscard]] bool valid() const { return created_; }

    /// The role that acks this surface's configures (null: none; nothing is acked).
    void set_role(surface_role* role) { role_ = role; }
    /// The role's configure: drawn for at once, or at the outstanding frame callback.
    void configure(const surface_configure& c);
    /// Commits pending wl_surface state now (a role's double-buffered request, such as a
    /// layer surface's keyboard interactivity), without a new buffer.
    void commit_state();

    [[nodiscard]] context& owner() const { return *ctx_; }
    [[nodiscard]] lx::ui::ui_root& root() { return *root_; }
    [[nodiscard]] const lx::ui::ui_root& root() const { return *root_; }
    [[nodiscard]] lx::ui::ui_context& ui() { return *ui_; }
    [[nodiscard]] input_router& input() { return input_; }
    [[nodiscard]] const input_router& input() const { return input_; }
    [[nodiscard]] lx::wayland::surface& wl() { return surface_; }
    [[nodiscard]] const lx::wayland::surface& wl() const { return surface_; }
    [[nodiscard]] lx::gfx::client_tier tier() const;
    [[nodiscard]] const lx::gfx::client_gpu& gpu() const;
    [[nodiscard]] const lx::gfx::client_swapchain& swapchain() const { return swapchain_; }
    [[nodiscard]] lx::size2i logical_size() const { return logical_; }
    [[nodiscard]] lx::size2i buffer_size() const { return buffer_; }
    [[nodiscard]] float scale() const { return scale_; }
    /// wl_surface.buffer_scale in use (1 with a viewport).
    [[nodiscard]] int buffer_scale() const { return buffer_scale_; }
    /// The viewport's destination (0 × 0: no viewport).
    [[nodiscard]] lx::size2i viewport_destination() const { return viewport_dst_; }
    [[nodiscard]] bool configured() const { return configured_; }
    [[nodiscard]] const surface_stats& stats() const { return stats_; }
    /// The last frame's damage_buffer rects (device px).
    [[nodiscard]] const lx::gfx::damage_region& last_damage() const { return last_damage_; }
    [[nodiscard]] const surface_timeline& timeline() const { return timeline_; }
    /// The frame ring, oldest first through `frame_count()` entries (at most `k_frame_ring`;
    /// 0 without `surface_options::frame_records`).
    [[nodiscard]] const frame_record& frame_at(unsigned i) const;
    [[nodiscard]] unsigned frame_count() const;
    /// The last frame's draw list (device px).
    [[nodiscard]] const lx::scene::draw_list& last_draws() const { return *draws_; }
    /// The last presented image's pixels on the CPU tier (XRGB8888 or ARGB8888, `stride`
    /// bytes), else null.
    [[nodiscard]] const lx::gfx::pixel_surface* cpu_pixels() const;
    /// The context's atlas (shared by every surface of it).
    [[nodiscard]] lx::gfx::atlas& atlas();

    /// wl_surface.set_opaque_region / set_input_region, kept until changed. An opaque
    /// surface's region is its whole size unless set here.
    void set_opaque_region(std::span<const lx::rect2i> rects);
    void set_input_region(std::span<const lx::rect2i> rects);
    void reset_input_region();

    /// Draw every frame (whole surface damaged) while true — `lumen-hello --frames`.
    void set_continuous(bool on) { continuous_ = on; }
    /// The keyboard focus entered or left this surface (the context's seat router). The
    /// launcher closes when it loses the keyboard to anything else (Kickoff's rule).
    void set_keyboard_focus_handler(void (*fn)(bool entered, void* user), void* user) {
        keyboard_focus_fn_ = fn;
        keyboard_focus_user_ = user;
    }
    /// `surface_options::presentation_feedback`, from the next frame.
    void set_presentation_feedback(bool on) { presentation_feedback_ = on; }
    /// The earliest deadline the context must wake for (a held key's repeat, a UI timer
    /// still ahead of `now_ns`, a configure held for a callback); 0: none.
    [[nodiscard]] std::uint64_t next_deadline_ns(std::uint64_t now_ns) const;
    /// The context's timer fired.
    void on_deadline(std::uint64_t now_ns);

    // ── seat events for this surface (the context routes them) ──
    void route_pointer(lx::wayland::seat& s, const lx::wayland::pointer_frame& f) { input_.on_pointer(s, f); }
    void route_keyboard_focus(lx::wayland::seat& s, bool entered) {
        input_.on_keyboard_focus(s, entered);
        if (keyboard_focus_fn_)
            keyboard_focus_fn_(entered, keyboard_focus_user_);
    }
    void route_key(lx::wayland::seat& s, const lx::wayland::key_event& e) { input_.on_key(s, e); }
    void route_modifiers(lx::wayland::seat& s, std::uint32_t m) { input_.on_modifiers(s, m); }
    void route_repeat_info(lx::wayland::seat& s, std::int32_t r, std::int32_t d) { input_.on_repeat_info(s, r, d); }
    void route_touch(lx::wayland::seat& s, const lx::wayland::touch_event& e) { input_.on_touch(s, e); }

    // ── the frame source (lx::runtime::event_source over the surface) ──
    [[nodiscard]] bool frame_due() const;
    void run_frame();

    // surface_handler
    void on_frame(lx::wayland::surface& s, std::uint32_t time_ms) override;
    void on_preferred_scale(lx::wayland::surface& s, std::uint32_t scale120) override;
    void on_presentation(lx::wayland::surface& s, const lx::wayland::presentation_event& e) override;
    // buffer_handler
    void on_release(lx::wayland::buffer& b) override;
    void on_failed(lx::wayland::buffer& b) override;
    // swapchain_listener
    void on_image_created(const lx::gfx::swapchain_image& image) override;
    void on_image_destroyed(std::uint32_t id) override;

private:
    struct frame_source;
    struct buffer_slot {
        std::uint32_t id = 0;
        /// The wl_shm_pool (`pools_`) the buffer was made from; 0: a dma-buf.
        std::uint32_t pool_id = 0;
        lx::wayland::buffer buffer{};
        lx::wayland::syncobj_timeline timeline{};
    };
    /// One wl_shm_pool per swapchain shm block (`swapchain_image::pool_id`), alive while
    /// the block has images.
    struct shm_pool_slot {
        std::uint32_t id = 0;
        unsigned refs = 0;
        lx::wayland::shm_pool pool{};
    };
    static constexpr unsigned k_buffer_slots =
        lx::gfx::k_swapchain_images + lx::gfx::k_swapchain_retired;
    static constexpr unsigned k_shm_pools = 1 + lx::gfx::k_swapchain_retired;
    /// The longest a configure waits for an outstanding frame callback.
    static constexpr std::uint64_t k_configure_wait_ns = 100'000'000;

    /// Applies a pending configure and scale: bounds, scale, viewport, swapchain size.
    [[nodiscard]] lx::result<void> apply_geometry();
    [[nodiscard]] bool draw();
    void ack_pending();
    void restore_damage();
    [[nodiscard]] lx::result<void> render(lx::gfx::swapchain_image& image,
                                          const lx::gfx::damage_region& repaint);
    [[nodiscard]] buffer_slot* slot_of(std::uint32_t id);
    void release_pool(std::uint32_t pool_id);
    void collect_releases();
    [[nodiscard]] static std::uint64_t now_ns();
    /// CLOCK_THREAD_CPUTIME_ID: what a frame cost the UI thread, preemption excluded.
    [[nodiscard]] static std::uint64_t thread_cpu_ns();

    context* ctx_ = nullptr;
    surface_role* role_ = nullptr;
    surface_options options_{};
    bool created_ = false;
    bool registered_ = false;
    bool broken_ = false;

    lx::wayland::surface surface_{};
    input_router input_{};

    std::unique_ptr<lx::runtime::memory_arena> arena_{};
    std::unique_ptr<lx::ui::ui_context> ui_{};
    std::unique_ptr<lx::ui::ui_root> root_{};
    std::unique_ptr<lx::scene::draw_list> draws_{};

    lx::gfx::format_choice format_{};
    lx::gfx::client_swapchain swapchain_{};
    bool pixel_path_ = false;
    std::unique_ptr<frame_source> source_{};
    std::unique_ptr<lx::runtime::fd_source> release_source_{};
    buffer_slot buffers_[k_buffer_slots]{};
    shm_pool_slot pools_[k_shm_pools]{};
    const lx::gfx::swapchain_image* last_image_ = nullptr;
    bool opaque_region_set_ = false;

    // configure and scale
    surface_configure pending_cfg_{};
    bool configure_pending_ = false;
    bool configured_ = false;
    bool scale_pending_ = false;
    bool fractional_ = false;
    bool viewport_ = false;
    lx::size2i logical_{};
    lx::size2i buffer_{};
    lx::size2i viewport_dst_{};
    float scale_ = 1.f;
    int buffer_scale_ = 1;

    bool waiting_release_ = false;
    bool continuous_ = false;
    bool presentation_feedback_ = false;
    void (*keyboard_focus_fn_)(bool entered, void* user) = nullptr;
    void* keyboard_focus_user_ = nullptr;
    /// When the outstanding frame callback was requested (a held configure's limit).
    std::uint64_t frame_requested_ns_ = 0;
    /// The wp_linux_drm_syncobj_surface_v1 exists (explicit sync settled on).
    bool syncobj_ = false;
    std::uint64_t frame_start_ns_ = 0;
    std::uint64_t frame_start_cpu_ns_ = 0;
    std::uint64_t frame_input_ns_ = 0;

    /// The frame ring (`k_frame_ring` records), only with `frame_records`: the caller's
    /// (`surface_options::frame_ring`) or `ring_owned_`.
    frame_record* ring_ = nullptr;
    std::unique_ptr<frame_record[]> ring_owned_{};
    std::uint64_t ring_next_ = 0;
    surface_stats stats_{};
    lx::gfx::damage_region last_damage_{};
    surface_timeline timeline_{};
};

} // namespace lx::ui::host
