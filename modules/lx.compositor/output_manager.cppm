module;

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

struct output_state {
    lx::output_id id{};
    lx::rect2i geometry{};
    float scale = 1.f;
    present_mode present = present_mode::auto_select;
    bool supports_kms_damage = false;
};

class output_manager {
public:
    [[nodiscard]] unsigned count() const;
    [[nodiscard]] lx::output_id nth(unsigned index) const;
    [[nodiscard]] output_state state(lx::output_id output) const;

    [[nodiscard]] lx::rect2i geometry(lx::output_id output) const;
    [[nodiscard]] float scale(lx::output_id output) const;
    [[nodiscard]] present_mode present_mode_for(lx::output_id output) const;

    void set_present_mode(lx::output_id output, present_mode mode);
    void set_scale(lx::output_id output, float scale);
    void refresh_from_drm(lx::drm::kms_device& device);

private:
    output_state outputs_[16]{};
    unsigned count_ = 0;
};

[[nodiscard]] present_mode resolve_present_mode(present_mode requested,
                                              lx::gfx::gpu_tier tier);

} // namespace lx::compositor


unsigned lx::compositor::output_manager::count() const { return count_; }
lx::output_id lx::compositor::output_manager::nth(unsigned index) const {
    return index < count_ ? outputs_[index].id : lx::output_id{};
}

lx::compositor::output_state lx::compositor::output_manager::state(lx::output_id output) const {
    for (unsigned i = 0; i < count_; ++i)
        if (outputs_[i].id == output) return outputs_[i];
    return {};
}

lx::rect2i lx::compositor::output_manager::geometry(lx::output_id output) const {
    return state(output).geometry;
}

float lx::compositor::output_manager::scale(lx::output_id output) const {
    return state(output).scale;
}

lx::compositor::present_mode
lx::compositor::output_manager::present_mode_for(lx::output_id output) const {
    const auto st = state(output);
    if (st.present != present_mode::auto_select) return st.present;
    return lx::compositor::resolve_present_mode(present_mode::auto_select,
                                                lx::gfx::device_selector::probe_tier());
}

void lx::compositor::output_manager::set_present_mode(lx::output_id output, present_mode mode) {
    for (unsigned i = 0; i < count_; ++i)
        if (outputs_[i].id == output) outputs_[i].present = mode;
}

void lx::compositor::output_manager::set_scale(lx::output_id output, float scale) {
    for (unsigned i = 0; i < count_; ++i)
        if (outputs_[i].id == output) outputs_[i].scale = scale;
}

void lx::compositor::output_manager::refresh_from_drm(lx::drm::kms_device& device) {
    // Rebuild from connected connectors only; geometry comes from the active mode,
    // never from a fabricated default. No connectors means no outputs.
    const unsigned connectors = device.connector_count();
    unsigned next = 0;
    int cursor_x = 0;

    for (unsigned i = 0; i < connectors && next < 16; ++i) {
        const auto info = device.connector(i);
        if (info.status != lx::drm::connector_status::connected)
            continue;

        auto active = device.active_mode(i);
        if (!active)
            continue;
        const auto m = active.value();

        auto& out = outputs_[next];
        const float previous_scale = (next < count_) ? outputs_[next].scale : 1.f;
        out.id = lx::output_id{info.id};
        out.geometry = {cursor_x, 0, static_cast<int>(m.width), static_cast<int>(m.height)};
        out.scale = previous_scale > 0.f ? previous_scale : 1.f;
        cursor_x += static_cast<int>(m.width);
        ++next;
    }

    count_ = next;
}

lx::compositor::present_mode
lx::compositor::resolve_present_mode(present_mode requested, lx::gfx::gpu_tier tier) {
    if (requested != present_mode::auto_select) return requested;
    if (tier == lx::gfx::gpu_tier::discrete) return present_mode::mailbox;
    return present_mode::fifo_relaxed;
}
