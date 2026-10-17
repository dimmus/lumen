module;

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

import lx.foundation;
import lx.gfx;
import lx.drm;

export module lx.compositor:output;

export namespace lx::compositor {

enum class present_mode {
    auto_select,  ///< discrete → mailbox, integrated → fifo_relaxed
    mailbox,      ///< low latency, may drop frames (discrete)
    fifo,         ///< vsync locked, no tear (integrated)
    fifo_relaxed, ///< adaptive fifo when supported
};

/// The nearest scale a fractional-scale client can be told exactly: n/120
/// (wp_fractional_scale_v1.preferred_scale is in 120ths), never below 1/120 — niri's
/// `closest_representable_scale`. A non-positive or non-finite request is 1.
[[nodiscard]] double representable_scale(double requested);

/// sway's `compute_default_scale`: 2 for a HiDPI panel — at least 1200 rows, and more than
/// 192 DPI on both axes from a physical size that is not an aspect-ratio placeholder (a
/// size of exactly 16:9 or 16:10 cm, or under 1 cm, is what EDIDs of projectors and TVs
/// carry) — else 1. Fractional defaults are not guessed (ADR 008 §G).
[[nodiscard]] double compute_default_scale(int width_px, int height_px, int phys_w_mm,
                                           int phys_h_mm);

struct output_state {
    lx::output_id id{};
    /// Top-left in the layout (logical coordinates): where this output's logical rect sits
    /// among the others.
    lx::point2i position{};
    /// The current mode, in pixels.
    lx::size2i mode{};
    /// Output scale, n/120 (`representable_scale`). Logical size = mode / scale.
    double scale = 1.0;
    present_mode present = present_mode::auto_select;
    bool supports_kms_damage = false;
    /// wl_output.name / zxdg_output_v1.name: the connector name ("Virtual-1", "DP-2"), or
    /// "HEADLESS-n" for a virtual output. Never changes for a slot (wl_output v4).
    char name[32]{};
    /// wlroots' format: "make model serial (connector)", or the connector alone without EDID.
    char description[160]{};
    /// From EDID; empty when absent.
    char make[64]{};
    char model[64]{};
    char serial[64]{};
    /// The persistent key of the monitor (ADR 008 M1): "make model serial" with "Unknown"
    /// for missing parts (sway's `output_get_identifier`, what kanshi matches), or
    /// "connector:<name>" without EDID. Two identical monitors get their connector appended.
    char identifier[208]{};
    /// Refresh of the current mode, mHz (wl_output.mode).
    unsigned refresh_mhz = 0;
    /// wl_output.transform value; always normal until output transforms exist.
    unsigned transform = 0;
    /// Physical size in mm; 0 when unknown.
    int phys_w_mm = 0;
    int phys_h_mm = 0;
    /// wl_output.subpixel.
    unsigned subpixel = 0;
    /// A virtual output (HEADLESS-n, `--virtual-outputs`), not a connector.
    bool virtual_output = false;
    /// Placed by the automatic layout (M2); an explicit position clears it.
    bool auto_position = true;
    /// The mode follows the connector's preferred mode (H3); an explicit mode clears it.
    bool follow_preferred = true;
    /// The host decides modes and layout (`hotplug_mode_update`, virtual GPUs): its
    /// `suggested_x/y` are used as the position unless they overlap (Mutter).
    bool hotplug_mode_update = false;
    long suggested_x = -1;
    long suggested_y = -1;
    /// KMS: the connector, its possible CRTCs, the CRTC it was routed to when probed, and
    /// the mode in use (what a modeset programs). Zero for a virtual output.
    unsigned connector_id = 0;
    unsigned possible_crtcs = 0;
    unsigned routed_crtc_id = 0;
    lx::drm::kms_mode kms_mode{};
    /// From the newest probe of the connector (ADR 008 §B.2): its preferred mode, a hash of
    /// its whole mode list, and `link-status` Bad. What a new probe is compared against.
    lx::drm::kms_mode preferred_mode{};
    unsigned modes_hash = 0;
    bool link_bad = false;
    /// The identity came from an EDID (else the connector name stands for the monitor).
    bool edid = false;
    /// Insertion order: the automatic layout keeps outputs in it.
    unsigned order = 0;
    /// Adaptive sync (VRR) is on, and the connector can do it (`vrr_capable`).
    bool adaptive_sync = false;
    bool vrr_capable = false;
    /// DRM_MODE_CONNECTOR_* (custom modes: analog connectors get standard CVT blanking).
    unsigned connector_type = 0;
    /// The mode is a custom one (CVT timings the connector does not list): a new probe of
    /// the connector keeps it instead of falling back to the preferred mode.
    bool custom_mode = false;

    /// The output in the layout: `position`, and the mode divided by the scale, rounded
    /// half away from zero (wlroots' `wlr_output_effective_resolution`). A fractional
    /// product leaves the last row and column partly outside, which the scanout clips.
    [[nodiscard]] lx::rect2i logical_rect() const;
    /// The output's own pixels: `{0, 0, mode}`.
    [[nodiscard]] lx::rect2i mode_rect() const { return {0, 0, mode.width, mode.height}; }
};

/// What a new output is made of (`output_manager::add_output`).
struct output_desc {
    const char* name = "";
    lx::size2i mode{};
    unsigned refresh_mhz = 60000;
    /// 0: the default scale (`set_default_scale`, a per-connector override, or
    /// `compute_default_scale`).
    double scale = 0.0;
    /// Explicit layout position; with `auto_position` the automatic layout places it.
    lx::point2i position{};
    bool auto_position = true;
    bool virtual_output = false;
    int phys_w_mm = 0;
    int phys_h_mm = 0;
    unsigned subpixel = 0;
    const char* make = "";
    const char* model = "";
    const char* serial = "";
    bool edid = false;
    bool hotplug_mode_update = false;
    long suggested_x = -1;
    long suggested_y = -1;
    unsigned connector_id = 0;
    unsigned possible_crtcs = 0;
    unsigned routed_crtc_id = 0;
    lx::drm::kms_mode kms_mode{};
    lx::drm::kms_mode preferred_mode{};
    unsigned modes_hash = 0;
    bool link_bad = false;
    /// wl_output.transform (0..7).
    unsigned transform = 0;
    bool adaptive_sync = false;
    bool vrr_capable = false;
    unsigned connector_type = 0;
    /// Follow the connector's preferred mode (H3); false for a mode chosen explicitly.
    bool follow_preferred = true;
    bool custom_mode = false;
};

/// The persistent key and the wl_output description of a monitor (ADR 008 M1): "make model
/// serial" (sway's `output_get_identifier`, "Unknown" for missing parts) and wlroots'
/// "make model serial (connector)" from an EDID; "connector:<name>" and the name without.
/// (Two identical monitors are told apart by `output_manager`, which appends the connector.)
void monitor_identity(const char* name, const char* make, const char* model, const char* serial,
                      bool edid, char* identifier, unsigned identifier_size, char* description,
                      unsigned description_size);

/// A hash of a probe's mode list (size, refresh, flags, the preferred bit, in order): two
/// probes with the same hash offer the same modes.
[[nodiscard]] unsigned mode_list_hash(const lx::drm::connector_probe& probe);
/// What an output made from connector `probe` in mode `mode` is: name, EDID identity, mode,
/// physical size, the host's suggested position, routing.
[[nodiscard]] output_desc desc_from_probe(const lx::drm::connector_probe& probe,
                                          const lx::drm::kms_mode& mode);
/// A connector that makes an output: connected, not `non-desktop`, with a mode.
[[nodiscard]] bool desktop_connector(const lx::drm::connector_probe& probe);

/// The outputs, in stable slots: an output keeps its slot for as long as its connector stays
/// connected, because surfaces remember which outputs they are on as a bit mask per slot
/// (wl_surface.enter/leave). Output ids are a per-session counter and never reused, so a
/// reused slot never matches a stale reference (ADR 008 M1). UI affinity.
///
/// Without a connected connector (headless, nested without KMS, every display unplugged)
/// there is one virtual output, HEADLESS-1, 1920x1080 at 60 Hz — the nominal size the
/// window manager places against, visible to clients too.
class output_manager {
public:
    static constexpr unsigned k_max = 16;

    [[nodiscard]] unsigned count() const;
    /// The `index`-th output in slot order.
    [[nodiscard]] lx::output_id nth(unsigned index) const;
    [[nodiscard]] output_state state(lx::output_id output) const;

    /// Layout (logical) rect of `output`: what the window manager, input and the scene
    /// place against. Empty for an unknown output.
    [[nodiscard]] lx::rect2i logical_rect(lx::output_id output) const;
    /// `{0, 0, mode}` of `output`: what scanout, KMS damage and the cursor plane use.
    [[nodiscard]] lx::rect2i mode_rect(lx::output_id output) const;
    [[nodiscard]] double scale(lx::output_id output) const;
    [[nodiscard]] present_mode present_mode_for(lx::output_id output) const;

    void set_present_mode(lx::output_id output, present_mode mode);
    /// Rounded through `representable_scale`. Moves `generation` on a change, and re-runs
    /// the automatic layout (a logical width changed).
    void set_scale(lx::output_id output, double scale);
    /// The scale outputs get when they first appear (`--scale`). Rounded through
    /// `representable_scale`; 0 (the default) means sway's HiDPI rule
    /// (`compute_default_scale`).
    void set_default_scale(double scale) {
        default_scale_ = scale > 0.0 ? representable_scale(scale) : 0.0;
    }
    /// The configured default; 1 when automatic.
    [[nodiscard]] double default_scale() const { return default_scale_ > 0.0 ? default_scale_ : 1.0; }
    /// A scale for one connector by name (`--output-scale NAME=S`), over the default. At most
    /// `k_max` names.
    void set_scale_override(const char* name, double scale);
    /// wl_output transform of `output` (0..7): the logical size turns with it; the layout
    /// runs again.
    void set_transform(lx::output_id output, unsigned transform);
    /// An explicit layout position: the output leaves the automatic layout.
    void set_position(lx::output_id output, lx::point2i position);
    /// The output goes back to the automatic layout (M2).
    void set_auto_position(lx::output_id output);
    /// Adaptive sync on or off (what the next modeset states). Moves `generation`.
    void set_adaptive_sync(unsigned slot, bool on);
    /// Whether the output follows its connector's preferred mode (H3).
    void set_follow_preferred(unsigned slot, bool follow);
    /// The scale an output of this connector gets when nothing chose one: a per-connector
    /// override, `--scale`, else sway's HiDPI rule.
    [[nodiscard]] double default_scale_for(const char* name, int width, int height, int phys_w_mm,
                                           int phys_h_mm) const {
        return scale_for(name, width, height, phys_w_mm, phys_h_mm);
    }

    /// Adds every connected desktop connector of `scan` that has a mode (connector order;
    /// preferred mode, default scale, automatic or suggested position), or HEADLESS-1 when
    /// there is none. The start-up scan (hotplug diffs scans in Part B).
    void configure_from_scan(const lx::drm::connector_scan& scan);
    /// Adds one output in the lowest free slot (or in `slot`, which must be free) and lays
    /// the automatic outputs out again. Returns the slot, -1 when it cannot.
    int add_output(const output_desc& desc, int slot = -1);
    /// The lowest slot that is neither live nor in `reserved`, -1 when none is.
    [[nodiscard]] int free_slot(std::uint32_t reserved) const;
    /// The output in `slot` changes mode (a hotplug's preferred mode, a link retrain): its
    /// logical size follows, the automatic layout runs again. `follow_preferred` says
    /// whether it keeps following the connector's preferred mode (H3).
    void set_mode(unsigned slot, const lx::drm::kms_mode& mode, bool follow_preferred,
                  bool custom = false);
    /// A new probe of the output's connector that changes no mode: the preferred mode, the
    /// mode list hash, link status, physical size and the host's suggested position are
    /// taken (a suggested position moves an automatic output).
    void update_head(unsigned slot, const lx::drm::connector_probe& probe);

    // ── slots ──

    /// The slot of `output`, -1 when unknown.
    [[nodiscard]] int slot_of(lx::output_id output) const;
    [[nodiscard]] bool slot_live(unsigned slot) const { return slot < k_max && live_[slot]; }
    /// The state in `slot` (meaningful only while `slot_live`).
    [[nodiscard]] const output_state& slot_state(unsigned slot) const {
        return outputs_[slot < k_max ? slot : 0];
    }
    /// Bit `slot` set for every live output.
    [[nodiscard]] std::uint32_t live_mask() const;
    /// Bumped whenever anything a client sees of any output changes (mode, position,
    /// scale, an output appearing or going).
    [[nodiscard]] unsigned generation() const { return generation_; }
    /// Bumped when the layout changes: an output added or removed, a position, scale or mode
    /// changed. What the window manager, the scene views and input re-derive from.
    [[nodiscard]] unsigned layout_generation() const { return layout_generation_; }

    /// Puts an output that is not a connector in the first free slot (the headless one;
    /// tests): `layout.x/y` is its logical position, `layout.width/height` its mode in
    /// pixels. `scale` 0 means the default scale. Returns the slot, -1 when every slot is
    /// taken.
    int add_virtual(const char* name, lx::rect2i layout, unsigned refresh_mhz,
                    double scale = 0.0);
    void remove_slot(unsigned slot);

    /// Re-runs the automatic layout (M2): explicitly placed outputs stay; a connector with a
    /// host-suggested position (`hotplug_mode_update`) takes it unless it overlaps one placed
    /// already; every other automatic output goes to the right of the rightmost one,
    /// top-aligned, in insertion order — so a removed output's gap closes.
    void arrange();

private:
    void recount();
    [[nodiscard]] double scale_for(const char* name, int w, int h, int phys_w, int phys_h) const;
    void make_identity(output_state& o, bool edid);

    output_state outputs_[k_max]{};
    bool live_[k_max]{};
    unsigned count_ = 0;
    unsigned generation_ = 0;
    unsigned layout_generation_ = 0;
    unsigned next_id_ = 1;
    unsigned next_order_ = 1;
    unsigned next_virtual_ = 1;
    double default_scale_ = 0.0;
    struct scale_override {
        char name[32]{};
        double scale = 0.0;
    };
    scale_override overrides_[k_max]{};
    unsigned override_count_ = 0;
};

[[nodiscard]] present_mode resolve_present_mode(present_mode requested,
                                              lx::gfx::gpu_tier tier);

// ── Output membership and layout (pure; exported for tests) ────────────────

/// The outputs `r` (global) overlaps, one bit per slot. Rects that only touch an output's
/// edge are not on it; an empty rect is on none.
[[nodiscard]] std::uint32_t outputs_for_rect(const output_manager& outputs, lx::rect2i r);
/// The slot `r` overlaps most (wlroots' "primary output"), -1 for none. Ties go to the
/// lower slot.
[[nodiscard]] int primary_output_for_rect(const output_manager& outputs, lx::rect2i r);
/// The pace output of a surface (ADR 008 M8, wlroots' scene `primary_output`): among the
/// outputs in `shown_mask` (where its newest frame showed it), the one its `rect` overlaps
/// most; one with 10 % of the rect's area or less does not count unless it is the only one.
/// Ties go to the lower slot. -1 when `shown_mask` holds no live output.
[[nodiscard]] int pace_output(const output_manager& outputs, std::uint32_t shown_mask,
                              lx::rect2i rect);
/// wl_surface.preferred_buffer_scale for a surface on the outputs in `mask`: the ceiling of
/// the largest scale among them (wlroots, KWin), so the content is sharp on every output it
/// shows on; 1 when it is on none.
[[nodiscard]] int preferred_buffer_scale(const output_manager& outputs, std::uint32_t mask);
/// wp_fractional_scale_v1.preferred_scale for a surface on the outputs in `mask`: the
/// largest scale among them in 120ths (wlroots' scene uses the maximum too); 120 when it is
/// on none.
[[nodiscard]] unsigned preferred_scale_120(const output_manager& outputs, std::uint32_t mask);
/// wl_surface.preferred_buffer_transform: the primary output's transform, normal (0) when
/// there is none.
[[nodiscard]] unsigned preferred_buffer_transform(const output_manager& outputs, int primary);
/// The bounding box of every live output's logical rect (empty without outputs): what an
/// absolute pointer device spans (M9).
[[nodiscard]] lx::rect2i layout_bounds(const output_manager& outputs);
/// The output whose logical rect holds the point, -1 for none (a gap, or off the layout).
[[nodiscard]] int output_at(const output_manager& outputs, double x, double y);
/// Where the pointer may be (M9, wlroots' `wlr_output_layout_closest_point`): the point
/// itself when it is on an output, else the nearest point of the nearest output, where an
/// output spans [x, x + w − 1] × [y, y + h − 1] as the single-output clamp did. False when
/// there is no output (the point is left as it is).
bool closest_point(const output_manager& outputs, double& x, double& y);

} // namespace lx::compositor

namespace {

/// What a client can see of an output (present mode aside).
[[nodiscard]] bool same_visible(const lx::compositor::output_state& a,
                                const lx::compositor::output_state& b) {
    return a.id == b.id && a.position.x == b.position.x && a.position.y == b.position.y &&
           a.mode.width == b.mode.width && a.mode.height == b.mode.height &&
           a.scale == b.scale && a.refresh_mhz == b.refresh_mhz &&
           a.transform == b.transform && a.phys_w_mm == b.phys_w_mm &&
           a.phys_h_mm == b.phys_h_mm && std::strcmp(a.name, b.name) == 0 &&
           std::strcmp(a.description, b.description) == 0;
}

[[nodiscard]] long long overlap_area(lx::rect2i a, lx::rect2i b) {
    const long long x0 = a.x > b.x ? a.x : b.x;
    const long long y0 = a.y > b.y ? a.y : b.y;
    const long long x1 = static_cast<long long>(a.x) + a.width < static_cast<long long>(b.x) + b.width
                             ? static_cast<long long>(a.x) + a.width
                             : static_cast<long long>(b.x) + b.width;
    const long long y1 = static_cast<long long>(a.y) + a.height < static_cast<long long>(b.y) + b.height
                             ? static_cast<long long>(a.y) + a.height
                             : static_cast<long long>(b.y) + b.height;
    return x1 > x0 && y1 > y0 ? (x1 - x0) * (y1 - y0) : 0;
}

void copy_str(char* dst, unsigned cap, const char* src) {
    std::snprintf(dst, cap, "%s", src ? src : "");
}

} // namespace

void lx::compositor::output_manager::recount() {
    count_ = 0;
    for (bool l : live_)
        count_ += l ? 1u : 0u;
}

unsigned lx::compositor::output_manager::count() const { return count_; }

lx::output_id lx::compositor::output_manager::nth(unsigned index) const {
    for (unsigned s = 0; s < k_max; ++s) {
        if (!live_[s])
            continue;
        if (index == 0)
            return outputs_[s].id;
        --index;
    }
    return lx::output_id{};
}

int lx::compositor::output_manager::slot_of(lx::output_id output) const {
    if (!output)
        return -1;
    for (unsigned s = 0; s < k_max; ++s)
        if (live_[s] && outputs_[s].id == output)
            return static_cast<int>(s);
    return -1;
}

lx::compositor::output_state lx::compositor::output_manager::state(lx::output_id output) const {
    const int s = slot_of(output);
    return s >= 0 ? outputs_[s] : output_state{};
}

double lx::compositor::representable_scale(double requested) {
    if (!(requested > 0.0) || requested > 1.0e6)
        return 1.0;
    const double n = std::round(requested * 120.0);
    return (n < 1.0 ? 1.0 : n) / 120.0;
}

double lx::compositor::compute_default_scale(int width_px, int height_px, int phys_w_mm,
                                             int phys_h_mm) {
    (void)width_px;
    // A physical size that is an aspect ratio in disguise (EDIDs of projectors and TVs say
    // 16x9 or 16x10 cm), or none at all, says nothing about density.
    if (phys_w_mm <= 10 || phys_h_mm <= 10)
        return 1.0;
    if ((phys_w_mm == 160 && phys_h_mm == 90) || (phys_w_mm == 160 && phys_h_mm == 100) ||
        (phys_w_mm == 1600 && phys_h_mm == 900) || (phys_w_mm == 1600 && phys_h_mm == 1000))
        return 1.0;
    if (height_px < 1200)
        return 1.0;
    constexpr double k_mm_per_inch = 25.4;
    const double dpi_x = width_px / (phys_w_mm / k_mm_per_inch);
    const double dpi_y = height_px / (phys_h_mm / k_mm_per_inch);
    return dpi_x > 192.0 && dpi_y > 192.0 ? 2.0 : 1.0;
}

lx::rect2i lx::compositor::output_state::logical_rect() const {
    const double s = scale > 0.0 ? scale : 1.0;
    // A 90°/270° transform turns the output: its logical width is the mode's height.
    const bool turned = (transform & 1u) != 0;
    const int w = turned ? mode.height : mode.width;
    const int h = turned ? mode.width : mode.height;
    return {position.x, position.y, static_cast<int>(std::lround(static_cast<double>(w) / s)),
            static_cast<int>(std::lround(static_cast<double>(h) / s))};
}

lx::rect2i lx::compositor::output_manager::logical_rect(lx::output_id output) const {
    const int s = slot_of(output);
    return s >= 0 ? outputs_[s].logical_rect() : lx::rect2i{};
}

lx::rect2i lx::compositor::output_manager::mode_rect(lx::output_id output) const {
    const int s = slot_of(output);
    return s >= 0 ? outputs_[s].mode_rect() : lx::rect2i{};
}

double lx::compositor::output_manager::scale(lx::output_id output) const {
    const int s = slot_of(output);
    return s >= 0 ? outputs_[s].scale : 1.0;
}

std::uint32_t lx::compositor::output_manager::live_mask() const {
    std::uint32_t mask = 0;
    for (unsigned s = 0; s < k_max; ++s)
        if (live_[s])
            mask |= 1u << s;
    return mask;
}

lx::compositor::present_mode
lx::compositor::output_manager::present_mode_for(lx::output_id output) const {
    const auto st = state(output);
    if (st.present != present_mode::auto_select) return st.present;
    return lx::compositor::resolve_present_mode(present_mode::auto_select,
                                                lx::gfx::device_selector::probe_tier());
}

void lx::compositor::output_manager::set_present_mode(lx::output_id output, present_mode mode) {
    const int s = slot_of(output);
    if (s >= 0)
        outputs_[s].present = mode;
}

void lx::compositor::output_manager::set_scale(lx::output_id output, double requested) {
    const int s = slot_of(output);
    const double scale = representable_scale(requested);
    if (s < 0 || outputs_[s].scale == scale)
        return;
    outputs_[s].scale = scale;
    ++generation_;
    ++layout_generation_;
    arrange();
}

void lx::compositor::output_manager::set_scale_override(const char* name, double scale) {
    if (!name || !*name || !(scale > 0.0))
        return;
    for (unsigned i = 0; i < override_count_; ++i) {
        if (std::strcmp(overrides_[i].name, name) == 0) {
            overrides_[i].scale = representable_scale(scale);
            return;
        }
    }
    if (override_count_ >= k_max)
        return;
    copy_str(overrides_[override_count_].name, sizeof(overrides_[0].name), name);
    overrides_[override_count_].scale = representable_scale(scale);
    ++override_count_;
}

double lx::compositor::output_manager::scale_for(const char* name, int w, int h, int phys_w,
                                                 int phys_h) const {
    for (unsigned i = 0; i < override_count_; ++i)
        if (name && std::strcmp(overrides_[i].name, name) == 0)
            return overrides_[i].scale;
    if (default_scale_ > 0.0)
        return default_scale_;
    return compute_default_scale(w, h, phys_w, phys_h);
}

void lx::compositor::output_manager::set_transform(lx::output_id output, unsigned transform) {
    const int s = slot_of(output);
    if (s < 0 || outputs_[s].transform == transform)
        return;
    outputs_[s].transform = transform;
    ++generation_;
    ++layout_generation_;
    arrange();
}

void lx::compositor::output_manager::set_position(lx::output_id output, lx::point2i position) {
    const int s = slot_of(output);
    if (s < 0)
        return;
    output_state& o = outputs_[s];
    o.auto_position = false;
    if (o.position.x == position.x && o.position.y == position.y)
        return;
    o.position = position;
    ++generation_;
    ++layout_generation_;
    arrange();
}

void lx::compositor::monitor_identity(const char* name, const char* make_in,
                                      const char* model_in, const char* serial_in, bool edid,
                                      char* identifier, unsigned identifier_size,
                                      char* description, unsigned description_size) {
    const char* nm = name ? name : "";
    if (edid) {
        const char* make = make_in && make_in[0] ? make_in : "Unknown";
        const char* model = model_in && model_in[0] ? model_in : "Unknown";
        const char* serial_s = serial_in ? serial_in : "";
        const char* serial = serial_s[0] ? serial_s : "Unknown";
        if (identifier && identifier_size)
            std::snprintf(identifier, identifier_size, "%s %s %s", make, model, serial);
        if (description && description_size)
            std::snprintf(description, description_size, "%s %s%s%s (%s)", make, model,
                          serial_s[0] ? " " : "", serial_s, nm);
    } else {
        if (identifier && identifier_size)
            std::snprintf(identifier, identifier_size, "connector:%s", nm);
        if (description && description_size)
            std::snprintf(description, description_size, "%s", nm);
    }
}

void lx::compositor::output_manager::set_auto_position(lx::output_id output) {
    const int s = slot_of(output);
    if (s < 0 || outputs_[s].auto_position)
        return;
    outputs_[s].auto_position = true;
    arrange(); // bumps both generations when a position moved
}

void lx::compositor::output_manager::set_adaptive_sync(unsigned slot, bool on) {
    if (slot >= k_max || !live_[slot])
        return;
    output_state& o = outputs_[slot];
    const bool v = on && o.vrr_capable;
    if (o.adaptive_sync == v)
        return;
    o.adaptive_sync = v;
    ++generation_;
}

void lx::compositor::output_manager::set_follow_preferred(unsigned slot, bool follow) {
    if (slot < k_max && live_[slot])
        outputs_[slot].follow_preferred = follow;
}

void lx::compositor::output_manager::make_identity(output_state& o, bool edid) {
    monitor_identity(o.name, o.make, o.model, o.serial, edid, o.identifier,
                     sizeof(o.identifier), o.description, sizeof(o.description));
    // Two identical monitors without serials: the connector tells them apart (niri).
    for (unsigned s = 0; s < k_max; ++s) {
        if (!live_[s] || &outputs_[s] == &o || std::strcmp(outputs_[s].identifier, o.identifier) != 0)
            continue;
        const std::size_t n = std::strlen(o.identifier);
        std::snprintf(o.identifier + n, sizeof(o.identifier) - n, " connector:%s", o.name);
        break;
    }
}

int lx::compositor::output_manager::free_slot(std::uint32_t reserved) const {
    for (unsigned s = 0; s < k_max; ++s)
        if (!live_[s] && !(reserved & (1u << s)))
            return static_cast<int>(s);
    return -1;
}

void lx::compositor::output_manager::set_mode(unsigned slot, const lx::drm::kms_mode& m,
                                              bool follow_preferred, bool custom) {
    if (slot >= k_max || !live_[slot] || !m.valid())
        return;
    output_state& o = outputs_[slot];
    o.follow_preferred = follow_preferred;
    o.custom_mode = custom;
    if (o.kms_mode.same_timings(m) && o.mode.width == static_cast<int>(m.width) &&
        o.mode.height == static_cast<int>(m.height) && o.refresh_mhz == m.refresh_mhz)
        return;
    o.kms_mode = m;
    o.mode = {static_cast<int>(m.width), static_cast<int>(m.height)};
    o.refresh_mhz = m.refresh_mhz;
    ++generation_;
    ++layout_generation_;
    arrange();
}

void lx::compositor::output_manager::update_head(unsigned slot,
                                                 const lx::drm::connector_probe& e) {
    if (slot >= k_max || !live_[slot])
        return;
    output_state& o = outputs_[slot];
    if (const lx::drm::kms_mode* pm = e.preferred_mode())
        o.preferred_mode = *pm;
    o.modes_hash = mode_list_hash(e);
    o.link_bad = e.link_bad;
    o.possible_crtcs = e.possible_crtcs;
    bool visible = false;
    if (o.phys_w_mm != static_cast<int>(e.phys_w_mm) || o.phys_h_mm != static_cast<int>(e.phys_h_mm)) {
        o.phys_w_mm = static_cast<int>(e.phys_w_mm);
        o.phys_h_mm = static_cast<int>(e.phys_h_mm);
        visible = true;
    }
    const bool moved_hint = o.hotplug_mode_update != e.hotplug_mode_update ||
                            o.suggested_x != e.suggested_x || o.suggested_y != e.suggested_y;
    o.hotplug_mode_update = e.hotplug_mode_update;
    o.suggested_x = e.suggested_x;
    o.suggested_y = e.suggested_y;
    if (visible)
        ++generation_;
    if (moved_hint)
        arrange(); // bumps both generations when a position moved
}

int lx::compositor::output_manager::add_output(const output_desc& d, int slot) {
    for (unsigned s = slot >= 0 ? static_cast<unsigned>(slot) : 0u; s < k_max; ++s) {
        if (live_[s]) {
            if (slot >= 0)
                return -1;
            continue;
        }
        output_state o{};
        o.id = lx::output_id{next_id_++};
        o.mode = d.mode;
        o.refresh_mhz = d.refresh_mhz;
        o.position = d.position;
        o.auto_position = d.auto_position;
        o.virtual_output = d.virtual_output;
        o.phys_w_mm = d.phys_w_mm;
        o.phys_h_mm = d.phys_h_mm;
        o.subpixel = d.subpixel;
        copy_str(o.name, sizeof(o.name), d.name && *d.name ? d.name : "HEADLESS-1");
        copy_str(o.make, sizeof(o.make), d.make);
        copy_str(o.model, sizeof(o.model), d.model);
        copy_str(o.serial, sizeof(o.serial), d.serial);
        o.scale = d.scale > 0.0 ? representable_scale(d.scale)
                                : scale_for(o.name, d.mode.width, d.mode.height, d.phys_w_mm,
                                            d.phys_h_mm);
        o.hotplug_mode_update = d.hotplug_mode_update;
        o.suggested_x = d.suggested_x;
        o.suggested_y = d.suggested_y;
        o.connector_id = d.connector_id;
        o.possible_crtcs = d.possible_crtcs;
        o.routed_crtc_id = d.routed_crtc_id;
        o.kms_mode = d.kms_mode;
        o.preferred_mode = d.preferred_mode;
        o.modes_hash = d.modes_hash;
        o.link_bad = d.link_bad;
        o.edid = d.edid;
        o.order = next_order_++;
        o.transform = d.transform < 8 ? d.transform : 0u;
        o.adaptive_sync = d.adaptive_sync && d.vrr_capable;
        o.vrr_capable = d.vrr_capable;
        o.connector_type = d.connector_type;
        o.follow_preferred = d.follow_preferred;
        o.custom_mode = d.custom_mode;
        make_identity(o, d.edid);
        outputs_[s] = o;
        live_[s] = true;
        ++generation_;
        ++layout_generation_;
        recount();
        arrange();
        return static_cast<int>(s);
    }
    return -1;
}

int lx::compositor::output_manager::add_virtual(const char* name, lx::rect2i layout,
                                                unsigned refresh_mhz, double scale) {
    output_desc d{};
    char generated[32];
    if (!name || !*name) {
        std::snprintf(generated, sizeof(generated), "HEADLESS-%u", next_virtual_);
        name = generated;
    }
    ++next_virtual_;
    d.name = name;
    d.mode = {layout.width, layout.height};
    d.refresh_mhz = refresh_mhz;
    d.scale = scale;
    d.position = {layout.x, layout.y};
    d.auto_position = false;
    d.virtual_output = true;
    return add_output(d);
}

void lx::compositor::output_manager::remove_slot(unsigned slot) {
    if (slot >= k_max || !live_[slot])
        return;
    live_[slot] = false;
    outputs_[slot] = {};
    ++generation_;
    ++layout_generation_;
    recount();
    arrange();
}

void lx::compositor::output_manager::arrange() {
    // Insertion order, explicit outputs first (they do not move).
    unsigned idx[k_max];
    unsigned n = 0;
    for (unsigned s = 0; s < k_max; ++s)
        if (live_[s])
            idx[n++] = s;
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && outputs_[idx[j]].order < outputs_[idx[j - 1]].order; --j) {
            const unsigned t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    bool placed[k_max]{};
    for (unsigned i = 0; i < n; ++i)
        placed[idx[i]] = !outputs_[idx[i]].auto_position;
    const auto overlaps_placed = [&](unsigned self, lx::rect2i r) {
        for (unsigned s = 0; s < k_max; ++s)
            if (live_[s] && placed[s] && s != self && overlap_area(r, outputs_[s].logical_rect()) > 0)
                return true;
        return false;
    };
    bool changed = false;
    const auto move_to = [&](unsigned s, lx::point2i p) {
        output_state& o = outputs_[s];
        if (o.position.x != p.x || o.position.y != p.y) {
            o.position = p;
            changed = true;
        }
        placed[s] = true;
    };
    // The host's layout (a VM window per monitor) where it fits.
    for (unsigned i = 0; i < n; ++i) {
        const unsigned s = idx[i];
        const output_state& o = outputs_[s];
        if (placed[s] || !o.hotplug_mode_update || o.suggested_x < 0 || o.suggested_y < 0)
            continue;
        const lx::point2i p{static_cast<int>(o.suggested_x), static_cast<int>(o.suggested_y)};
        lx::rect2i r = o.logical_rect();
        r.x = p.x;
        r.y = p.y;
        if (!overlaps_placed(s, r))
            move_to(s, p);
    }
    // The rest left to right, top-aligned, after the rightmost placed output.
    for (unsigned i = 0; i < n; ++i) {
        const unsigned s = idx[i];
        if (placed[s])
            continue;
        int right = 0;
        bool any = false;
        for (unsigned t = 0; t < k_max; ++t) {
            if (!live_[t] || !placed[t])
                continue;
            const lx::rect2i l = outputs_[t].logical_rect();
            if (!any || l.x + l.width > right)
                right = l.x + l.width;
            any = true;
        }
        move_to(s, {any ? right : 0, 0});
    }
    if (changed) {
        ++generation_;
        ++layout_generation_;
    }
}

void lx::compositor::output_manager::configure_from_scan(const lx::drm::connector_scan& scan) {
    for (unsigned i = 0; i < scan.count; ++i) {
        const lx::drm::connector_probe& e = scan.entries[i];
        if (!desktop_connector(e))
            continue;
        (void)add_output(desc_from_probe(e, *e.preferred_mode()));
    }
    if (count_ == 0)
        (void)add_virtual("HEADLESS-1", {0, 0, 1920, 1080}, 60000);
}

unsigned lx::compositor::mode_list_hash(const lx::drm::connector_probe& e) {
    // FNV-1a over what tells two modes apart; order matters (it is the driver's order).
    unsigned h = 2166136261u;
    const auto mix = [&h](unsigned v) {
        for (unsigned i = 0; i < 4; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 16777619u;
        }
    };
    mix(e.mode_count);
    for (unsigned i = 0; i < e.mode_count; ++i) {
        const lx::drm::kms_mode& m = e.modes[i];
        mix(m.width);
        mix(m.height);
        mix(m.refresh_mhz);
        mix(m.flags);
        mix(m.preferred ? 1u : 0u);
    }
    return h;
}

bool lx::compositor::desktop_connector(const lx::drm::connector_probe& e) {
    if (e.status != lx::drm::connector_status::connected || e.non_desktop)
        return false;
    const lx::drm::kms_mode* m = e.preferred_mode();
    return m && m->valid();
}

lx::compositor::output_desc lx::compositor::desc_from_probe(const lx::drm::connector_probe& e,
                                                           const lx::drm::kms_mode& m) {
    output_desc d{};
    d.name = e.name;
    d.mode = {static_cast<int>(m.width), static_cast<int>(m.height)};
    d.refresh_mhz = m.refresh_mhz;
    d.phys_w_mm = static_cast<int>(e.phys_w_mm);
    d.phys_h_mm = static_cast<int>(e.phys_h_mm);
    d.subpixel = e.subpixel;
    d.make = e.make;
    d.model = e.model;
    d.serial = e.serial;
    d.edid = e.edid;
    d.hotplug_mode_update = e.hotplug_mode_update;
    d.suggested_x = e.suggested_x;
    d.suggested_y = e.suggested_y;
    d.connector_id = e.connector_id;
    d.possible_crtcs = e.possible_crtcs;
    d.routed_crtc_id = e.current_crtc_id;
    d.kms_mode = m;
    if (const lx::drm::kms_mode* pm = e.preferred_mode())
        d.preferred_mode = *pm;
    d.modes_hash = mode_list_hash(e);
    d.link_bad = e.link_bad;
    d.vrr_capable = e.vrr_capable;
    d.connector_type = e.type;
    return d;
}

lx::compositor::present_mode
lx::compositor::resolve_present_mode(present_mode requested, lx::gfx::gpu_tier tier) {
    if (requested != present_mode::auto_select) return requested;
    if (tier == lx::gfx::gpu_tier::discrete) return present_mode::mailbox;
    return present_mode::fifo_relaxed;
}

std::uint32_t lx::compositor::outputs_for_rect(const output_manager& outputs, lx::rect2i r) {
    if (r.width <= 0 || r.height <= 0)
        return 0;
    std::uint32_t mask = 0;
    for (unsigned s = 0; s < output_manager::k_max; ++s)
        if (outputs.slot_live(s) && overlap_area(r, outputs.slot_state(s).logical_rect()) > 0)
            mask |= 1u << s;
    return mask;
}

int lx::compositor::primary_output_for_rect(const output_manager& outputs, lx::rect2i r) {
    if (r.width <= 0 || r.height <= 0)
        return -1;
    int best = -1;
    long long best_area = 0;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const long long a = overlap_area(r, outputs.slot_state(s).logical_rect());
        if (a > best_area) {
            best_area = a;
            best = static_cast<int>(s);
        }
    }
    return best;
}

int lx::compositor::pace_output(const output_manager& outputs, std::uint32_t shown_mask,
                                lx::rect2i rect) {
    const long long whole =
        rect.width > 0 && rect.height > 0 ? static_cast<long long>(rect.width) * rect.height : 0;
    int best = -1;          // the largest overlap over 10 %
    long long best_area = 0;
    int fallback = -1;      // the largest overlap of any candidate (all slivers)
    long long fallback_area = -1;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!(shown_mask & (1u << s)) || !outputs.slot_live(s))
            continue;
        const long long a = overlap_area(rect, outputs.slot_state(s).logical_rect());
        if (a > fallback_area) {
            fallback_area = a;
            fallback = static_cast<int>(s);
        }
        // wlroots: an output showing 10 % of the surface or less does not pace it.
        if (whole > 0 && a * 10 <= whole)
            continue;
        if (a > best_area) {
            best_area = a;
            best = static_cast<int>(s);
        }
    }
    return best >= 0 ? best : fallback;
}

namespace {

[[nodiscard]] double max_scale(const lx::compositor::output_manager& outputs, std::uint32_t mask) {
    double best = 0.0;
    for (unsigned s = 0; s < lx::compositor::output_manager::k_max; ++s)
        if ((mask & (1u << s)) && outputs.slot_live(s) && outputs.slot_state(s).scale > best)
            best = outputs.slot_state(s).scale;
    return best;
}

} // namespace

int lx::compositor::preferred_buffer_scale(const output_manager& outputs, std::uint32_t mask) {
    const double best = max_scale(outputs, mask);
    if (!(best > 0.0))
        return 1;
    // Scales are n/120, so the ceiling is exact without an epsilon.
    const int c = static_cast<int>(std::ceil(best));
    return c < 1 ? 1 : c;
}

unsigned lx::compositor::preferred_scale_120(const output_manager& outputs, std::uint32_t mask) {
    const double best = max_scale(outputs, mask);
    if (!(best > 0.0))
        return 120;
    const long n = std::lround(best * 120.0);
    return n < 1 ? 1u : static_cast<unsigned>(n);
}

unsigned lx::compositor::preferred_buffer_transform(const output_manager& outputs, int primary) {
    if (primary < 0 || !outputs.slot_live(static_cast<unsigned>(primary)))
        return 0;
    return outputs.slot_state(static_cast<unsigned>(primary)).transform;
}

lx::rect2i lx::compositor::layout_bounds(const output_manager& outputs) {
    bool any = false;
    long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const lx::rect2i l = outputs.slot_state(s).logical_rect();
        if (l.width <= 0 || l.height <= 0)
            continue;
        const long long r = static_cast<long long>(l.x) + l.width;
        const long long b = static_cast<long long>(l.y) + l.height;
        if (!any) {
            x0 = l.x;
            y0 = l.y;
            x1 = r;
            y1 = b;
            any = true;
            continue;
        }
        x0 = l.x < x0 ? l.x : x0;
        y0 = l.y < y0 ? l.y : y0;
        x1 = r > x1 ? r : x1;
        y1 = b > y1 ? b : y1;
    }
    if (!any)
        return {};
    return {static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(x1 - x0),
            static_cast<int>(y1 - y0)};
}

int lx::compositor::output_at(const output_manager& outputs, double x, double y) {
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const lx::rect2i l = outputs.slot_state(s).logical_rect();
        if (x >= l.x && y >= l.y && x < static_cast<double>(l.x) + l.width &&
            y < static_cast<double>(l.y) + l.height)
            return static_cast<int>(s);
    }
    return -1;
}

bool lx::compositor::closest_point(const output_manager& outputs, double& x, double& y) {
    if (output_at(outputs, x, y) >= 0)
        return true;
    bool any = false;
    double best_x = x;
    double best_y = y;
    double best_d = 0.0;
    for (unsigned s = 0; s < output_manager::k_max; ++s) {
        if (!outputs.slot_live(s))
            continue;
        const lx::rect2i l = outputs.slot_state(s).logical_rect();
        if (l.width <= 0 || l.height <= 0)
            continue;
        const double max_x = static_cast<double>(l.x) + l.width - 1;
        const double max_y = static_cast<double>(l.y) + l.height - 1;
        const double cx = x < l.x ? l.x : (x > max_x ? max_x : x);
        const double cy = y < l.y ? l.y : (y > max_y ? max_y : y);
        const double d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (!any || d < best_d) {
            best_d = d;
            best_x = cx;
            best_y = cy;
            any = true;
        }
    }
    if (!any)
        return false;
    x = best_x;
    y = best_y;
    return true;
}
