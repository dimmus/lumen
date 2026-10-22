module;

// The GL objects of screen capture (design-app-compat-capture D.4, CC1–CC5): a client's
// dma-buf as a framebuffer (imported once per buffer, kept while the buffer lives), the one
// blit that copies a composited frame into it, and for shm clients offscreen framebuffers
// over LINEAR GBM buffers (a frame or a window's capture view is blitted or composited into
// one, then `read_bo` maps and copies it on a thread that may block), with an asynchronous
// read-back through a pixel-pack buffer — read behind a native fence, mapped once it has
// signalled — where there is no GBM buffer or no such thread. The render thread never waits
// on the GPU.
//
// Needs OpenGL ES 3.0 (glBlitFramebuffer, pixel-pack buffers, GL_PACK_ROW_LENGTH); Mesa gives
// a 3.x context for the 2.0 the compositor asks for wherever the driver has it (SVGA3D: 3.1).
// Without it `initialize` fails and capture falls back to what the caller can do without.
// Render affinity: the EGL context is current on the render thread. Bodies in
// gl_capture.cpp.

export module lx.gfx:gl_capture;

import lx.foundation;
import :damage;
import :gl_renderer;

export namespace lx::gfx {

class gl_capture {
public:
    /// dma-buf framebuffers kept (a screencast cycles a handful of buffers; ≥ 4 per stream).
    static constexpr unsigned k_max_dmabuf_targets = 32;
    /// Per-owner (session) offscreen framebuffers and pixel-pack buffers.
    static constexpr unsigned k_max_owners = 64;

    gl_capture() = default;
    ~gl_capture();
    gl_capture(const gl_capture&) = delete;
    gl_capture& operator=(const gl_capture&) = delete;

    /// Requires `device` current on this thread. Fails without ES 3.0.
    [[nodiscard]] lx::result<void> initialize(egl_device& device);
    void shutdown();
    [[nodiscard]] bool ready() const { return ready_; }
    /// GL_EXT_read_format_bgra: read-backs land in wl_shm's byte order without a swizzle.
    [[nodiscard]] bool reads_bgra() const { return read_bgra_; }

    /// The framebuffer over the client dma-buf `key` (a never-reused id of its wl_buffer),
    /// importing it on first use. `fd` is borrowed. 0 with an error when the driver refuses
    /// the buffer as a render target.
    [[nodiscard]] lx::result<unsigned> dmabuf_framebuffer(unsigned key, int fd, unsigned offset,
                                                          unsigned stride, unsigned width,
                                                          unsigned height, lx::fourcc format,
                                                          unsigned long long modifier);
    /// The wl_buffer behind `key` is gone: its import goes.
    void forget_dmabuf(unsigned key);

    /// An offscreen framebuffer (`width` x `height`, ARGB) owned by `owner` (re-created on a
    /// size change): a LINEAR GBM buffer object where the device has GBM — what a shm capture
    /// is read from off the render thread (`offscreen_bo`, `read_bo`) — else a renderbuffer.
    [[nodiscard]] lx::result<unsigned> offscreen_framebuffer(unsigned owner, unsigned width,
                                                             unsigned height);
    /// The GBM buffer object (`gbm_bo*`) behind `owner`'s offscreen framebuffer; null for a
    /// renderbuffer one.
    [[nodiscard]] void* offscreen_bo(unsigned owner);
    /// `owner` (a session) is gone: its offscreen framebuffer and pixel-pack buffer go.
    void forget_owner(unsigned owner);

    /// Copies `rects` (full: everything) of `src`, a `width` x `height` framebuffer with row 0
    /// on top, into `dst` at the same place less (`dx`, `dy`) — a crop of `src` when `dst` is
    /// the size of the cropped area (rects outside it are clipped by the caller).
    [[nodiscard]] lx::result<void> blit(unsigned src, unsigned dst, unsigned width,
                                        unsigned height, const damage_region& rects, int dx = 0,
                                        int dy = 0);
    /// Clears `dst` to `colour` (the lock curtain).
    [[nodiscard]] lx::result<void> clear(unsigned dst, unsigned width, unsigned height,
                                         lx::color colour);
    /// Starts reading `rects` of `src` (`width` x `height`) into `owner`'s pixel-pack buffer:
    /// rows of `width` × 4 bytes, BGRA (RGBA without `reads_bgra`).
    [[nodiscard]] lx::result<void> read_async(unsigned owner, unsigned src, unsigned width,
                                              unsigned height, const damage_region& rects);
    /// Copies `rects` of `owner`'s last read (of a `width` x `height` source) into `dst`
    /// (BGRA rows `dst_stride` apart) at their place less (`dx`, `dy`), clipped to
    /// `dst_width` x `dst_height`. Maps the buffer: call once its fence has signalled, or it
    /// waits.
    [[nodiscard]] lx::result<void> finish_read(unsigned owner, unsigned char* dst,
                                               unsigned dst_stride, unsigned width,
                                               unsigned height, const damage_region& rects,
                                               int dx = 0, int dy = 0, unsigned dst_width = 0,
                                               unsigned dst_height = 0);
    /// A sync_file signalled when the GL work issued so far completes; -1 without native
    /// fences (then the work was flushed and the caller cannot wait without blocking; it
    /// should treat the work as done when it next runs).
    [[nodiscard]] int fence();

    /// Copies `rects` of an offscreen GBM buffer object (`offscreen_bo`, `width` x `height`,
    /// ARGB) into `dst` (BGRA rows `dst_stride` apart) at their place less (`dx`, `dy`),
    /// clipped to `dst_width` x `dst_height`. No GL: callable on any thread once the GPU
    /// work that wrote the buffer has completed — the map is where a driver without
    /// asynchronous read-backs (SVGA3D) spends its time, so it belongs off the render thread.
    /// `opaque`: the alpha byte is forced to 0xFF.
    [[nodiscard]] static bool read_bo(void* bo, unsigned width, unsigned height,
                                      const damage_region& rects, unsigned char* dst,
                                      unsigned dst_stride, int dx, int dy, unsigned dst_width,
                                      unsigned dst_height, bool opaque);

private:
    struct dmabuf_target {
        unsigned key = 0;
        void* image = nullptr;
        unsigned renderbuffer = 0;
        unsigned framebuffer = 0;
        unsigned width = 0;
        unsigned height = 0;
        unsigned long long used = 0;
    };
    struct owner_objects {
        unsigned owner = 0;
        unsigned renderbuffer = 0;
        unsigned framebuffer = 0;
        unsigned width = 0;
        unsigned height = 0;
        unsigned pbo = 0;
        unsigned pbo_bytes = 0;
        /// The offscreen buffer's GBM buffer object and its EGLImage (null: a renderbuffer).
        void* bo = nullptr;
        void* image = nullptr;
        bool used = false;
    };
    void destroy(dmabuf_target& t);
    void destroy(owner_objects& o);
    [[nodiscard]] owner_objects* owner(unsigned id, bool create);

    egl_device* device_ = nullptr;
    bool ready_ = false;
    bool read_bgra_ = false;
    dmabuf_target targets_[k_max_dmabuf_targets]{};
    owner_objects owners_[k_max_owners]{};
    unsigned long long clock_ = 0;
};

} // namespace lx::gfx
