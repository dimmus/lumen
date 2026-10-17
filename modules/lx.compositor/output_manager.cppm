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
    /// "HEADLESS-1" for the virtual output. Never changes for a slot (wl_output v4).
    char name[32]{};
    char description[64]{};
    /// Refresh of the current mode, mHz (wl_output.mode).
    unsigned refresh_mhz = 0;
    /// wl_output.transform value; always normal until output transforms exist.
    unsigned transform = 0;
    /// Physical size in mm; 0 when unknown.
    int phys_w_mm = 0;
    int phys_h_mm = 0;
    /// The headless/nested stand-in, not a connector.
    bool virtual_output = false;

    /// The output in the layout: `position`, and the mode divided by the scale, rounded
    /// half away from zero (wlroots' `wlr_output_effective_resolution`). A fractional
    /// product leaves the last row and column partly outside, which the scanout clips.
    [[nodiscard]] lx::rect2i logical_rect() const;
    /// The output's own pixels: `{0, 0, mode}`.
    [[nodiscard]] lx::rect2i mode_rect() const { return {0, 0, mode.width, mode.height}; }
};

/// The outputs, in stable slots: an output keeps its slot for as long as its connector stays
/// connected, because surfaces remember which outputs they are on as a bit mask per slot
/// (wl_surface.enter/leave). UI affinity.
///
/// Without a connected connector (headless, nested without KMS, every display unplugged)
/// there is one virtual output, HEADLESS-1, 1920x1080 at 60 Hz — the nominal size the
/// window manager places against, now visible to clients too.
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
    /// Rounded through `representable_scale`. Moves `generation` on a change.
    void set_scale(lx::output_id output, double scale);
    /// The scale a connector gets when it first appears (`config::output_scale`, i.e.
    /// `--scale`). Rounded through `representable_scale`.
    void set_default_scale(double scale) { default_scale_ = representable_scale(scale); }
    [[nodiscard]] double default_scale() const { return default_scale_; }
    /// wl_output transform of `output` (nothing rotates outputs yet; tests, and D5).
    void set_transform(lx::output_id output, unsigned transform);
    /// Brings the outputs in line with the connected connectors and their modes. Called
    /// every tick by the hotplug poll, so it compares values: `generation` moves only when
    /// something a client can see changed.
    void refresh_from_drm(lx::drm::kms_device& device);

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

    /// Puts an output that is not a connector in the first free slot (the headless one;
    /// tests): `layout.x/y` is its logical position, `layout.width/height` its mode in
    /// pixels. `scale` 0 means the default scale. Returns the slot, -1 when every slot is
    /// taken.
    int add_virtual(const char* name, lx::rect2i layout, unsigned refresh_mhz,
                    double scale = 0.0);
    void remove_slot(unsigned slot);

private:
    void recount();

    output_state outputs_[k_max]{};
    bool live_[k_max]{};
    unsigned count_ = 0;
    unsigned generation_ = 0;
    unsigned next_virtual_id_ = 1;
    double default_scale_ = 1.0;
};

[[nodiscard]] present_mode resolve_present_mode(present_mode requested,
                                              lx::gfx::gpu_tier tier);

// ── Output membership (pure; exported for tests) ───────────────────────────

/// The outputs `r` (global) overlaps, one bit per slot. Rects that only touch an output's
/// edge are not on it; an empty rect is on none.
[[nodiscard]] std::uint32_t outputs_for_rect(const output_manager& outputs, lx::rect2i r);
/// The slot `r` overlaps most (wlroots' "primary output"), -1 for none. Ties go to the
/// lower slot.
[[nodiscard]] int primary_output_for_rect(const output_manager& outputs, lx::rect2i r);
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

constexpr unsigned k_virtual_id_base = 0xFFFF'0000u;

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

lx::rect2i lx::compositor::output_state::logical_rect() const {
    const double s = scale > 0.0 ? scale : 1.0;
    return {position.x, position.y,
            static_cast<int>(std::lround(static_cast<double>(mode.width) / s)),
            static_cast<int>(std::lround(static_cast<double>(mode.height) / s))};
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
}

void lx::compositor::output_manager::set_transform(lx::output_id output, unsigned transform) {
    const int s = slot_of(output);
    if (s < 0 || outputs_[s].transform == transform)
        return;
    outputs_[s].transform = transform;
    ++generation_;
}

int lx::compositor::output_manager::add_virtual(const char* name, lx::rect2i layout,
                                                unsigned refresh_mhz, double scale) {
    for (unsigned s = 0; s < k_max; ++s) {
        if (live_[s])
            continue;
        output_state o{};
        o.id = lx::output_id{k_virtual_id_base + next_virtual_id_++};
        o.position = {layout.x, layout.y};
        o.mode = {layout.width, layout.height};
        o.scale = scale > 0.0 ? representable_scale(scale) : default_scale_;
        o.refresh_mhz = refresh_mhz;
        o.virtual_output = true;
        std::snprintf(o.name, sizeof(o.name), "%s", name ? name : "HEADLESS-1");
        std::snprintf(o.description, sizeof(o.description), "%s", o.name);
        outputs_[s] = o;
        live_[s] = true;
        ++generation_;
        recount();
        return static_cast<int>(s);
    }
    return -1;
}

void lx::compositor::output_manager::remove_slot(unsigned slot) {
    if (slot >= k_max || !live_[slot])
        return;
    live_[slot] = false;
    outputs_[slot] = {};
    ++generation_;
    recount();
}

void lx::compositor::output_manager::refresh_from_drm(lx::drm::kms_device& device) {
    // Geometry comes from the active mode, never from a fabricated default. Each connected
    // connector keeps the slot it had; outputs are laid out left to right in connector order.
    bool seen[k_max]{};
    bool any_real = false;
    int cursor_x = 0;
    const unsigned connectors = device.connector_count();
    for (unsigned i = 0; i < connectors; ++i) {
        const auto info = device.connector(i);
        if (info.status != lx::drm::connector_status::connected)
            continue;
        auto active = device.active_mode(i);
        if (!active)
            continue;
        const auto m = active.value();
        const lx::output_id id{info.id};

        int slot = -1;
        for (unsigned s = 0; s < k_max && slot < 0; ++s)
            if (live_[s] && !outputs_[s].virtual_output && outputs_[s].id == id)
                slot = static_cast<int>(s);
        for (unsigned s = 0; s < k_max && slot < 0; ++s)
            if (!live_[s] && !seen[s])
                slot = static_cast<int>(s);
        if (slot < 0)
            continue;

        output_state next = live_[slot] ? outputs_[slot] : output_state{};
        next.id = id;
        next.virtual_output = false;
        next.position = {cursor_x, 0};
        next.mode = {static_cast<int>(m.width), static_cast<int>(m.height)};
        next.refresh_mhz = m.refresh_millihz;
        next.phys_w_mm = static_cast<int>(info.phys_w_mm);
        next.phys_h_mm = static_cast<int>(info.phys_h_mm);
        if (std::strcmp(next.name, info.name ? info.name : "") != 0) {
            std::snprintf(next.name, sizeof(next.name), "%s", info.name ? info.name : "");
            std::snprintf(next.description, sizeof(next.description), "%s", next.name);
        }
        // A connector seen for the first time takes the configured scale; one already in a
        // slot keeps its own.
        if (!live_[slot] || !(next.scale > 0.0))
            next.scale = default_scale_;
        if (!live_[slot] || !same_visible(outputs_[slot], next)) {
            outputs_[slot] = next;
            live_[slot] = true;
            ++generation_;
        }
        seen[slot] = true;
        any_real = true;
        // Laid out left to right in logical units (a 2x output takes half its mode).
        cursor_x += next.logical_rect().width;
    }

    bool changed = false;
    for (unsigned s = 0; s < k_max; ++s) {
        if (!live_[s] || seen[s])
            continue;
        // A connector that went away, or the virtual output once a real one exists.
        if (!outputs_[s].virtual_output || any_real) {
            live_[s] = false;
            outputs_[s] = {};
            changed = true;
        }
    }
    if (changed) {
        ++generation_;
        recount();
    }
    recount();
    if (count_ == 0)
        (void)add_virtual("HEADLESS-1", {0, 0, 1920, 1080}, 60000);
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
