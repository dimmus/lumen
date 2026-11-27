#pragma once

import std;
import lx.foundation;
import lx.layout;
import lx.text;
import lx.ui;

namespace lumen::dm {
namespace field_detail {

constexpr int k_caret_width = 2;
constexpr std::size_t k_max_bullets = 32;
// The KMS form is at most 468 logical pixels wide.
constexpr std::size_t k_max_field_width = 468;
constexpr std::string_view k_bullet = "\xe2\x80\xa2";

// The conversation has already validated its visible text as UTF-8.
[[nodiscard]] constexpr bool scalar_start(char c) {
    return (static_cast<unsigned char>(c) & 0xc0u) != 0x80u;
}

[[nodiscard]] inline std::size_t byte_at(std::string_view text, std::size_t scalar) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!scalar_start(text[i]))
            continue;
        if (scalar == 0)
            return i;
        --scalar;
    }
    return text.size();
}

struct props {
    std::size_t first_scalar = 0;
    std::uint32_t caret_byte = 0;
};

class node final : public lx::ui::ui_node {
public:
    [[nodiscard]] static lx::ui::element_type_id static_type() {
        static const auto id = lx::ui::next_element_type_id();
        return id;
    }
    [[nodiscard]] lx::ui::element_type_id type() const override { return static_type(); }
    [[nodiscard]] const char* type_name() const override { return "greeter_field"; }

    bool apply(const props& p) {
        const bool moved = p.caret_byte != caret_byte_;
        first_scalar_ = p.first_scalar;
        caret_byte_ = p.caret_byte;
        return moved;
    }

    [[nodiscard]] lx::size2i measure(lx::layout::constraints c, lx::ui::size_hint) const override {
        const auto* label = text();
        const int h = label ? label->measure_cached({}, lx::ui::size_hint::preferred).height : 0;
        // The field stretches to its container, never to the length of its content.
        return {c.min.width, std::clamp(h > 0 ? h : 17, c.min.height, c.max.height)};
    }

    void arrange(lx::rect2i bounds) override {
        bounds_ = bounds;
        clear_layout_dirty();
        auto* label = text();
        if (!label || bounds.width <= 0)
            return;
        const auto size = label->measure_cached({}, lx::ui::size_hint::preferred);
        const auto& layout = label->layout();
        const double scale = 64.0 * label->device_scale();
        const int caret_x = static_cast<int>(std::lround(layout.caret(caret_byte_).x / scale));
        int left = std::min(caret_x, 0);
        int right = std::max(caret_x, size.width);
        // Trailing spaces can extend past the measured width (to the left for RTL).
        for (const auto& line : layout.lines()) {
            const auto runs = layout.runs(line);
            if (runs.empty())
                continue;
            left = std::min(left, static_cast<int>(std::floor(runs.front().x / scale)));
            const auto& last = runs.back();
            right = std::max(right, static_cast<int>(std::ceil((last.x + last.width) / scale)));
        }
        const int span = right - left;
        const int last_caret_x = std::max(bounds.width - k_caret_width, 0);
        const int caret_from_left = caret_x - left;
        const int align = layout.direction() == lx::text::text_direction::rtl
                              ? std::max(last_caret_x - span, 0)
                              : 0;
        // Keep as much of the previous viewport as possible while bringing the caret back.
        scroll_ = std::clamp(scroll_, std::max(caret_from_left - last_caret_x, 0),
                             std::min(caret_from_left, std::max(span - last_caret_x, 0)));
        const int origin = bounds.x + align - left - scroll_;
        label->place({origin, bounds.y, std::max(right, 1), bounds.height});
        caret_x_ = bounds.x + std::clamp(origin + caret_x - bounds.x, 0, last_caret_x);
    }

    void emit(lx::ui::draw_scope& out) const override {
        const auto* label = text();
        if (!visible() || !label || bounds_.width <= 0 || bounds_.height <= 0)
            return;
        out.push_clip_local(bounds_);
        label->emit(out);
        lx::ui::quad_style caret{};
        caret.background = lx::ui::active_theme().color(lx::ui::tok::player0_cursor);
        out.quad({caret_x_, bounds_.y, k_caret_width, bounds_.height}, caret);
        out.pop();
    }

    [[nodiscard]] bool child_viewport(const lx::ui::ui_node&, lx::point2i& offset,
                                      lx::rect2i& clip) const override {
        offset = {};
        clip = bounds_;
        return true;
    }

    [[nodiscard]] bool right_to_left() const {
        const auto* label = text();
        return label && label->layout().direction() == lx::text::text_direction::rtl;
    }

    [[nodiscard]] std::optional<std::size_t> caret_at(lx::point2i point) const {
        const auto* label = text();
        // The surrounding div owns the field skin and its padding remains clickable.
        if (!visible() || !parent() || !parent()->bounds().contains(point) || !label ||
            bounds_.width <= 0 || bounds_.height <= 0)
            return std::nullopt;
        const int x = std::clamp(point.x, bounds_.x, bounds_.x + bounds_.width);
        const double scale = 64.0 * label->device_scale();
        const auto& layout = label->layout();
        const auto hit =
            layout.hit_test(static_cast<std::int32_t>((x - label->bounds().x) * scale), 0);
        const auto prefix = layout.text().substr(0, hit.index);
        return first_scalar_ +
               static_cast<std::size_t>(std::ranges::count_if(prefix, scalar_start));
    }

private:
    [[nodiscard]] lx::ui::text_block_node* text() const {
        return static_cast<lx::ui::text_block_node*>(child_at(0));
    }

    std::size_t first_scalar_ = 0;
    std::uint32_t caret_byte_ = 0;
    int caret_x_ = 0;
    int scroll_ = 0;
};

} // namespace field_detail

// For secret prompts only mask_length and caret_index are used, never visible_value.
[[nodiscard]] inline lx::ui::child greeter_field(std::string_view visible_value, bool secret,
                                                 std::size_t mask_length, std::size_t caret_index) {
    using namespace lx::ui;
    using lx::layout::px;
    auto* ctx = build_context::current();
    if (!ctx)
        return nullptr;

    field_detail::props p{};
    const std::size_t count = std::min(mask_length, field_detail::k_max_bullets);
    const std::size_t bytes = secret ? count * field_detail::k_bullet.size() : visible_value.size();
    auto* display = ctx->alloc_chars(static_cast<unsigned>(bytes + 1));
    if (!display)
        return nullptr;
    if (secret) {
        const std::size_t caret = std::min(caret_index, mask_length);
        p.first_scalar = std::min(caret > count / 2 ? caret - count / 2 : 0, mask_length - count);
        p.caret_byte =
            static_cast<std::uint32_t>((caret - p.first_scalar) * field_detail::k_bullet.size());
        for (std::size_t i = 0; i < bytes; i += field_detail::k_bullet.size())
            std::ranges::copy(field_detail::k_bullet, display + i);
    } else {
        std::ranges::copy(visible_value, display);
        p.caret_byte =
            static_cast<std::uint32_t>(field_detail::byte_at(visible_value, caret_index));
    }
    display[bytes] = '\0';

    const child text = label(display).size_px(17).single_line();
    const child field = with_children(describe<field_detail::node>(p), {&text, 1});
    return div(style{}
                   .w_full()
                   .h(px(42))
                   .px(px(12))
                   .flex_col()
                   .justify_center()
                   .bg(tok::background)
                   .border_1(tok::border_variant)
                   .rounded(radius::md)
                   .overflow_hidden(),
               {&field, 1});
}

struct field_hit_map {
    lx::rect2i bounds{};
    std::array<std::uint16_t, field_detail::k_max_field_width> stops{};
    bool rtl = false;

    // Stops name positions in the live buffer, even while this map's pixels are old.
    constexpr void rebase(std::size_t first, std::size_t removed, std::size_t inserted) {
        for (int x = 0; x < bounds.width; ++x) {
            const std::size_t at = stops[x];
            if (at > first)
                stops[x] = static_cast<std::uint16_t>(
                    first + inserted + (at > first + removed ? at - first - removed : 0));
        }
    }

    [[nodiscard]] std::optional<std::size_t> caret_at(lx::point2i point) const {
        if (!bounds.contains(point))
            return std::nullopt;
        return stops[point.x - bounds.x];
    }
};

// Snapshot hit geometry without retaining another copy of the displayed string.
[[nodiscard]] inline const field_detail::node* find_field(const lx::ui::ui_node& tree) {
    if (!tree.visible())
        return nullptr;
    if (tree.type() == field_detail::node::static_type())
        return static_cast<const field_detail::node*>(&tree);
    for (unsigned i = 0; i < tree.child_count(); ++i) {
        if (const auto* child = tree.child_at(i)) {
            if (const auto* field = find_field(*child))
                return field;
        }
    }
    return nullptr;
}

[[nodiscard]] inline field_hit_map snapshot_field(const lx::ui::ui_node* tree) {
    field_hit_map map{};
    const auto* field = tree ? find_field(*tree) : nullptr;
    if (!field || !field->parent())
        return map;
    const auto bounds = field->parent()->bounds();
    if (bounds.width <= 0 || bounds.height <= 0 ||
        static_cast<std::size_t>(bounds.width) > map.stops.size())
        return map;
    map.bounds = bounds;
    map.rtl = field->right_to_left();
    for (int x = 0; x < bounds.width; ++x)
        map.stops[x] = static_cast<std::uint16_t>(
            field->caret_at({bounds.x + x, bounds.y + bounds.height / 2}).value_or(0));
    return map;
}

} // namespace lumen::dm
