module;

// xdg-desktop-portal-lumen's interactive surfaces (design-app-compat-capture E.2), drawn
// as lx.ui trees on lx.ui.host surfaces (ADR-010 Phase 5 P5-9) on the overlay's own
// lx.wayland.client connection — the capture machinery stays on `desktop`'s raw one —
// with wlr-layer-shell overlay surfaces and exclusive keyboard focus:
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
// The backdrop is the capture as an atlas tile drawn 1:1, a dim quad over it; the selection
// shows it undimmed through a clipped second child, the pick's swatch and the hint pill are
// literal boxes. A change re-describes; the engine's damage ledger repaints only what
// moved. The surfaces sit on the automatic client tier like every view's.

export module lx.portal:overlay;

import std;

import lx.foundation;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.text;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
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

    /// A row's stable identity for the tree's callbacks.
    struct row_ref {
        overlay* self = nullptr;
        int entry = -1;
    };
    /// One surface: a backdrop view (`output` into the shots) or the chooser (-1).
    struct view {
        overlay* owner = nullptr;
        int output = -1;
        lx::ui::host::surface surface{};
        lx::ui::host::layer_role role{};
        bool dirty = true;
        /// The chooser's rows' callback identities (the retained nodes hold them).
        row_ref rows[10]{};
        /// The backdrop, composed once at the surface's pixel size, its atlas tile and
        /// the tile's RGBA bytes (re-encoded only when the size changes).
        image composed{};
        std::vector<std::uint8_t> rgba{};
        int tile_w = 0;
        int tile_h = 0;
        lx::gfx::atlas_tile_id tile{};
    };

    /// After events were dispatched: re-describes what changed and runs the views' frame
    /// loops.
    void update();
    /// The overlay's own event loop step: flushes and dispatches the ui connection (and,
    /// with `timeout_ms` ≥ 0, waits on it and `desktop`'s fd together — the chooser's
    /// drive). False when a connection broke.
    [[nodiscard]] bool pump(int timeout_ms);
    /// The ui connection's fd for a caller's poll set (-1: none); `pump_ui` flushes and
    /// dispatches it (false: broken).
    [[nodiscard]] int ui_fd() const;
    [[nodiscard]] bool pump_ui();
    [[nodiscard]] bool done() const { return result_.what != overlay_result::kind::pending; }
    [[nodiscard]] const overlay_result& result() const { return result_; }

private:
    enum class mode : unsigned char { none, select, pick, choose };
    friend class overlay_root_node;
    friend lx::ui::child overlay_describe(lx::ui::build_context& ctx);

    [[nodiscard]] lx::result<void> start_ui();
    [[nodiscard]] unsigned slot_of(const std::string& output_name) const;
    [[nodiscard]] lx::result<void> add_view(int output_index);
    void finish(overlay_result r);
    [[nodiscard]] lx::ui::child build_ui(view& v);
    [[nodiscard]] lx::gfx::atlas_tile_id backdrop_tile(view& v);
    [[nodiscard]] lx::rect2i hint_box(const view& v) const;
    [[nodiscard]] bool swatch_box(const view& v, lx::rect2i& box) const;
    [[nodiscard]] const char* hint_text() const;
    void pointer_moved();
    void button(unsigned code, bool pressed);
    void key(unsigned code);
    void mark_all_dirty();
    void on_root_key(view& v, const lx::ui::key_event& e);
    void on_root_pointer(view& v, const lx::ui::pointer_event& e);
    void on_root_hover(view& v, bool hovered);
    void on_row_hover(const row_ref& ref, bool hovered);
    void on_row_event(const row_ref& ref, const lx::ui::pointer_event& e);
    void on_key_focus(view& v);

    desktop& desktop_;
    mode mode_ = mode::none;
    std::span<const output_shot> shots_{};
    std::vector<std::unique_ptr<view>> views_;
    overlay_result result_{};

    /// The overlay's own connection and UI context (started at the first start_*).
    lx::wayland::display client_{};
    lx::runtime::event_loop ui_loop_{};
    lx::ui::host::context ui_{};
    bool ui_started_ = false;

    // The pointer, in layout coordinates (the shot's logical rect offsets the surface's).
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
