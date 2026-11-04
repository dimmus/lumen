// popup-probe — the bell-menu anatomy in the smallest client that reproduces it: a layer
// surface (the "panel", its own text the control) and, a second later, an xdg_popup of it
// carrying the menu's own row construction (absolutely placed boxes, ellipsized text
// blocks, a check glyph, the card's background quad). The point: whether the popup's text
// reaches the screen while its boxes do — the damaged-history-menu symptom, in vitro.
//
//   popup-probe [socket-name] [--hold SECONDS]   (default 12 s)

module;

#include <stdlib.h>
#include <unistd.h>

export module lumen.popup_probe;

import std;
import lx.foundation;
import lx.layout;
import lx.runtime;
import lx.text;
import lx.trace;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;

using namespace lx;

namespace {

constexpr lx::trace::category log_probe{"popup-probe"};

// ── the popup's content: menu_popup's own row anatomy ──────────────────────────────

[[nodiscard]] lx::ui::style at(lx::rect2i r) {
    return lx::ui::style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

[[nodiscard]] lx::ui::child text_at(lx::rect2i r, const char* text) {
    lx::ui::text_block_props t{};
    t.text = text;
    t.font = {"Sans", 10.f, lx::text::weight::normal, lx::text::style::normal};
    t.wrap = lx::text::text_wrap::none;
    t.ellipsize = true;
    t.context = lx::ui::active_ui_resources().text;
    const lx::ui::child inside[1] = {lx::ui::text_block(t)};
    lx::ui::control_props box{};
    box.layout = at(r).flex_row().items_center().overflow_hidden();
    return lx::ui::control(box, inside);
}

lx::ui::child describe_popup(lx::ui::build_context&) {
    // 165x95: the history menu's own table — a check row, a plain row, the header row, on
    // the card's background quad (the one part the damaged menu does show).
    constexpr int w = 165, row_h = 26, pad = 4;
    const lx::ui::child cells1[2] = {
        text_at({6, 0, 20, row_h}, "\xE2\x9C\x93"),
        text_at({28, 0, w - pad - 28 - 8, row_h}, "Do not disturb"),
    };
    const lx::ui::child cells2[1] = {text_at({28, 0, w - pad - 28 - 8, row_h}, "Clear all")};
    const lx::ui::child cells4[1] = {
        text_at({28, 0, w - pad - 28 - 8, row_h}, "No notifications")};
    lx::ui::control_props r1{}, r2{}, r4{};
    r1.layout = at({pad, pad, w - 2 * pad, row_h}).flex_row().items_center();
    r2.layout = at({pad, pad + row_h, w - 2 * pad, row_h}).flex_row().items_center();
    r4.layout = at({pad, pad + 2 * row_h + 9, w - 2 * pad, row_h}).flex_row().items_center();
    const lx::ui::child rows[3] = {lx::ui::control(r1, cells1), lx::ui::control(r2, cells2),
                                   lx::ui::control(r4, cells4)};
    lx::ui::control_props root{};
    root.layout = lx::ui::style{}.flex_col().size_full();
    root.paint = lx::ui::control_paint::fixed(
        {lx::ui::paint(lx::ui::tok::background), lx::ui::k_transparent});
    return lx::ui::control(root, rows);
}

lx::ui::child describe_layer(lx::ui::build_context&) {
    return text_at({8, 6, 184, 24}, "layer text — the control");
}

// ── the client: the shell's own connection shape ───────────────────────────────────

struct probe {
    lx::wayland::display display{};
    lx::runtime::event_loop loop{};
    lx::ui::host::context ctx{};
    lx::ui::host::surface layer{};
    lx::ui::host::layer_role layer_role{};
    lx::ui::host::surface popup{};
    lx::ui::host::popup_role popup_role{};

    [[nodiscard]] int run(const char* socket, int hold_s) {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        lx::wayland::connect_options co{};
        co.name = socket;
        if (auto connected = display.connect(co); !connected) {
            log_probe.error(connected.get_error());
            return 1;
        }
        if (auto started = ctx.start(display, loop, {.watch_theme = 0}); !started) {
            log_probe.error(started.get_error());
            return 1;
        }

        lx::ui::host::surface_options so{};
        so.describe = &describe_layer;
        so.size = {200, 36};
        if (auto made = layer.create(ctx, so); !made) {
            log_probe.error(made.get_error());
            return 1;
        }
        lx::ui::host::layer_role_options lo{};
        lo.layer.level = lx::wayland::layer::top;
        lo.layer.name_space = "popup-probe";
        lo.size = {200, 36};
        lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_right;
        lo.margin[1] = 16;
        lo.margin[0] = 48;
        if (auto made = layer_role.create(layer, lo); !made) {
            log_probe.error(made.get_error());
            return 1;
        }
        if (auto shown = layer.show(); !shown) {
            log_probe.error(shown.get_error());
            return 1;
        }

        // The popup a second in, after the layer's first frame: exactly the menu's order.
        const auto open_popup = [&] {
            lx::ui::host::surface_options po{};
            po.describe = &describe_popup;
            po.size = {165, 95};
            po.opaque = false;
            if (auto made = popup.create(ctx, po); !made) {
                log_probe.error(made.get_error());
                return;
            }
            lx::wayland::positioner_desc pos{};
            pos.size = {165, 95};
            pos.anchor_rect = {0, 0, 1, 1};
            pos.anchor = 6;  // bottom_left
            pos.gravity = 8; // bottom_right
            pos.constraint_adjustment = 1 | 8 | 2;
            if (auto made = popup_role.create(popup, layer_role, pos); !made) {
                log_probe.error(made.get_error());
                return;
            }
            if (auto shown = popup.show(); !shown)
                log_probe.error(shown.get_error());
            log_probe.info("the popup is up: check + 'Do not disturb', 'Clear all', "
                           "'No notifications' on the card");
        };
        std::thread dropper;
        const auto drop_popup = [&] {
            popup_role.destroy();
            popup.destroy();
        };
        // The loop's own thread drives both; the timers are other threads' posts (the
        // loop's queue is single-producer, so one timer thread posts both).
        std::thread timer{[&loop = loop, open_popup, drop_popup, hold_s] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            loop.post(lx::runtime::task{open_popup});
            std::this_thread::sleep_for(std::chrono::seconds(hold_s));
            loop.post(lx::runtime::task{[&loop, drop_popup] {
                drop_popup();
                loop.quit(0);
            }});
        }};
        const int rc = loop.run();
        timer.join();
        layer_role.destroy();
        layer.destroy();
        ctx.stop();
        display.disconnect();
        return rc;
    }
};

} // namespace

int main(int argc, char** argv) {
    const char* socket = nullptr;
    int hold = 12;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--hold" && i + 1 < argc)
            hold = std::atoi(argv[++i]);
        else
            socket = argv[i];
    }
    probe p{};
    return p.run(socket && *socket ? socket : nullptr, hold);
}
