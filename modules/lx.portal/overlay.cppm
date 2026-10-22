module;

// xdg-desktop-portal-lumen's interactive surfaces (design-app-compat-capture E.2), drawn
// with lumen-shell's shm canvas (lx.shell `shm_canvas`: two buffers, frame pacing,
// fractional scale) on wlr-layer-shell overlay surfaces with exclusive keyboard focus:
//
//   - select: one surface per output showing the frozen capture dimmed. Drag to select an
//     area (it shows undimmed, with a border), click to take that output, Enter for every
//     output, Escape or a right click to cancel.
//   - pick: the same frozen backdrop; a swatch with the colour under the pointer follows it,
//     a click picks that colour (read from the capture at the output's own resolution).
//   - choose: a list (the ScreenCast source chooser, the Screenshot window target):
//     Up/Down/Home/End or the pointer to choose, Enter or a click to accept, Escape to
//     decline.
//
// Only what changed is repainted: a buffer remembers the dynamic rect (selection, swatch)
// it last drew, and the next paint into it rewrites that rect and the new one, nothing
// else. The backdrop is composed once per output at the surface's pixel size (the
// compositor's preferred fractional scale), so it shows pixel for pixel.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

export module lx.portal:overlay;

import lx.foundation;
import :compose;
import :chooser;
import :desktop;

export namespace lx::portal {

struct overlay_result {
    enum class kind : unsigned char { pending, cancelled, area, output, all, colour, choice, failed };
    kind what = kind::pending;
    lx::rect2i area{};       // area: layout rect
    int output = -1;         // output: index into the shots
    std::uint32_t colour = 0; // colour: 0xFFRRGGBB
    int choice = -1;         // choice: index into the entries
    const char* reason = ""; // failed: why
};

class overlay {
public:
    explicit overlay(desktop& d);
    ~overlay();
    overlay(const overlay&) = delete;
    overlay& operator=(const overlay&) = delete;

    /// Area / output / everything over the frozen `shots` (kept alive by the caller).
    [[nodiscard]] lx::result<void> start_select(std::span<const output_shot> shots);
    /// A colour from the frozen `shots`.
    [[nodiscard]] lx::result<void> start_pick(std::span<const output_shot> shots);
    /// One of `entries` under `heading`.
    [[nodiscard]] lx::result<void> start_choose(std::vector<chooser_entry> entries, std::string heading);

    /// After events were dispatched: repaint what changed (where a buffer is free).
    void update();
    [[nodiscard]] bool done() const { return result_.what != overlay_result::kind::pending; }
    [[nodiscard]] const overlay_result& result() const { return result_; }

    struct events;
    struct view;

private:
    friend struct events;
    enum class mode : unsigned char { none, select, pick, choose };

    [[nodiscard]] lx::result<void> setup_input();
    [[nodiscard]] lx::result<void> add_view(int output_index);
    void finish(overlay_result r);
    void paint_backdrop_view(view& v);
    void paint_chooser_view(view& v);
    [[nodiscard]] lx::rect2i dynamic_rect(const view& v) const;
    [[nodiscard]] bool swatch_box(const view& v, lx::rect2i& box) const;
    void pointer_moved();
    void button(unsigned code, bool pressed);
    void key(unsigned code);
    void mark_all_dirty();

    desktop& desktop_;
    mode mode_ = mode::none;
    std::span<const output_shot> shots_{};
    std::vector<std::unique_ptr<view>> views_;
    overlay_result result_{};

    void* pointer_ = nullptr;       // wl_pointer*
    void* keyboard_ = nullptr;      // wl_keyboard*
    void* cursor_device_ = nullptr; // wp_cursor_shape_device_v1*
    view* pointer_view_ = nullptr;
    double gx_ = 0, gy_ = 0; // pointer, layout coordinates
    bool pointer_known_ = false;
    bool pressed_ = false;
    bool dragging_ = false;
    double press_x_ = 0, press_y_ = 0;
    // Selection (layout, logical): empty when none.
    double sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = 0, sel_y1_ = 0;
    bool has_sel_ = false;

    // Chooser.
    std::vector<chooser_entry> entries_;
    std::string heading_;
    int selected_ = 0;
    int top_ = 0;
};

} // namespace lx::portal
