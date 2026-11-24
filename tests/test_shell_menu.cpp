// The shell menu's row anatomy in vitro: rows and their cells as absolutely positioned
// boxes (menu_popup's at()-style arithmetic) over lx.ui — the labels must land at their
// rect's width, whatever their text, and the whole list must read top to bottom. The
// in-app symptom this guards: some rows' labels collapsing to slivers while the first
// reads fine.
//
// The second half (menu_popup_close_lifecycle, after the LUMEN_TESTs) drives the real
// popup close over the Wayland wire against an in-process compositor: the menu's own
// input router delivering the Enter that activates and closes it, the Escape close, and
// the outside-click dismissal. The teardown must not free the level's surface — its root
// and ui context — while that delivery is still on the stack (`input_router::
// delivering()`'s rule): under the ASan build this is the regression test for the
// tray-menu heap-use-after-free (evidence/2026-11-24-shell-tray-menu-uaf/), and the
// state checks hold on every build.

#include "lumen_test.hpp"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <thread>

import std;
import lx.foundation;
import lx.a11y;
import lx.compositor;
import lx.gfx;
import lx.input;
import lx.layout;
import lx.runtime;
import lx.scheduler;
import lx.scene;
import lx.text;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
import lx.shell;

namespace {

using namespace lx::ui;

/// menu_popup's at(): an absolutely placed box of exactly this rect.
[[nodiscard]] style at(lx::rect2i r) {
    return style{}
        .absolute()
        .top(lx::layout::px(static_cast<float>(r.y)))
        .left(lx::layout::px(static_cast<float>(r.x)))
        .w(lx::layout::px(static_cast<float>(r.width)))
        .h(lx::layout::px(static_cast<float>(r.height)));
}

[[maybe_unused]] int g_labels = 0; // the labels the damaged popup would have drawn
const char* const g_words[4] = {"Do not disturb", "Clear all", "No notifications",
                                "14:32  org.lumen.Settings: saved"};

child describe_menu_rows(build_context&) {
    // The menu: 200 wide, 26-px rows at 4-px padding, a label from x 28 to ~row width.
    constexpr int w = 200, row_h = 26, pad = 4;
    const auto label_at = [&](int /*row*/, const char* text) {
        text_block_props t{};
        t.text = text;
        t.font = lx::shell::menu_font();
        t.wrap = lx::text::text_wrap::none;
        t.ellipsize = true;
        t.context = active_ui_resources().text;
        const child inside[1] = {text_block(t)};
        control_props box{};
        box.layout = at({28, 0, w - pad - 28 - 8, row_h}).flex_row().items_center()
                         .overflow_hidden();
        return control(box, inside);
    };
    child rows[4];
    for (unsigned i = 0; i < 4; ++i) {
        const child cells[1] = {label_at(i * (row_h + 2), g_words[i])};
        control_props row{};
        row.layout = at({pad, static_cast<int>(i) * (row_h + 2), w - 2 * pad, row_h})
                         .flex_row().items_center().overflow_hidden();
        rows[i] = control(row, cells);
    }
    control_props list{};
    list.layout = style{}.flex_col().relative().w(px(static_cast<float>(w)))
                         .h(px(static_cast<float>(4 * (row_h + 2) + 2 * pad)));
    const child kids[4] = {rows[0], rows[1], rows[2], rows[3]};
    return control(list, kids);
}

LUMEN_TEST(popup_font_uses_active_theme_family_and_ui_scale) {
    const lx::text::font_desc font = lx::shell::menu_font();
    const auto& themed = active_theme().fonts();
    LUMEN_CHECK((font.family == nullptr && themed.ui_family[0] == '\0') ||
                (font.family && std::strcmp(font.family, themed.ui_family) == 0));
    LUMEN_CHECK(font.size_pt ==
                text_size_px(text_size::ui, active_theme(), ui_rem_px()) * 0.75f);
}

LUMEN_TEST(menu_row_labels_keep_their_rect_widths) {
    lx::runtime::memory_arena arena{256 * 1024};
    ui_root root{arena};
    root.set_describer(&describe_menu_rows);
    root.set_bounds({0, 0, 200, 124});
    auto list = std::make_unique<lx::scene::draw_list>();
    LUMEN_CHECK(root.tick(*list));

    // Every label cell sits at its absolute rect: x 28+4, width 160, its row's y.
    unsigned found = 0;
    std::function<void(lx::ui::ui_node*, int)> walk = [&](lx::ui::ui_node* n, int depth) {
        if (!n)
            return;
        const lx::rect2i b = n->bounds();
        if (n->type() == control_node::static_type() && b.width >= 150 && b.width <= 170 &&
            b.height >= 24 && b.height <= 28) {
            ++found;
            LUMEN_CHECK(b.x == 32);
        }
        for (unsigned i = 0; i < n->child_count(); ++i)
            walk(n->child_at(i), depth + 1);
    };
    walk(root.tree(), 0);
    LUMEN_CHECK(found == 4); // one label cell per row, none collapsed
}

} // namespace

// ── The popup close lifecycle against an in-process compositor ────────────────
// The parent runs the compositor (CPU backend, its own socket) and serves synthesized
// seat events over a socketpair; the forked child (the shell's side of the wire) hosts
// the panel layer surface and the menu. test_clock_popup.cpp's harness shape, with the
// client running in the forked image directly: the module globals are constructed
// before main, so both halves share them safely, and the child keeps a single thread.

namespace {

namespace host = lx::ui::host;
constexpr const char* k_socket = "lumen-test-shell-menu";
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 0;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}
void tick() {
    std::lock_guard<std::mutex> hold(*g_lock);
    lx::scheduler::frame_tick t{};
    t.timestamp_ns = static_cast<long long>(now_ns());
    t.delta_seconds = 1.0 / 60.0;
    t.frame_index = static_cast<unsigned>(++g_frame);
    g_comp->tick_ui(t);
    g_comp->wayland().flush();
    (void)g_comp->render_and_wait();
    g_comp->run_posted_ui_tasks();
    g_comp->wayland().flush();
}
std::string serve(const std::string& line) {
    std::istringstream in{line};
    std::string cmd;
    in >> cmd;
    std::lock_guard<std::mutex> hold(*g_lock);
    if (cmd == "move") {
        double x = 0, y = 0;
        in >> x >> y;
        lx::input::pointer_motion_event e{};
        e.x = x;
        e.y = y;
        g_comp->inject_pointer_motion(e);
    } else if (cmd == "button") {
        int down = 0;
        in >> down;
        lx::input::pointer_button_event e{};
        e.which = lx::input::button::left;
        e.evdev_code = 0x110;
        e.pressed = down != 0;
        g_comp->inject_pointer_button(e);
    } else if (cmd == "key") {
        unsigned code = 0;
        int down = 0;
        in >> code >> down;
        lx::input::key_event e{};
        e.evdev_keycode = code;
        e.pressed = down != 0;
        g_comp->inject_key(e);
    } else {
        return "fail unknown command";
    }
    g_comp->wayland().flush();
    return "ok";
}
struct parent_link {
    int fd = -1;
    std::string call(const std::string& cmd) {
        (void)::send(fd, cmd.data(), cmd.size(), MSG_NOSIGNAL);
        char buf[128];
        const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            return "fail";
        buf[n] = 0;
        return buf;
    }
};

lx::ui::child describe_panel(lx::ui::build_context&) {
    using namespace lx::ui;
    const child kids[1] = {button("Menu").name("menu")};
    return div(style{}
                   .size_full()
                   .bg(tok::title_bar_background)
                   .flex_col()
                   .items_center()
                   .justify_center(),
               kids);
}

int g_failed = 0;
int g_ran = 0;
void verify(const char* name, bool ok, std::string detail = {}) {
    ++g_ran;
    std::printf("  %s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : " — ",
                detail.c_str());
    std::fflush(stdout);
    if (!ok)
        ++g_failed;
}

// The menu's callbacks: what the activate and the close told the owner.
int g_menu_id = 0;
unsigned g_menu_activations = 0;
unsigned g_menu_closes = 0;
void on_menu_activate(int id, void*) {
    g_menu_id = id;
    ++g_menu_activations;
}
void on_menu_closed(void*) {
    ++g_menu_closes;
}

int menu_popup_close_lifecycle(parent_link& parent) {
    // The parent brings the compositor up after the fork: wait for its go-ahead before
    // connecting (the socket must exist).
    char go[8]{};
    if (::recv(parent.fd, go, sizeof(go) - 1, 0) <= 0 || std::strncmp(go, "ready", 5) != 0)
        return 1;
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    // The shell's own loop shape (lx.shell.cppm: the context's event loop exists but is
    // never run): every event comes from a direct dispatch, every frame from the owner's
    // after_dispatch — the menu's describe is indexed by the level guard it installs, so
    // a frame the context's own frame source would drive would describe nothing.
    lx::runtime::event_loop idle_loop{};
    lx::wayland::display d{};
    if (!d.connect({.name = k_socket}).ok())
        return 1;
    host::context ctx{};
    host::context_options co{};
    co.backend = lx::gfx::client_tier::cpu;
    co.theme = "one-dark";
    co.watch_theme = 0;
    co.async_icons = false;
    if (!ctx.start(d, idle_loop, co))
        return 1;
    host::surface panel{};
    host::layer_role panel_role{};
    // One iteration of the shell's loop: dispatch the wire, then the owners' frames.
    auto step = [&] {
        (void)d.dispatch(0);
        if (panel.valid() && panel.frame_due())
            panel.run_frame();
    };
    auto pump = [&](int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        do {
            step();
            std::this_thread::sleep_for(std::chrono::microseconds(800));
        } while (std::chrono::steady_clock::now() < end);
    };
    auto met = [&](const std::function<bool()>& done, int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (!done()) {
            step();
            if (std::chrono::steady_clock::now() >= end)
                return done();
            std::this_thread::sleep_for(std::chrono::microseconds(800));
        }
        return true;
    };
    host::surface_options so{};
    so.describe = &describe_panel;
    so.size = {1280, 32};
    so.frame_records = false;
    host::layer_role_options lo{};
    lo.layer.level = lx::wayland::layer::top;
    lo.layer.name_space = "lumen-test-shell-menu";
    lo.size = {0, 32};
    lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_left | lx::wayland::anchor_right;
    lo.exclusive_zone = 32;
    lo.keyboard = lx::wayland::keyboard_interactivity::on_demand;
    const bool made =
        panel.create(ctx, so).ok() && panel_role.create(panel, lo).ok() && panel.show().ok();
    const bool presented =
        made && met([&] { return panel.timeline().first_presented_ns != 0; }, 5000);
    verify("panel layer surface presents", presented);

    const unsigned serial = [&] {
        (void)parent.call("move 600 16");
        (void)parent.call("button 1");
        pump(60);
        const unsigned s = ctx.last_press_serial();
        (void)parent.call("button 0");
        pump(60);
        return s;
    }();
    const bool focused =
        presented && serial != 0 && met([&] { return ctx.keyboard_focus() == &panel; }, 2000);
    verify("panel takes the keyboard after the press", focused, std::format("serial {}", serial));

    lx::shell::menu_popup popup{};
    popup.init(lx::shell::menu_globals{&ctx}, nullptr, nullptr);
    const lx::rect2i anchor{580, 0, 40, 32};
    auto open_menu = [&](unsigned press) {
        std::vector<lx::shell::menu_entry> items;
        lx::shell::menu_entry open{};
        open.label = "_Open";
        open.id = 1;
        items.push_back(open);
        lx::shell::menu_entry sep{};
        sep.separator = true;
        items.push_back(sep);
        lx::shell::menu_entry quit{};
        quit.label = "_Quit";
        quit.id = 2;
        items.push_back(quit);
        return popup.open(panel_role, anchor, std::move(items), press, &on_menu_activate,
                          &on_menu_closed, nullptr, nullptr, "test");
    };
    const auto keyboard_on_menu = [&] {
        return ctx.keyboard_focus() && ctx.keyboard_focus() != &panel;
    };

    // Enter on the first row: the delivery itself must survive the close it triggers.
    // after_dispatch runs right after the open and inside every wait, as the shell's
    // loop does.
    const auto menu_focused_after_open = [&]() -> bool {
        return met(
            [&] {
                popup.after_dispatch();
                return keyboard_on_menu();
            },
            5000);
    };
    bool opened = focused && open_menu(serial);
    if (opened)
        popup.after_dispatch();
    const bool menu_focused = opened && menu_focused_after_open();
    verify("grabbed menu takes the keyboard", menu_focused);
    if (menu_focused) {
        (void)parent.call("key 108 1"); // KEY_DOWN
        (void)parent.call("key 108 0");
        pump(80);
        (void)parent.call("key 28 1"); // KEY_ENTER
        (void)parent.call("key 28 0");
        const bool closed_by_enter = met(
            [&] {
                popup.after_dispatch();
                return !popup.is_open();
            },
            2000);
        verify("Enter activates the first row and closes the menu",
               closed_by_enter && g_menu_activations == 1 && g_menu_id == 1 && g_menu_closes == 1,
               std::format("activations {} id {} closes {}", g_menu_activations, g_menu_id,
                           g_menu_closes));
    }

    // Escape: the same delivery window from the keyboard's dismiss path.
    const unsigned serial2 = [&] {
        (void)parent.call("move 600 16");
        (void)parent.call("button 1");
        pump(60);
        const unsigned s = ctx.last_press_serial();
        (void)parent.call("button 0");
        pump(60);
        return s;
    }();
    opened = serial2 != 0 && open_menu(serial2);
    if (opened)
        popup.after_dispatch();
    const bool menu2_focused = opened && menu_focused_after_open();
    if (menu2_focused) {
        (void)parent.call("key 1 1"); // KEY_ESC
        (void)parent.call("key 1 0");
        const bool closed_by_escape = met(
            [&] {
                popup.after_dispatch();
                return !popup.is_open();
            },
            2000);
        verify("menu reopens after the Enter close, Escape closes it",
               closed_by_escape && g_menu_closes == 2, std::format("closes {}", g_menu_closes));
    } else {
        verify("menu reopens after the Enter close", false);
    }

    // A click outside: the compositor's popup_done, closed out of the Wayland listener.
    const unsigned serial3 = [&] {
        (void)parent.call("move 600 16");
        (void)parent.call("button 1");
        pump(60);
        const unsigned s = ctx.last_press_serial();
        (void)parent.call("button 0");
        pump(60);
        return s;
    }();
    opened = serial3 != 0 && open_menu(serial3);
    if (opened)
        popup.after_dispatch();
    const bool menu3_focused = opened && menu_focused_after_open();
    if (menu3_focused) {
        // The press must be outside the grabbing client's surfaces (the panel is the same
        // client as the popup): the empty output below the panel.
        (void)parent.call("move 640 400");
        (void)parent.call("button 1");
        (void)parent.call("button 0");
        const bool closed_by_done = met(
            [&] {
                popup.after_dispatch();
                return !popup.is_open();
            },
            2000);
        verify("menu reopens after the Escape close, outside click dismisses it",
               closed_by_done && g_menu_closes == 3, std::format("closes {}", g_menu_closes));
    } else {
        verify("menu reopens after the Escape close", false);
    }

    if (popup.is_open())
        popup.close();
    panel_role.destroy();
    panel.destroy();
    popup.after_dispatch();
    ctx.stop();
    d.disconnect();
    std::printf("%d checks, %d failed\n", g_ran, g_failed);
    return g_failed;
}

int run_child(pid_t child, int fd) {
    int status = -1;
    double last_tick = 0;
    const auto start = std::chrono::steady_clock::now();
    char buf[256];
    while (::waitpid(child, &status, WNOHANG) != child) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(60)) {
            ::kill(child, SIGKILL);
            (void)::waitpid(child, &status, 0);
            break;
        }
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 2) > 0 && (p.revents & POLLIN)) {
            const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                const std::string out = serve(buf);
                (void)::send(fd, out.data(), out.size(), MSG_NOSIGNAL);
            }
        }
        const double elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (elapsed - last_tick >= 16.6) {
            last_tick = elapsed;
            tick();
        }
    }
    ::close(fd);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

} // namespace

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const int status = lumen_test::run_all(argc, argv);
    if (status != 0)
        return status;
    // The in-process compositor phase: a crash anywhere in it (the ASan abort of the
    // close's use-after-free, pre-fix) fails the test as a whole. The client half is
    // forked BEFORE the compositor exists, so it inherits no live compositor state —
    // only the pre-main module globals both halves share.
    int sv[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0)
        return 1;
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(sv[0]);
        parent_link parent{sv[1]};
        const int failed = menu_popup_close_lifecycle(parent);
        ::close(sv[1]);
        ::_exit(failed);
    }
    ::close(sv[1]);
    lx::compositor::config cfg{};
    cfg.socket_name = k_socket;
    cfg.target_fps = 60.0;
    cfg.present = lx::compositor::present_backend::cpu;
    cfg.virtual_outputs = "1280x800";
    cfg.ping_timeout_ms = 0;
    lx::compositor::compositor comp{cfg};
    ::setenv("VK_LOADER_DRIVERS_DISABLE", "*", 1);
    const auto started = comp.start();
    ::unsetenv("VK_LOADER_DRIVERS_DISABLE");
    if (!started) {
        ::kill(child, SIGKILL);
        (void)::waitpid(child, nullptr, 0);
        return 1;
    }
    std::atomic<bool> pump{true};
    std::mutex lock;
    g_comp = &comp;
    g_lock = &lock;
    std::thread worker([&] {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int fd = comp.wayland().event_fd();
        while (pump.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> hold(lock);
                (void)comp.wayland().dispatch(0);
                comp.wayland().flush();
            }
            pollfd p{fd, POLLIN, 0};
            (void)::poll(&p, 1, 2);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    tick();
    (void)::send(sv[0], "ready", 5, MSG_NOSIGNAL);
    const int failed = run_child(child, sv[0]);
    pump.store(false, std::memory_order_release);
    worker.join();
    return failed;
}
