module;

// lx.wayland.client:display — the connection: connect (an explicit name, WAYLAND_SOCKET,
// WAYLAND_DISPLAY — libwayland's order), the registry with every global the client runtime
// binds at a capped version, outputs and seats as fixed tables, xdg_wm_base ping/pong, the
// libwayland read protocol for a caller's poll loop and `display_source` for lx.runtime's,
// and protocol errors reported once as wayland_err. Bodies in display.cpp.

struct wl_display;
struct wl_registry;
struct wl_output;
struct zxdg_output_v1;

export module lx.wayland.client:display;

import std;

import lx.foundation;
import lx.runtime;
import :types;
import :seat;
import :dmabuf;

export namespace lx::wayland {

class display_source;

struct connect_options {
    /// A socket name or absolute path. Null: WAYLAND_SOCKET (an inherited fd), then
    /// WAYLAND_DISPLAY, then "wayland-0" — what wl_display_connect does.
    const char* name = nullptr;
    /// An already connected socket (wl_display_connect_to_fd); takes ownership. -1: none.
    int fd = -1;
    /// The thread that dispatches (asserted at every entry point in debug builds).
    lx::runtime::affinity affinity = lx::runtime::affinity::ui;
    /// Bind wl_output / xdg_output and keep `output_info` per output.
    bool track_outputs = true;
    /// Request the default dmabuf feedback at connect (linux-dmabuf ≥ 4).
    bool default_dmabuf_feedback = true;
};

/// One output global. Owned by the display.
class output {
public:
    [[nodiscard]] const output_info& info() const { return info_; }
    [[nodiscard]] bool bound() const { return proxy_ != nullptr; }
    [[nodiscard]] void* native() const { return proxy_; }

    // Lifecycle and event entry points (display.cpp).
    void bind(display* d, wl_output* proxy, std::uint32_t global_name, std::uint32_t version);
    void bind_xdg_output(void* manager);
    void release();
    void on_geometry(std::int32_t x, std::int32_t y, std::int32_t pw, std::int32_t ph,
                     std::int32_t subpixel, const char* make, const char* model,
                     std::int32_t transform);
    void on_mode(std::uint32_t flags, std::int32_t w, std::int32_t h, std::int32_t refresh);
    void on_scale(std::int32_t scale);
    void on_name(const char* name);
    void on_description(const char* description);
    void on_done();
    void on_logical_position(std::int32_t x, std::int32_t y);
    void on_logical_size(std::int32_t w, std::int32_t h);

private:
    friend class display;
    display* display_ = nullptr;
    wl_output* proxy_ = nullptr;
    zxdg_output_v1* xdg_ = nullptr;
    std::uint32_t version_ = 0;
    output_info info_{};
    /// `global_handler::on_output_added` was raised (its first done).
    bool announced_ = false;
};

/// The client connection. Non-movable: libwayland's listeners hold its address. UI
/// affinity (or the one `connect_options::affinity` names). Objects made from it (surfaces,
/// buffers, roles, text inputs, feedback) are destroyed before it disconnects — their
/// destroy requests need the connection, as with libwayland itself.
///
/// Allocation happens at connect (libwayland's display, the registry's proxies, the default
/// feedback's snapshot, an xkb context with the first keymap) and when globals come and go;
/// dispatching events allocates nothing in this wrapper (libwayland's own per-message
/// closure is its business, as for every toolkit).
class display {
public:
    static constexpr unsigned k_max_seats = 4;

    display() = default;
    ~display();
    display(const display&) = delete;
    display& operator=(const display&) = delete;

    /// Connects, binds the globals (two roundtrips: the registry, then the state the bound
    /// globals send at once — output modes, seat capabilities, xdg_wm_base's first ping, the
    /// default dma-buf feedback, requested as linux-dmabuf is bound) and fails with `wayland_err::bind_failed` when wl_compositor, wl_shm or xdg_wm_base
    /// is missing. Missing optional globals are logged once at info and left absent.
    [[nodiscard]] lx::result<void> connect(const connect_options& options = {});
    /// Destroys every proxy this display bound and closes the connection.
    void disconnect();
    [[nodiscard]] bool connected() const { return display_ != nullptr; }

    // ── the libwayland read protocol (for a caller's own poll loop) ──
    //
    //   while (!d.prepare_read()) d.dispatch_pending();
    //   if (d.flush() == again) wait for POLLOUT too;
    //   poll(d.fd()) → readable ? d.read_events() : d.cancel_read();
    //   d.dispatch_pending();
    [[nodiscard]] int fd() const;
    /// False when events are already queued: dispatch them first.
    [[nodiscard]] bool prepare_read();
    [[nodiscard]] lx::result<void> read_events();
    void cancel_read();
    /// Runs the queued events' handlers; the count dispatched.
    [[nodiscard]] lx::result<int> dispatch_pending();
    enum class flush_status : std::uint8_t {
        done,
        /// The socket is full (EAGAIN): wait for it to be writable and flush again.
        again,
    };
    [[nodiscard]] lx::result<flush_status> flush();
    /// Flush, wait at most `timeout_ms` (-1: forever) for events, read and dispatch them —
    /// one blocking iteration for simple loops and tests. The count dispatched.
    [[nodiscard]] lx::result<int> dispatch(int timeout_ms);
    /// `dispatch` and `roundtrip` read the socket themselves. Called from inside an
    /// lx::runtime loop the attached `display_source` holds a prepared read across (a UI
    /// strand task, another source's dispatch), they first cancel that read — libwayland's
    /// second reader on one thread would wait for the first forever — and the source
    /// prepares again on the loop's next iteration.
    [[nodiscard]] lx::result<void> roundtrip();

    /// The connection's error (none while healthy). A protocol error is logged once, with
    /// its interface, object and code, when it is first seen.
    [[nodiscard]] const protocol_error_info& protocol_error() const { return error_; }
    [[nodiscard]] bool failed() const { return error_.display_errno != 0; }

    // ── globals ──
    [[nodiscard]] const global_info& global(global_kind k) const {
        return globals_[static_cast<unsigned>(k)];
    }
    /// Bound version, 0 when absent.
    [[nodiscard]] std::uint32_t version(global_kind k) const { return global(k).bound; }
    [[nodiscard]] bool has(global_kind k) const { return version(k) != 0; }
    /// The bound proxy (wl_compositor*, xdg_wm_base*, ...) as `void*`; null when absent.
    [[nodiscard]] void* native_global(global_kind k) const {
        return proxies_[static_cast<unsigned>(k)];
    }
    [[nodiscard]] void* native() const { return display_; }
    [[nodiscard]] void* native_registry() const { return registry_; }
    /// Unknown globals and removals go here (extensions the caller binds itself).
    void set_global_handler(global_handler* h) { global_handler_ = h; }
    /// wl_shm formats seen (bit 0 argb8888, bit 1 xrgb8888) and how many in all.
    [[nodiscard]] std::uint32_t shm_format_mask() const { return shm_formats_; }
    [[nodiscard]] unsigned shm_format_count() const { return shm_format_count_; }
    /// wp_presentation.clock_id (CLOCK_MONOTONIC is 1); ~0u until it arrives or without it.
    [[nodiscard]] std::uint32_t presentation_clock() const { return presentation_clock_; }
    /// The default dmabuf feedback (requested at connect when linux-dmabuf ≥ 4); null
    /// without it.
    [[nodiscard]] dmabuf_feedback* default_feedback() { return feedback_.get(); }
    /// xdg_wm_base pings answered.
    [[nodiscard]] unsigned pings() const { return pings_; }

    // ── outputs and seats (fixed tables; a slot is stable while its global lives) ──
    [[nodiscard]] unsigned output_capacity() const { return k_max_outputs; }
    [[nodiscard]] const output* output_at(unsigned slot) const;
    /// The slot of a `wl_output*`, or ~0u.
    [[nodiscard]] unsigned output_slot(const void* wl_output) const;
    [[nodiscard]] unsigned output_count() const;
    [[nodiscard]] seat* default_seat();
    [[nodiscard]] seat* seat_at(unsigned slot);
    /// Globals that could not be tracked because a fixed table was full.
    [[nodiscard]] unsigned dropped_globals() const { return dropped_globals_; }

    /// Asserts the dispatching affinity (debug builds).
    void check_affinity() const;

    // Object lifecycle (surface, text_input and display_source call these).
    /// Every surface and text input made from this display, intrusively listed (no
    /// allocation): an output going clears its bit from every surface, a surface being
    /// destroyed is dropped from every seat's and text input's focus.
    void link_surface(surface* s);
    void unlink_surface(surface* s);
    void link_text_input(text_input* t);
    void unlink_text_input(text_input* t);
    void on_output_done(output& o);

    // Registry entry points (listener thunks).
    void on_global(std::uint32_t name, const char* interface, std::uint32_t version);
    void on_global_remove(std::uint32_t name);
    void on_shm_format(std::uint32_t format);
    void on_ping(std::uint32_t serial);
    void on_presentation_clock(std::uint32_t clock);

private:
    friend class display_source;
    lx::result<void> fail(lx::wayland_err code, const char* what);
    void note_error();
    /// Cancels the attached display_source's prepared read (see `roundtrip`).
    void release_source_read();
    [[nodiscard]] unsigned slot_of(const output& o) const;

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    global_info globals_[k_global_kinds]{};
    void* proxies_[k_global_kinds]{};
    output outputs_[k_max_outputs]{};
    seat seats_[k_max_seats]{};
    std::unique_ptr<dmabuf_feedback> feedback_{};
    global_handler* global_handler_ = nullptr;
    display_source* source_ = nullptr;
    surface* surfaces_ = nullptr;
    text_input* text_inputs_ = nullptr;
    protocol_error_info error_{};
    lx::runtime::affinity affinity_ = lx::runtime::affinity::ui;
    std::uint32_t shm_formats_ = 0;
    unsigned shm_format_count_ = 0;
    std::uint32_t presentation_clock_ = ~0u;
    unsigned pings_ = 0;
    unsigned dropped_globals_ = 0;
    bool track_outputs_ = true;
    bool want_feedback_ = true;
    bool error_logged_ = false;
};

/// The display as an lx::runtime::event_loop source, the way GTK's GSource and calloop's
/// WaylandSource do it: prepare = prepare_read (or dispatch what is already queued) +
/// flush (EAGAIN → also wait for POLLOUT, never drop or spin); readable → read_events, else
/// cancel_read; dispatch → dispatch_pending. Nothing runs on a timer: an idle connection
/// costs no wakeups. `on_error` runs once when the connection dies.
class display_source final : public lx::runtime::event_source {
public:
    display_source(display& d, lx::runtime::event_loop& loop,
                   lx::runtime::callback on_error = {});
    ~display_source() override;
    display_source(const display_source&) = delete;
    display_source& operator=(const display_source&) = delete;

    /// Adds itself to the loop / removes itself (cancelling a prepared read).
    void attach();
    void detach();

    bool prepare(lx::runtime::clock_time& timeout) override;
    bool check() override;
    bool dispatch() override;
    [[nodiscard]] int poll_fd() const override;
    void on_readable() override { readable_ = true; }
    void on_writable() override { writable_ = true; }

    /// Times events were read from the socket (diagnostics, G1).
    [[nodiscard]] unsigned long long reads() const { return reads_; }

private:
    friend class display;
    void fail();

    display& display_;
    lx::runtime::event_loop& loop_;
    lx::runtime::callback on_error_{};
    unsigned long long reads_ = 0;
    bool attached_ = false;
    bool prepared_ = false;
    bool queued_ = false;
    bool readable_ = false;
    bool writable_ = false;
    bool want_write_ = false;
    bool dead_ = false;
};

} // namespace lx::wayland
