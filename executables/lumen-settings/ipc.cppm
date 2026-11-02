// lumen-settings' Varlink client (ADR-013 Phase G): GetVersion and GetHealth over the
// compositor's control socket, polled on a worker thread and posted to the UI strand —
// the same trust model as lumen-ctl (the session's own UID, socket 0600), used for display.
//
// The reply JSON gets a small recursive-descent reader (varlink replies are JSON; the
// health object is the one schema this reads). No connection: a stale snapshot keeps
// showing with a "compositor unreachable" note — the app's file editing never depends on
// this.
module;
#ifdef LUMEN_SETTINGS_HAS_VARLINK
#include <systemd/sd-varlink.h>
#endif

export module lumen.settings_ui:ipc;

import std;
import lx.foundation;
import lx.runtime;

export namespace settings {

/// One poll's result, as the Session pane shows it.
struct health_snapshot {
    bool ok = false;               ///< the last poll answered
    bool attempted = false;        ///< a poll ran at all
    std::string version_line;      /// "0.3.0 · zlm_shell v7 · vulkan"
    std::string level_line;        /// "health: ok (degraded, critical …)"
    std::string parts_line;        /// "wayland ok · display ok · graphics ok · …"
    std::string memory_line;       /// "memory: normal · rss 19 MiB · staging 1/64 MiB"
    std::string config_line;       /// "config generation 12 · 0 errors · 2 warnings"
    std::string devices_line;      /// the input devices, one per line
    std::string note;              /// empty, or why the last poll failed
};

/// Polls the compositor's control socket every 2 s on `worker`'s thread; each fresh
/// snapshot lands on the UI strand through `deliver`. `stop_ipc` joins. Null socket or a
/// failed start: `deliver` gets one snapshot with `note` set.
void start_ipc(const char* socket_path, std::function<void(const health_snapshot&)> deliver);
void stop_ipc();

} // namespace settings

// ── implementation ──────────────────────────────────────────────────────────────

namespace {

using namespace settings;

#ifdef LUMEN_SETTINGS_HAS_VARLINK

/// The control socket's path (set once by start_ipc; the poll thread reads it).
std::string g_socket; // NOLINT(cppcoreguidelines-avoid-non-const-global-variable)
std::atomic<bool> g_running{false};
std::thread g_thread;

/// The poll thread formats with plain string plumbing (`frame_format` is UI-thread).
template <typename... Args>
[[nodiscard]] std::string frame_line(std::string_view fmt, Args&&... args) {
    return std::vformat(fmt, std::make_format_args(args...));
}

[[nodiscard]] std::string_view level_word(std::string_view l) {
    return l == "degraded" ? "degraded" : l == "critical" ? "critical"
                                 : l == "unknown"        ? "unknown"
                                                         : "ok";
}

/// One connection for a whole poll. The replies and error ids of `sd_varlink_call` are
/// borrowed from the connection — valid until it is unreffed (and until its next call) —
/// so every reply is consumed before the next call, and the link outlives them all.
struct connection_guard {
    sd_varlink* link = nullptr;
    ~connection_guard() {
        if (link)
            sd_varlink_flush_close_unref(link);
    }
};

/// One Varlink call on `link`; the reply in `reply` is borrowed (see `connection_guard`).
/// False with `note` set when the call or the method failed.
[[nodiscard]] bool varlink_call(sd_varlink* link, const char* method,
                                sd_json_variant** reply, std::string& note) {
    const char* error_id = nullptr;
    const int c = sd_varlink_call(link, method, nullptr, reply, &error_id);
    if (c < 0) {
        note = std::string(method) + " failed (" +
               std::generic_category().message(-c) + ")";
        return false;
    }
    if (error_id) {
        // error_id is borrowed from the connection too: copy the text while it stands.
        note = std::string(method) + ": " + error_id;
        return false;
    }
    return true;
}

void fill_snapshot(health_snapshot& h) {
    h.attempted = true;
    connection_guard conn;
    const int r = sd_varlink_connect_address(&conn.link, g_socket.c_str());
    if (r < 0) {
        h.ok = false;
        h.note = std::string("cannot connect (") + std::generic_category().message(-r) + ")";
        return;
    }
    (void)sd_varlink_set_description(conn.link, "lumen-settings");
    (void)sd_varlink_set_relative_timeout(conn.link, 5'000'000);
    sd_varlink* link = conn.link;
    sd_json_variant* version_reply = nullptr;
    std::string note;
    if (!varlink_call(link, "io.lumen.Compositor.GetVersion", &version_reply, note)) {
        h.ok = false;
        h.note = note;
        return;
    }
    const auto field = [](sd_json_variant* v, const char* key) -> sd_json_variant* {
        return v ? sd_json_variant_by_key(v, key) : nullptr;
    };
    const auto str = [field](sd_json_variant* v, const char* key) {
        sd_json_variant* f = field(v, key);
        return f && sd_json_variant_is_string(f) ? std::string_view(sd_json_variant_string(f))
                                                 : std::string_view{};
    };
    const auto num = [field](sd_json_variant* v, const char* key) -> long long {
        sd_json_variant* f = field(v, key);
        if (f && sd_json_variant_is_integer(f))
            return sd_json_variant_integer(f);
        return f && sd_json_variant_is_unsigned(f)
                   ? static_cast<long long>(sd_json_variant_unsigned(f))
                   : 0ll;
    };
    h.version_line = frame_line("{} · zlm_shell v{} · {}", str(version_reply, "version"),
                                num(version_reply, "zlm_version"), str(version_reply, "backend"));

    sd_json_variant* health = nullptr;
    if (!varlink_call(link, "io.lumen.Compositor.GetHealth", &health, note)) {
        h.ok = true; // the version answered; health is the optional extra
        h.note = note;
        return;
    }
    if (sd_json_variant* l = sd_json_variant_by_key(health, "level"))
        h.level_line = std::string(
            "health: ") +
            std::string(level_word(sd_json_variant_is_string(l) ? sd_json_variant_string(l)
                                                               : "?"));
    static constexpr const char* k_parts[] = {"wayland",     "display", "graphics",
                                              "render",      "input",   "session",
                                              "presentation", "systemd"};
    std::string parts;
    const auto part_level = [&](const char* part) {
        sd_json_variant* p = sd_json_variant_by_key(health, part);
        sd_json_variant* l = p ? sd_json_variant_by_key(p, "level") : nullptr;
        return std::string(level_word(l && sd_json_variant_is_string(l)
                                          ? sd_json_variant_string(l)
                                          : "?"));
    };
    for (const char* part : k_parts) {
        if (!parts.empty())
            parts += " · ";
        parts += std::string(part) + " " + part_level(part);
    }
    h.parts_line = parts;
    if (sd_json_variant* mem = sd_json_variant_by_key(health, "memory")) {
        const auto mem_num = [&](const char* key) -> long long {
            sd_json_variant* f = sd_json_variant_by_key(mem, key);
            if (f && sd_json_variant_is_integer(f))
                return sd_json_variant_integer(f);
            return f && sd_json_variant_is_unsigned(f)
                       ? static_cast<long long>(sd_json_variant_unsigned(f))
                       : 0ll;
        };
        sd_json_variant* pressure = sd_json_variant_by_key(mem, "pressure");
        char buf[160];
        std::snprintf(buf, sizeof(buf), "memory: %s · rss %.0f MiB · staging %.0f/%.0f MiB",
                      pressure && sd_json_variant_is_string(pressure)
                          ? sd_json_variant_string(pressure)
                          : "?",
                      static_cast<double>(mem_num("rss_kib")) / 1024.0,
                      static_cast<double>(mem_num("staging_bytes")) / 1048576.0,
                      static_cast<double>(mem_num("staging_budget")) / 1048576.0);
        h.memory_line = buf;
    }
    char cfg[128];
    std::snprintf(cfg, sizeof(cfg), "config generation %lld · %lld errors · %lld warnings",
                  num(health, "config_generation"), num(health, "config_errors"),
                  num(health, "config_warnings"));
    h.config_line = cfg;

    // The input devices (the Input pane's device list; the compositor is the only one who
    // knows libinput's names). The last call on the connection: its reply is read before
    // the guard unrefs the link.
    sd_json_variant* dev_reply = nullptr;
    std::string dev_note;
    if (varlink_call(link, "io.lumen.Compositor.ListInputDevices", &dev_reply, dev_note)) {
        if (sd_json_variant* arr =
                dev_reply ? sd_json_variant_by_key(dev_reply, "devices") : nullptr;
            arr && sd_json_variant_is_array(arr)) {
            std::string list;
            const std::size_t n = sd_json_variant_elements(arr);
            for (std::size_t i = 0; i < n && i < 32; ++i) {
                sd_json_variant* e = sd_json_variant_by_index(arr, i);
                auto* name_f = sd_json_variant_by_key(e, "name");
                auto* kind_f = sd_json_variant_by_key(e, "kind");
                auto* kb = sd_json_variant_by_key(e, "keyboard");
                list += "· ";
                list += name_f && sd_json_variant_is_string(name_f)
                            ? sd_json_variant_string(name_f)
                            : "?";
                list += " (";
                list += kind_f && sd_json_variant_is_string(kind_f)
                            ? sd_json_variant_string(kind_f)
                            : "?";
                if (kb && sd_json_variant_boolean(kb))
                    list += " +keyboard";
                list += ")\n";
            }
            h.devices_line = std::move(list);
        }
    }
    h.ok = true;
    h.note.clear();
}

#else // !LUMEN_SETTINGS_HAS_VARLINK

void fill_snapshot(health_snapshot& h) {
    h.attempted = true;
    h.note = "this build has no Varlink (no libsystemd >= 257)";
}

#endif // LUMEN_SETTINGS_HAS_VARLINK

} // namespace

export namespace settings {

#ifdef LUMEN_SETTINGS_HAS_VARLINK
void start_ipc(const char* socket_path, std::function<void(const health_snapshot&)> deliver) {
    if (!socket_path || !socket_path[0]) {
        health_snapshot h{};
        h.attempted = true;
        h.note = "no control socket (the compositor runs without one)";
        deliver(h);
        return;
    }
    g_socket = socket_path;
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true))
        return;
    g_thread = std::thread([deliver = std::move(deliver)] {
        while (g_running.load(std::memory_order_relaxed)) {
            health_snapshot h;
            fill_snapshot(h);
            deliver(h);
            for (int i = 0; i < 20 && g_running.load(std::memory_order_relaxed); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void stop_ipc() {
    g_running.store(false, std::memory_order_relaxed);
    if (g_thread.joinable())
        g_thread.join();
}
#else
void start_ipc(const char*, std::function<void(const health_snapshot&)> deliver) {
    health_snapshot h{};
    h.attempted = true;
    h.note = "this build has no Varlink (no libsystemd >= 257)";
    deliver(h);
}
void stop_ipc() {}
#endif

} // namespace settings