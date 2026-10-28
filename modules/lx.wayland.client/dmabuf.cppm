module;

// lx.wayland.client:dmabuf — zwp_linux_dmabuf_feedback_v1 (linux-dmabuf v4+): the format
// table (mmapped, read once per table), the main device, tranches with their target device,
// flags and format/modifier indices, folded into one immutable snapshot per `done`. Bodies
// in dmabuf.cpp.

struct zwp_linux_dmabuf_feedback_v1;

export module lx.wayland.client:dmabuf;

import std;

import lx.foundation;
import :types;

export namespace lx::wayland {

/// A default feedback (`display::default_feedback()`) or a surface's
/// (`create(display, &surface)`). The snapshot is allocated once at `create` (≈64 KiB with
/// the fixed caps); `done` rewrites it in place, so delivering feedback allocates nothing.
/// UI affinity.
class dmabuf_feedback {
public:
    dmabuf_feedback() = default;
    ~dmabuf_feedback();
    dmabuf_feedback(const dmabuf_feedback&) = delete;
    dmabuf_feedback& operator=(const dmabuf_feedback&) = delete;

    /// get_default_feedback, or get_surface_feedback for `target`. Needs linux-dmabuf ≥ 4.
    [[nodiscard]] lx::result<void> create(display& d, surface* target = nullptr);
    void destroy();
    [[nodiscard]] bool valid() const { return proxy_ != nullptr; }

    void set_handler(dmabuf_feedback_handler* h) { handler_ = h; }
    /// The last complete feedback (generation 0: none yet).
    [[nodiscard]] const dmabuf_feedback_snapshot& snapshot() const;
    [[nodiscard]] bool received() const;

    // Event entry points (the listener thunks call these).
    void on_format_table(int fd, std::uint32_t size);
    void on_main_device(std::uint64_t dev);
    void on_tranche_target_device(std::uint64_t dev);
    void on_tranche_flags(std::uint32_t flags);
    void on_tranche_formats(const std::uint16_t* indices, std::size_t count);
    void on_tranche_done();
    void on_done();

private:
    void unmap_table();

    zwp_linux_dmabuf_feedback_v1* proxy_ = nullptr;
    dmabuf_feedback_handler* handler_ = nullptr;
    std::unique_ptr<dmabuf_feedback_snapshot> current_{};
    /// The tranche being received, resolved through the table as its indices arrive.
    std::unique_ptr<dmabuf_feedback_snapshot> pending_{};
    const unsigned char* table_ = nullptr;
    std::size_t table_bytes_ = 0;
    bool tranche_open_ = false;
    bool warned_ = false;
};

} // namespace lx::wayland
