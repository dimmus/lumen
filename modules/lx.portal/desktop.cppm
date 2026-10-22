module;

// xdg-desktop-portal-lumen's Wayland connection (design-app-compat-capture E.2): the outputs
// with their layout rects (xdg-output), the toplevels (ext-foreign-toplevel-list-v1), and
// still captures over ext-image-capture-source-v1 + ext-image-copy-capture-v1 into shm.
//
// One connection per portal request, opened when the request arrives and closed with it: an
// idle portal holds no Wayland connection and wakes for nothing, and every request sees the
// outputs as they are now (hotplug needs no tracking). Captures are shm frames — a
// screenshot is read by the CPU (PNG encoding) anyway, and the compositor's shm path is one
// GPU blit plus a worker-thread copy (Part 4) — turned upright and handed over as
// `output_shot`s. A frame the compositor fails with `buffer_constraints` (the mode changed
// under it) is retried with the new constraints; `stopped` (the output went, or the session
// was locked) fails the capture: the portal never returns a lock screen as a screenshot.
//
// No Wayland type crosses this interface; the overlay reaches the raw objects through the
// `void*` accessors.

export module lx.portal:desktop;

import std;

import lx.foundation;
import :compose;

export namespace lx::portal {

struct desktop_output {
    std::string name;
    std::string description;
    lx::rect2i logical{};    // layout rect (xdg-output, else wl_output geometry and mode)
    lx::size2i mode{};       // current mode, buffer pixels
    int transform = 0;       // wl_output.transform
    int scale = 1;           // wl_output.scale (integer)
    unsigned global = 0;
    void* wl_output = nullptr;
    void* xdg_output = nullptr;
    bool gone = false;
};

struct desktop_toplevel {
    std::string identifier;
    std::string title;
    std::string app_id;
    void* handle = nullptr; // ext_foreign_toplevel_handle_v1*
    bool closed = false;
};

class desktop {
public:
    desktop() = default;
    ~desktop();
    desktop(const desktop&) = delete;
    desktop& operator=(const desktop&) = delete;

    /// Connects to `display` (null: $WAYLAND_DISPLAY) and reads the globals, outputs and
    /// toplevels (two round trips).
    [[nodiscard]] static lx::result<std::unique_ptr<desktop>> connect(const char* display = nullptr);

    [[nodiscard]] int fd() const;
    /// Dispatches what is queued and flushes requests; false when the connection broke.
    [[nodiscard]] bool prepare();
    /// The fd was readable: read and dispatch. False when the connection broke.
    [[nodiscard]] bool dispatch_readable();
    /// Waits up to `timeout_ms` for events and dispatches them.
    [[nodiscard]] bool pump(int timeout_ms);
    [[nodiscard]] bool broken() const { return broken_; }

    [[nodiscard]] const std::vector<std::unique_ptr<desktop_output>>& outputs() const { return outputs_; }
    [[nodiscard]] const std::vector<std::unique_ptr<desktop_toplevel>>& toplevels() const {
        return toplevels_;
    }
    [[nodiscard]] bool can_capture_outputs() const;
    [[nodiscard]] bool can_capture_toplevels() const;

    /// Every output, upright, at its own resolution. `paint_cursor` asks for the pointer.
    [[nodiscard]] lx::result<std::vector<output_shot>> capture_outputs(bool paint_cursor,
                                                                       int timeout_ms);
    /// One window (by ext-foreign-toplevel-list identifier), upright.
    [[nodiscard]] lx::result<image> capture_toplevel(std::string_view identifier, bool paint_cursor,
                                                     int timeout_ms);

    // Raw globals for the overlay (wl_display*, wl_compositor*, …; null when absent).
    [[nodiscard]] void* display() const { return display_; }
    [[nodiscard]] void* compositor() const { return compositor_; }
    [[nodiscard]] void* shm() const { return shm_; }
    [[nodiscard]] void* seat() const { return seat_; }
    /// wl_seat.capabilities (pointer 1, keyboard 2).
    [[nodiscard]] unsigned seat_caps() const { return seat_caps_; }
    [[nodiscard]] void* layer_shell() const { return layer_shell_; }
    [[nodiscard]] void* cursor_shape() const { return cursor_shape_; }
    [[nodiscard]] void* viewporter() const { return viewporter_; }
    [[nodiscard]] void* fractional() const { return fractional_; }

    struct events;

private:
    friend struct events;
    struct capture_job;
    [[nodiscard]] lx::result<void> run_captures(std::vector<capture_job*>& jobs, int timeout_ms);

    void* display_ = nullptr;
    void* registry_ = nullptr;
    void* compositor_ = nullptr;
    void* shm_ = nullptr;
    void* seat_ = nullptr;
    unsigned seat_caps_ = 0;
    void* layer_shell_ = nullptr;
    void* cursor_shape_ = nullptr;
    void* viewporter_ = nullptr;
    void* fractional_ = nullptr;
    void* xdg_output_manager_ = nullptr;
    void* toplevel_list_ = nullptr;
    void* output_sources_ = nullptr;
    void* toplevel_sources_ = nullptr;
    void* copy_manager_ = nullptr;
    std::vector<std::unique_ptr<desktop_output>> outputs_;
    std::vector<std::unique_ptr<desktop_toplevel>> toplevels_;
    bool broken_ = false;
};

} // namespace lx::portal
