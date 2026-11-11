// Native calendar popup input against an in-process compositor.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>

import std;
import lx.foundation;
import lx.config;
import lx.gfx;
import lx.runtime;
import lx.scheduler;
import lx.input;
import lx.compositor;
import lx.ui;
import lx.ui.host;
import lx.wayland.client;
import lx.shell;

namespace {
namespace host = lx::ui::host;
constexpr const char* k_socket = "lumen-test-clock-popup";
lx::compositor::compositor* g_comp = nullptr;
std::mutex* g_lock = nullptr;
unsigned long long g_frame = 0;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
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
    std::string reply = "ok";
    if (cmd == "record") {
        std::string app; in >> app;
        const lx::compositor::toplevel_record* found=nullptr;
        g_comp->toplevels().for_each([&](const lx::compositor::toplevel_record& r){
            if(r.app_id && app==r.app_id && r.mapped && (!found || r.id.id()>found->id.id())) found=&r;
        });
        reply=found?std::format("ok 1 {} {} {} {}",found->geometry.x,found->geometry.y,found->geometry.width,found->geometry.height):"ok 0 0 0 0 0";
    } else if (cmd == "move") {
        double x = 0, y = 0;
        in >> x >> y;
        lx::input::pointer_motion_event e{};
        e.x = x; e.y = y;
        g_comp->inject_pointer_motion(e);
    } else if (cmd == "button") {
        int down = 0; in >> down;
        lx::input::pointer_button_event e{};
        e.which = lx::input::button::left; e.evdev_code = 0x110; e.pressed = down != 0;
        g_comp->inject_pointer_button(e);
    } else if (cmd == "key") {
        unsigned code = 0; int down = 0; in >> code >> down;
        lx::input::key_event e{}; e.evdev_keycode = code; e.pressed = down != 0;
        g_comp->inject_key(e);
    } else {
        reply = "fail unknown command";
    }
    g_comp->wayland().flush();
    return reply;
}
int run_child() {
    int sv[2] = {-1,-1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return 1;
    std::fflush(stdout);
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(sv[0]);
        const int fd = ::dup(sv[1]);
        char self[] = "/proc/self/exe", flag[] = "--client", f[24];
        std::snprintf(f, sizeof(f), "%d", fd);
        char* argv[] = {self, flag, f, nullptr};
        ::execv(self, argv);
        ::_exit(127);
    }
    ::close(sv[1]);
    int status = -1;
    double last_tick = 0;
    const auto start = std::chrono::steady_clock::now();
    char buf[256];
    while (::waitpid(child, &status, WNOHANG) != child) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(60)) {
            ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); break;
        }
        pollfd p{sv[0], POLLIN, 0};
        if (::poll(&p, 1, 2) > 0 && (p.revents & POLLIN)) {
            const ssize_t n = ::recv(sv[0], buf, sizeof(buf)-1, 0);
            if (n > 0) {
                buf[n] = 0;
                const std::string out = serve(buf);
                (void)::send(sv[0], out.data(), out.size(), MSG_NOSIGNAL);
            }
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed - last_tick >= 16.6) { last_tick = elapsed; tick(); }
    }
    ::close(sv[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
struct parent_link {
    int fd = -1;
    std::string call(const std::string& cmd) {
        (void)::send(fd, cmd.data(), cmd.size(), MSG_NOSIGNAL);
        char buf[128];
        const ssize_t n = ::recv(fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) return "fail";
        buf[n] = 0; return buf;
    }
};
void run_for(lx::runtime::event_loop& loop, int ms);
unsigned press_serial(parent_link& p, lx::runtime::event_loop& loop, host::context& ctx, double x, double y) {
    (void)p.call(std::format("move {} {}", x, y));
    (void)p.call("button 1");
    run_for(loop,40);
    const unsigned serial=ctx.last_press_serial();
    (void)p.call("button 0");
    run_for(loop,40);
    return serial;
}
void click(parent_link& p, double x, double y) {
    (void)p.call(std::format("move {} {}", x, y));
    (void)p.call("button 1");
    (void)p.call("button 0");
}
bool wait_for(lx::runtime::event_loop& loop, const std::function<bool()>& done, int ms) {
    if (done()) return true;
    bool met = false;
    lx::runtime::timer_source poll{1'000'000, lx::runtime::task{[&] {
        if (done()) { met = true; loop.quit(0); }
    }}};
    lx::runtime::timer_source limit{0, lx::runtime::task{[&] { loop.quit(0); }}, false};
    limit.schedule_at(loop.now() + static_cast<long long>(ms) * 1'000'000);
    loop.add_source(&poll); loop.add_source(&limit); (void)loop.run();
    loop.remove_source(&limit); loop.remove_source(&poll);
    return met || done();
}
void run_for(lx::runtime::event_loop& loop, int ms) {
    lx::runtime::timer_source limit{0, lx::runtime::task{[&loop] { loop.quit(0); }}, false};
    limit.schedule_at(loop.now() + static_cast<long long>(ms) * 1'000'000);
    loop.add_source(&limit); (void)loop.run(); loop.remove_source(&limit);
}
int g_failed = 0;
int g_ran = 0;
constexpr const char* k_app_other = "org.lumen.test.clock.other";
struct other_client {
    lx::wayland::display d{};
    lx::wayland::surface s{};
    lx::wayland::toplevel t{};
    lx::wayland::shm_pool pool{};
    lx::wayland::buffer b{};
    struct events final : lx::wayland::toplevel_handler {
        std::uint32_t serial = 0;
        void on_configure(lx::wayland::toplevel&, const lx::wayland::toplevel_configure& c) override { serial=c.serial; }
    } ev{};
    bool open() {
        if(!d.connect({.name=k_socket,.track_outputs=false,.default_dmabuf_feedback=false}).ok() ||
           !s.create(d).ok() || !t.create(d,s,{.app_id=k_app_other}).ok()) return false;
        t.set_handler(&ev); s.commit();
        for(int i=0;i<200 && !ev.serial;++i) if(!d.roundtrip().ok()) return false;
        if(!ev.serial) return false;
        t.ack_configure(ev.serial);
        if(!pool.create(d,96*96*4).ok()) return false;
        auto* px=reinterpret_cast<std::uint32_t*>(pool.data());
        for(int i=0;i<96*96;++i)px[i]=0xff206040u;
        if(!b.create_shm(pool,0,{96,96},96*4,0).ok()) return false;
        s.attach(&b);s.damage_buffer({0,0,96,96});s.commit();return d.roundtrip().ok();
    }
    void close(){t.destroy();b.destroy();pool.destroy();s.destroy();d.disconnect();}
};
lx::rect2i query_rect(parent_link& p,const std::string& cmd){
    std::istringstream in{p.call(cmd)};std::string ok;int mapped=0;lx::rect2i r{};
    in>>ok>>mapped>>r.x>>r.y>>r.width>>r.height;return r;
}
void verify(const char* name, bool ok, std::string detail = {}) {
    ++g_ran;
    std::printf("  %s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail.empty() ? "" : " — ", detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failed;
}
lx::ui::child describe_panel(lx::ui::build_context&) {
    using namespace lx::ui;
    const child kids[1] = {button("Clock").name("clock")};
    return div(style{}.size_full().bg(tok::title_bar_background).flex_col().items_center().justify_center(), kids);
}
bool write_ppm(const host::surface& s, const char* path) {
    const auto* px=s.cpu_pixels();
    if(!px || !px->pixels) return false;
    FILE* f=std::fopen(path,"wb");
    if(!f) return false;
    std::fprintf(f,"P6\n%u %u\n255\n",px->width,px->height);
    for(unsigned y=0;y<px->height;++y) {
        const auto* row=reinterpret_cast<const std::uint32_t*>(px->pixels+static_cast<std::size_t>(y)*px->stride);
        for(unsigned x=0;x<px->width;++x) {
            const unsigned char rgb[3]={static_cast<unsigned char>(row[x]>>16),static_cast<unsigned char>(row[x]>>8),static_cast<unsigned char>(row[x])};
            (void)std::fwrite(rgb,1,3,f);
        }
    }
    return std::fclose(f)==0;
}
void on_settings(unsigned serial, void* user) {
    auto* v = static_cast<std::pair<unsigned, unsigned>*>(user);
    v->first = serial; ++v->second;
}
int client_main(int fd) {
    parent_link parent{fd};
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::runtime::event_loop loop{};
    lx::wayland::display d{};
    if (!d.connect({.name = k_socket}).ok()) return 1;
    lx::wayland::display_source source{d, loop, lx::runtime::task{[&] { loop.quit(1); }}};
    source.attach();
    host::context ctx{};
    host::context_options co{};
    co.backend = lx::gfx::client_tier::cpu; co.theme = "one-dark";
    co.watch_theme = 0; co.async_icons = false;
    if (!ctx.start(d, loop, co)) return 1;
    host::surface panel{};
    host::layer_role panel_role{};
    host::surface_options so{}; so.describe = &describe_panel; so.size = {1280,32}; so.frame_records = false;
    host::layer_role_options lo{};
    lo.layer.level = lx::wayland::layer::top; lo.layer.name_space = "lumen-test-clock";
    lo.size = {0,32}; lo.anchor = lx::wayland::anchor_top | lx::wayland::anchor_left | lx::wayland::anchor_right;
    lo.exclusive_zone = 32; lo.keyboard = lx::wayland::keyboard_interactivity::on_demand;
    bool made = panel.create(ctx,so).ok() && panel_role.create(panel,lo).ok() && panel.show().ok();
    if (!made || !wait_for(loop,[&]{return panel.timeline().first_presented_ns != 0;},5000)) return 1;
    const unsigned serial = press_serial(parent,loop,ctx,600,16);
    const bool got_focus = wait_for(loop,[&]{return ctx.keyboard_focus() == &panel;},2000);
    std::pair<unsigned,unsigned> settings{};
    lx::shell::calendar_popup popup{}; popup.init(ctx);
    const lx::rect2i anchor{580,0,40,32};
    lx::config::clock_settings opts{};
    bool opened = got_focus && serial != 0 && popup.open(panel_role,anchor,serial,opts,1'799'000'000,&on_settings,&settings);
    auto* surface = popup.host_surface();
    const bool presented = opened && wait_for(loop,[&]{return surface->timeline().first_presented_ns != 0;},5000);
    const auto cfg = popup.popup_parent().popup().last_configure();
    verify("valid press serial opens below the panel with an anchored popup",presented && popup.is_open() &&
               cfg.geometry.width == lx::shell::k_calendar_width && cfg.geometry.height == lx::shell::k_calendar_height &&
               cfg.geometry.x == anchor.x + anchor.width - lx::shell::k_calendar_width - 12 &&
               cfg.geometry.y == anchor.y + anchor.height + 8,
           std::format("serial {} geometry {}x{}+{}+{}",serial,cfg.geometry.width,cfg.geometry.height,cfg.geometry.x,cfg.geometry.y));
    const auto initial = popup.model();
    const auto initial_selected = initial.selected;
    const auto initial_month = initial.displayed;
    const bool grid_ok = initial.cells[0].date.ok() && initial.cells[41].date.ok() && initial.selected.ok();
    verify("calendar model exposes complete 42-day grid",grid_ok,
           std::format("month {}-{}",int(initial_month.year()),unsigned(initial_month.month())));
    if (presented) {
        run_for(loop, 80);
        const bool artifact = write_ppm(*surface, "calendar-popup.ppm");
        const auto* pixels = surface->cpu_pixels();
        bool nonempty = pixels && pixels->pixels;
        if (nonempty) {
            const auto* p = reinterpret_cast<const std::uint32_t*>(pixels->pixels);
            const auto first = p[0];
            nonempty = false;
            for (unsigned y=0; y<pixels->height && !nonempty; ++y) {
                const auto* row=reinterpret_cast<const std::uint32_t*>(pixels->pixels+static_cast<std::size_t>(y)*pixels->stride);
                for (unsigned x=0; x<pixels->width; ++x) if(row[x]!=first){nonempty=true;break;}
            }
        }
        verify("CPU popup readback is nonempty and saved", artifact && nonempty, "calendar-popup.ppm");
        unsigned focus_cell=0;
        while(focus_cell<41 && (!popup.model().cells[focus_cell].in_month || !popup.model().cells[focus_cell+1].in_month ||
                              popup.model().cells[focus_cell].date==initial_selected)) ++focus_cell;
        const auto clicked_day=popup.model().cells[focus_cell].date;
        const int px=cfg.geometry.x+lx::shell::k_calendar_grid_x+int(focus_cell%7)*lx::shell::k_calendar_cell_w+lx::shell::k_calendar_cell_w/2;
        const int py=cfg.geometry.y+lx::shell::k_calendar_grid_y+int(focus_cell/7)*lx::shell::k_calendar_cell_h+lx::shell::k_calendar_cell_h/2;
        click(parent,px,py);
        run_for(loop,80); popup.after_dispatch();
        verify("pointer selects an in-month day",popup.model().selected==clicked_day && clicked_day!=initial_selected,
               std::format("selected {}-{}-{}",int(clicked_day.year()),unsigned(clicked_day.month()),unsigned(clicked_day.day())));
        (void)parent.call("key 106 1"); // KEY_RIGHT
        (void)parent.call("key 106 0");
        run_for(loop,80); popup.after_dispatch();
        const auto right_day=popup.model().selected;
        verify("Right advances from the focused calendar day",right_day!=clicked_day);
        (void)parent.call("key 28 1"); // KEY_ENTER
        (void)parent.call("key 28 0");
        run_for(loop,80); popup.after_dispatch();
        verify("Enter preserves keyboard-selected day instead of old click target",popup.model().selected==right_day);
        (void)parent.call("key 57 1"); // KEY_SPACE
        (void)parent.call("key 57 0");
        run_for(loop,80); popup.after_dispatch();
        verify("Space preserves keyboard-selected day instead of old click target",popup.model().selected==right_day);
        const auto month_before = popup.model().displayed;
        click(parent,cfg.geometry.x+lx::shell::k_calendar_next_month.x+lx::shell::k_calendar_next_month.width/2,
              cfg.geometry.y+lx::shell::k_calendar_next_month.y+lx::shell::k_calendar_next_month.height/2);
        run_for(loop,80); popup.after_dispatch();
        verify("pointer navigates to next month",popup.model().displayed != month_before);
        const auto before_today = popup.model().selected;
        click(parent,cfg.geometry.x+lx::shell::k_calendar_today.x+lx::shell::k_calendar_today.width/2,
              cfg.geometry.y+lx::shell::k_calendar_today.y+lx::shell::k_calendar_today.height/2);
        run_for(loop,80); popup.after_dispatch();
        verify("Today selects the current day",popup.model().selected == popup.model().today && popup.model().selected != before_today);
        const auto before_home = popup.model().selected;
        (void)parent.call("key 102 1"); // KEY_HOME
        (void)parent.call("key 102 0");
        run_for(loop,80); popup.after_dispatch();
        const auto home_date=popup.model().selected;
        const auto before_end=home_date;
        (void)parent.call("key 107 1"); // KEY_END
        (void)parent.call("key 107 0");
        run_for(loop,80); popup.after_dispatch();
        verify("Home and End move within the current week",home_date != before_home && popup.model().selected != before_end);
        opts.week_numbers=true;
        opts.first_day=lx::config::clock_first_day::sunday;
        popup.configure(opts,1'799'000'000);
        run_for(loop,80); popup.after_dispatch();
        const auto first_weekday=std::chrono::weekday{std::chrono::sys_days{popup.model().cells[0].date}}.c_encoding();
        verify("week-number mode starts Sunday and keeps 42 valid cells",first_weekday==0 &&
                   popup.model().cells[0].date.ok() && popup.model().cells[41].date.ok());
        unsigned last_column=6;
        for(unsigned row=0;row<6;++row)
            if(popup.model().cells[row*7+6].in_month){last_column=row*7+6;break;}
        const auto last_day=popup.model().cells[last_column].date;
        const int last_x=cfg.geometry.x+lx::shell::k_calendar_grid_x+lx::shell::k_calendar_week_gutter+6*lx::shell::k_calendar_cell_w+lx::shell::k_calendar_cell_w/2;
        const int last_y=cfg.geometry.y+lx::shell::k_calendar_grid_y+int(last_column/7)*lx::shell::k_calendar_cell_h+lx::shell::k_calendar_cell_h/2;
        click(parent,last_x,last_y);
        run_for(loop,80); popup.after_dispatch();
        verify("week-number layout last column selects within popup bounds",last_day.ok() &&
                   popup.model().selected==last_day && last_x < cfg.geometry.x+cfg.geometry.width);
        opts.week_numbers=false;
        opts.first_day=lx::config::clock_first_day::monday;
        popup.configure(opts,1'799'000'000);
        run_for(loop,80); popup.after_dispatch();
        (void)parent.call("key 1 1"); // Escape
        (void)parent.call("key 1 0");
        const bool escaped = wait_for(loop,[&]{popup.after_dispatch(); return !popup.is_open();},2000);
        verify("Escape dismisses the popup",escaped);
    }
    if (popup.is_open()) popup.close();
    run_for(loop,60);
    // A new press obtains a fresh valid serial for reopen and settings callback tests.
    const unsigned serial2=press_serial(parent,loop,ctx,600,16);
    opened = serial2 != 0 && popup.open(panel_role,anchor,serial2,opts,1'799'000'000,&on_settings,&settings);
    const bool reopened=opened && wait_for(loop,[&]{return popup.host_surface()->timeline().first_presented_ns != 0;},5000);
    verify("popup can reopen after dismissal",reopened);
    if (reopened) {
        // Exercise a valid nested popup press on a separate client, which must end the grab.
        other_client other{};
        const bool other_ready = other.open();
        const auto other_record = query_rect(parent,std::string{"record "}+k_app_other);
        click(parent,other_record.x+other_record.width/2.0,other_record.y+other_record.height/2.0);
        const bool outside_done = wait_for(loop,[&]{popup.after_dispatch();return !popup.is_open();},2000);
        verify("outside-client click dismisses grabbed popup",other_ready && outside_done,"xdg_popup.done");
        other.close();
    }
    const unsigned serial3=press_serial(parent,loop,ctx,600,16);
    opened = serial3 != 0 && popup.open(panel_role,anchor,serial3,opts,1'799'000'000,&on_settings,&settings);
    const bool settings_reopened=opened && wait_for(loop,[&]{return popup.host_surface()->timeline().first_presented_ns != 0;},5000);
    verify("popup reopens for settings callback",settings_reopened);
    if (settings_reopened) {
        const auto popup_geometry=popup.popup_parent().popup().last_configure().geometry;
        click(parent,popup_geometry.x+lx::shell::k_calendar_settings.x+lx::shell::k_calendar_settings.width/2,
              popup_geometry.y+lx::shell::k_calendar_settings.y+lx::shell::k_calendar_settings.height/2);
        run_for(loop,80); popup.after_dispatch();
        verify("settings action reports the activating press serial",settings.second == 1 && settings.first != 0,
               std::format("callback count {} serial {}",settings.second,settings.first));
    }
    const unsigned idle_serial=press_serial(parent,loop,ctx,600,16);
    const bool idle_open=idle_serial && popup.open(panel_role,anchor,idle_serial,opts,1'799'000'000,&on_settings,&settings) &&
        wait_for(loop,[&]{return popup.host_surface()->timeline().first_presented_ns != 0;},5000);
    const auto commits=popup.host_surface()->stats().commits;
    const auto frames=popup.host_surface()->stats().frame_requests;
    run_for(loop,500); popup.after_dispatch();
    verify("open popup stays idle without commits or frame requests",idle_open &&
           popup.host_surface()->stats().commits == commits && popup.host_surface()->stats().frame_requests == frames);
    if (popup.is_open()) popup.close();
    panel_role.destroy(); panel.destroy();
    popup.after_dispatch();
    ctx.stop(); source.detach(); d.disconnect();
    std::printf("%d checks, %d failed\n",g_ran,g_failed);
    return g_failed;
}
} // namespace
int main(int argc,char** argv) {
    if(argc==3 && std::strcmp(argv[1],"--client")==0) return client_main(std::atoi(argv[2]));
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::compositor::config cfg{}; cfg.socket_name=k_socket; cfg.target_fps=60.0;
    cfg.present=lx::compositor::present_backend::cpu; cfg.virtual_outputs="1280x800"; cfg.ping_timeout_ms=0;
    lx::compositor::compositor comp{cfg};
    ::setenv("VK_LOADER_DRIVERS_DISABLE","*",1);
    auto started=comp.start(); ::unsetenv("VK_LOADER_DRIVERS_DISABLE");
    if(!started) return 1;
    std::atomic<bool> pump{true}; std::mutex lock; g_comp=&comp; g_lock=&lock;
    std::thread worker([&]{
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        const int fd=comp.wayland().event_fd();
        while(pump.load(std::memory_order_acquire)) {
            { std::lock_guard<std::mutex> hold(lock); (void)comp.wayland().dispatch(0); comp.wayland().flush(); }
            pollfd p{fd,POLLIN,0}; (void)::poll(&p,1,2);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); tick();
    const int failed=run_child(); pump.store(false,std::memory_order_release); worker.join();
    return failed;
}
