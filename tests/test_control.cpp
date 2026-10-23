// The control socket (lx.compositor:control, design-session-and-wm S.8): io.lumen.Compositor
// over sd-varlink with a fake host — every method's reply shape, the asynchronous GetHealth
// and its timeout, the errors, the event stream and its overflow drop, the peer-UID check,
// the socket file's life. No compositor, no socket: connections are socketpairs.
#include "lumen_test.hpp"

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <systemd/sd-json.h>
#include <systemd/sd-varlink.h>

import std;
import lx.health;
import lx.compositor;

namespace {

using lx::compositor::control_action_result;
using lx::compositor::control_server;

struct fake_host {
    unsigned health_token = ~0u;
    unsigned health_calls = 0;
    unsigned timeouts = 0;
    bool answer_on_timeout = true;
    std::string logout_source;
    bool locked = false;
    std::string last_action;
    control_server* server = nullptr;

    static lx::compositor::control_host make(fake_host& h) {
        lx::compositor::control_host host{};
        host.user = &h;
        host.version = [](void*, lx::compositor::control_version& v) {
            v.version = "0.3.0";
            v.zlm_version = 6;
            v.backend = "gl";
        };
        host.health = [](void* u, unsigned token) {
            auto* h = static_cast<fake_host*>(u);
            h->health_token = token;
            ++h->health_calls;
        };
        host.health_timeout = [](void* u, unsigned token) {
            auto* h = static_cast<fake_host*>(u);
            ++h->timeouts;
            h->health_token = token;
        };
        host.outputs = [](void*, lx::compositor::control_output* out, unsigned cap) {
            if (cap < 2)
                return 0u;
            out[0] = {};
            std::snprintf(out[0].name, sizeof(out[0].name), "Virtual-1");
            out[0].width = 1280;
            out[0].height = 800;
            out[0].mode_width = 1280;
            out[0].mode_height = 800;
            out[0].refresh_mhz = 60000;
            out[0].scale = 1.0;
            out[1] = out[0];
            std::snprintf(out[1].name, sizeof(out[1].name), "Virtual-2");
            out[1].x = 1280;
            out[1].scale = 1.5;
            return 2u;
        };
        host.windows = [](void*, lx::compositor::control_window* out, unsigned cap) {
            if (cap < 1)
                return 0u;
            out[0] = {};
            out[0].id = 7;
            std::snprintf(out[0].app_id, sizeof(out[0].app_id), "foot");
            std::snprintf(out[0].title, sizeof(out[0].title), "a \"quoted\" title");
            out[0].pid = 1234;
            out[0].focused = true;
            out[0].workspace = 3;
            out[0].width = 640;
            out[0].height = 480;
            return 1u;
        };
        host.workspaces = [](void*, lx::compositor::control_workspace* out, unsigned cap) {
            if (cap < 2)
                return 0u;
            out[0] = {};
            out[0].id = 3;
            std::snprintf(out[0].output, sizeof(out[0].output), "Virtual-1");
            out[0].index = 1;
            std::snprintf(out[0].name, sizeof(out[0].name), "1");
            out[0].active = true;
            out[0].windows = 1;
            out[1] = out[0];
            out[1].id = 4;
            out[1].index = 2;
            std::snprintf(out[1].name, sizeof(out[1].name), "2");
            out[1].active = false;
            out[1].urgent = true;
            out[1].windows = 0;
            return 2u;
        };
        host.focused = [](void*, lx::compositor::control_window& w) {
            w = {};
            w.id = 7;
            std::snprintf(w.app_id, sizeof(w.app_id), "foot");
            return true;
        };
        // Answered at once here; the compositor answers once the generation is applied.
        host.reload = [](void* u, unsigned token) {
            auto* h = static_cast<fake_host*>(u);
            const lx::compositor::control_diagnostic d[] = {
                {"warning", "/home/u/.config/lumen/config.toml", 4, "input.touchpad.tapp",
                 "unknown key (did you mean tap?); ignored"}};
            h->server->finish_reload(token, 3, false, d);
        };
        host.run_action = [](void* u, const char* action, const char* argument,
                             const char*& message) {
            auto* h = static_cast<fake_host*>(u);
            h->last_action = action;
            const std::string_view a{action};
            if (a == "session.logout")
                return control_action_result::ok;
            if (a == "window.close") {
                if (h->locked)
                    return control_action_result::session_locked;
                if (argument) {
                    message = "this action takes no argument";
                    return control_action_result::invalid_argument;
                }
                return control_action_result::ok;
            }
            if (a == "shell.launcher")
                return control_action_result::shell_not_connected;
            return control_action_result::unknown_action;
        };
        host.logout = [](void* u, const char* source) {
            static_cast<fake_host*>(u)->logout_source = source;
        };
        return host;
    }
};

struct reply {
    sd_json_variant* params = nullptr;
    std::string error;
    bool continues = false;
};

/// A Varlink client over one end of a socketpair, the server holding the other.
struct client {
    sd_varlink* link = nullptr;
    std::vector<reply> replies;

    ~client() {
        for (auto& r : replies)
            sd_json_variant_unref(r.params);
        sd_varlink_close_unref(link);
    }

    static int on_reply(sd_varlink* l, sd_json_variant* params, const char* error_id,
                        sd_varlink_reply_flags_t flags, void*) {
        auto* c = static_cast<client*>(sd_varlink_get_userdata(l));
        reply r{};
        r.params = sd_json_variant_ref(params);
        r.error = error_id ? error_id : "";
        r.continues = (flags & SD_VARLINK_REPLY_CONTINUES) != 0;
        c->replies.push_back(r);
        return 0;
    }

    bool connect(control_server& server, const lx::compositor::control_peer* peer) {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) != 0)
            return false;
        if (!server.add_connection(sv[0], peer)) {
            ::close(sv[0]);
            ::close(sv[1]);
            return false;
        }
        if (sd_varlink_connect_fd(&link, sv[1]) < 0)
            return false;
        sd_varlink_set_userdata(link, this);
        return sd_varlink_bind_reply(link, &client::on_reply) >= 0;
    }

    void invoke(const char* method, sd_json_variant* params = nullptr) {
        LUMEN_CHECK(sd_varlink_invoke(link, method, params) >= 0);
    }
    void observe(const char* method) { LUMEN_CHECK(sd_varlink_observe(link, method, nullptr) >= 0); }
};

/// Runs both sides until `done()` or `ms` pass.
template <typename Done>
bool pump(control_server& server, client& c, Done done, int ms = 2000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        server.dispatch();
        for (int i = 0; i < 16 && sd_varlink_process(c.link) > 0; ++i) {
        }
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

const char* str(sd_json_variant* v, const char* key) {
    sd_json_variant* f = v ? sd_json_variant_by_key(v, key) : nullptr;
    return f && sd_json_variant_is_string(f) ? sd_json_variant_string(f) : "";
}
long long num(sd_json_variant* v, const char* key) {
    sd_json_variant* f = v ? sd_json_variant_by_key(v, key) : nullptr;
    return f && sd_json_variant_is_integer(f) ? sd_json_variant_integer(f) : -1;
}

struct fixture {
    fake_host host;
    control_server server;
    client c;
    fixture() {
        host.server = &server;
        LUMEN_CHECK(server.open(fake_host::make(host)).ok());
        LUMEN_CHECK(c.connect(server, nullptr));
    }
    reply& call(const char* method, sd_json_variant* params = nullptr) {
        const std::size_t before = c.replies.size();
        c.invoke(method, params);
        LUMEN_CHECK(pump(server, c, [&] { return c.replies.size() > before; }));
        return c.replies.back();
    }
};

} // namespace

LUMEN_TEST(the_interface_parses_and_describes_itself) {
    fixture f;
    auto& r = f.call("io.lumen.Compositor.GetVersion");
    LUMEN_CHECK(r.error.empty());
    LUMEN_CHECK(std::string_view{str(r.params, "version")} == "0.3.0");
    LUMEN_CHECK(num(r.params, "zlm_version") == 6);
    LUMEN_CHECK(std::string_view{str(r.params, "backend")} == "gl");

    sd_json_variant* p = nullptr;
    (void)sd_json_variant_set_field_string(&p, "interface", "io.lumen.Compositor");
    auto& d = f.call("org.varlink.service.GetInterfaceDescription", p);
    sd_json_variant_unref(p);
    LUMEN_CHECK(d.error.empty());
    LUMEN_CHECK(std::string_view{str(d.params, "description")}.find("method GetHealth") !=
                std::string_view::npos);
}

LUMEN_TEST(health_is_answered_when_the_host_finishes_it) {
    fixture f;
    f.c.invoke("io.lumen.Compositor.GetHealth");
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.host.health_calls == 1; }));
    LUMEN_CHECK(f.c.replies.empty()); // not before the host answers

    lx::health::health_inputs in{};
    in.backend = "gl";
    in.wayland_serving = true;
    in.render_answered = true;
    in.input_open = true;
    in.input_devices = 2;
    in.shell_connected = true;
    in.shell_pid = 9;
    in.shell_binds = 2;
    in.logind_control = true;
    in.session_id = "1";
    in.display = lx::health::display_path::kms;
    lx::health::output_input o{};
    o.name.set("Virtual-1");
    o.flip_paced = true;
    in.outputs.push_back(o);
    f.server.finish_health(f.host.health_token, lx::health::evaluate(in));
    LUMEN_CHECK(pump(f.server, f.c, [&] { return !f.c.replies.empty(); }));
    auto& r = f.c.replies.back();
    LUMEN_CHECK(r.error.empty());
    sd_json_variant* h = sd_json_variant_by_key(r.params, "health");
    LUMEN_CHECK(std::string_view{str(h, "level")} == "ok");
    sd_json_variant* parts = sd_json_variant_by_key(h, "parts");
    LUMEN_CHECK(parts && sd_json_variant_elements(parts) >= 7);
    sd_json_variant* procs = sd_json_variant_by_key(h, "processes");
    LUMEN_CHECK(num(sd_json_variant_by_index(procs, 1), "binds") == 2);
    LUMEN_CHECK(sd_json_variant_elements(sd_json_variant_by_key(h, "speed")) == 1);

    // A late or repeated answer for the same token is dropped.
    f.server.finish_health(f.host.health_token, lx::health::evaluate(in));
    pump(f.server, f.c, [] { return false; }, 50);
    LUMEN_CHECK(f.c.replies.size() == 1);
}

LUMEN_TEST(health_times_out_to_the_host) {
    fixture f;
    f.c.invoke("io.lumen.Compositor.GetHealth");
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.host.timeouts == 1; }, 3000));
    lx::health::health_inputs in{};
    in.render_answered = false;
    f.server.finish_health(f.host.health_token, lx::health::evaluate(in));
    LUMEN_CHECK(pump(f.server, f.c, [&] { return !f.c.replies.empty(); }));
    sd_json_variant* h = sd_json_variant_by_key(f.c.replies.back().params, "health");
    LUMEN_CHECK(std::string_view{str(h, "level")} == "critical");
}

LUMEN_TEST(lists_have_their_shapes) {
    fixture f;
    auto& o = f.call("io.lumen.Compositor.ListOutputs");
    LUMEN_CHECK(o.error.empty());
    sd_json_variant* outs = sd_json_variant_by_key(o.params, "outputs");
    LUMEN_CHECK(sd_json_variant_elements(outs) == 2);
    LUMEN_CHECK(num(sd_json_variant_by_index(outs, 1), "x") == 1280);
    auto& w = f.call("io.lumen.Compositor.ListWindows");
    sd_json_variant* wins = sd_json_variant_by_key(w.params, "windows");
    LUMEN_CHECK(sd_json_variant_elements(wins) == 1);
    LUMEN_CHECK(std::string_view{str(sd_json_variant_by_index(wins, 0), "title")} ==
                "a \"quoted\" title");
    LUMEN_CHECK(num(sd_json_variant_by_index(wins, 0), "pid") == 1234);
    LUMEN_CHECK(num(sd_json_variant_by_index(wins, 0), "workspace") == 3);
    auto& ws = f.call("io.lumen.Compositor.ListWorkspaces");
    LUMEN_CHECK(ws.error.empty());
    sd_json_variant* wss = sd_json_variant_by_key(ws.params, "workspaces");
    LUMEN_CHECK(sd_json_variant_elements(wss) == 2);
    sd_json_variant* w2 = sd_json_variant_by_index(wss, 1);
    LUMEN_CHECK(num(w2, "id") == 4 && num(w2, "index") == 2 &&
                std::string_view{str(w2, "output")} == "Virtual-1" &&
                std::string_view{str(w2, "name")} == "2" &&
                sd_json_variant_boolean(sd_json_variant_by_key(w2, "urgent")) &&
                !sd_json_variant_boolean(sd_json_variant_by_key(w2, "active")));
    auto& rl = f.call("io.lumen.Compositor.Reload");
    LUMEN_CHECK(rl.error.empty() && num(rl.params, "generation") == 3);
    sd_json_variant* diags = sd_json_variant_by_key(rl.params, "diagnostics");
    LUMEN_CHECK(sd_json_variant_elements(diags) == 1);
    LUMEN_CHECK(num(sd_json_variant_by_index(diags, 0), "line") == 4);
    LUMEN_CHECK(std::string_view{str(sd_json_variant_by_index(diags, 0), "key")} ==
                "input.touchpad.tapp");
}

LUMEN_TEST(actions_answer_with_their_errors) {
    fixture f;
    auto action = [&](const char* name, const char* arg) -> reply& {
        sd_json_variant* p = nullptr;
        (void)sd_json_variant_set_field_string(&p, "action", name);
        if (arg)
            (void)sd_json_variant_set_field_string(&p, "argument", arg);
        reply& r = f.call("io.lumen.Compositor.RunAction", p);
        sd_json_variant_unref(p);
        return r;
    };
    LUMEN_CHECK(action("window.close", nullptr).error.empty());
    LUMEN_CHECK(action("nope", nullptr).error == "io.lumen.Compositor.UnknownAction");
    LUMEN_CHECK(std::string_view{str(f.c.replies.back().params, "action")} == "nope");
    LUMEN_CHECK(action("window.close", "x").error == "io.lumen.Compositor.InvalidArgument");
    LUMEN_CHECK(std::string_view{str(f.c.replies.back().params, "message")}.size() > 0);
    LUMEN_CHECK(action("shell.launcher", nullptr).error == "io.lumen.Compositor.ShellNotConnected");
    f.host.locked = true;
    LUMEN_CHECK(action("window.close", nullptr).error == "io.lumen.Compositor.SessionLocked");
    // A call missing its required parameter never reaches the host.
    f.host.last_action.clear();
    auto& bad = f.call("io.lumen.Compositor.RunAction");
    LUMEN_CHECK(bad.error == "org.varlink.service.InvalidParameter");
    LUMEN_CHECK(f.host.last_action.empty());

    LUMEN_CHECK(f.call("io.lumen.Compositor.Logout").error.empty());
    LUMEN_CHECK(f.host.logout_source == "lumen-ctl");
}

LUMEN_TEST(subscribe_streams_the_focus_first_then_changes) {
    fixture f;
    auto& no_more = f.call("io.lumen.Compositor.Subscribe");
    LUMEN_CHECK(no_more.error == "org.varlink.service.ExpectedMore");

    f.c.observe("io.lumen.Compositor.Subscribe");
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.c.replies.size() == 2; }));
    LUMEN_CHECK(f.server.subscribers() == 1);
    LUMEN_CHECK(std::string_view{str(f.c.replies[1].params, "event")} == "focus");
    LUMEN_CHECK(f.c.replies[1].continues);

    lx::compositor::control_window w{};
    w.id = 9;
    std::snprintf(w.app_id, sizeof(w.app_id), "gtk4-demo");
    f.server.publish_window("window-opened", w);
    f.server.publish_health(lx::health::level::degraded);
    f.server.publish_config(4);
    lx::compositor::control_workspace ws{};
    ws.id = 5;
    ws.index = 2;
    std::snprintf(ws.output, sizeof(ws.output), "Virtual-1");
    std::snprintf(ws.name, sizeof(ws.name), "2");
    ws.active = true;
    f.server.publish_workspace("workspace", ws);
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.c.replies.size() == 6; }));
    LUMEN_CHECK(std::string_view{str(f.c.replies[5].params, "event")} == "workspace");
    LUMEN_CHECK(num(sd_json_variant_by_key(f.c.replies[5].params, "workspace"), "id") == 5);
    LUMEN_CHECK(std::string_view{str(f.c.replies[2].params, "event")} == "window-opened");
    LUMEN_CHECK(num(sd_json_variant_by_key(f.c.replies[2].params, "window"), "id") == 9);
    LUMEN_CHECK(std::string_view{str(f.c.replies[3].params, "level")} == "degraded");
    LUMEN_CHECK(num(f.c.replies[4].params, "generation") == 4);

    // The client goes: so does its subscription.
    sd_varlink_close_unref(f.c.link);
    f.c.link = nullptr;
    for (int i = 0; i < 50 && f.server.subscribers() > 0; ++i) {
        f.server.dispatch();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    LUMEN_CHECK(f.server.subscribers() == 0);
}

LUMEN_TEST(a_subscriber_that_does_not_read_is_dropped) {
    fixture f;
    f.c.observe("io.lumen.Compositor.Subscribe");
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.server.subscribers() == 1; }));
    // From here the client never reads: the socket fills, the backlog grows, the server
    // drops it instead of queueing without bound.
    lx::compositor::control_window w{};
    std::memset(w.title, 'x', sizeof(w.title) - 1);
    for (int i = 0; i < 20000 && f.server.subscribers() > 0; ++i) {
        w.id = static_cast<unsigned>(i);
        f.server.publish_window("window-changed", w);
        f.server.dispatch();
    }
    LUMEN_CHECK(f.server.subscribers() == 0);
}

// A burst inside one UI iteration (a session restore mapping 40 windows: ~4 events each) is
// queued before anything is written. Counting every event queued behind unwritten output
// dropped a monitor that reads normally.
LUMEN_TEST(a_burst_before_the_next_dispatch_does_not_drop_a_reading_subscriber) {
    fixture f;
    f.c.observe("io.lumen.Compositor.Subscribe");
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.c.replies.size() == 1; }));
    constexpr unsigned k_burst = 4 * control_server::k_max_backlog;
    lx::compositor::control_window w{};
    for (unsigned i = 0; i < k_burst; ++i) {
        w.id = i + 1;
        f.server.publish_window("window-changed", w);
    }
    LUMEN_CHECK(f.server.subscribers() == 1);
    LUMEN_CHECK(pump(f.server, f.c, [&] { return f.c.replies.size() == 1 + k_burst; }));
    LUMEN_CHECK(f.server.subscribers() == 1);
    LUMEN_CHECK(num(sd_json_variant_by_key(f.c.replies.back().params, "window"), "id") ==
                static_cast<long long>(k_burst));
}

LUMEN_TEST(only_this_uid_may_connect) {
    fake_host host;
    control_server server;
    LUMEN_CHECK(server.open(fake_host::make(host)).ok());
    lx::compositor::control_peer other{};
    other.uid = static_cast<unsigned>(::getuid()) + 1;
    other.gid = static_cast<unsigned>(::getgid());
    other.pid = 1;
    client c;
    // SD_VARLINK_SERVER_MYSELF_ONLY: refused when the connection is added.
    LUMEN_CHECK(!c.connect(server, &other));
    lx::compositor::control_peer self{};
    self.uid = static_cast<unsigned>(::getuid());
    self.gid = static_cast<unsigned>(::getgid());
    self.pid = static_cast<int>(::getpid());
    client ok;
    LUMEN_CHECK(ok.connect(server, &self));
    ok.invoke("io.lumen.Compositor.GetVersion");
    LUMEN_CHECK(pump(server, ok, [&] { return !ok.replies.empty(); }));
    LUMEN_CHECK(ok.replies.back().error.empty());
}

LUMEN_TEST(the_socket_file_is_private_replaced_when_stale_and_removed_at_close) {
    char dir[] = "/tmp/lumen-control-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir) != nullptr);
    const std::string path = std::string{dir} + "/lumen-test.varlink";
    {
        // A stale file of a compositor that died.
        std::FILE* stale = std::fopen(path.c_str(), "w");
        LUMEN_CHECK(stale != nullptr);
        std::fclose(stale);
    }
    fake_host host;
    {
        control_server server;
        LUMEN_CHECK(server.open(fake_host::make(host)).ok());
        LUMEN_CHECK(server.listen(path.c_str()).ok());
        struct stat st {};
        LUMEN_CHECK(::stat(path.c_str(), &st) == 0);
        LUMEN_CHECK(S_ISSOCK(st.st_mode));
        LUMEN_CHECK((st.st_mode & 0777) == 0600);
        LUMEN_CHECK(server.fd() >= 0);
    }
    struct stat st {};
    LUMEN_CHECK(::stat(path.c_str(), &st) != 0);
    ::rmdir(dir);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
