module;

// The notification popups (design-session-and-wm N.3): one wlr-layer-shell surface per
// output in use holding the whole stack — the newest card at the anchor edge, at most
// `max-visible`, a "+n more" line under them — so a new notification is one buffer and one
// commit, never a surface per card (mako's model, not GNOME's banner-per-surface).
//
// The surface sits on the `top` layer (below fullscreen windows, as GNOME and KDE suppress
// banners over fullscreen) and moves to `overlay` while a critical notification is shown;
// exclusive zone 0, so it keeps clear of the panel; no keyboard. Its input region is the
// cards (clicks between them reach what is beneath). Card: 360 logical px wide, a 48 px
// icon, the summary in bold on one line, the body wrapped to four lines (markup: bold,
// italic, underline, links), up to three action buttons (more go into a menu), a close
// button. The pointer over the stack pauses every timer (GNOME, KDE).
//
// The layout is pure (`layout_notifications`, tested); drawing repaints the whole buffer
// (a few cards) and damages only the cards that changed. Event driven: nothing runs while
// nothing changes. Shell thread only.

import lx.foundation;
import lx.text;

export module lx.shell:notify_view;

import std;

import :notifications;
import :shm_canvas;

export namespace lx::shell {

inline constexpr int k_card_width = 360;
inline constexpr int k_card_pad = 12;
inline constexpr int k_card_icon = 48;
inline constexpr int k_card_gap = 8;
inline constexpr int k_card_summary_h = 20;
inline constexpr int k_card_button_h = 28;
inline constexpr int k_card_close = 20;
inline constexpr int k_card_more_h = 24;
inline constexpr int k_card_radius = 10;
inline constexpr unsigned k_card_body_lines = 4;
inline constexpr unsigned k_card_buttons = 3;
inline constexpr unsigned k_max_cards = 10;
/// Kept from the output's edge (and from the panel: exclusive zone 0).
inline constexpr int k_notify_margin = 8;

enum class notify_position : unsigned char { top_right, top_center, top_left, bottom_right, bottom_left };

/// Where one card's parts are (surface coordinates, logical).
struct card_layout {
    unsigned id = 0;
    unsigned rev = 0;
    lx::rect2i box{};
    bool has_icon = false;
    lx::rect2i icon{};
    lx::rect2i summary{};
    lx::rect2i body{};
    lx::rect2i close{};
    unsigned buttons = 0;
    lx::rect2i button[k_card_buttons]{};
    /// Index into the notification's actions; -1: "More…" (the rest in a menu).
    int button_action[k_card_buttons]{};
    bool critical = false;
};

struct notify_layout {
    int width = 0;
    int height = 0;
    unsigned count = 0;
    card_layout cards[k_max_cards]{};
    /// Popups waiting beyond the shown ones; their line's rect.
    unsigned more = 0;
    lx::rect2i more_box{};
};

/// The body's height at `width` (logical px; 0 when there is no body).
using notify_body_fn = int (*)(const notification& n, int width, void* user);
/// Lays out `shown` (newest first) for `position`: the newest card at the anchor edge.
/// `has_icon` decides each card's icon column.
void layout_notifications(const notification* const* shown, unsigned n, unsigned waiting,
                          notify_position position, notify_body_fn body_height, void* user,
                          bool (*has_icon)(const notification& n, void* user),
                          notify_layout& out);
/// The actions shown as buttons (not "default"); `out` gets action indices.
unsigned notify_button_actions(const notification& n, int* out, unsigned cap);

struct notify_globals {
    void* compositor = nullptr;  // wl_compositor
    void* shm = nullptr;         // wl_shm
    void* layer_shell = nullptr; // zwlr_layer_shell_v1 (v2+: set_layer)
    void* viewporter = nullptr;  // wp_viewporter
    void* fractional = nullptr;  // wp_fractional_scale_manager_v1
};

/// A notification's icon at `px`² pixels (premultiplied ARGB; null: none), except
/// image-data, which the view draws itself: image-path, app_icon, the desktop entry's.
using notify_icon_fn = const std::uint32_t* (*)(const notification& n, int px, void* user);

/// What a click on the stack asks for.
struct notify_click {
    enum class kind : unsigned char { none, dismiss, invoke, link, more } what = kind::none;
    unsigned id = 0;
    /// invoke: the action key; link: the URL.
    std::string arg{};
    /// more: the button (surface coordinates), to anchor the menu.
    lx::rect2i anchor{};
};

class notification_view {
public:
    notification_view() = default;
    ~notification_view();
    notification_view(const notification_view&) = delete;
    notification_view& operator=(const notification_view&) = delete;

    void init(notify_globals g, notify_icon_fn icons, void* user);
    void set_position(notify_position p);
    /// Shows `c`'s popups on `output` (wl_output*; another output moves the stack there), or
    /// closes the surface when there are none. Cheap when nothing changed (its rev).
    void update(const notification_center& c, void* output);
    void close();
    [[nodiscard]] bool is_open() const { return surface_ != nullptr; }
    [[nodiscard]] void* surface() const { return surface_; }
    /// zwlr_layer_surface_v1* (a menu's parent).
    [[nodiscard]] void* layer() const { return layer_; }
    [[nodiscard]] const notify_layout& layout() const { return layout_; }
    /// Draws when something changed and the compositor took the last frame.
    void after_dispatch();
    /// Frames committed since it opened.
    [[nodiscard]] unsigned frames() const { return frames_; }

    // ── Pointer, routed here by the shell ──
    void pointer_enter(double x, double y);
    void pointer_leave();
    void pointer_motion(double x, double y);
    [[nodiscard]] notify_click pointer_button(unsigned button, bool pressed);
    /// The pointer is over a card (the stack's timers pause).
    [[nodiscard]] bool hovered() const { return hover_card_ >= 0; }

    struct events;

private:
    friend struct events;
    struct card_cache {
        unsigned id = 0;
        unsigned rev = 0;
        int width = 0;
        double scale = 0.0;
        lx::text::wrapped_text body{};
        int body_h = 0;
        /// image-data resampled to the drawn size.
        int image_px = 0;
        std::vector<std::uint32_t> image{};
    };
    /// What is under the pointer: a card, and its part.
    struct hit {
        int card = -1;
        /// 0 body, 1 close, 2+i button i, 10+p link piece p.
        int part = -1;
    };
    [[nodiscard]] hit hit_at(double x, double y) const;
    card_cache* cache_for(const notification& n);
    void relayout();
    void draw();
    void paint_card(int bi, const card_layout& c, const notification& n);
    void apply_size();

    notify_globals g_{};
    notify_icon_fn icons_ = nullptr;
    void* user_ = nullptr;
    notify_position position_ = notify_position::top_right;
    const notification_center* center_ = nullptr;
    shm_canvas canvas_{};
    notify_layout layout_{};
    std::vector<std::unique_ptr<card_cache>> cache_;
    void* output_ = nullptr;
    void* surface_ = nullptr; // wl_surface*
    void* layer_ = nullptr;   // zwlr_layer_surface_v1*
    unsigned center_rev_ = 0;
    bool overlay_ = false;
    bool configured_ = false;
    bool dirty_ = false;
    /// The size asked for (set_size) and the size configured.
    int want_w_ = 0;
    int want_h_ = 0;
    int conf_w_ = 0;
    int conf_h_ = 0;
    double pointer_x_ = -1.0;
    double pointer_y_ = -1.0;
    int hover_card_ = -1;
    int hover_part_ = -1;
    hit pressed_{};
    unsigned frames_ = 0;
};

} // namespace lx::shell
