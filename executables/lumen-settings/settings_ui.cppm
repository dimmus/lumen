// lumen-settings' window (ADR-013 Phase B): the schema-driven settings surface. Every
// configurable key renders from `lx.config`'s own descriptors (`schema_keys()` /
// `pointer_keys()`) — the value kind picks the control, the range constrains it, the reload
// class writes its badge — so a key added to the schema appears here with no pane code.
//
// Layout (ADR-013 §5.2): a search row over a left rail (the panes) and the content column
// (grouped setting rows; ghost hovers), a status bar under it. Values edit in place: a
// control's change mutates the session's snapshot and asks for one frame.
//
// Values edit the session's snapshot and the user's config.toml: every edit splices the
// user layer (lx.config:edit), re-parses to verify and renames atomically (ADR-013
// Phase C); the Varlink reads (version, health, devices) feed the Session pane.
module;

#include <fcntl.h>
#include <unistd.h>

export module lumen.settings_ui;

import std;
import lx.foundation;
import lx.a11y;
import lx.config;
import lx.layout;
import lx.text;
import lx.trace;
import lx.ui;

export import :displays;
export import :ipc;

export namespace settings {

// ── Panes (the rail; ADR-013 §5.5's order) ────────────────────────────────────────────

enum class pane_id : unsigned char {
    appearance,   ///< cursor, background, panel
    displays,     ///< per-output render scale (the wlr manager arrives in Phase F)
    input,        ///< keyboard, touchpad, mouse, trackpoint
    shortcuts,    ///< the bindings editor (Phase E)
    windows,      ///< focus, snapping, tiling, workspaces, switcher, animations
    notifications,
    idle,         ///< idle & lock
    session,      ///< general (version and health arrive with the Varlink reads)
};

inline constexpr unsigned k_pane_count = 8;

inline constexpr std::string_view k_pane_names[k_pane_count] = {
    "Appearance", "Displays", "Input", "Shortcuts",
    "Windows & Workspaces", "Notifications", "Idle & Lock", "Session",
};

inline constexpr lx::ui::icon k_pane_icons[k_pane_count] = {
    lx::ui::icon::sun,      lx::ui::icon::monitor, lx::ui::icon::keyboard,
    lx::ui::icon::settings, lx::ui::icon::folder,  lx::ui::icon::bell,
    lx::ui::icon::clock,    lx::ui::icon::power,
};

// ── The key catalog ───────────────────────────────────────────────────────────────────

/// One renderable setting: a `key_desc` (non-pointer sections) or a `pointer_key_desc` in
/// one of the three pointer sections, with its pane, group header, human label and the
/// lower-case text the search matches ("section key label").
struct row_ref {
    const lx::config::key_desc* desc = nullptr;
    const lx::config::pointer_key_desc* pdesc = nullptr;
    /// Pointer sections only: 0 touchpad, 1 mouse, 2 trackpoint.
    unsigned pointer_section = 0;
    const char* label = nullptr;
    const char* description = nullptr;
    const char* group = nullptr;
    pane_id owner = pane_id::appearance;
    /// The unit readout suffix ("ms", "s", "px"), "" when none.
    const char* unit = nullptr;
    char search[72]{};
};

/// Every schema key, in file order, assigned to its pane and group. Built once on first
/// use (start-up, UI thread); a key added to the schema appears here with no change.
[[nodiscard]] const std::vector<row_ref>& catalog();

/// The string-key rows' slots: row catalog index → `session::string_buffer` slot.
[[nodiscard]] unsigned string_slot(unsigned row);

// ── The session ───────────────────────────────────────────────────────────────────────

/// A string key's editable buffer, viewing storage the session owns.
struct string_field {
    char storage[560]{};
    lx::ui::text_buffer buffer{};
    std::uint32_t revision = 1;

    void bind() {
        buffer.storage = {storage, sizeof(storage)};
        buffer.size = static_cast<std::uint32_t>(std::strlen(storage));
        buffer.revision = ++revision;
    }
    void set(std::string_view text) {
        const std::size_t n = std::min(text.size(), sizeof(storage) - 1);
        std::memcpy(storage, text.data(), n);
        storage[n] = '\0';
        bind();
    }
};

/// This process's settings state: the snapshot the rows render and edit, the selected
/// pane, the search query, one text buffer per string key. One per process (UI affinity).
class session {
public:
    static constexpr unsigned k_no_row = ~0u;
    /// The schema's string keys each get their own edit buffer (9 today). Headroom for the
    /// editors still open (per-device overrides, window rules); a schema past the cap makes
    /// those rows read-only — never two rows sharing one buffer.
    static constexpr unsigned k_max_string_rows = 16;

    /// `load_files`: read the layered configuration (main does this; tests keep the
    /// defaults). Seeds `config_` and the string buffers from what Lumen runs with.
    explicit session(bool load_files = false) {
        for (string_field& f : strings_)
            f.bind();
        if (load_files)
            load_from_files();
        load_strings();
    }

    /// The layered configuration the compositor itself reads (locate → read → parse).
    void load_from_files();

    [[nodiscard]] const lx::config::config_snapshot& config() const { return config_; }
    [[nodiscard]] lx::config::config_snapshot& config() { return config_; }

    [[nodiscard]] pane_id current() const { return current_; }
    void set_current(pane_id p) {
        current_ = p;
        open_menu_ = k_no_row;
        repaint();
    }

    [[nodiscard]] lx::ui::text_buffer& query_buffer() { return query_.buffer; }

    /// The slot's buffer (a string key's field edits through it).
    [[nodiscard]] lx::ui::text_buffer& string_buffer(unsigned slot) {
        return strings_[slot < k_max_string_rows ? slot : 0].buffer;
    }

    /// A boolean key's value flips.
    void set_bool(unsigned row, bool v);
    /// An integer key takes `v` (already clamped to the descriptor's range).
    void set_int(unsigned row, int v);
    /// A number key takes `v`.
    void set_number(unsigned row, double v);
    /// An enumeration key takes the word index.
    void set_enum(unsigned row, unsigned v);
    /// A string key's buffer applies to its field (an input's change or submit).
    void commit_string(unsigned row);
    /// Applies the [ui] section to this app live (the theme and the density; the font size
    /// applies to applications at their start).
    void apply_ui_live();
    /// Compiles a Zed theme-family JSON into the user theme directory, loads it live and
    /// writes [ui].theme. `status` gets a short human result either way.
    void import_theme(std::string_view json_path);
    /// The user theme directory (created on demand): $XDG_CONFIG_HOME/lumen/themes.
    [[nodiscard]] static std::string user_theme_dir();
    /// The installed themes: the two built-ins plus the user directory's .lxtheme files.
    [[nodiscard]] static const std::vector<std::string>& installed_themes();
    /// The theme-import row's buffer and its last status (the pane shows both).
    [[nodiscard]] lx::ui::text_buffer& import_buffer() { return import_.buffer; }
    [[nodiscard]] std::string_view import_status() const { return import_status_; }

    // ── the Session pane's Varlink reads (ADR-013 Phase G; the ipc partition delivers) ──
    void set_health(const health_snapshot& h) {
        health_ = h;
        if (root && root->tree())
            invalidate_layout(*root->tree());
    }
    [[nodiscard]] const health_snapshot& health() const { return health_; }

    // ── the bindings editor (ADR-013 Phase E) ──
    /// The action being edited (an index into `actions()`), `k_no_row` when none.
    [[nodiscard]] unsigned editing_action() const { return editing_action_; }
    void begin_edit_action(unsigned index) {
        editing_action_ = index;
        error_[0] = '\0';
        const std::vector<row_ref>& rows = catalog(); // (no-op: keeps the catalog warm)
        (void)rows;
        const lx::config::combo current = binding_of_action(index);
        if (current.kind != lx::config::combo_kind::tap || current.mods || current.code) {
            char canon[64];
            lx::config::format_combo(current, canon, sizeof(canon));
            edit_.set(canon);
        } else {
            edit_.set({});
        }
        repaint();
    }
    void cancel_edit_action() {
        editing_action_ = k_no_row;
        repaint();
    }
    [[nodiscard]] lx::ui::text_buffer& edit_buffer() { return edit_.buffer; }
    /// The current binding of the action `index` (its first; a fresh combo when unbound).
    [[nodiscard]] lx::config::combo binding_of_action(unsigned index) const;
    /// The canonical spelling of the combo bound to this action in the user layer, "" when
    /// it comes from a default or a lower layer.
    [[nodiscard]] std::string_view binding_is_user(const lx::config::combo& c) const;
    /// Applies the edit: validates, checks conflicts and reserved keys, splices
    /// `[bindings]`. `error_` says why not; empty means applied.
    void apply_edit_action();
    /// The edit's validation error ("" while clean); live as the user types.
    [[nodiscard]] std::string_view edit_error() const { return error_; }
    /// Removes the user layer's line for this binding (the default returns).
    void reset_binding(unsigned index);
    [[nodiscard]] std::string_view reset_note() const { return reset_note_; }

private:
    /// The user layer's bytes ("" when missing; false-returning when unreadable).
    [[nodiscard]] bool user_layer_text(std::string& out) const;
    /// Verifies the edited layer parses cleanly, then writes it atomically (the write_key
    /// tail); `config_` re-reads the layers afterwards.
    bool commit_layer(const std::string& edited);

public:
    /// The row whose select's dropdown is open, or `k_no_row`.
    void set_open_menu(unsigned row) {
        open_menu_ = row;
        repaint();
    }
    [[nodiscard]] unsigned open_menu() const { return open_menu_; }

    /// One repaint after an edit that did not reach the file (the status bar's note).
    void repaint();
    /// A re-describe after an edit that did (the file is the truth now).
    void written() {
        save_ = save_state::written;
        refresh();
    }
    /// A re-describe without touching the save state (the search field's typing).
    void refresh() {
        if (root && root->tree())
            invalidate_layout(*root->tree());
    }

    enum class save_state : unsigned char { untouched, written, memory_only };
    [[nodiscard]] save_state state() const { return save_; }

    /// The window's root, set once by main; handlers repaint through it.
    lx::ui::ui_root* root = nullptr;
    /// Tests point the write path at their own file (load_from_files names the real one).
    void force_user_file(const char* path) {
        std::snprintf(user_file_, sizeof(user_file_), "%s", path);
        user_file_disabled_ = !path || !path[0];
        forced_user_file_ = !user_file_disabled_;
    }

private:
    void load_strings();
    /// Re-reads the configuration the rows show after a write: the located layers, or
    /// just the forced user file (tests). `config_` stands when they do not parse — the
    /// layer just written verified, so an older one is the problem (the compositor
    /// reports it on its reload).
    void reload_effective();
    /// The file dance of one edit (ADR-013 §4.3): splice the user layer, re-parse to
    /// verify, atomic rename. False (the in-memory edit already happened) when there is
    /// no writable user layer or the verification refused.
    bool write_key(unsigned row, const lx::config::edit_value& v);
    [[nodiscard]] void* field_of(unsigned row);
    [[nodiscard]] string_field& slot_buffer(unsigned row) {
        return strings_[row < k_max_string_rows ? row : 0];
    }

    lx::config::config_snapshot config_{};
    pane_id current_ = pane_id::appearance;
    unsigned open_menu_ = k_no_row;
    save_state save_ = save_state::untouched;
    bool user_file_disabled_ = true;
    /// The user file came from `force_user_file` (a test's temp file), not the located
    /// layers — the post-write reload reads it alone.
    bool forced_user_file_ = false;
    char user_file_[512]{};
    string_field query_{};
    string_field import_{};
    std::string import_status_{"Paste the path to a Zed theme-family JSON, then Import."};
    /// The last Varlink poll (the Session pane; ipc.cppm delivers on the UI strand).
    health_snapshot health_{};
    /// The bindings editor's state.
    unsigned editing_action_ = k_no_row;
    string_field edit_{};
    char error_[128]{};
    std::string reset_note_{};
    /// The last [ui] theme string applied live (skips re-work on unrelated edits).
    std::string live_theme_{"\x01"}; // not a valid name: the first apply runs
    lx::config::ui_appearance live_appearance_ = lx::config::ui_appearance::dark;
    lx::config::ui_density_kind live_density_ = lx::config::ui_density_kind::regular;
    /// The user theme directory's file list (refreshed by import_theme).
    static std::vector<std::string> g_installed;
    string_field strings_[k_max_string_rows]{};
    /// Catalog row index each `strings_` slot edits, `k_no_row` for the unused ones.
    unsigned string_rows_[k_max_string_rows]{};
};

/// The session `describe` renders (UI affinity; main sets it before the run).
void set_active_session(session* s);
[[nodiscard]] session* active_session();

/// The window's UI: search row, rail, content, status bar.
[[nodiscard]] lx::ui::child describe(lx::ui::build_context& ctx);

/// A subsequence match of `query` (lower case) against a row's search text.
[[nodiscard]] bool row_matches(const row_ref& r, std::string_view query);

// ── Element keys (tests find nodes by them) ───────────────────────────────────────────
inline constexpr lx::ui::element_key k_search_key = 0x73'0001;
inline constexpr lx::ui::element_key k_rail_key = 0x73'0002;
inline constexpr lx::ui::element_key k_content_key = 0x73'0003;
inline constexpr lx::ui::element_key k_status_key = 0x73'0004;
/// Row `i`'s value control: `k_row_control_base + i` (the catalog index).
inline constexpr lx::ui::element_key k_row_control_base = 0x73'1000;

} // namespace settings

// ── implementation ──────────────────────────────────────────────────────────────────
// (The engine's convention: no private fragment — plain definitions, file-local helpers
// in anonymous namespaces, declared-exported names defined in `namespace settings`.)

namespace {

using namespace settings;
using namespace lx::ui;
namespace cfg = lx::config;

settings::session* g_session = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)

constexpr float k_rail_w = 220.f;   ///< px at the rem
constexpr float k_value_w = 220.f;  ///< the value column, px at the rem
constexpr float k_row_h = 32.f;     ///< a setting row without a badge
constexpr float k_badge_row_h = 40.f;

// ── The components' file-local vocabulary, mirrored ───────────────────────────────────

[[nodiscard]] float st_rem() { return lx::ui::ui_rem_px(); }
[[nodiscard]] lx::layout::length st_px16(float px) { return lx::layout::rem(px / 16.f); }
[[nodiscard]] lx::ui::style st_h_flex() { return lx::ui::style{}.flex_row().items_center(); }
[[nodiscard]] lx::ui::style st_v_flex() { return lx::ui::style{}.flex_col(); }

[[nodiscard]] lx::ui::control_paint st_rounded(lx::ui::control_paint p, lx::ui::radius r) {
    for (auto& c : p.corners)
        c = r;
    return p;
}

[[nodiscard]] lx::ui::control_paint st_bordered(lx::ui::control_paint p, std::uint8_t w) {
    for (auto& b : p.border_widths)
        b = w;
    return p;
}

[[nodiscard]] lx::ui::child st_box(const lx::ui::style& s, const lx::ui::control_paint& p,
                                   std::span<const lx::ui::child> kids,
                                   lx::ui::element_key key = 0) {
    lx::ui::control_props c{};
    c.layout = s;
    c.paint = p;
    return control(c, kids, key);
}

[[nodiscard]] lx::ui::child st_text(const char* text, float px, tok color, bool bold = false,
                                    bool muted_line = false) {
    lx::ui::text_block_props p{};
    p.text = text ? text : "";
    const char* family = active_theme().fonts().ui_family;
    p.font = {family && family[0] ? family : lx::text::k_ui_family, px * 0.75f,
              bold ? lx::text::weight::bold : lx::text::weight::normal, lx::text::style::normal};
    p.color = active_theme().color(color);
    p.line_height = muted_line ? 1.f : lx::ui::k_phi;
    p.wrap = lx::text::text_wrap::none;
    p.context = active_ui_resources().text;
    return text_block(p);
}

[[nodiscard]] float st_text_px(float px) { return px * st_rem() / 16.f; }

[[nodiscard]] unsigned st_a11y(lx::a11y::state_flag f) { return static_cast<unsigned>(f); }

// ── Labels ────────────────────────────────────────────────────────────────────────────

constexpr std::string_view k_label_overrides[][2] = {
    {"dnd-while-sharing", "Do not disturb while sharing"},
    {"tap-button-map", "Tap button map"},
    {"numlock", "Num Lock at start"},
    {"lock-command", "Screen lock command"},
    {"blank-after", "Blank after"},
    {"blank-locked-after", "Blank (session locked) after"},
    {"render-scale", "Render scale"},
    {"default-bindings", "Default key bindings"},
    {"scroll-method", "Scroll method"},
    {"click-method", "Click method"},
    {"accel-profile", "Acceleration profile"},
    {"accel-speed", "Acceleration speed"},
    {"natural-scroll", "Natural scrolling"},
    {"disable-while-typing", "Disable while typing"},
    {"middle-emulation", "Middle-button emulation"},
    {"three-finger-drag", "Three-finger drag"},
    {"drag-lock", "Drag lock"},
    {"left-handed", "Left-handed"},
    {"scroll-factor", "Scroll factor"},
    {"tap", "Tap to click"},
    {"drag", "Tap and drag"},
    {"master-ratio", "Master width"},
    {"master-count", "Master windows"},
    {"gaps", "Gaps"},
    {"switch-on-activate", "Switch on activate"},
    {"move-follows", "Window moves follow"},
    {"include-minimized", "Include minimized"},
    {"open-delay", "Open delay"},
    {"raise-on-click", "Raise on click"},
    {"raise-on-sloppy", "Raise on hover"},
    {"edge-snap", "Edge snapping"},
    {"edge-snap-distance", "Snap distance"},
    {"edge-snap-corner", "Corner size"},
    {"snap-preview", "Snap preview"},
    {"drag-restore", "Restore on drag"},
    {"critical-timeout", "Critical timeout"},
    {"timeout-low", "Low-urgency timeout"},
    {"timeout", "Timeout"},
    {"max-visible", "Most visible"},
    {"history", "History size"},
    {"layout", "Layout"},
    {"variant", "Variant"},
    {"options", "Options"},
    {"model", "Model"},
    {"rules", "Rules"},
    {"repeat-rate", "Repeat rate"},
    {"repeat-delay", "Repeat delay"},
    {"theme", "Theme"},
    {"size", "Size"},
    {"spec", "Background"},
    {"blur", "Blur"},
    {"mode", "Mode"},
    {"count", "Count"},
    {"names", "Names"},
    {"stiffness", "Stiffness"},
    {"damping-ratio", "Damping"},
    {"enabled", "Enabled"},
    {"policy", "Policy"},
    {"lock-on-blank", "Lock on blank"},
    {"position", "Position"},
};

/// One-line explanations for the keys that need one (the label says the rest). The value
/// block's control states the effect; the description says the trade-off.
constexpr std::string_view k_descriptions[][2] = {
    {"repeat-rate", "Keys repeat this often while held"},
    {"repeat-delay", "Delay before a held key starts repeating"},
    {"numlock", "Num Lock's state when the session opens"},
    {"blank-after", "Blank the screen after this long idle (0: never)"},
    {"blank-locked-after", "Lock the session after this long idle"},
    {"lock-command", "Runs to lock the session (blanking runs it too when set to lock)"},
    {"master-ratio", "The master column's share of the tiling area"},
    {"gaps", "Space between tiled windows"},
    {"stiffness", "How fast windows animate (higher: snappier)"},
    {"damping-ratio", "1: no overshoot; below springs past"},
    {"open-delay", "How long Alt is held before the switcher opens"},
    {"max-visible", "Notifications on screen at once; the rest queue"},
    {"history", "Dismissed notifications kept for review"},
    {"critical-timeout", "0: critical notifications wait for dismissal"},
    {"render-scale", "Supersampling for this output (1: follows its scale)"},
    {"default-bindings", "Start from the default key bindings"},
    {"edge-snap-distance", "Distance from an edge that snaps a dragged window"},
    {"edge-snap-corner", "Corner size that snaps to quarter tiling"},
    {"scroll-factor", "Multiplier on lines scrolled per notch"},
    {"accel-speed", "-1 slowest, 1 fastest"},
};

/// The description for `key`, or null.
[[nodiscard]] const char* description_of(std::string_view key) {
    for (const auto& [k, text] : k_descriptions)
        if (k == key)
            return text.data();
    return nullptr;
}

/// "repeat-rate" → "Repeat rate", the overrides above taking precedence. Interned once.
[[nodiscard]] const char* intern_label(std::string_view key) {
    static std::vector<std::string> g_interned; // filled during catalog(), then stable
    for (const auto& [k, label] : k_label_overrides)
        if (k == key)
            return label.data();
    std::string out;
    out.reserve(key.size());
    for (std::size_t i = 0; i < key.size(); ++i) {
        const char c = key[i];
        out.push_back(i == 0 || key[i - 1] == '-' ? (c >= 'a' && c <= 'z') ? char(c - 32) : c
                                                  : c == '-' ? ' ' : c);
    }
    g_interned.push_back(std::move(out));
    return g_interned.back().c_str();
}

constexpr const char* k_pointer_section_names[3] = {"input.touchpad", "input.mouse",
                                                    "input.trackpoint"};

/// A pointer section's field mask — the same keys the schema's parser accepts per class.
[[nodiscard]] bool section_accepts(unsigned section, std::string_view key) {
    static constexpr std::string_view k_mouse[] = {"natural-scroll", "accel-profile",
                                                   "accel-speed", "left-handed",
                                                   "middle-emulation", "scroll-factor"};
    static constexpr std::string_view k_trackpoint[] = {"accel-profile", "accel-speed",
                                                        "scroll-factor"};
    if (section == 0)
        return true;
    const std::span<const std::string_view> mask =
        section == 1 ? std::span<const std::string_view>{k_mouse}
                     : std::span<const std::string_view>{k_trackpoint};
    return std::find(mask.begin(), mask.end(), key) != mask.end();
}

/// A key's unit for the readout ("", when none).
[[nodiscard]] const char* unit_of(const char* section, const char* key) {
    const std::string_view k = key;
    if (k == "repeat-rate" || k == "repeat-delay" || k == "open-delay")
        return "ms";
    if (k == "timeout" || k == "timeout-low" || k == "critical-timeout")
        return "s";
    if (k == "blank-after" || k == "blank-locked-after")
        return "s";
    if (k == "size" || k == "gaps" || k == "edge-snap-distance" || k == "edge-snap-corner")
        return "px";
    (void)section;
    return "";
}

/// The pane and group header a section renders under (ADR-013 §5.5's table).
void assign_section(std::string_view section, pane_id& owner, const char*& group) {
    struct section_route {
        std::string_view section;
        pane_id pane;
        const char* group;
    };
    static constexpr section_route k_routes[] = {
        {"input.keyboard", pane_id::input, "Keyboard"},
        {"input.touchpad", pane_id::input, "Touchpad"},
        {"input.mouse", pane_id::input, "Mouse"},
        {"input.trackpoint", pane_id::input, "Trackpoint"},
        {"cursor", pane_id::appearance, "Cursor"},
        {"ui", pane_id::appearance, "Theme & density"},
        {"background", pane_id::appearance, "Background"},
        {"panel", pane_id::appearance, "Panel"},
        {"output", pane_id::displays, "Per-output rendering"},
        {"focus", pane_id::windows, "Focus"},
        {"windows", pane_id::windows, "Snapping"},
        {"tiling", pane_id::windows, "Tiling"},
        {"workspaces", pane_id::windows, "Workspaces"},
        {"switcher", pane_id::windows, "Window switcher"},
        {"animations", pane_id::windows, "Animations"},
        {"notifications", pane_id::notifications, "Notifications"},
        {"idle", pane_id::idle, "Idle"},
        {"general", pane_id::session, "General"},
    };
    for (const section_route& r : k_routes)
        if (r.section == section) {
            owner = r.pane;
            group = r.group;
            return;
        }
    owner = pane_id::session;
    group = "General";
}

} // namespace

// ── catalog (public: the anon-namespace helpers must come first) ───────────────────────

namespace {

std::vector<row_ref> g_catalog; // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)
/// Catalog row index → `session` string-buffer slot (`k_no_row`: no slot), for the
/// schema's string keys.
unsigned g_string_slots[128]{}; // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)

/// This file's channel (main.cpp declares its own in its translation unit) — for the one
/// warning build_catalog can raise.
constexpr lx::trace::category log_settings{"settings"};

void build_catalog() {
    g_catalog.clear();
    g_catalog.reserve(96);
    const auto push = [&](row_ref r) {
        std::string s;
        if (r.desc)
            s = std::string(r.desc->section) + ' ' + r.desc->key + ' ' + r.label;
        else
            s = std::string(k_pointer_section_names[r.pointer_section]) + ' ' + r.pdesc->key +
                ' ' + r.label;
        for (char& c : s)
            c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        std::snprintf(r.search, sizeof(r.search), "%s", s.c_str());
        if (r.desc)
            r.unit = unit_of(r.desc->section, r.desc->key);
        else
            r.unit = unit_of(k_pointer_section_names[r.pointer_section], r.pdesc->key);
        assign_section(r.desc ? std::string_view(r.desc->section)
                              : std::string_view(k_pointer_section_names[r.pointer_section]),
                       r.owner, r.group);
        g_catalog.push_back(std::move(r));
    };

    for (const cfg::key_desc& d : cfg::schema_keys()) {
        row_ref r{};
        r.desc = &d;
        r.label = intern_label(d.key);
        r.description = description_of(d.key);
        push(std::move(r));
    }
    for (unsigned section = 0; section < 3; ++section)
        for (const cfg::pointer_key_desc& p : cfg::pointer_keys()) {
            if (!section_accepts(section, p.key))
                continue;
            row_ref r{};
            r.pdesc = &p;
            r.pointer_section = section;
            r.label = intern_label(p.key);
            push(std::move(r));
        }
    // The string rows take their buffer slots in catalog order; a string row past the cap
    // gets k_no_row (read-only) — never another row's buffer.
    for (unsigned& s : g_string_slots)
        s = session::k_no_row;
    unsigned slot = 0;
    for (unsigned i = 0; i < g_catalog.size() && i < std::size(g_string_slots); ++i) {
        if (!g_catalog[i].desc || g_catalog[i].desc->kind != cfg::value_kind::string)
            continue;
        if (slot >= session::k_max_string_rows) {
            log_settings.warn("settings: {} string keys, the session holds {} — the rest "
                              "render read-only",
                              slot + 1, session::k_max_string_rows);
            break;
        }
        g_string_slots[i] = slot++;
    }
}

/// Everything a control needs about a row's field, descriptor or not.
struct field_spec {
    cfg::value_kind kind = cfg::value_kind::boolean;
    double lo = 0.0;
    double hi = 0.0;
    const char* const* names = nullptr;
    unsigned name_count = 0;
    unsigned capacity = 0;
    cfg::reload_class reload = cfg::reload_class::live;
};

[[nodiscard]] field_spec spec_of(const row_ref& r) {
    if (r.desc)
        return {r.desc->kind, r.desc->lo, r.desc->hi, r.desc->names, r.desc->name_count,
                r.desc->capacity, r.desc->reload};
    return {r.pdesc->kind, r.pdesc->lo, r.pdesc->hi, r.pdesc->names, r.pdesc->name_count, 0,
            cfg::reload_class::live};
}

} // namespace

namespace settings {

const std::vector<row_ref>& catalog() {
    static const bool g_built = [] {
        build_catalog();
        return true;
    }();
    (void)g_built;
    return g_catalog;
}

unsigned string_slot(unsigned row) {
    return row < std::size(g_string_slots) ? g_string_slots[row] : session::k_no_row;
}

void set_active_session(session* s) { g_session = s; }

session* active_session() { return g_session; }

bool row_matches(const row_ref& r, std::string_view query) {
    if (query.empty())
        return true;
    std::string_view text = r.search;
    for (const char q : query) {
        if (q == ' ')
            continue;
        const auto at = text.find(q);
        if (at == std::string_view::npos)
            return false;
        text.remove_prefix(at + 1);
    }
    return true;
}

} // namespace settings

// ── session ────────────────────────────────────────────────────────────────────────────

namespace {

/// The value `row`'s descriptor points at in `c` (the pointer sections through their class
/// struct; the parser's own read/write contract: bool / int / double / char / uchar).
[[nodiscard]] void* field_of(cfg::config_snapshot& c, const row_ref& r) {
    if (r.desc)
        return r.desc->field(c);
    cfg::pointer_settings& p = r.pointer_section == 0   ? c.touchpad
                               : r.pointer_section == 1 ? c.mouse
                                                        : c.trackpoint;
    return r.pdesc->field(p);
}

} // namespace

namespace settings {

void session::load_strings() {
    for (unsigned& s : string_rows_)
        s = k_no_row;
    const std::vector<row_ref>& rows = catalog();
    for (unsigned i = 0, used = 0; i < rows.size() && used < k_max_string_rows; ++i) {
        if (!rows[i].desc || rows[i].desc->kind != cfg::value_kind::string)
            continue;
        const cfg::key_desc& d = *rows[i].desc;
        string_rows_[used] = i;
        const char* value = static_cast<const char*>(d.field(config_));
        strings_[used].set(value ? std::string_view(value) : std::string_view{});
        ++used;
    }
    // The query buffer starts empty.
    query_.storage[0] = '\0';
    query_.bind();
    query_.buffer.size = 0;
}

void* session::field_of(unsigned row) {
    const std::vector<row_ref>& rows = catalog();
    if (row >= rows.size())
        return nullptr;
    return ::field_of(config_, rows[row]);
}

void session::load_from_files() {
    namespace cfg = lx::config;
    const cfg::config_location where = cfg::locate_config();
    if (where.disabled)
        return; // LUMEN_CONFIG=0: the defaults, in memory only
    cfg::config_text text{};
    cfg::diagnostics d{};
    if (!cfg::read_config(where, text, d) || text.layers.empty())
        return; // an unreadable layer: the compositor refuses it too; show the defaults
    cfg::config_snapshot parsed{};
    if (!cfg::parse_config(text, parsed, d))
        return; // a syntax error on disk: the compositor keeps its previous config; the
                // pane shows the defaults until the file is valid
    config_ = parsed;
    if (!where.user_file.path[0])
        return;
    std::snprintf(user_file_, sizeof(user_file_), "%s", where.user_file.path);
    user_file_disabled_ = false;
}

void session::reload_effective() {
    namespace cfg = lx::config;
    if (!forced_user_file_) {
        load_from_files();
        return;
    }
    // A test's temp file is the whole truth (load_from_files would locate the real
    // layers instead).
    const char* why = nullptr;
    bool missing = false;
    std::string content;
    cfg::config_text one{};
    cfg::diagnostics d{};
    cfg::config_snapshot parsed{};
    if (cfg::read_file(user_file_, content, why, missing) && !missing &&
        one.add_layer(user_file_, content) && cfg::parse_config(one, parsed, d))
        config_ = parsed;
}

bool session::write_key(unsigned row, const lx::config::edit_value& v) {
    namespace cfg = lx::config;
    if (user_file_disabled_)
        return false;
    const std::vector<row_ref>& rows = catalog();
    if (row >= rows.size())
        return false;
    const row_ref& r = rows[row];
    const char* section = r.desc ? r.desc->section : k_pointer_section_names[r.pointer_section];
    const char* key = r.desc ? r.desc->key : r.pdesc->key;

    // The user layer's bytes (a missing file splices into empty text: the first edit
    // creates it).
    std::string text;
    {
        const char* why = nullptr;
        bool missing = false;
        std::string content;
        if (cfg::read_file(user_file_, content, why, missing) && !missing)
            text = std::move(content);
        else if (!missing)
            return false; // unreadable: the compositor refuses it too; nothing to splice
    }
    cfg::edit_report report{};
    const std::string edited = cfg::splice_key(text, section, key, v, report);
    if (report.status != cfg::edit_status::ok)
        return false;

    // Verify before the write: the edited layer must parse, without errors, and give the
    // value that was asked for (§4.3's "never write a file whose meaning was not
    // verified") — into a probe, not `config_`: a refusal leaves the rows showing the
    // effective configuration, lower layers included.
    {
        cfg::config_text one{};
        cfg::diagnostics d{};
        cfg::config_snapshot probe{};
        if (!one.add_layer(user_file_, edited) || !cfg::parse_config(one, probe, d) ||
            d.errors())
            return false;
        // The splice changed exactly the line it meant to (the value round-trips).
        void* f = ::field_of(probe, r);
        if (!f)
            return false;
        const field_spec spec = spec_of(r);
        switch (v.type) {
        case cfg::edit_value::kind::boolean:
            if (*static_cast<bool*>(f) != v.b)
                return false;
            break;
        case cfg::edit_value::kind::integer:
            if (*static_cast<int*>(f) != static_cast<int>(v.i))
                return false;
            break;
        case cfg::edit_value::kind::number:
            if (std::abs(*static_cast<double*>(f) - v.d) > 1e-9)
                return false;
            break;
        case cfg::edit_value::kind::word:
            if (spec.name_count &&
                *static_cast<unsigned char*>(f) >= spec.name_count)
                return false;
            break;
        case cfg::edit_value::kind::string:
            if (std::string_view(static_cast<const char*>(f)) != v.text)
                return false;
            break;
        case cfg::edit_value::kind::string_list:
            break;
        }
    }

    // Atomic write: temp in the same directory, fsync, rename (output_config_store's
    // pattern). Human-frequency edits of a ≤ 256 KiB file: synchronous, on the UI thread.
    {
        std::filesystem::path target{user_file_};
        std::filesystem::path tmp = target;
        tmp += ".settings-tmp";
        std::error_code ec;
        // The first edit creates the user layer: its directory may not exist yet.
        std::filesystem::create_directories(target.parent_path(), ec);
        {
            std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
            if (!out)
                return false;
            out.write(edited.data(), static_cast<std::streamsize>(edited.size()));
            out.flush();
            if (!out)
                return false;
        }
        const int tmp_fd = ::open(tmp.c_str(), O_RDONLY);
        if (tmp_fd >= 0) {
            ::fsync(tmp_fd);
            ::close(tmp_fd);
        }
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // The file is the truth now: the rows re-read the effective configuration.
    reload_effective();
    save_ = save_state::written;
    return true;
}

void session::repaint() {
    if (save_ == save_state::untouched)
        save_ = save_state::memory_only;
    refresh();
}

void session::set_bool(unsigned row, bool v) {
    if (write_key(row, lx::config::edit_value::boolean(v)))
        return written();
    if (void* f = field_of(row))
        *static_cast<bool*>(f) = v;
    repaint();
}

void session::set_int(unsigned row, int v) {
    const std::vector<row_ref>& rows = catalog();
    if (row >= rows.size())
        return;
    const field_spec spec = spec_of(rows[row]);
    v = std::clamp(v, static_cast<int>(spec.lo), static_cast<int>(spec.hi));
    if (write_key(row, lx::config::edit_value::integer(v)))
        return written();
    *static_cast<int*>(field_of(row)) = v;
    repaint();
}

void session::set_number(unsigned row, double v) {
    const std::vector<row_ref>& rows = catalog();
    if (row >= rows.size())
        return;
    const field_spec spec = spec_of(rows[row]);
    v = std::clamp(v, spec.lo, spec.hi);
    if (write_key(row, lx::config::edit_value::number(v)))
        return written();
    *static_cast<double*>(field_of(row)) = v;
    repaint();
}

void session::set_enum(unsigned row, unsigned v) {
    const std::vector<row_ref>& rows = catalog();
    if (row >= rows.size())
        return;
    const field_spec spec = spec_of(rows[row]);
    v = std::min(v, spec.name_count ? spec.name_count - 1u : 0u);
    const bool ui_row = rows[row].desc && std::strcmp(rows[row].desc->section, "ui") == 0;
    if (write_key(row, lx::config::edit_value::word(spec.names[v]))) {
        if (ui_row)
            apply_ui_live();
        return written();
    }
    *static_cast<unsigned char*>(field_of(row)) = static_cast<unsigned char>(v);
    repaint();
}

void session::commit_string(unsigned row) {
    const std::vector<row_ref>& rows = catalog();
    if (row >= rows.size())
        return;
    const field_spec spec = spec_of(rows[row]);
    const unsigned slot = string_slot(row);
    if (slot == k_no_row)
        return; // no buffer of its own (past the cap): nothing to commit
    string_field& f = slot_buffer(slot);
    if (write_key(row, lx::config::edit_value::string(f.storage))) {
        // The snapshot now carries the file's value; the input keeps typing in its buffer.
        if (rows[row].desc && std::strcmp(rows[row].desc->section, "ui") == 0)
            apply_ui_live();
        written();
        return;
    }
    std::snprintf(static_cast<char*>(field_of(row)), spec.capacity ? spec.capacity : 560, "%s",
                  f.storage);
    repaint();
}

std::vector<std::string> session::g_installed; // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)

void session::apply_ui_live() {
    const lx::config::ui_settings& ui = config_.ui;
    const std::string name =
        ui.theme[0]
            ? std::string(ui.theme)
            : std::string(ui.appearance == lx::config::ui_appearance::light ? "one-light"
                                                                            : "one-dark");
    if (name == live_theme_ && ui.appearance == live_appearance_ &&
        ui.density == live_density_)
        return;
    live_theme_ = name;
    live_appearance_ = ui.appearance;
    live_density_ = ui.density;
    if (name == "one-dark") {
        set_active_builtin_theme(lx::ui::appearance::dark);
    } else if (name == "one-light") {
        set_active_builtin_theme(lx::ui::appearance::light);
    } else if (auto loaded = lx::ui::load_theme_file(name.c_str())) {
        set_active_theme(std::move(loaded).value());
    }
    // A theme file that does not load: the row keeps its value and the app keeps the
    // previous theme (the file write already happened; the compositor and the shell apply
    // it on their side and report).
    switch (ui.density) {
    case lx::config::ui_density_kind::compact: set_ui_density(lx::ui::density::compact); break;
    case lx::config::ui_density_kind::comfortable:
        set_ui_density(lx::ui::density::comfortable);
        break;
    case lx::config::ui_density_kind::regular:
    default: set_ui_density(lx::ui::density::standard); break;
    }
    refresh();
}

std::string session::user_theme_dir() {
    const char* home = std::getenv("XDG_CONFIG_HOME");
    const std::string base =
        home && home[0] ? home : [] {
            const char* h = std::getenv("HOME");
            return h && h[0] ? std::string(h) + "/.config" : std::string{};
        }();
    if (base.empty())
        return {};
    const std::filesystem::path dir = std::filesystem::path(base) / "lumen" / "themes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

const std::vector<std::string>& session::installed_themes() {
    if (g_installed.empty()) {
        g_installed.push_back("one-dark");
        g_installed.push_back("one-light");
        const std::string dir = user_theme_dir();
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            std::string name = entry.path().filename().string();
            if (name.size() > 8 && name.ends_with(".lxtheme"))
                g_installed.push_back(name.substr(0, name.size() - 8));
        }
    }
    return g_installed;
}

void session::import_theme(std::string_view json_path) {
    namespace fs = std::filesystem;
    const fs::path input{json_path};
    if (json_path.empty() || !fs::exists(input)) {
        import_status_ = "No such file.";
        refresh();
        return;
    }
    // The output name: the input's stem (my-theme.json → my-theme).
    std::string stem = input.stem().string();
    for (char& c : stem)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            c = '-';
    if (stem.empty()) {
        import_status_ = "The file name has no usable characters.";
        refresh();
        return;
    }
    const std::string out = user_theme_dir() + "/" + stem + ".lxtheme";
    const lx::ui::theme_compiler compiler{};
    const auto report = compiler.compile({input.c_str(), out.c_str(), nullptr});
    if (!report) {
        import_status_ = frame_format("That file is not a Zed theme family ({}).",
                                      report.get_error().message);
        refresh();
        return;
    }
    // Installed: refresh the list, load it live, write [ui].theme (the shell and the
    // applications that start later follow).
    g_installed.clear();
    (void)installed_themes();
    if (auto loaded = lx::ui::load_theme_file(out.c_str()))
        set_active_theme(std::move(loaded).value());
    import_status_ = frame_format("Compiled {} ({} fallbacks applied) — installed and active.",
                                  stem, report.value().fallbacks.size());
    // [ui].theme = the compiled file, through the same write path as a row edit.
    const std::vector<row_ref>& rows = catalog();
    for (unsigned i = 0; i < rows.size(); ++i) {
        if (rows[i].desc && std::strcmp(rows[i].desc->section, "ui") == 0 &&
            std::strcmp(rows[i].desc->key, "theme") == 0) {
            const unsigned slot = string_slot(i);
            if (slot != k_no_row)
                slot_buffer(slot).set(out);
            commit_string(i);
            break;
        }
    }
    refresh();
}

lx::config::combo session::binding_of_action(unsigned index) const {
    const std::span<const lx::config::action_info> all = lx::config::actions();
    if (index >= all.size())
        return {};
    lx::config::combo fallback{};
    for (const lx::config::binding& b : config_.bindings) {
        if (b.action != all[index].id)
            continue;
        if (b.layer != lx::config::k_no_layer)
            return b.keys; // the effective (user) one
        if (fallback.kind == lx::config::combo_kind::tap && fallback.mods == 0 &&
            fallback.code == 0)
            fallback = b.keys; // the first default
    }
    return fallback;
}

std::string_view session::binding_is_user(const lx::config::combo& c) const {
    for (const lx::config::binding& b : config_.bindings)
        if (b.keys == c && b.layer != lx::config::k_no_layer)
            return "from the user layer";
    return {};
}

bool session::user_layer_text(std::string& out) const {
    const char* why = nullptr;
    bool missing = false;
    std::string content;
    if (!lx::config::read_file(user_file_, content, why, missing))
        return missing; // a missing layer splices into empty text; an unreadable one fails
    if (!missing)
        out = std::move(content);
    return true;
}

bool session::commit_layer(const std::string& edited) {
    // Verify before the write: the edited layer must parse without errors (§4.3) — into a
    // probe; `config_` re-reads the layers after the write instead.
    {
        lx::config::config_text one{};
        lx::config::diagnostics d{};
        lx::config::config_snapshot probe{};
        if (!one.add_layer(user_file_, edited) ||
            !lx::config::parse_config(one, probe, d) || d.errors())
            return false;
    }
    std::filesystem::path target{user_file_};
    std::filesystem::path tmp = target;
    tmp += ".settings-tmp";
    std::error_code ec;
    // The first edit creates the user layer: its directory may not exist yet.
    std::filesystem::create_directories(target.parent_path(), ec);
    {
        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        if (!out)
            return false;
        out.write(edited.data(), static_cast<std::streamsize>(edited.size()));
        out.flush();
        if (!out)
            return false;
    }
    const int tmp_fd = ::open(tmp.c_str(), O_RDONLY);
    if (tmp_fd >= 0) {
        ::fsync(tmp_fd);
        ::close(tmp_fd);
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    reload_effective();
    save_ = save_state::written;
    return true;
}

void session::apply_edit_action() {
    namespace cfg = lx::config;
    const std::span<const cfg::action_info> all = cfg::actions();
    if (editing_action_ >= all.size())
        return;
    const cfg::action_info& action = all[editing_action_];
    const std::string_view typed = edit_.buffer.view();

    // Empty: the action becomes unbound ("none" removes the default binding's line).
    cfg::combo combo{};
    char canon[64]{};
    if (!typed.empty()) {
        const char* why = nullptr;
        if (!cfg::parse_combo(typed, combo, why)) {
            std::snprintf(error_, sizeof(error_), "%s", why ? why : "not a combination");
            refresh();
            return;
        }
        if (cfg::reserved(combo)) {
            cfg::format_combo(combo, canon, sizeof(canon));
            std::snprintf(error_, sizeof(error_),
                          "%s is Lumen's own (the ways out of a stuck session)", canon);
            refresh();
            return;
        }
        if (action.pointer && combo.kind != cfg::combo_kind::button) {
            std::snprintf(error_, sizeof(error_), "%s needs a pointer button (…+BTN_LEFT)",
                          action.name);
            refresh();
            return;
        }
        if (!action.pointer && combo.kind == cfg::combo_kind::button) {
            std::snprintf(error_, sizeof(error_), "%s is keyboard-only", action.name);
            refresh();
            return;
        }
        cfg::format_combo(combo, canon, sizeof(canon));
    }

    // The target combo bound to a different action: a conflict (clear that one first).
    for (const cfg::binding& b : config_.bindings) {
        if (typed.empty() || !(b.keys == combo) || b.action == cfg::action_id::none)
            continue;
        const cfg::action_info* other = nullptr;
        for (const cfg::action_info& a : all)
            if (a.id == b.action)
                other = &a;
        if (other && other->id != action.id) {
            std::snprintf(error_, sizeof(error_), "%s is bound to %s — unbind it there first",
                          canon, other->name);
            refresh();
            return;
        }
    }

    if (user_file_disabled_) {
        std::snprintf(error_, sizeof(error_), "No writable config.toml — nothing changed");
        refresh();
        return;
    }
    std::string text;
    if (!user_layer_text(text)) {
        std::snprintf(error_, sizeof(error_), "config.toml cannot be read");
        refresh();
        return;
    }
    cfg::edit_report report{};
    if (typed.empty()) {
        // "none" on the action's current canonical combo removes the default binding.
        const cfg::combo previous = binding_of_action(editing_action_);
        char prev_canon[64];
        cfg::format_combo(previous, prev_canon, sizeof(prev_canon));
        const std::string prev_key = "\"" + std::string(prev_canon) + "\"";
        const std::string edited = cfg::splice_key(text, "bindings", prev_key,
                                                   cfg::edit_value::string("none"), report);
        if (report.status != cfg::edit_status::ok || !commit_layer(edited)) {
            std::snprintf(error_, sizeof(error_), "Writing the file failed");
            refresh();
            return;
        }
    } else {
        // The value: "action", or "action 1" for the index actions (their own argument
        // field is a later refinement; 1 is the workspace default).
        std::string value = action.name;
        if (action.arg == cfg::action_arg::index)
            value += " 1";
        const std::string key = "\"" + std::string(canon) + "\"";
        const std::string edited =
            cfg::splice_key(text, "bindings", key, cfg::edit_value::string(value), report);
        if (report.status != cfg::edit_status::ok || !commit_layer(edited)) {
            std::snprintf(error_, sizeof(error_), "Writing the file failed");
            refresh();
            return;
        }
    }
    written();
    error_[0] = '\0';
    editing_action_ = k_no_row;
    refresh();
}

void session::reset_binding(unsigned index) {
    const lx::config::combo current = binding_of_action(index);
    if (user_file_disabled_) {
        reset_note_ = "No writable config.toml.";
        refresh();
        return;
    }
    char canon[64];
    lx::config::format_combo(current, canon, sizeof(canon));
    const std::string key = "\"" + std::string(canon) + "\"";
    lx::config::edit_report report{};
    std::string text;
    if (!user_layer_text(text)) {
        reset_note_ = "config.toml cannot be read.";
        refresh();
        return;
    }
    const std::string edited = lx::config::remove_key(text, "bindings", key, report);
    if (report.line == 0) {
        reset_note_ = "Its binding is not from the user layer — nothing to reset.";
        refresh();
        return;
    }
    if (!commit_layer(edited)) {
        reset_note_ = "Writing the file failed.";
        refresh();
        return;
    }
    reset_note_.clear();
    written();
}

} // namespace settings

// ── value formatting ───────────────────────────────────────────────────────────────────

namespace {

[[nodiscard]] bool read_bool(const row_ref& r, const cfg::config_snapshot& c) {
    return *static_cast<const bool*>(field_of(const_cast<cfg::config_snapshot&>(c), r));
}

[[nodiscard]] int read_int(const row_ref& r, const cfg::config_snapshot& c) {
    return *static_cast<const int*>(field_of(const_cast<cfg::config_snapshot&>(c), r));
}

[[nodiscard]] double read_number(const row_ref& r, const cfg::config_snapshot& c) {
    return *static_cast<const double*>(field_of(const_cast<cfg::config_snapshot&>(c), r));
}

[[nodiscard]] unsigned read_enum(const row_ref& r, const cfg::config_snapshot& c) {
    return *static_cast<const unsigned char*>(field_of(const_cast<cfg::config_snapshot&>(c), r));
}

/// The value text a row's readout shows ("25 ms", "0.55").
[[nodiscard]] std::string value_text(const row_ref& r, const cfg::config_snapshot& c) {
    char buf[48];
    switch (r.desc ? r.desc->kind : r.pdesc->kind) {
    case cfg::value_kind::boolean:
        return read_bool(r, c) ? "on" : "off";
    case cfg::value_kind::integer: {
        const int v = read_int(r, c);
        std::snprintf(buf, sizeof(buf), "%d", v);
        break;
    }
    case cfg::value_kind::number: {
        const double v = read_number(r, c);
        std::snprintf(buf, sizeof(buf), "%.2g", v);
        break;
    }
    case cfg::value_kind::enumeration:
        return r.desc ? cfg::enum_word(r.desc->names, r.desc->name_count, read_enum(r, c))
                      : cfg::enum_word(r.pdesc->names, r.pdesc->name_count, read_enum(r, c));
    case cfg::value_kind::string:
        return static_cast<const char*>(field_of(const_cast<cfg::config_snapshot&>(c), r));
    case cfg::value_kind::string_list:
        return "[names]";
    default:
        return "";
    }
    return std::string(buf) + (r.unit && r.unit[0] ? std::string(" ") + r.unit : std::string());
}

/// The reload badge ("next login", "next time the switcher opens"), null for `live`.
[[nodiscard]] const char* reload_badge(const row_ref& r) {
    const cfg::reload_class reload = r.desc ? r.desc->reload : cfg::reload_class::live;
    if (reload == cfg::reload_class::start)
        return "at the next login";
    if (reload == cfg::reload_class::next_open)
        return "the next time the switcher opens";
    return nullptr;
}

// ── the row framework ──────────────────────────────────────────────────────────────────

/// A value control for `row`, wired to `s`. The lambdas capture the session and the row
/// index only (24-byte handler cap); they re-read the catalog for the ranges.
[[nodiscard]] lx::ui::child value_control(const row_ref& r, unsigned index, session* s) {
    const field_spec spec = spec_of(r);
    const lx::ui::element_key key = k_row_control_base + index;
    const cfg::config_snapshot& c = s->config();
    switch (spec.kind) {
    case cfg::value_kind::boolean:
        return toggle_switch(read_bool(r, c))
            .on_toggle([s, index](int v) { s->set_bool(index, v != 0); })
            .name(r.label)
            .key(key);
    case cfg::value_kind::enumeration: {
        const std::span<const char* const> words{spec.names, spec.name_count};
        const unsigned v = read_enum(r, c);
        if (spec.name_count <= 3)
            return segmented(words, static_cast<int>(v))
                .on_select([s, index](int i) { s->set_enum(index, static_cast<unsigned>(i)); })
                .name(r.label)
                .key(key);
        return select(words, static_cast<int>(v))
            .open(s->open_menu() == index)
            .on_open([s, index](int) { s->set_open_menu(index); })
            .on_dismiss([s, index](int) { s->set_open_menu(session::k_no_row); })
            .on_select([s, index](int i) {
                s->set_enum(index, static_cast<unsigned>(i));
                s->set_open_menu(session::k_no_row);
            })
            .name(r.label)
            .key(key);
    }
    case cfg::value_kind::integer:
    case cfg::value_kind::number: {
        const int steps =
            std::max(1, static_cast<int>(std::min(spec.hi - spec.lo, spec.kind ==
                                                                        cfg::value_kind::integer
                                                                    ? 32.0
                                                                    : 120.0)));
        lx::ui::child readout_text = st_text(frame_format("{}", value_text(r, c)),
                                             st_text_px(12.f), tok::text_muted, false, true);
        const child readout =
            st_box(st_h_flex().flex_none().w(st_px16(56.f)).justify_end(),
                   control_paint::fixed({k_transparent, k_transparent}),
                   {&readout_text, 1u});
        lx::ui::slider_builder bar = slider(spec.kind == cfg::value_kind::integer
                                                ? static_cast<float>(read_int(r, c))
                                                : static_cast<float>(read_number(r, c)),
                                            static_cast<float>(spec.lo),
                                            static_cast<float>(spec.hi))
                                         .steps(steps)
                                         .on_change([s, index, kind = spec.kind](int idx) {
                                             const std::vector<row_ref>& rows = catalog();
                                             if (index >= rows.size())
                                                 return;
                                             const field_spec f = spec_of(rows[index]);
                                             const double v =
                                                 f.lo + (f.hi - f.lo) *
                                                            static_cast<double>(idx) /
                                                            static_cast<double>(
                                                                std::max(1, static_cast<int>(
                                                                                 std::min(
                                                                                     f.hi - f.lo,
                                                                                     kind ==
                                                                                         cfg::value_kind::integer
                                                                                         ? 32.0
                                                                                         : 120.0))));
                                             if (kind == cfg::value_kind::integer)
                                                 s->set_int(index,
                                                            static_cast<int>(std::lround(v)));
                                             else
                                                 s->set_number(index, v);
                                         })
                                         .name(r.label)
                                         .key(key);
        lx::ui::child bar_child = bar.layout(
            st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                .h(st_px16(20.f)));
        const child parts[2] = {bar_child, readout};
        return st_box(st_h_flex().flex_none().w(st_px16(k_value_w)).gap(st_px16(8.f)),
                      control_paint{}, parts);
    }
    case cfg::value_kind::string: {
        const unsigned slot = string_slot(index);
        if (slot == session::k_no_row) {
            // Past the session's buffer cap (build_catalog warned): the value shows,
            // it does not edit.
            lx::ui::child text = st_text(frame_format("{}", value_text(r, c)),
                                         st_text_px(12), tok::text_muted, false, true);
            return st_box(st_h_flex().flex_none().w(st_px16(k_value_w)).justify_end(),
                          control_paint::fixed({k_transparent, k_transparent}),
                          {&text, 1});
        }
        lx::ui::child field =
            text_input()
                .buffer(s->string_buffer(slot))
                .max_bytes(spec.capacity ? spec.capacity - 1u : 559u)
                .on_change([s](int) { s->refresh(); })
                .on_submit([s, index](int) { s->commit_string(index); })
                .name(r.label)
                .tab_index(0)
                .layout(st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                            .h(st_px16(28.f)))
                .key(key);
        // The value column keeps every control aligned down the pane (§5.3).
        return st_box(st_h_flex().flex_none().w(st_px16(k_value_w)), control_paint{},
                      {&field, 1});
    }
    case cfg::value_kind::string_list: {
        lx::ui::child list_text = st_text(frame_format("{}", value_text(r, c)),
                                          st_text_px(12.f), tok::text_muted, false, true);
        return st_box(st_h_flex().flex_none().w(st_px16(k_value_w)).justify_end(),
                      control_paint::fixed({k_transparent, k_transparent}),
                      {&list_text, 1u});
    }
    default:
        return nullptr;
    }
}

/// One setting row: the label block (label + reload badge) and the value control, a ghost
/// hover over the whole row (ADR-013 §5.3).
[[nodiscard]] lx::ui::child setting_row(const row_ref& r, unsigned index, session* s) {
    const char* badge = reload_badge(r);
    lx::ui::child texts[3]{};
    unsigned n = 0;
    texts[n++] = st_text(r.label, st_text_px(14.f), tok::text);
    if (r.description)
        texts[n++] = st_text(r.description, st_text_px(12.f), tok::text_muted, false, true);
    if (badge)
        texts[n++] = st_text(badge, st_text_px(11.f), tok::text_muted, false, true);
    lx::ui::child label_block = st_box(
        st_v_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f)).justify_center(),
        control_paint{}, {texts, n});
    lx::ui::child control_child = value_control(r, index, s);
    lx::ui::child kids[2] = {label_block, control_child};
    lx::ui::control_props row{};
    row.layout = st_h_flex().flex_none().w_full()
                     .min_h(st_px16((badge || r.description) ? k_badge_row_h : k_row_h))
                     .px(st_px16(12.f)).gap(st_px16(12.f)).rounded(lx::ui::radius::sm);
    lx::ui::control_paint p{};
    p.rest = {k_transparent, k_transparent};
    p.hover = {paint(tok::ghost_element_hover), k_transparent};
    p.active = {paint(tok::ghost_element_active), k_transparent};
    row.paint = p;
    row.role = lx::a11y::role::group;
    row.name = r.label;
    return control(row, {kids, 2u});
}

/// A group header: the group's name over a rule.
[[nodiscard]] lx::ui::child group_header(const char* name) {
    lx::ui::child rule = divider();
    lx::ui::child text =
        st_text(name, st_text_px(11.f), tok::text_muted, true, true);
    lx::ui::child kids[2] = {text, rule};
    return st_box(st_v_flex().flex_none().w_full().gap(st_px16(4.f))
                      .pt(st_px16(16.f)).pb(st_px16(2.f)),
                  control_paint{}, {kids, 2u});
}

/// The pane note under its rows (what this build does not yet edit: ADR-013's phases).
[[nodiscard]] lx::ui::child pane_note(pane_id p) {
    const char* note = p == pane_id::displays
                           ? "Modes, scale and arrangement are driven through the output "
                             "manager and arrive with the Displays pane (ADR-013 Phase F)."
                         : p == pane_id::shortcuts
                             ? "The bindings editor arrives with the write path (ADR-013 "
                               "Phase E); the defaults are on the Session pane."
                             : nullptr;
    if (!note)
        return nullptr;
    lx::ui::child note_text = st_text(note, st_text_px(12.f), tok::text_muted, false, true);
    return st_box(st_v_flex().flex_none().w_full().pt(st_px16(8.f)),
                  control_paint::fixed({k_transparent, k_transparent}),
                  {&note_text, 1u});
}

/// The Shortcuts pane (ADR-013 Phase E): every action with its effective binding, the
/// editor for one of them, resets. Bindings write `[bindings]` lines through the same
/// splice editor as every other key.
[[nodiscard]] lx::ui::child shortcuts_pane(session* s) {
    lx::ui::build_context* ctx = lx::ui::build_context::current();
    const std::span<const lx::config::action_info> all = lx::config::actions();
    unsigned count = 0;
    unsigned groups = 0;
    lx::config::action_owner last_owner = lx::config::action_owner::none;
    for (const lx::config::action_info& a : all) {
        ++count;
        if (a.owner != last_owner) {
            ++groups;
            last_owner = a.owner;
        }
    }
    const bool editing = s->editing_action() != session::k_no_row;
    const auto** kids = ctx ? ctx->alloc_children(count + groups + (editing ? 5 : 0) + 2) : nullptr;
    if (!kids)
        return nullptr;
    unsigned n = 0;
    last_owner = lx::config::action_owner::none;
    for (unsigned i = 0; i < all.size(); ++i) {
        const lx::config::action_info& a = all[i];
        if (a.owner != last_owner) {
            kids[n++] = group_header(a.owner == lx::config::action_owner::shell
                                         ? "Shell (its windows, launcher, notifications)"
                                         : "Compositor (windows, tiling, workspaces, session)");
            last_owner = a.owner;
        }
        // The label block: the summary over the action's name; the value: the keycaps and
        // the edit / reset affordances.
        lx::ui::child texts[2]{};
        unsigned tn = 0;
        texts[tn++] = st_text(a.summary, st_text_px(14.f), tok::text);
        lx::ui::child name_line =
            st_text(frame_format("{}{}", a.name, a.available ? "" : " (not in this build)"),
                    st_text_px(11.f), tok::text_muted, false, true);
        texts[tn++] = name_line;
        lx::ui::child label_block = st_box(
            st_v_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                .justify_center(),
            control_paint{}, {texts, tn});
        // The binding: the canonical combo as a keycap hint, or "unbound".
        char canon[64]{};
        const lx::config::combo bound = s->binding_of_action(i);
        const bool has = bound.kind != lx::config::combo_kind::tap || bound.mods || bound.code;
        if (has)
            lx::config::format_combo(bound, canon, sizeof(canon));
        // The canonical combo as muted mono-ish text (the keycap hint wants a span that
        // outlives the describe; the text says the same).
        lx::ui::child combo_text =
            st_text(has ? frame_format("{}", canon) : "unbound", st_text_px(12.f),
                    has ? tok::text : tok::text_muted, false, true);
        lx::ui::child combo_box = st_box(
            st_h_flex().flex_none().w(st_px16(150.f)).justify_end(), control_paint{},
            {&combo_text, 1u});
        lx::ui::child edit_btn = button("Edit").style(lx::ui::button_style::subtle)
                                     .on_press([s, i](int) { s->begin_edit_action(i); })
                                     .name(frame_format("Edit {}", a.summary));
        lx::ui::child value_kids[2] = {combo_box, edit_btn};
        lx::ui::child value = st_box(st_h_flex().flex_none().gap(st_px16(8.f)),
                                     control_paint{}, {value_kids, 2u});
        lx::ui::child row_kids[2] = {label_block, value};
        lx::ui::control_props row{};
        row.layout = st_h_flex().flex_none().w_full().min_h(st_px16(k_row_h))
                         .px(st_px16(12.f)).gap(st_px16(12.f)).rounded(lx::ui::radius::sm);
        lx::ui::control_paint p{};
        p.rest = {k_transparent, k_transparent};
        p.hover = {paint(tok::ghost_element_hover), k_transparent};
        row.paint = p;
        row.role = lx::a11y::role::group;
        row.name = a.summary;
        kids[n++] = control(row, {row_kids, 2u}, k_row_control_base + i);
    }

    // The editor for one action: a modal row (title, the combo field, the live error, the
    // buttons) at the top of the pane, under its own header.
    if (editing) {
        const unsigned idx = s->editing_action();
        const lx::config::action_info& a = all[idx];
        kids[n++] = group_header(frame_format("Edit: {}", a.summary));
        lx::ui::child field =
            text_input()
                .buffer(s->edit_buffer())
                .placeholder("Super+T — an empty field unbinds")
                .name(frame_format("Binding for {}", a.summary))
                .on_change([s](int) { s->refresh(); })
                .on_submit([s](int) { s->apply_edit_action(); })
                .layout(st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                            .h(st_px16(28.f)));
        lx::ui::child save = button("Save").style(lx::ui::button_style::filled)
                                 .on_press([s](int) { s->apply_edit_action(); })
                                 .name("Save binding");
        lx::ui::child cancel = button("Cancel").on_press([s](int) { s->cancel_edit_action(); });
        lx::ui::child field_box = st_box(
            st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f)),
            control_paint{}, {&field, 1u});
        lx::ui::child row_kids[3] = {field_box, save, cancel};
        kids[n++] = st_box(st_h_flex().flex_none().w_full().gap(st_px16(8.f)),
                           control_paint::fixed({k_transparent, k_transparent}), {row_kids, 3u});
        const std::string_view err = s->edit_error();
        lx::ui::child error_line = st_text(
            err.empty() ? frame_format("The binding writes [bindings] in config.toml; {}",
                                       s->binding_is_user(s->binding_of_action(idx)).empty()
                                           ? "the default is active"
                                           : "your line is active")
                        : frame_format("{}", err),
            st_text_px(12.f), err.empty() ? tok::text_muted : tok::error, false, true);
        kids[n++] = st_box(st_v_flex().flex_none().w_full(),
                           control_paint::fixed({k_transparent, k_transparent}),
                           {&error_line, 1u});
    }
    return st_box(st_v_flex().w_full(), control_paint{}, {kids, n});
}

/// The Displays pane (ADR-013 Phase F): one group per head — the live state, the staged
/// edits (mode, scale, transform, position, adaptive sync, enabled), an Apply that asks
/// the compositor (which tests, applies and persists outputs.toml itself).
[[nodiscard]] lx::ui::child displays_pane(session* s) {
    lx::ui::build_context* ctx = lx::ui::build_context::current();
    const std::vector<display_head>& heads = output_heads();
    if (!outputs_available()) {
        lx::ui::child none[2] = {
            headline("No output management").size(lx::ui::headline_size::medium),
            st_text("The compositor does not advertise zwlr_output_manager_v1 to this "
                    "client. Modes, scale and position stay where the session put them.",
                    st_text_px(12.f), tok::text_muted, false, true),
        };
        return st_box(st_v_flex().gap(st_px16(8.f)).p(st_px16(24.f)), control_paint{},
                      {none, 2u});
    }
    constexpr unsigned k_per_head = 10; // rows per head worst case
    const auto** kids = ctx ? ctx->alloc_children(heads.size() * (k_per_head + 1) + 2) : nullptr;
    if (!kids)
        return nullptr;
    unsigned n = 0;
    for (unsigned i = 0; i < heads.size(); ++i) {
        const display_head& d = heads[i];
        kids[n++] = group_header(
            frame_format("{} — {}{}", d.name, d.description, d.staged ? "  ●" : ""));
        // Enabled.
        lx::ui::child enabled = toggle_switch(d.staged ? d.staged_enabled : d.enabled)
                                    .on_toggle([i](int v) { stage_enabled(i, v != 0); })
                                    .name(frame_format("{} enabled", d.name));
        kids[n++] = st_box(st_h_flex().flex_none().w_full().px(st_px16(12.f)),
                           control_paint{}, {&enabled, 1u});
        if (!(d.staged ? d.staged_enabled : d.enabled))
            continue;
        // Mode (a select over the modes list).
        const auto& modes = d.modes;
        std::span<const char* const> words{};
        // The words point into the frame arena (built fresh per describe).
        auto* arena_words = reinterpret_cast<const char**>(
            ctx ? ctx->alloc_chars(sizeof(const char*) * std::max<unsigned>(modes.size(), 1))
                : nullptr);
        int current_mode = d.staged ? d.staged_mode : -1;
        if (arena_words) {
            for (unsigned m = 0; m < modes.size(); ++m) {
                arena_words[m] = frame_format("{} × {} @ {:.2f} Hz{}", modes[m].w, modes[m].h,
                                              static_cast<double>(modes[m].refresh_mhz) / 1000.0,
                                              modes[m].preferred ? "  (preferred)" : "");
                if (modes[m].current && !d.staged)
                    current_mode = static_cast<int>(m);
            }
            words = {arena_words, modes.size()};
        }
        lx::ui::child mode_ctl =
            select(words, current_mode)
                .open(s->open_menu() == 0x4000u + i)
                .on_open([i](int) { /* the pane re-opens through the session */ })
                .on_select([i](int v) { stage_mode(i, v); })
                .name(frame_format("{} mode", d.name));
        // The select's open state: reuse the session's row-open slot with a pane-specific
        // key beyond the catalog's range.
        lx::ui::child mode_row = st_box(
            st_h_flex().flex_none().w_full().px(st_px16(12.f)).gap(st_px16(12.f)),
            control_paint::fixed({k_transparent, k_transparent}),
            {&mode_ctl, 1u});
        kids[n++] = mode_row;
        // Scale.
        lx::ui::child scale = slider(static_cast<float>(d.staged ? d.staged_scale : d.scale),
                                     0.25f, 4.f)
                                  .steps(60)
                                  .on_change([i](int idx) {
                                      stage_scale(i, 0.25 + idx * (4.0 - 0.25) / 60.0);
                                  })
                                  .layout(st_h_flex().flex_grow(1.f).flex_shrink(1.f)
                                              .min_w(lx::layout::px(0.f)).h(st_px16(20.f)))
                                  .name(frame_format("{} scale", d.name));
        lx::ui::child scale_label = st_text(
            frame_format("scale {:.2f}", d.staged ? d.staged_scale : d.scale), st_text_px(12.f),
            tok::text_muted, false, true);
        lx::ui::child scale_kids[2] = {scale, scale_label};
        kids[n++] = st_box(st_h_flex().flex_none().w_full().px(st_px16(12.f)).gap(st_px16(8.f)),
                           control_paint{}, {scale_kids, 2u});
        // Transform.
        static const char* const k_transforms[8] = {"normal",    "90",         "180",
                                                    "270",       "flipped",    "flipped-90",
                                                    "flipped-180", "flipped-270"};
        lx::ui::child transform =
            segmented(std::span<const char* const>{k_transforms, 8},
                      [&] {
                          const std::string_view t = d.staged && !d.staged_transform.empty()
                                                         ? std::string_view(d.staged_transform)
                                                         : std::string_view(d.transform);
                          for (unsigned t2 = 0; t2 < 8; ++t2)
                              if (k_transforms[t2] == t)
                                  return static_cast<int>(t2);
                          return 0;
                      }())
                .on_select([i](int v) {
                    static const char* const k[8] = {"normal",  "90",       "180",
                                                     "270",    "flipped",  "flipped-90",
                                                     "flipped-180", "flipped-270"};
                    stage_transform(i, k[v]);
                })
                .name(frame_format("{} transform", d.name));
        kids[n++] = st_box(st_h_flex().flex_none().w_full().px(st_px16(12.f)),
                           control_paint{}, {&transform, 1u});
        // Position + adaptive sync + Apply.
        lx::ui::child pos_label = st_text(frame_format("at {}, {}", d.staged_x, d.staged_y),
                                          st_text_px(12.f), tok::text_muted, false, true);
        lx::ui::child adaptive = toggle_switch(d.staged ? d.staged_adaptive : d.adaptive_sync)
                                     .label("VRR")
                                     .on_toggle([i](int v) { stage_adaptive(i, v != 0); })
                                     .name(frame_format("{} adaptive sync", d.name));
        lx::ui::child tail_kids[2] = {pos_label, adaptive};
        kids[n++] = st_box(st_h_flex().flex_none().w_full().px(st_px16(12.f)).gap(st_px16(16.f)),
                           control_paint{}, {tail_kids, 2u});
        if (d.staged) {
            lx::ui::child apply = button("Apply").style(lx::ui::button_style::filled)
                                      .on_press([i](int) { apply_head(i); })
                                      .name(frame_format("Apply {}", d.name));
            lx::ui::child discard =
                button("Discard").on_press([i](int) { discard_staged(i); });
            lx::ui::child btns[2] = {apply, discard};
            kids[n++] = st_box(
                st_h_flex().flex_none().w_full().px(st_px16(12.f)).gap(st_px16(8.f)),
                control_paint{}, {btns, 2u});
        }
    }
    if (!outputs_note().empty()) {
        lx::ui::child note_text = st_text(frame_format("{}", outputs_note()), st_text_px(12.f),
                                          tok::text_muted, false, true);
        kids[n++] = st_box(st_v_flex().flex_none().w_full().pt(st_px16(8.f)),
                           control_paint::fixed({k_transparent, k_transparent}),
                           {&note_text, 1u});
    }
    return st_box(st_v_flex().w_full(), control_paint{}, {kids, n});
}

/// The content column: the pane's rows grouped by their group header, or the search
/// results across all panes.
[[nodiscard]] lx::ui::child pane_content(session* s) {
    const std::vector<row_ref>& rows = catalog();
    const std::string_view query = [&] {
        const char* q = s->query_buffer().storage.data();
        return std::string_view(q ? q : "", s->query_buffer().size);
    }();
    const bool searching = !query.empty();

    // Count first (fixed arrays come from the frame arena).
    unsigned count = 0;
    unsigned groups = 0;
    const char* last_group = nullptr;
    for (unsigned i = 0; i < rows.size(); ++i) {
        if (searching ? !row_matches(rows[i], query) : rows[i].owner != s->current())
            continue;
        ++count;
        const char* g = searching ? k_pane_names[static_cast<unsigned>(rows[i].owner)].data()
                                  : rows[i].group;
        if (g != last_group) {
            ++groups;
            last_group = g;
        }
    }
    if (!searching && s->current() == pane_id::shortcuts)
        return shortcuts_pane(s);
    if (!searching && s->current() == pane_id::displays)
        return displays_pane(s);
    const bool import_row = !searching && s->current() == pane_id::appearance;
    lx::ui::build_context* ctx = lx::ui::build_context::current();
    const auto** kids =
        ctx ? ctx->alloc_children(count + groups + 1 + (import_row ? 3 : 0)) : nullptr;
    if (!kids)
        return nullptr;

    unsigned n = 0;
    last_group = nullptr;
    pane_id last_pane = pane_id::appearance;
    bool first_pane_row = true;
    for (unsigned i = 0; i < rows.size(); ++i) {
        if (searching ? !row_matches(rows[i], query) : rows[i].owner != s->current())
            continue;
        if (searching && (rows[i].owner != last_pane || first_pane_row)) {
            kids[n++] = group_header(
                frame_format("{}", k_pane_names[static_cast<unsigned>(rows[i].owner)]));
            first_pane_row = false;
            last_pane = rows[i].owner;
        } else if (!searching && rows[i].group != last_group) {
            kids[n++] = group_header(rows[i].group);
            last_group = rows[i].group;
        }
        kids[n++] = setting_row(rows[i], i, s);
    }
    // The Input pane's live device list (the compositor knows libinput's names; the
    // per-device overrides below match them exactly).
    if (!searching && s->current() == pane_id::input && !s->health().devices_line.empty()) {
        kids[n++] = group_header("Devices");
        lx::ui::child devices_text = st_text(frame_format("{}", s->health().devices_line),
                                             st_text_px(12.f), tok::text_muted, false, true);
        kids[n++] = st_box(st_v_flex().flex_none().w_full().pt(st_px16(4.f)),
                           control_paint::fixed({k_transparent, k_transparent}),
                           {&devices_text, 1u});
    }
    // The Session pane's live reads (ADR-013 Phase G): the compositor's version and
    // health, polled over the control socket, with the state it came from.
    const bool session_card = !searching && s->current() == pane_id::session;
    if (session_card) {
        const health_snapshot& h = s->health();
        kids[n++] = group_header("Compositor");
        std::string lines;
        if (h.ok) {
            lines = h.version_line + "\n" + h.level_line;
            if (!h.parts_line.empty())
                lines += "\n" + h.parts_line;
            if (!h.memory_line.empty())
                lines += "\n" + h.memory_line;
            if (!h.config_line.empty())
                lines += "\n" + h.config_line;
        } else if (h.attempted) {
            lines = "Compositor unreachable: " + h.note;
        } else {
            lines = "Polling the compositor…";
        }
        lx::ui::child card = st_text(frame_format("{}", lines), st_text_px(12.f),
                                     h.ok ? tok::text : tok::text_muted, false, true);
        kids[n++] = st_box(st_v_flex().flex_none().w_full().gap(st_px16(2.f))
                               .pt(st_px16(4.f)),
                           control_paint::fixed({k_transparent, k_transparent}),
                           {&card, 1u});
    }
    // The Appearance pane's Zed-theme import (ADR-013 §4.7): the path, the button, the
    // status — the whole app is the preview of the theme it installs.
    if (import_row) {
        kids[n++] = group_header("Zed theme import");
        lx::ui::child field =
            text_input()
                .buffer(s->import_buffer())
                .placeholder("/path/to/theme.json")
                .name("Zed theme JSON path")
                .on_change([s](int) { s->refresh(); })
                .layout(st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                            .h(st_px16(28.f)));
        lx::ui::child field_box =
            st_box(st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f)),
                   control_paint{}, {&field, 1u});
        lx::ui::child import_btn = button("Import")
                                       .style(lx::ui::button_style::filled)
                                       .on_press([s](int) {
                                           const char* p =
                                               s->import_buffer().storage.data();
                                           s->import_theme(p ? std::string_view(p)
                                                               : std::string_view{});
                                       })
                                       .name("Import theme");
        lx::ui::child row_kids[2] = {field_box, import_btn};
        kids[n++] = st_box(st_h_flex().flex_none().w_full().gap(st_px16(8.f)),
                           control_paint::fixed({k_transparent, k_transparent}), {row_kids, 2u});
        lx::ui::child status_text =
            st_text(frame_format("{}", s->import_status()), st_text_px(12.f), tok::text_muted,
                    false, true);
        kids[n++] = st_box(st_v_flex().flex_none().w_full(),
                           control_paint::fixed({k_transparent, k_transparent}),
                           {&status_text, 1u});
    }
    const lx::ui::child note = searching ? nullptr : pane_note(s->current());
    if (note)
        kids[n++] = note;
    if (n == 0) {
        // A pane with nothing of its own yet (Shortcuts in this build).
        lx::ui::child empty[2] = {
            headline("Nothing here yet").size(lx::ui::headline_size::medium),
            st_text("This pane's editor arrives with a later phase of ADR-013.",
                    st_text_px(12.f), tok::text_muted, false, true),
        };
        return st_box(st_v_flex().gap(st_px16(8.f)).p(st_px16(24.f)), control_paint{},
                      {empty, 2u});
    }
    return st_box(st_v_flex().w_full(), control_paint{}, {kids, n});
}

// ── the window shell ───────────────────────────────────────────────────────────────────

[[nodiscard]] lx::ui::child rail(session* s) {
    lx::ui::build_context* ctx = lx::ui::build_context::current();
    const auto** rows = ctx ? ctx->alloc_children(k_pane_count) : nullptr;
    if (!rows)
        return nullptr;
    for (unsigned i = 0; i < k_pane_count; ++i) {
        const pane_id p = static_cast<pane_id>(i);
        const bool active = s->current() == p;
        lx::ui::child glyph = icon_view(k_pane_icons[i], st_text_px(14.f),
                                        active ? tok::icon_accent : tok::icon_muted);
        lx::ui::child text = st_text(k_pane_names[i].data(), st_text_px(13.f),
                                     active ? tok::text_accent : tok::text);
        lx::ui::child pair[2] = {glyph, text};
        lx::ui::control_props row{};
        row.layout = st_h_flex().flex_none().w_full().h(st_px16(32.f)).px(st_px16(12.f))
                         .gap(st_px16(8.f)).rounded(lx::ui::radius::sm);
        lx::ui::control_paint paint_{};
        if (active) {
            paint_.rest = {paint(tok::ghost_element_selected), k_transparent};
            paint_.hover = paint_.active = paint_.rest;
        } else {
            paint_.rest = {paint(tok::ghost_element_background), k_transparent};
            paint_.hover = {paint(tok::ghost_element_hover), k_transparent};
            paint_.active = {paint(tok::ghost_element_active), k_transparent};
        }
        row.paint = paint_;
        row.on_activate = [s, p](int) { s->set_current(p); };
        row.role = lx::a11y::role::button;
        row.name = k_pane_names[i].data();
        row.a11y_state = active ? st_a11y(lx::a11y::state_flag::selected) : 0u;
        row.interactive = true;
        row.focusable = true;
        row.selected = active;
        row.focus_ring = true;
        rows[i] = control(row, {pair, 2u});
    }
    lx::ui::child header = st_text("Lumen", st_text_px(11.f), tok::text_muted, true, true);
    lx::ui::child kids[2] = {
        st_box(st_v_flex().flex_none().w_full().px(st_px16(16.f)).pt(st_px16(16.f))
                   .pb(st_px16(4.f)),
               control_paint{}, {&header, 1u}),
        st_box(st_v_flex().flex_none().w_full().gap(st_px16(2.f)).px(st_px16(8.f)),
               control_paint{}, {rows, k_pane_count}),
    };
    lx::ui::control_props rail_props{};
    rail_props.layout = st_v_flex().flex_none().w(st_px16(k_rail_w)).h_full()
                            .bg(tok::surface_background)
                            .border_r_1(tok::border_variant);
    rail_props.paint = control_paint::fixed({paint(tok::surface_background), k_transparent});
    rail_props.role = lx::a11y::role::list;
    rail_props.name = "Panes";
    return control(rail_props, {kids, 2u}, k_rail_key);
}

[[nodiscard]] lx::ui::child search_row(session* s) {
    lx::ui::child glyph = icon_view(lx::ui::icon::search, st_text_px(14.f), tok::icon_muted);
    lx::ui::child field =
        text_input()
            .buffer(s->query_buffer())
            .placeholder("Search settings")
            .name("Search settings")
            .on_change([s](int) { s->refresh(); })
            .layout(st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f))
                        .h(st_px16(28.f)))
            .key(k_search_key);
    lx::ui::child kids[2] = {glyph, field};
    lx::ui::control_props bar{};
    bar.layout = st_h_flex().flex_none().w_full().h(st_px16(44.f)).px(st_px16(16.f))
                     .gap(st_px16(8.f));
    lx::ui::control_paint paint_ = control_paint::fixed(
        {paint(tok::background), k_transparent});
    paint_.border_widths[2] = 1; // a rule under the search row
    paint_.rest.border = paint(tok::border_variant);
    paint_.hover = paint_.active = paint_.rest;
    bar.paint = paint_;
    bar.role = lx::a11y::role::toolbar;
    bar.name = "Search";
    return control(bar, {kids, 2u});
}

[[nodiscard]] lx::ui::child status_line(session* s) {
    lx::ui::child left_kids[1] = {st_text("Lumen Settings", st_text_px(11.f), tok::text_muted,
                                          true, true)};
    const char* right_text =
        s->state() == session::save_state::written
            ? "saved to config.toml — Lumen applies it as it watches the file"
        : s->state() == session::save_state::memory_only
            ? "in memory only (no writable config.toml: LUMEN_CONFIG or a bad file)"
            : "the configuration as Lumen runs it";
    lx::ui::child right_kids[1] = {st_text(right_text, st_text_px(11.f), tok::text_muted, false,
                                           true)};
    return status_bar({left_kids, 1u}, {right_kids, 1u}).key(k_status_key);
}

} // namespace

namespace settings {

lx::ui::child describe(lx::ui::build_context&) {
    static session g_fallback; // tests without a session of their own
    session* s = g_session ? g_session : &g_fallback;
    (void)catalog();

    lx::ui::child search = search_row(s);
    lx::ui::child rail_child = rail(s);
    lx::ui::child content = pane_content(s);
    lx::ui::child body_kids[2] = {
        rail_child,
        scroll_area(st_v_flex().flex_grow(1.f).flex_shrink(1.f).min_w(lx::layout::px(0.f)),
                    {&content, 1u}, lx::ui::scroll_axes::vertical,
                    st_v_flex().w_full().p(st_px16(20.f)).gap(st_px16(0.f))),
    };
    lx::ui::child body = st_box(st_h_flex().flex_grow(1.f).flex_shrink(1.f).min_h(lx::layout::px(0.f)),
                                control_paint{}, {body_kids, 2u});
    lx::ui::child shell[3] = {search, body, status_line(s)};
    lx::ui::child root[1] = {st_box(st_v_flex().size_full().bg(tok::background),
                                    control_paint{}, {shell, 3u}, k_content_key)};
    return div(st_v_flex().size_full().bg(tok::background), {root, 1u});
}

} // namespace settings

