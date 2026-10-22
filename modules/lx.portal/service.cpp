module;

// Bodies of `:service`. See service.cppm for the model.

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#if defined(LUMEN_PORTAL_DBUS)
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#endif

module lx.portal;

import lx.foundation;
import lx.trace;
import :compose;
import :chooser;
import :desktop;
import :files;
import :overlay;
import :png;
import :service;

namespace lx::portal {

namespace {

constexpr const char* k_category = "portal";
constexpr int k_capture_timeout_ms = 3000;
constexpr long k_private_max_age_s = 3600;

void log_line(lx::trace::level lvl, const char* fmt, auto... args) {
    if constexpr (sizeof...(args) == 0) {
        lx::trace::logger::global().log(lvl, k_category, fmt);
    } else {
        char buf[1024];
        std::snprintf(buf, sizeof buf, fmt, args...);
        lx::trace::logger::global().log(lvl, k_category, buf);
    }
}

double now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

/// Saves `img` as a new PNG and returns its path (empty on failure, logged).
std::string save_screenshot(const image& img, bool interactive) {
    std::string dir;
    std::string stem;
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    if (interactive) {
        dir = pictures_dir(std::getenv("HOME"), std::getenv("XDG_CONFIG_HOME"), std::getenv("XDG_PICTURES_DIR")) +
              "/Screenshots";
        stem = screenshot_stem(local);
        if (auto r = make_dirs(dir, 0755); !r.ok()) {
            lx::trace::logger::global().log_error(r.get_error(), k_category);
            return {};
        }
    } else {
        const char* runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || runtime[0] != '/') {
            log_line(lx::trace::level::error, "XDG_RUNTIME_DIR is not set: nowhere private to save to");
            return {};
        }
        dir = std::string(runtime) + "/lumen-portal";
        if (auto r = make_dirs(dir, 0700); !r.ok()) {
            lx::trace::logger::global().log_error(r.get_error(), k_category);
            return {};
        }
        (void)prune_older(dir, "Screenshot-", k_private_max_age_s);
        char buf[64];
        std::strftime(buf, sizeof buf, "Screenshot-%Y%m%d-%H%M%S", &local);
        stem = buf;
    }
    std::string path;
    lx::result<int> fd = create_unique(dir, stem, !interactive, path);
    if (!fd.ok()) {
        lx::trace::logger::global().log_error(fd.get_error(), k_category);
        return {};
    }
    const lx::result<void> wrote = write_png(fd.value(), img);
    const bool closed = ::close(fd.value()) == 0;
    if (!wrote.ok() || !closed) {
        ::unlink(path.c_str());
        if (!wrote.ok())
            lx::trace::logger::global().log_error(wrote.get_error(), k_category);
        return {};
    }
    return path;
}

/// Whether the user's seat session is locked (logind's LockedHint, which Lumen's compositor
/// sets with ext-session-lock): the portal then captures nothing and answers 2 — the
/// compositor would only deliver the black lock curtain, and a black "screenshot" helps no
/// one. The session is seat0's active one when it is this user's (the portal itself runs
/// as a user service, outside it), else logind's display session of the user.
bool seat_session_locked() {
#if defined(LUMEN_PORTAL_DBUS)
    char* session = nullptr;
    uid_t uid = 0;
    if (sd_seat_get_active("seat0", &session, &uid) < 0 || uid != getuid()) {
        std::free(session);
        session = nullptr;
        if (sd_uid_get_display(getuid(), &session) < 0 || !session)
            return false;
    }
    sd_bus* sys = nullptr;
    char* path = nullptr;
    int locked = 0;
    if (sd_bus_open_system(&sys) >= 0 &&
        sd_bus_path_encode("/org/freedesktop/login1/session", session, &path) >= 0) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        if (sd_bus_get_property_trivial(sys, "org.freedesktop.login1", path, "org.freedesktop.login1.Session",
                                        "LockedHint", &err, 'b', &locked) < 0)
            locked = 0;
        sd_bus_error_free(&err);
    }
    std::free(path);
    std::free(session);
    if (sys)
        sd_bus_flush_close_unref(sys);
    return locked != 0;
#else
    return false;
#endif
}

} // namespace

#if defined(LUMEN_PORTAL_DBUS) && defined(LUMEN_PORTAL_WAYLAND)

namespace {

struct request_state {
    enum class kind : unsigned char { screenshot, pick };
    kind what = kind::screenshot;
    sd_bus_message* call = nullptr; // the method call, answered when the request ends
    sd_bus_slot* slot = nullptr;    // the Request object at `handle`
    std::string handle;
    std::string app_id;
    bool interactive = false;
    bool closed = false; // Request.Close arrived: answered 2 outside the callback
    unsigned target = 0;
    double started_ms = 0;
    std::unique_ptr<desktop> wl;
    std::vector<output_shot> shots;
    std::vector<std::string> window_ids;
    std::unique_ptr<overlay> ov;
};

class service {
public:
    explicit service(const service_options& o) : options_{o} {}
    ~service();
    [[nodiscard]] int run();

    // sd-bus callbacks (the vtables below refer to them).
    static int on_screenshot(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_pick_color(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_close(sd_bus_message* m, void* user, sd_bus_error* err);
    static int on_name_lost(sd_bus_message* m, void* user, sd_bus_error* err);
    static int get_version(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                           sd_bus_error*);
    static int get_targets(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                           sd_bus_error*);

private:
    int accept(sd_bus_message* m, request_state::kind what);
    void start();
    void after_events();
    void complete_overlay();
    void answer(unsigned response, const std::string& uri = {}, const std::uint32_t* colour = nullptr);
    void save_and_answer(const image& img, const char* what);

    service_options options_;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* object_slot_ = nullptr;
    sd_bus_slot* match_slot_ = nullptr;
    std::unique_ptr<request_state> req_;
    bool quit_ = false;
};

const sd_bus_vtable k_screenshot_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Screenshot", "ossa{sv}", "ua{sv}", service::on_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PickColor", "ossa{sv}", "ua{sv}", service::on_pick_color, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("version", "u", service::get_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("AvailableTargets", "u", service::get_targets, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END};

const sd_bus_vtable k_request_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", service::on_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

} // namespace

service::~service() {
    if (req_) {
        if (req_->call)
            sd_bus_message_unref(req_->call);
        if (req_->slot)
            sd_bus_slot_unref(req_->slot);
    }
    if (match_slot_)
        sd_bus_slot_unref(match_slot_);
    if (object_slot_)
        sd_bus_slot_unref(object_slot_);
    if (bus_) {
        sd_bus_flush(bus_);
        sd_bus_unref(bus_);
    }
}

int service::get_version(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                         sd_bus_error*) {
    return sd_bus_message_append(reply, "u", k_screenshot_version);
}

int service::get_targets(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*,
                         sd_bus_error*) {
    return sd_bus_message_append(reply, "u", k_screenshot_targets);
}

int service::on_name_lost(sd_bus_message* m, void* user, sd_bus_error*) {
    const char* name = nullptr;
    if (sd_bus_message_read(m, "s", &name) >= 0 && name && std::strcmp(name, k_bus_name) == 0) {
        log_line(lx::trace::level::info, "bus name taken over by another instance; exiting");
        static_cast<service*>(user)->quit_ = true;
    }
    return 0;
}

int service::accept(sd_bus_message* m, request_state::kind what) {
    const char* handle = nullptr;
    const char* app_id = nullptr;
    const char* parent = nullptr;
    int r = sd_bus_message_read(m, "oss", &handle, &app_id, &parent);
    if (r < 0)
        return r;
    bool interactive = false;
    unsigned target = 0;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
        return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char* key = nullptr;
        if ((r = sd_bus_message_read(m, "s", &key)) < 0)
            return r;
        if (std::strcmp(key, "interactive") == 0 && sd_bus_message_verify_type(m, 'v', "b") > 0) {
            int b = 0;
            if ((r = sd_bus_message_read(m, "v", "b", &b)) < 0)
                return r;
            interactive = b != 0;
        } else if (std::strcmp(key, "target") == 0 && sd_bus_message_verify_type(m, 'v', "u") > 0) {
            uint32_t u = 0;
            if ((r = sd_bus_message_read(m, "v", "u", &u)) < 0)
                return r;
            target = u;
        } else if ((r = sd_bus_message_skip(m, "v")) < 0) {
            return r;
        }
        if ((r = sd_bus_message_exit_container(m)) < 0)
            return r;
    }
    if (r < 0)
        return r;
    sd_bus_message_exit_container(m);

    if (req_) {
        log_line(lx::trace::level::warn, "%s from '%s' while another request runs: answered 2",
                 what == request_state::kind::pick ? "PickColor" : "Screenshot", app_id);
        return sd_bus_reply_method_return(m, "ua{sv}", 2u, 0);
    }
    auto req = std::make_unique<request_state>();
    req->what = what;
    req->handle = handle;
    req->app_id = app_id;
    req->interactive = interactive;
    req->target = target;
    req->started_ms = now_ms();
    req->call = sd_bus_message_ref(m);
    r = sd_bus_add_object_vtable(bus_, &req->slot, handle, "org.freedesktop.impl.portal.Request", k_request_vtable,
                                 this);
    if (r < 0) {
        sd_bus_message_unref(req->call);
        return r;
    }
    log_line(lx::trace::level::info, "%s app='%s' interactive=%d target=%u",
             what == request_state::kind::pick ? "PickColor" : "Screenshot", app_id, interactive ? 1 : 0, target);
    req_ = std::move(req);
    start();
    return 1; // answered later (or already, by start)
}

int service::on_screenshot(sd_bus_message* m, void* user, sd_bus_error*) {
    return static_cast<service*>(user)->accept(m, request_state::kind::screenshot);
}

int service::on_pick_color(sd_bus_message* m, void* user, sd_bus_error*) {
    return static_cast<service*>(user)->accept(m, request_state::kind::pick);
}

int service::on_close(sd_bus_message* m, void* user, sd_bus_error*) {
    auto& s = *static_cast<service*>(user);
    if (s.req_ && s.req_->handle == sd_bus_message_get_path(m)) {
        log_line(lx::trace::level::info, "request closed by the caller");
        s.req_->closed = true;
    }
    return sd_bus_reply_method_return(m, "");
}

void service::answer(unsigned response, const std::string& uri, const std::uint32_t* colour) {
    if (!req_)
        return;
    sd_bus_message* call = req_->call;
    int r = 0;
    if (response == 0 && !uri.empty()) {
        r = sd_bus_reply_method_return(call, "ua{sv}", 0u, 1, "uri", "s", uri.c_str());
    } else if (response == 0 && colour) {
        const double red = ((*colour >> 16) & 0xFFu) / 255.0;
        const double green = ((*colour >> 8) & 0xFFu) / 255.0;
        const double blue = (*colour & 0xFFu) / 255.0;
        r = sd_bus_reply_method_return(call, "ua{sv}", 0u, 1, "color", "(ddd)", red, green, blue);
    } else {
        r = sd_bus_reply_method_return(call, "ua{sv}", response == 0 ? 2u : response, 0);
    }
    if (r < 0)
        log_line(lx::trace::level::warn, "could not answer the request: %s", std::strerror(-r));
    log_line(lx::trace::level::info, "request answered %u in %.0f ms%s%s", response, now_ms() - req_->started_ms,
             uri.empty() ? "" : " uri=", uri.c_str());
    sd_bus_message_unref(call);
    sd_bus_slot_unref(req_->slot);
    // The overlay before the connection it draws on.
    req_->ov.reset();
    req_->wl.reset();
    req_.reset();
}

void service::save_and_answer(const image& img, const char* what) {
    if (img.empty()) {
        answer(2);
        return;
    }
    const double t0 = now_ms();
    const std::string path = save_screenshot(img, req_->interactive || req_->target == 2u || req_->target == 4u);
    if (path.empty()) {
        answer(2);
        return;
    }
    log_line(lx::trace::level::info, "screenshot (%s) %dx%d written to %s in %.0f ms", what, img.width, img.height,
             path.c_str(), now_ms() - t0);
    answer(0, file_uri(path));
}

void service::start() {
    request_state& q = *req_;
    if (seat_session_locked()) {
        log_line(lx::trace::level::info, "the session is locked: nothing is captured");
        answer(2);
        return;
    }
    lx::result<std::unique_ptr<desktop>> wl = desktop::connect();
    if (!wl.ok()) {
        lx::trace::logger::global().log_error(wl.get_error(), k_category);
        answer(2);
        return;
    }
    q.wl = std::move(wl.value());
    const bool window_target = q.what == request_state::kind::screenshot && q.target == 2u;
    if (window_target) {
        // The user chooses a window; it is captured on its own once chosen.
        std::vector<chooser_entry> entries;
        for (const auto& t : q.wl->toplevels()) {
            if (t->closed || t->identifier.empty())
                continue;
            chooser_entry e;
            e.type = chooser_entry::kind::window;
            e.title = t->title.empty() ? (t->app_id.empty() ? "Untitled window" : t->app_id) : t->title;
            e.detail = t->app_id;
            e.line = t->identifier;
            q.window_ids.push_back(t->identifier);
            entries.push_back(std::move(e));
        }
        if (entries.empty()) {
            log_line(lx::trace::level::warn, "window screenshot: no window to choose from");
            answer(2);
            return;
        }
        q.ov = std::make_unique<overlay>(*q.wl);
        if (auto r = q.ov->start_choose(std::move(entries), "Take a screenshot of a window"); !r.ok()) {
            lx::trace::logger::global().log_error(r.get_error(), k_category);
            answer(2);
        }
        return;
    }
    lx::result<std::vector<output_shot>> shots = q.wl->capture_outputs(false, k_capture_timeout_ms);
    if (!shots.ok()) {
        lx::trace::logger::global().log_error(shots.get_error(), k_category);
        answer(2);
        return;
    }
    q.shots = std::move(shots.value());
    if (q.what == request_state::kind::screenshot && !q.interactive && (q.target == 0u || q.target == 1u)) {
        const lx::rect2i all = layout_bounds(q.shots);
        save_and_answer(compose(q.shots, all, compose_scale(q.shots, all)), "all outputs");
        return;
    }
    q.ov = std::make_unique<overlay>(*q.wl);
    const lx::result<void> started =
        q.what == request_state::kind::pick ? q.ov->start_pick(q.shots) : q.ov->start_select(q.shots);
    if (!started.ok()) {
        lx::trace::logger::global().log_error(started.get_error(), k_category);
        answer(2);
    }
}

void service::complete_overlay() {
    request_state& q = *req_;
    const overlay_result res = q.ov->result();
    switch (res.what) {
    case overlay_result::kind::pending: return;
    case overlay_result::kind::cancelled: answer(1); return;
    case overlay_result::kind::failed:
        log_line(lx::trace::level::warn, "overlay failed: %s", res.reason);
        answer(2);
        return;
    case overlay_result::kind::colour: {
        const std::uint32_t c = res.colour;
        log_line(lx::trace::level::info, "colour picked #%06X", c & 0xFFFFFFu);
        answer(0, {}, &c);
        return;
    }
    case overlay_result::kind::choice: {
        if (res.choice < 0 || res.choice >= static_cast<int>(q.window_ids.size())) {
            answer(2);
            return;
        }
        const std::string id = q.window_ids[static_cast<std::size_t>(res.choice)];
        // The list goes before the capture, so the window is not shot under it (a window
        // capture never includes other surfaces anyway).
        q.ov.reset();
        lx::result<image> img = q.wl->capture_toplevel(id, false, k_capture_timeout_ms);
        if (!img.ok()) {
            lx::trace::logger::global().log_error(img.get_error(), k_category);
            answer(2);
            return;
        }
        save_and_answer(img.value(), "window");
        return;
    }
    case overlay_result::kind::all: {
        const lx::rect2i all = layout_bounds(q.shots);
        save_and_answer(compose(q.shots, all, compose_scale(q.shots, all)), "all outputs");
        return;
    }
    case overlay_result::kind::output: {
        const output_shot& s = q.shots[static_cast<std::size_t>(res.output)];
        save_and_answer(compose(q.shots, s.logical, shot_scale(s)), "output");
        return;
    }
    case overlay_result::kind::area:
        save_and_answer(compose(q.shots, res.area, compose_scale(q.shots, res.area)), "area");
        return;
    }
}

void service::after_events() {
    if (req_ && req_->closed) {
        answer(2);
        return;
    }
    if (!req_ || !req_->wl)
        return;
    if (req_->wl->broken()) {
        log_line(lx::trace::level::warn, "the Wayland connection broke during a request");
        answer(2);
        return;
    }
    if (!req_->ov)
        return;
    req_->ov->update();
    if (req_->ov->done())
        complete_overlay();
}

int service::run() {
    int r = sd_bus_open_user(&bus_);
    if (r < 0) {
        log_line(lx::trace::level::error, "cannot connect to the session bus: %s", std::strerror(-r));
        return 1;
    }
    r = sd_bus_add_object_vtable(bus_, &object_slot_, k_object_path, "org.freedesktop.impl.portal.Screenshot",
                                 k_screenshot_vtable, this);
    if (r < 0) {
        log_line(lx::trace::level::error, "cannot export the Screenshot object: %s", std::strerror(-r));
        return 1;
    }
    r = sd_bus_match_signal(bus_, &match_slot_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                            "org.freedesktop.DBus", "NameLost", on_name_lost, this);
    if (r < 0)
        log_line(lx::trace::level::warn, "cannot watch for losing the bus name: %s", std::strerror(-r));
    const uint64_t flags = SD_BUS_NAME_ALLOW_REPLACEMENT | (options_.replace ? SD_BUS_NAME_REPLACE_EXISTING : 0);
    r = sd_bus_request_name(bus_, k_bus_name, flags);
    if (r < 0) {
        log_line(lx::trace::level::error, "cannot own %s: %s%s", k_bus_name, std::strerror(-r),
                 options_.replace ? "" : " (another instance? -r replaces it)");
        return 1;
    }
    log_line(lx::trace::level::info, "serving %s (Screenshot v%u, targets 0x%x) on display %s", k_bus_name,
             k_screenshot_version, k_screenshot_targets,
             std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "(unset)");

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);
    const int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);

    int status = 0;
    while (!quit_) {
        for (;;) {
            r = sd_bus_process(bus_, nullptr);
            if (r < 0) {
                log_line(lx::trace::level::error, "D-Bus processing failed: %s", std::strerror(-r));
                quit_ = true;
                status = 1;
                break;
            }
            if (r == 0)
                break;
        }
        if (quit_)
            break;
        after_events();
        pollfd fds[3]{};
        nfds_t n = 0;
        fds[n++] = {sd_bus_get_fd(bus_), static_cast<short>(sd_bus_get_events(bus_)), 0};
        fds[n++] = {sfd, POLLIN, 0};
        bool with_wl = false;
        if (req_ && req_->wl && req_->wl->prepare()) {
            fds[n++] = {req_->wl->fd(), POLLIN, 0};
            with_wl = true;
        } else if (req_ && req_->wl) {
            after_events(); // broken: answers 2
            continue;
        }
        uint64_t usec = UINT64_MAX;
        int timeout = -1;
        if (sd_bus_get_timeout(bus_, &usec) >= 0 && usec != UINT64_MAX) {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
            timeout = usec > now ? static_cast<int>((usec - now + 999) / 1000) : 0;
        }
        if (poll(fds, n, timeout) < 0 && errno != EINTR) {
            status = 1;
            break;
        }
        if (fds[1].revents & POLLIN) {
            signalfd_siginfo si{};
            if (read(sfd, &si, sizeof si) > 0) {
                log_line(lx::trace::level::info, "signal %u: exiting", si.ssi_signo);
                quit_ = true;
            }
        }
        if (with_wl && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            (void)req_->wl->dispatch_readable();
            after_events();
        }
    }
    if (req_)
        answer(2);
    if (sfd >= 0)
        close(sfd);
    return status;
}


int run_service(const service_options& options) {
    service s{options};
    return s.run();
}

#else

int run_service(const service_options&) {
    lx::trace::logger::global().log_error(lx::not_implemented("lx::portal::run_service"), k_category);
    return 2;
}

#endif

int run_chooser(std::string_view input, std::string& chosen) {
    std::vector<chooser_entry> entries = parse_chooser_input(input);
    if (seat_session_locked()) {
        // The list would sit under the lock screen; nothing is shared while locked.
        log_line(lx::trace::level::info, "chooser: the session is locked: declined");
        return 1;
    }
    if (entries.empty()) {
        log_line(lx::trace::level::warn, "chooser: nothing to choose from");
        return 1;
    }
    lx::result<std::unique_ptr<desktop>> wl = desktop::connect();
    if (!wl.ok()) {
        lx::trace::logger::global().log_error(wl.get_error(), k_category);
        return 2;
    }
    std::unique_ptr<desktop> d = std::move(wl.value());
    // What a person recognises: a window's application rather than its identifier, an
    // output's description only when it says more than its name.
    for (chooser_entry& e : entries) {
        if (e.type == chooser_entry::kind::window) {
            std::string app;
            for (const auto& t : d->toplevels())
                if (!t->closed && t->identifier == e.detail)
                    app = t->app_id;
            e.detail = app;
        } else if (e.type == chooser_entry::kind::monitor && e.detail == e.title) {
            e.detail.clear();
        }
    }
    overlay_result res;
    {
        overlay ov{*d};
        if (auto r = ov.start_choose(entries, "Choose what to share"); !r.ok()) {
            lx::trace::logger::global().log_error(r.get_error(), k_category);
            return 2;
        }
        while (!ov.done()) {
            ov.update();
            if (!d->pump(-1)) {
                log_line(lx::trace::level::warn, "chooser: the Wayland connection broke");
                return 2;
            }
        }
        res = ov.result();
    }
    if (res.what != overlay_result::kind::choice || res.choice < 0 ||
        res.choice >= static_cast<int>(entries.size())) {
        log_line(lx::trace::level::info, "chooser: declined");
        return res.what == overlay_result::kind::failed ? 2 : 1;
    }
    chosen = entries[static_cast<std::size_t>(res.choice)].line;
    log_line(lx::trace::level::info, "chooser: chose '%s'", chosen.c_str());
    return 0;
}

} // namespace lx::portal
