module;

// The render half of screen capture (design-app-compat-capture D.4, D.10): jobs UI → render,
// completions render → UI, and the render thread's execution of them on whichever backend
// composites. Wayland-free: client shm memory is reached through `capture_shm_guards` (the
// libwayland access calls are hooks, like `shm_direct_uploads`), client dma-bufs as dup'd
// plane fds.
//
// Where frames come from (CC1, CC2):
//   - an output session copies the frame the output just composited — on GL one
//     glBlitFramebuffer per damage rect into the client's dma-buf, or for shm a blit into
//     a LINEAR GBM buffer that a worker thread maps and copies once the blit's fence has
//     signalled (the map is where SVGA3D spends ~12 ms; render never waits; without a
//     worker or GBM, a pixel-pack buffer read copied out by a second render job); on the CPU path the frame's draws are composited straight
//     into the client's shm; Vulkan reads its target back. A session that needs a frame with
//     nothing new composited (its first, a pointer move with paint_cursors) copies the
//     newest composited frame, which stays intact in its scanout slot until the next
//     composite of the output;
//   - a toplevel session composites its capture view (the window's tree, projected into
//     the buffer) into the client buffer, or an offscreen buffer read back for shm.
// Damage (CC3): per output, a ring of the damage of its last composites; a session copies
// everything since the composite it last copied, plus the client's damage. A job waits until
// its output composites something new — a capture request starts no clock (CC10).
//
// Bounded: one job per session in flight (the protocol has one frame per session), fixed
// tables, no allocation per frame (a session's staging is sized once).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

export module lx.compositor:capture_engine;

import lx.foundation;
import lx.gfx;
import lx.scene;
import :capture;

export namespace lx::compositor {

/// The software pointer's texture (cursor_manager's `k_cursor_texture_id`): left out of a
/// re-composite for a session that must not show the pointer.
inline constexpr unsigned k_capture_hidden_texture = 0x7FFF'FF00u;
/// The pointer image painted onto GL dma-buf copies (registered by the capture engine).
inline constexpr unsigned k_capture_cursor_texture = 0x7FFF'FF01u;

// ── client shm under a guard ─────────────────────────────────────────────────

/// Client shm buffers the render thread writes captured pixels into. The entry holds an
/// external wl_shm_pool reference (the mapping stays where it is), and its lock makes the
/// buffer's destruction wait for a copy in progress: once `cancel` returns, render will not
/// touch the memory again (the client may reuse it for another buffer). Handles are index +
/// 1; 0 is none. UI affinity except `write`.
class capture_shm_guards {
public:
    static constexpr unsigned k_max = 128;
    struct hooks {
        void (*begin_access)(void* shm_buffer) = nullptr;
        void (*end_access)(void* shm_buffer) = nullptr;
        void (*unref_pool)(void* shm_pool) = nullptr;
    };
    void bind(hooks h) { hooks_ = h; }
    /// 0 when full.
    [[nodiscard]] unsigned add(void* shm_buffer, void* shm_pool, unsigned char* pixels,
                               unsigned stride, unsigned width, unsigned height, bool opaque);
    /// The buffer is going (or its frame is): render will not write it after this returns.
    void cancel(unsigned handle);
    /// The job is over: the pool reference goes.
    void release(unsigned handle);
    /// Render. Calls `fn(pixels, stride, width, height, opaque)` with the memory guarded; false
    /// when the entry was cancelled (nothing was written).
    template <class Fn>
    bool write(unsigned handle, Fn&& fn) {
        if (handle == 0 || handle > k_max)
            return false;
        entry& e = entries_[handle - 1];
        std::lock_guard lock{e.mutex};
        if (!e.used || e.cancelled || !e.pixels)
            return false;
        if (hooks_.begin_access)
            hooks_.begin_access(e.shm);
        fn(e.pixels, e.stride, e.width, e.height, e.opaque);
        if (hooks_.end_access)
            hooks_.end_access(e.shm);
        return true;
    }
    [[nodiscard]] unsigned in_use() const { return used_; }

private:
    struct entry {
        std::mutex mutex{};
        void* shm = nullptr;
        void* pool = nullptr;
        unsigned char* pixels = nullptr;
        unsigned stride = 0;
        unsigned width = 0;
        unsigned height = 0;
        bool opaque = false;
        bool cancelled = false;
        bool used = false;
    };
    entry entries_[k_max]{};
    unsigned used_ = 0;
    hooks hooks_{};
};

// ── jobs and completions ─────────────────────────────────────────────────────

enum class capture_job_kind : unsigned char {
    /// Copy an output's composited frame.
    output,
    /// Composite a window's capture view.
    toplevel,
    /// The black lock curtain.
    curtain,
    /// The pixel-pack read of a shm frame has landed: copy it out and complete.
    finish,
    /// A dma-buf the render side imported is gone.
    forget_buffer,
    /// A session is gone: its render state (offscreen buffer, staging) goes.
    forget_session,
};

struct capture_target {
    bool dmabuf = false;
    unsigned width = 0;
    unsigned height = 0;
    /// dma-buf: never-reused key of the wl_buffer (its import is cached by it), plane 0 (the
    /// fd is the job's: render closes it), the fourcc and modifier.
    unsigned buffer_key = 0;
    int fd = -1;
    unsigned offset = 0;
    unsigned stride = 0;
    lx::fourcc format = 0;
    unsigned long long modifier = 0;
    /// shm: the guard entry.
    unsigned shm = 0;
};

struct capture_job {
    capture_job_kind kind = capture_job_kind::output;
    unsigned session = 0;
    unsigned generation = 0;
    /// The frame (per session, increasing): completions name it.
    unsigned serial = 0;
    unsigned output_slot = 0;
    /// toplevel: the scene view of the window's capture view, the frame it published, and
    /// its projection (where the pointer lands for paint_cursors).
    unsigned view = 0;
    unsigned view_frame = 0;
    lx::point2i view_origin{};
    double view_scale = 1.0;
    /// Copy everything, now (the session's first frame, new constraints, resume, unlock).
    bool full = false;
    /// output: the part of the source copied, to the buffer's origin (a wlr-screencopy
    /// region; source pixels). Empty: the whole source.
    lx::rect2i crop{};
    bool paint_cursors = false;
    /// The damage-ring seq the session last copied (0: never).
    unsigned since_seq = 0;
    /// The lock holder's session while locked: only frames from `min_frame` on.
    bool has_min_frame = false;
    unsigned min_frame = 0;
    /// `capture_channel::lock_epoch` when issued: a content job issued before a lock request
    /// is cancelled rather than run after it.
    unsigned lock_epoch = 0;
    capture_target target{};
    /// The client's damage_buffer (buffer pixels).
    lx::gfx::damage_region client_damage{};
    /// What the previous frame of this session painted of the pointer.
    capture_cursor_memo cursor{};
};

enum class capture_result : unsigned char {
    /// Copied: send the frame events and ready.
    ready,
    /// Copied by the GPU: ready once `fence_fd` signals.
    fence,
    /// A shm read is on its way: once `fence_fd` signals, post a `finish` job.
    readback,
    failed,
    /// Not run (the lock epoch moved on, or the session went): decide again.
    cancelled,
};

struct capture_completion {
    unsigned session = 0;
    unsigned generation = 0;
    unsigned serial = 0;
    capture_result result = capture_result::failed;
    capture_failure reason = capture_failure::unknown;
    /// Owned by the completion (the UI side closes it or hands it to the poller).
    int fence_fd = -1;
    /// What changed since the session's last ready (buffer pixels).
    lx::gfx::damage_region damage{};
    /// The damage-ring seq of the copied frame (output sessions).
    unsigned seq = 0;
    /// The scene frame copied, and when it was composited (CLOCK_MONOTONIC ns).
    unsigned frame = 0;
    unsigned long long time_ns = 0;
    /// The copy-out of a shm read (`finish`): the frame's facts came with its `readback`.
    bool finished_read = false;
    /// The job's lock epoch: UI decides again a content frame copied in an epoch the lock has
    /// moved past (it would show what the lock hides).
    unsigned lock_epoch = 0;
    /// The job's shm guard entry (0: none): released by UI once the completion is final —
    /// render no longer holds the handle then.
    unsigned shm = 0;
    /// What this frame painted of the pointer (the next job's `cursor`).
    capture_cursor_memo cursor{};
};

/// UI ↔ render. Fixed queues under one lock; a full queue keeps a job on the UI side until
/// the next tick (never dropped).
class capture_channel {
public:
    static constexpr unsigned k_jobs = 128;
    static constexpr unsigned k_completions = 128;

    /// UI. False when full (the caller retries later).
    [[nodiscard]] bool post(const capture_job& job);
    /// Render. Moves the posted jobs into `out` (up to `cap`); their count.
    unsigned take(capture_job* out, unsigned cap);
    /// Render. False when full: the completion is kept by the caller and pushed again.
    [[nodiscard]] bool complete(const capture_completion& c);
    /// UI. Moves the completions into `out` (up to `cap`); their count.
    unsigned drain(capture_completion* out, unsigned cap);
    [[nodiscard]] bool has_completions() const {
        return completions_ready_.load(std::memory_order_acquire);
    }
    /// UI writes at a lock request; render reads before running a content job.
    std::atomic<unsigned> lock_epoch{0};
    /// UI: sessions exist (render records composite damage only then).
    std::atomic<unsigned> sessions{0};

private:
    std::mutex mutex_{};
    capture_job jobs_[k_jobs]{};
    unsigned job_count_ = 0;
    capture_completion completions_[k_completions]{};
    unsigned completion_count_ = 0;
    std::atomic<bool> completions_ready_{false};
};

// ── what the compositor tells the wire ───────────────────────────────────────

enum class capture_backend : unsigned char { none, gl, cpu, vulkan };

/// One output as capture sees it (refreshed by the compositor before every capture tick and
/// whenever outputs change). UI affinity.
struct capture_output_info {
    bool live = false;
    lx::output_id id{};
    /// The scanout in pixels, and the projection (cursor placement, window scale).
    lx::size2i mode{};
    double scale = 1.0;
    lx::point2i origin{};
    lx::buffer_transform transform = lx::buffer_transform::normal;
    /// Its display is stopped: the VT is away, or it is powered off (CC8).
    bool held = false;
    /// The first frame built with the scene locked (valid while locked), and whether it has
    /// been published (CC7: the lock holder's sessions see frames from it on).
    unsigned lock_frame = 0;
    bool lock_shown = false;
};

/// The compositor's side of capture, owned by it and pointed at by the protocol context.
/// UI affinity (the channel and guards are shared with render by design).
struct capture_host {
    capture_backend backend = capture_backend::none;
    /// dma-buf targets (GL with ES 3.0), and the device clients allocate them on.
    bool dmabuf = false;
    unsigned long long dmabuf_device = 0;
    capture_output_info outputs[16]{};
    /// The VT is away: no content can change anywhere (window sessions hold).
    bool paused = false;
    capture_channel* channel = nullptr;
    capture_shm_guards* guards = nullptr;
    /// Toplevel sessions publish capture views of the scene.
    scene::scene_graph* scene = nullptr;
    /// The frame a capture tick builds (its capture views carry it).
    unsigned frame = 0;
    void* user = nullptr;
    /// Render has jobs.
    void (*wake_render)(void* user) = nullptr;
    /// Outputs whose clock must run (a capture view needs a tick).
    void (*request_frame)(void* user, std::uint32_t mask) = nullptr;
    /// When a frame of output `slot` composited at `composited_ns` reaches the screen (the
    /// frame clock's prediction); 0: unknown.
    unsigned long long (*predict)(void* user, unsigned slot, unsigned long long composited_ns) =
        nullptr;
};

// ── the render side ──────────────────────────────────────────────────────────

/// The pointer as one output (or capture view) shows it, for paint_cursors.
struct capture_cursor {
    /// Premultiplied ARGB, `stride_px` words per row; null: no pointer image.
    const std::uint32_t* pixels = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    unsigned stride_px = 0;
    /// Where it lands in the source's pixels.
    lx::rect2i rect{};
    bool visible = false;
    /// Drawn by the scene (the software cursor): it is in the composited frame already.
    bool in_frame = false;
    /// Changes with the image (re-uploaded for GL when it does).
    unsigned generation = 0;
};

/// One composited frame of an output, as the render thread hands it to capture.
struct capture_frame_source {
    unsigned frame = 0;
    unsigned long long time_ns = 0;
    lx::size2i size{};
    /// GL: the scanout framebuffer holding the frame (0: none).
    unsigned gl_framebuffer = 0;
    /// The frame's draws (CPU always; GL when available — needed to leave the software
    /// pointer out). Null: not available.
    const lx::gfx::blit_command* blits = nullptr;
    unsigned blit_count = 0;
    lx::color clear = lx::color::rgb(0.f, 0.f, 0.f);
    /// Vulkan: the target holding the frame.
    lx::gfx::render_target* vk_target = nullptr;
    capture_cursor cursor{};
};

/// A window capture waiting for its view (`capture_renderer::toplevel_jobs`).
struct capture_toplevel_job {
    unsigned view = 0;
    unsigned frame = 0;
    bool paint_cursors = false;
    lx::point2i origin{};
    double scale = 1.0;
    lx::size2i size{};
};

/// Render affinity. Executes capture jobs on the active backend.
class capture_renderer {
public:
    capture_renderer() = default;
    capture_renderer(const capture_renderer&) = delete;
    capture_renderer& operator=(const capture_renderer&) = delete;

    /// Exactly one backend: GL (with its EGL device for the capture objects), CPU, or Vulkan.
    void bind_gl(lx::gfx::gl_compositor* gl, lx::gfx::egl_device* egl) {
        gl_ = gl;
        egl_ = egl;
    }
    void bind_cpu(lx::gfx::cpu_compositor* cpu) { cpu_ = cpu; }
    void bind_vulkan(lx::gfx::vulkan_compositor* vk) { vk_ = vk; }
    void bind_guards(capture_shm_guards* guards) { guards_ = guards; }
    /// GL: where shm read-backs run — a thread that may block (the compositor's worker): the
    /// render thread only blits into a mappable buffer and fences it. `post` queues
    /// `fn(arg)` there (false: it could not, and the read runs on render); `notify` tells UI
    /// that completions are waiting (thread-safe). Without it reads go through pixel-pack
    /// buffers on render.
    void bind_worker(bool (*post)(void (*fn)(void*), void* arg), void (*notify)(void* user),
                     void* user) {
        post_ = post;
        notify_ = notify;
        notify_user_ = user;
    }
    /// GL: brings up the capture objects (on the render thread, the context current). False
    /// when they cannot exist (no ES 3.0): GL sessions then fail.
    bool ensure_gl();

    /// Takes UI's jobs: forgets apply at once, finishes run now, the rest wait for their
    /// source. Completions go to `out`.
    void take(capture_channel& in);

    /// Output `slot` composited a frame with `damage` (output pixels): recorded, and every
    /// job of that output runs on it.
    void output_composited(unsigned slot, const lx::gfx::damage_region& damage,
                           const capture_frame_source& src, capture_channel& out);
    /// Output `slot` has jobs that run without a new composite (a first frame, a pointer
    /// moved for paint_cursors, a curtain): the caller then calls `output_front` with the
    /// newest composited frame. `cursor` is where the pointer is now on that output.
    [[nodiscard]] bool wants_front(unsigned slot, const capture_cursor& cursor) const;
    void output_front(unsigned slot, const capture_frame_source& src, capture_channel& out);
    /// The output's targets went (mode change, retire, resume): no newest frame until it
    /// composites again, and older damage means everything.
    void output_reset(unsigned slot);
    /// Curtains run now (no source needed).
    void run_curtains(capture_channel& out);
    /// Toplevel jobs waiting, for the caller to acquire their views.
    [[nodiscard]] unsigned toplevel_jobs(capture_toplevel_job* out, unsigned cap) const;
    /// The capture view `view` (acquired by the caller) as `src`: its jobs run on it with the
    /// view's own damage (`damage`, since its previous publish).
    void toplevel_composited(unsigned view, const lx::gfx::damage_region& damage,
                             const capture_frame_source& src, capture_channel& out);
    /// Completions the queue could not take yet, pushed again.
    void flush(capture_channel& out);
    [[nodiscard]] bool has_pending() const { return pending_count_ > 0 || overflow_count_ > 0; }
    /// GL: a fence (sync_file) after this tick's capture composites that sampled client
    /// textures, or -1 when none did, `wanted` is false, or the driver exports none. The
    /// caller owns it; it is what explicit-sync release points must follow besides the
    /// output composites (the GPU completes work in order, so it covers both). Clears the
    /// mark either way.
    [[nodiscard]] int take_sample_fence(bool wanted);
    /// The newest capture-view frame composited (window captures; -1: none yet) — the
    /// buffer lifecycle's low-water mark may advance to it.
    [[nodiscard]] long long view_frame_done() const { return view_frame_done_; }

private:
    struct session_state {
        capture_renderer* owner = nullptr;
        unsigned generation = 0;
        /// A worker read of this session is in flight: its state and GL objects are the
        /// worker's until it clears this (render then neither runs a job of the session nor
        /// frees its objects). Forgotten while busy: freed once it is not (`doomed`).
        std::atomic<bool> busy{false};
        bool doomed = false;
        /// The worker read: the buffer object, its size, the completion it delivers and the
        /// channel it goes to, the fence it waits for first.
        void* read_bo = nullptr;
        unsigned read_bo_w = 0;
        unsigned read_bo_h = 0;
        int read_fence = -1;
        capture_completion read_done{};
        capture_channel* read_out = nullptr;
        /// shm read in flight: the region it covers (source pixels; the source's size, and the
        /// crop's origin it lands less), the cursor to blend once it lands.
        lx::gfx::damage_region read_region{};
        unsigned read_src_w = 0;
        unsigned read_src_h = 0;
        int read_dx = 0;
        int read_dy = 0;
        std::vector<std::uint32_t> cursor_copy{};
        lx::rect2i read_cursor_rect{};
        unsigned read_cursor_w = 0;
        unsigned read_cursor_h = 0;
        bool read_cursor = false;
        /// Vulkan: the read-back slot of `vulkan_compositor` the worker waits on.
        unsigned vk_slot = 0;
    };
    void complete(capture_channel& out, const capture_completion& c);
    /// GL shm: hands a read of `owner`'s offscreen buffer object to the worker (or runs it
    /// here). `c` is the completion to deliver once the pixels are in the client's buffer.
    void read_on_worker(session_state& st, const capture_job& job, capture_completion c,
                        unsigned w, unsigned h, const lx::gfx::damage_region& rects,
                        const capture_cursor* cursor, lx::rect2i cursor_at, capture_channel& out);
    static void worker_read(void* arg);
    /// Vulkan shm: waits for the slot's copy and writes the client's rows (a worker, or
    /// render when there is none).
    static void worker_read_vk(void* arg);
    /// Frees what forgotten sessions left while a worker read was in flight.
    void reap();
    /// Resets a session's state (not while busy).
    static void reset(session_state& st, unsigned generation);
    void fail(capture_channel& out, const capture_job& job, capture_failure reason);
    void close_job(capture_job& job);
    [[nodiscard]] session_state& state(const capture_job& job);
    /// Runs output job `job` on `src` with the damage of ring `slot` since its seq. False when
    /// it waits on (a crop nothing changed in).
    bool run_output(capture_job& job, const capture_frame_source& src, bool composited_now,
                    capture_channel& out);
    void run_finish(const capture_job& job, capture_channel& out);
    /// The pointer (if any) for job `job` onto a GL framebuffer / client rows.
    bool paint_cursor_gl(unsigned framebuffer, lx::size2i size, const capture_cursor& cursor);
    void remove_pending(unsigned index);
    /// Removes a job that will not run, telling UI (its guard is let go).
    void drop_pending(unsigned index, capture_channel& out);

    lx::gfx::gl_compositor* gl_ = nullptr;
    lx::gfx::egl_device* egl_ = nullptr;
    lx::gfx::cpu_compositor* cpu_ = nullptr;
    lx::gfx::vulkan_compositor* vk_ = nullptr;
    capture_shm_guards* guards_ = nullptr;
    bool (*post_)(void (*fn)(void*), void* arg) = nullptr;
    void (*notify_)(void* user) = nullptr;
    void* notify_user_ = nullptr;
    lx::gfx::gl_capture gl_capture_{};
    bool gl_tried_ = false;
    unsigned cursor_uploaded_gen_ = 0;

    capture_damage_ring rings_[16]{};
    capture_job pending_[capture_channel::k_jobs]{};
    unsigned pending_count_ = 0;
    capture_completion overflow_[capture_channel::k_completions]{};
    unsigned overflow_count_ = 0;
    session_state sessions_[k_capture_sessions]{};
    /// A capture composite sampled client textures since `take_sample_fence`.
    bool sampled_textures_ = false;
    long long view_frame_done_ = -1;
};

} // namespace lx::compositor
