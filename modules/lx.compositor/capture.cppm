module;

// Screen capture policy (design-app-compat-capture Part D): ext-image-copy-capture-v1
// sessions over ext-image-capture-source-v1 sources, and wlr-screencopy-unstable-v1 frames
// as one-shot sessions of the same model. Wayland-free and pure: the wire (wire_capture.cpp,
// wire_screencopy.cpp) keeps the protocol objects and asks this partition what a request
// means — which protocol error, whether a buffer fits the session's constraints, and what a
// requested frame gets now (a copy, the lock curtain, nothing yet, or `stopped`). The
// render-side work (blits, read-backs, re-composites) is `:capture_engine`'s.
//
// The rules, in short (design §0 CC1–CC10):
//   - one frame per session (the protocol's), at most `k_capture_sessions` sessions, 16 per
//     client — a session beyond either is created and stopped at once;
//   - an output session's frames are its output's composited scanout, a toplevel session's
//     its window's tree projected into the buffer (a capture view of the scene);
//   - from the lock request on, no frame carries session content: the next frame of every
//     session is a black curtain, then frames are held until unlock — except output
//     sessions of the client holding the lock, which see the lock surfaces once the scene
//     is locked (CC7);
//   - a paused (VT away) or dark output holds its sessions' frames (CC8);
//   - an output or window that goes stops its sessions (CC9).
//
// UI affinity. Bodies in capture.cpp (the Clang 23 partition rule, see session_lock.cpp).

export module lx.compositor:capture;

import std;

import lx.foundation;
import lx.gfx;

export namespace lx::compositor {

inline constexpr unsigned k_capture_sessions = 64;
inline constexpr unsigned k_capture_sessions_per_client = 16;
/// Toplevel sessions render through scene capture views, one per captured window (several
/// sessions of one window share it).
inline constexpr unsigned k_capture_toplevel_views = 8;

/// wl_shm.format codes (what ext_image_copy_capture_session_v1.shm_format and
/// zwlr_screencopy_frame_v1.buffer carry).
inline constexpr std::uint32_t k_wl_shm_argb8888 = 0;
inline constexpr std::uint32_t k_wl_shm_xrgb8888 = 1;

enum class capture_source_kind : unsigned char { none, output, toplevel };

/// ext_image_copy_capture_frame_v1.failure_reason.
enum class capture_failure : unsigned { unknown = 0, buffer_constraints = 1, stopped = 2 };

/// ext_image_copy_capture_frame_v1.error (0: none).
enum class capture_frame_error : unsigned {
    none = 0,
    no_buffer = 1,
    invalid_buffer_damage = 2,
    already_captured = 3,
};

/// What a session accepts (ext_image_copy_capture_session_v1 constraint events): the buffer
/// size, shm formats (wl_shm codes), dma-buf formats (fourcc, LINEAR only) and the dma-buf
/// device. Sent at creation and whenever it changes, closed by `done`.
struct capture_constraints {
    lx::size2i size{};
    std::uint32_t shm_formats[2]{};
    unsigned shm_count = 0;
    lx::fourcc dmabuf_formats[2]{};
    unsigned dmabuf_count = 0;
    /// dev_t of the dma-buf device; 0: dma-buf is not offered.
    unsigned long long dmabuf_device = 0;

    [[nodiscard]] bool operator==(const capture_constraints& o) const;
    [[nodiscard]] bool valid() const { return size.width > 0 && size.height > 0; }
};

/// What the backend offers: shm only (CPU, Vulkan), or shm and LINEAR dma-buf (GL).
[[nodiscard]] capture_constraints make_capture_constraints(lx::size2i size, bool dmabuf,
                                                           unsigned long long dmabuf_device);

/// A client buffer as the wire found it.
enum class capture_buffer_kind : unsigned char { none, shm, dmabuf };
struct capture_buffer_desc {
    capture_buffer_kind kind = capture_buffer_kind::none;
    unsigned width = 0;
    unsigned height = 0;
    /// shm: the wl_shm format code; dma-buf: the fourcc.
    std::uint32_t format = 0;
    unsigned long long modifier = 0;
    unsigned stride = 0;
    /// dma-buf planes (only single-plane buffers are accepted).
    unsigned planes = 0;
};

/// Whether a buffer matches the constraints: its size, a format offered for its kind, and
/// for dma-buf one plane with the LINEAR (or the implicit, invalid) modifier, for shm a
/// stride of at least width × 4. False is `failed(buffer_constraints)`.
[[nodiscard]] bool capture_buffer_fits(const capture_constraints& c, const capture_buffer_desc& b);

/// The frame object's requests before capture (ext_image_copy_capture_frame_v1).
struct capture_frame_requests {
    bool buffer = false;
    bool captured = false;
    /// damage_buffer, buffer pixels (clipped to the buffer at capture).
    lx::gfx::damage_region damage{};
};
/// attach_buffer: `already_captured` after capture.
[[nodiscard]] capture_frame_error capture_frame_attach(capture_frame_requests& f);
/// damage_buffer: `invalid_buffer_damage` for x < 0, y < 0, w ≤ 0, h ≤ 0; `already_captured`
/// after capture. Otherwise the rect joins the frame's damage.
[[nodiscard]] capture_frame_error capture_frame_damage(capture_frame_requests& f, int x, int y,
                                                       int w, int h);
/// capture: `already_captured` the second time, `no_buffer` without an attached buffer.
[[nodiscard]] capture_frame_error capture_frame_capture(capture_frame_requests& f);

/// A session as the policy sees it.
struct capture_session {
    bool used = false;
    /// Bumped at every open of the slot: render jobs and completions name it, so neither a
    /// late completion nor a cancelled job reaches the slot's next session.
    unsigned generation = 0;
    capture_source_kind kind = capture_source_kind::none;
    /// lx client id of the session's owner (0 for none).
    unsigned client = 0;
    /// Output sessions: the output slot (render) and its id (the session's identity: a slot
    /// reused by another output is not the same source).
    unsigned output_slot = 0;
    lx::output_id output{};
    /// Toplevel sessions.
    lx::toplevel_id toplevel{};
    bool paint_cursors = false;
    /// A one-shot session of a wlr-screencopy frame.
    bool screencopy = false;
    bool stopped = false;
    /// A frame went ready (the first one carries full damage and is never held back).
    bool delivered = false;
    /// The lock curtain was delivered (frames are held until unlock, then full damage).
    bool curtained = false;
    /// The next delivered frame carries full damage (first frame, constraints changed,
    /// resume, unlock).
    bool full_damage = true;
    capture_constraints constraints{};
};

/// Fixed table of sessions. Index + 1 is a session's handle (0: none).
class capture_session_table {
public:
    /// A new session for `client`. 0 when the table or the client's share is full — the
    /// wire then makes the object and stops it at once.
    [[nodiscard]] unsigned open(unsigned client, capture_source_kind kind);
    void close(unsigned handle);
    /// `capture_session*`, or `const capture_session*` on a const table.
    [[nodiscard]] auto get(this auto& self, unsigned handle) -> decltype(&self.sessions_[0]) {
        if (handle == 0 || handle > k_capture_sessions || !self.sessions_[handle - 1].used)
            return nullptr;
        return &self.sessions_[handle - 1];
    }
    /// The session of `handle` if it is still of `generation`.
    [[nodiscard]] capture_session* get(unsigned handle, unsigned generation);
    [[nodiscard]] unsigned count() const { return count_; }
    [[nodiscard]] unsigned count_of(unsigned client) const;
    /// Calls `fn(handle, session)` for every open session.
    template <class Fn>
    void for_each(Fn&& fn) {
        if (count_ == 0)
            return;
        for (unsigned i = 0; i < k_capture_sessions; ++i)
            if (sessions_[i].used)
                fn(i + 1, sessions_[i]);
    }

private:
    capture_session sessions_[k_capture_sessions]{};
    unsigned count_ = 0;
};

/// The world a requested frame is decided in.
struct capture_env {
    /// From the lock request until unlock (ext-session-lock-v1: `locking` and `locked`).
    bool locked = false;
    /// The lx client id holding the lock (0: none).
    unsigned locker_client = 0;
    /// The source output has published a frame built with the scene locked (`lock_frame`):
    /// its scanout shows only lock surfaces from that frame on.
    bool lock_scene_shown = false;
    /// The source is gone (output removed, window unmapped).
    bool source_gone = false;
    /// The source output's display is stopped: VT away, powered off.
    bool held = false;
};

enum class capture_action : unsigned char {
    /// Nothing can be delivered yet (held, curtained and locked): stays requested.
    wait,
    /// Copy the source (the render thread waits for new content unless the frame is the
    /// session's first, or its full damage is due).
    copy,
    /// The black lock curtain, full damage; afterwards frames are held until unlock.
    curtain,
    /// failed(stopped).
    stop,
};

/// What a session's requested frame gets now (CC7–CC9).
[[nodiscard]] capture_action capture_decide(const capture_session& s, const capture_env& env);

/// The damage the render thread composited into one output since a point: a ring of the
/// last `k_depth` composites' damage, each numbered (`seq`, from 1). A capture session
/// remembers the seq it last copied; the copy region is everything since. Render affinity.
class capture_damage_ring {
public:
    static constexpr unsigned k_depth = 8;
    /// A composite with `damage` (output pixels) happened; returns its seq.
    unsigned record(const lx::gfx::damage_region& damage);
    /// Union of the damage of composites (`after`, `seq()`] into `out`. False when that range
    /// is older than the ring keeps (the caller copies everything). `after == seq()`: empty.
    [[nodiscard]] bool since(unsigned after, lx::gfx::damage_region& out) const;
    [[nodiscard]] unsigned seq() const { return seq_; }
    /// The content changed in a way damage does not describe (a new mode): every older seq
    /// now means "everything".
    void reset();

private:
    lx::gfx::damage_region ring_[k_depth]{};
    unsigned seq_ = 0;
    unsigned floor_ = 0;
};

/// Where the pointer image lands in an output's pixels: the image `img_w` x `img_h` with its
/// hotspot (`hot_x`, `hot_y`, already turned by the output's transform when it is), placed
/// at the layout point (`x`, `y`) on an output at `origin` / `scale` / `transform` with mode
/// `mode`. The same math as the cursor plane's. Returns the rect (may reach off the output).
[[nodiscard]] lx::rect2i capture_cursor_rect(double x, double y, lx::point2i origin, double scale,
                                             lx::buffer_transform transform, lx::size2i mode,
                                             unsigned img_w, unsigned img_h, int hot_x, int hot_y);

/// Premultiplied ARGB8888 `src` (`src_w` x `src_h`, `src_stride_px` words per row) over the
/// BGRA rows of `dst` (`dst_w` x `dst_h`, `dst_stride` bytes) at (`x`, `y`), clipped. Opaque
/// destinations (XRGB) keep their alpha byte at 0xFF.
void capture_blend_argb(unsigned char* dst, unsigned dst_w, unsigned dst_h, unsigned dst_stride,
                        const std::uint32_t* src, unsigned src_w, unsigned src_h,
                        unsigned src_stride_px, int x, int y);

/// The pointer a frame painted (paint_cursors), in source pixels: the next frame repaints
/// where it was and decides whether a pointer move alone is reason for a frame. Kept by UI
/// per session (per client and output for wlr-screencopy's one-shot frames) and carried by
/// every render job and completion — render keeps no per-session pointer state.
struct capture_cursor_memo {
    bool known = false;
    bool painted = false;
    lx::rect2i rect{};
    unsigned generation = 0;
};

/// wlr-screencopy `copy_with_damage`: per (client, output), the damage-ring seq of that
/// client's last copy of that output — its next frame waits for a composite past it and
/// reports the damage since. Fixed table; the least recently used entry is taken over (its
/// next frame is then full).
class screencopy_seq_table {
public:
    static constexpr unsigned k_entries = 32;
    /// False when the client has not copied that output yet (or its entry was taken over).
    [[nodiscard]] bool get(unsigned client, lx::output_id output, unsigned& seq,
                           capture_cursor_memo& cursor) const;
    void set(unsigned client, lx::output_id output, unsigned seq,
             const capture_cursor_memo& cursor);
    /// A client is gone: its entries are freed.
    void forget_client(unsigned client);

private:
    struct entry {
        unsigned client = 0;
        lx::output_id output{};
        unsigned seq = 0;
        capture_cursor_memo cursor{};
        unsigned long long used = 0;
        bool live = false;
    };
    entry entries_[k_entries]{};
    unsigned long long clock_ = 0;
};

} // namespace lx::compositor
