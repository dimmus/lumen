export module lx.ui:chrome;

// ── The opinionated layer over the primitives (ADR-010 Phase 5, ui-architecture §9) ────
//
// lx.ui's primitives are unopinionated: a box is whatever colour you give it. The shell's
// views have to agree with each other — the panel, the switcher, the notifications, the
// menus and the launcher are one desktop — and before this partition each view spelled its
// own near-identical greys as literals. `chrome` is where the shared look lives: the
// palette (the exact values the views drew on the shm canvas, so the smoke suites that pin
// them pixel for pixel stay green), and the components built from them (a row, a
// selectable cell, a toolbar) so the next view does not hand-roll them again.
//
// A separate module above `lx.ui` remains the destination (ui-architecture §9); this
// partition is the consolidation step — everything above moved here first, the split out
// of lx.ui is mechanical once nothing else in lx.ui references it.

import lx.foundation;
import lx.layout;
import lx.scene;

import :node;
import :control;
import :flex;
import :element;

import std;

export namespace lx::ui::chrome {

/// The shell's palette (straight sRGB): the values the canvas views drew, now named.
inline constexpr struct {
    std::uint32_t background = 0xFF1E1E22u;       ///< the panel, the switcher, the menus
    std::uint32_t card = 0xFF24242Au;             ///< a notification's or menu's card
    std::uint32_t card_hover = 0xFF2C2C33u;
    std::uint32_t row_hover = 0xFF34343Au;
    std::uint32_t row_pressed = 0xFF2A2A30u;
    std::uint32_t selected = 0xFF3D6FB4u;         ///< the selection, on every view
    std::uint32_t search = 0xFF1E1E22u;           ///< the launcher's search field
    std::uint32_t separator = 0xFF3A3A42u;
    std::uint32_t button = 0xFF34343Au;
    std::uint32_t text = 0xFFECECEEu;
    std::uint32_t text_bright = 0xFFFFFFFFu;
    std::uint32_t text_dim = 0xFFA8A8ACu;
    std::uint32_t text_selected = 0xFFD0DCEEu;
    std::uint32_t text_warning = 0xFFF0B45Au;
    std::uint32_t critical = 0xFFE0312Bu;
    std::uint32_t border = 0xFFFFFFFFu;
    std::uint32_t caret = 0xFFE6E6E6u;
} palette{};

/// `argb` as the engine's colour.
[[nodiscard]] constexpr lx::color color(std::uint32_t argb) {
    return lx::color::rgb(static_cast<float>((argb >> 16) & 0xFF) / 255.f,
                          static_cast<float>((argb >> 8) & 0xFF) / 255.f,
                          static_cast<float>(argb & 0xFF) / 255.f,
                          static_cast<float>((argb >> 24) & 0xFF) / 255.f);
}

/// A row in a chrome list: the full width, `height` tall, its background `background`
/// (or `selected` when `selected`), the children laid out in a row, centred.
[[nodiscard]] lx::ui::child row(float height, bool selected, std::span<const lx::ui::child> children,
                                lx::ui::element_key key = 0) {
    lx::ui::control_props c{};
    c.layout = lx::ui::style{}
                   .w(lx::layout::percent(100.f))
                   .h(lx::layout::px(height))
                   .flex_row()
                   .items_center()
                   .overflow_hidden();
    c.paint.rest = {lx::ui::paint(selected ? lx::ui::tok::element_selected : lx::ui::tok::none),
                    lx::ui::k_transparent};
    (void)key;
    return lx::ui::control(c, children, key);
}

/// A toolbar strip: `background`, the children laid out in a row from the start, centred.
[[nodiscard]] lx::ui::child toolbar(float height, std::span<const lx::ui::child> children,
                                    lx::ui::element_key key = 0) {
    lx::layout::flex_style st{};
    st.width = lx::layout::percent(100.f);
    st.height = lx::layout::px(height);
    st.direction = lx::layout::flex_direction::column;
    st.justify_content = lx::layout::content_align::center;
    st.align_items = lx::layout::item_align::stretch;
    (void)key; // the strip is one decoration node; nothing looks it up by key
    return lx::ui::flex(st, children, color(palette.background));
}

} // namespace lx::ui::chrome
