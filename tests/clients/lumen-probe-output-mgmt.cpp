// lumen-probe output-mgmt: a deterministic zwlr_output_manager_v1 client (ADR 008 §D.2) — lists
// heads, applies or tests a configuration, provokes each protocol error. Separate from the
// rest of lumen-probe (no surfaces, its own connection); main() dispatches here.
//
//   lumen-probe output-mgmt [--watch S]
//       Prints every head (`head name=… enabled=… mode=WxH@mHz pos=X,Y scale=S transform=T
//       adaptive_sync=A modes=N`) and `done serial=N` after each done, for S seconds (0: the
//       first done only).
//   lumen-probe output-mgmt --apply SPEC | --test SPEC [--stale-serial]
//       SPEC: `NAME:key=value,…;NAME:off;…` with keys mode=WxH[@mHz] (one of the head's),
//       custom=WxH[@mHz], pos=X,Y, scale=S, transform=0..7, vrr=0|1. Heads not named are
//       enabled as they are (or left disabled). Prints `result succeeded|failed|cancelled`.
//       --stale-serial uses the serial before the current one (expects `cancelled`).
//   lumen-probe output-mgmt --error KIND
//       Sends the offending sequence and expects that protocol error: already_configured_head,
//       unconfigured_head, already_used, already_set, already_set_custom, invalid_mode,
//       invalid_custom_mode, invalid_transform, invalid_scale, invalid_adaptive_sync_state.
//       Prints `protocol error interface=… code=N` and exits 0 when it is the expected one.

#include <cctype>
#include <cerrno>
#include <poll.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#if __has_include("wlr-output-management-unstable-v1-client-protocol.h")
#include "wlr-output-management-unstable-v1-client-protocol.h"
#define PROBE_HAS_OUTPUT_MANAGEMENT 1
#endif

int output_mgmt_main(int argc, char** argv);

#if !defined(PROBE_HAS_OUTPUT_MANAGEMENT)

int output_mgmt_main(int, char**) {
    std::fprintf(stderr, "output-mgmt: built without wlr-output-management glue\n");
    return 2;
}

#else

namespace {

struct om_mode {
    zwlr_output_mode_v1* mode = nullptr;
    int w = 0, h = 0, refresh = 0;
    bool preferred = false;
    bool finished = false;
};

struct om_head {
    zwlr_output_head_v1* head = nullptr;
    std::string name, description, make, model, serial;
    std::vector<om_mode*> modes;
    bool enabled = false;
    om_mode* current = nullptr;
    int x = 0, y = 0, transform = 0, adaptive = -1, pw = 0, ph = 0;
    double scale = 1.0;
    bool finished = false;
};

struct om_state {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zwlr_output_manager_v1* manager = nullptr;
    std::vector<om_head*> heads;
    uint32_t serial = 0;
    uint32_t prev_serial = 0;
    unsigned dones = 0;
    int result = -1; // 0 succeeded, 1 failed, 2 cancelled
};

om_state g_om{};

void m_size(void* d, zwlr_output_mode_v1*, int32_t w, int32_t h) {
    static_cast<om_mode*>(d)->w = w;
    static_cast<om_mode*>(d)->h = h;
}
void m_refresh(void* d, zwlr_output_mode_v1*, int32_t r) { static_cast<om_mode*>(d)->refresh = r; }
void m_preferred(void* d, zwlr_output_mode_v1*) { static_cast<om_mode*>(d)->preferred = true; }
void m_finished(void* d, zwlr_output_mode_v1*) { static_cast<om_mode*>(d)->finished = true; }
const zwlr_output_mode_v1_listener k_mode_listener = {m_size, m_refresh, m_preferred, m_finished};

void h_name(void* d, zwlr_output_head_v1*, const char* s) { static_cast<om_head*>(d)->name = s; }
void h_description(void* d, zwlr_output_head_v1*, const char* s) {
    static_cast<om_head*>(d)->description = s;
}
void h_physical_size(void* d, zwlr_output_head_v1*, int32_t w, int32_t h) {
    static_cast<om_head*>(d)->pw = w;
    static_cast<om_head*>(d)->ph = h;
}
void h_mode(void* d, zwlr_output_head_v1*, zwlr_output_mode_v1* m) {
    auto* mi = new om_mode{};
    mi->mode = m;
    zwlr_output_mode_v1_add_listener(m, &k_mode_listener, mi);
    static_cast<om_head*>(d)->modes.push_back(mi);
}
void h_enabled(void* d, zwlr_output_head_v1*, int32_t e) { static_cast<om_head*>(d)->enabled = e; }
void h_current_mode(void* d, zwlr_output_head_v1*, zwlr_output_mode_v1* m) {
    auto* h = static_cast<om_head*>(d);
    for (auto* mi : h->modes)
        if (mi->mode == m)
            h->current = mi;
}
void h_position(void* d, zwlr_output_head_v1*, int32_t x, int32_t y) {
    static_cast<om_head*>(d)->x = x;
    static_cast<om_head*>(d)->y = y;
}
void h_transform(void* d, zwlr_output_head_v1*, int32_t t) { static_cast<om_head*>(d)->transform = t; }
void h_scale(void* d, zwlr_output_head_v1*, wl_fixed_t s) {
    static_cast<om_head*>(d)->scale = wl_fixed_to_double(s);
}
void h_finished(void* d, zwlr_output_head_v1*) { static_cast<om_head*>(d)->finished = true; }
void h_make(void* d, zwlr_output_head_v1*, const char* s) { static_cast<om_head*>(d)->make = s; }
void h_model(void* d, zwlr_output_head_v1*, const char* s) { static_cast<om_head*>(d)->model = s; }
void h_serial(void* d, zwlr_output_head_v1*, const char* s) { static_cast<om_head*>(d)->serial = s; }
void h_adaptive(void* d, zwlr_output_head_v1*, uint32_t a) {
    static_cast<om_head*>(d)->adaptive = static_cast<int>(a);
}
const zwlr_output_head_v1_listener k_head_listener = {
    h_name,      h_description, h_physical_size, h_mode,  h_enabled, h_current_mode, h_position,
    h_transform, h_scale,       h_finished,      h_make,  h_model,   h_serial,       h_adaptive};

void print_heads() {
    for (auto* h : g_om.heads) {
        if (h->finished)
            continue;
        unsigned live_modes = 0;
        for (auto* m : h->modes)
            live_modes += m->finished ? 0u : 1u;
        if (!h->enabled) {
            std::printf("head name=%s enabled=0 modes=%u description=\"%s\"\n", h->name.c_str(),
                        live_modes, h->description.c_str());
            continue;
        }
        std::printf("head name=%s enabled=1 mode=%dx%d@%d pos=%d,%d scale=%g transform=%d "
                    "adaptive_sync=%d modes=%u description=\"%s\"\n",
                    h->name.c_str(), h->current ? h->current->w : 0, h->current ? h->current->h : 0,
                    h->current ? h->current->refresh : 0, h->x, h->y, h->scale, h->transform,
                    h->adaptive, live_modes, h->description.c_str());
    }
}

void mgr_head(void*, zwlr_output_manager_v1*, zwlr_output_head_v1* h) {
    auto* hi = new om_head{};
    hi->head = h;
    zwlr_output_head_v1_add_listener(h, &k_head_listener, hi);
    g_om.heads.push_back(hi);
}
bool g_print_done = true;
void mgr_done(void*, zwlr_output_manager_v1*, uint32_t serial) {
    g_om.prev_serial = g_om.serial;
    g_om.serial = serial;
    ++g_om.dones;
    if (g_print_done) {
        print_heads();
        std::printf("done serial=%u\n", serial);
    }
}
void mgr_finished(void*, zwlr_output_manager_v1*) { std::printf("manager finished\n"); }
const zwlr_output_manager_v1_listener k_manager_listener = {mgr_head, mgr_done, mgr_finished};

void reg_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t version) {
    if (!std::strcmp(iface, zwlr_output_manager_v1_interface.name)) {
        g_om.manager = static_cast<zwlr_output_manager_v1*>(
            wl_registry_bind(r, name, &zwlr_output_manager_v1_interface, version < 4 ? version : 4));
        zwlr_output_manager_v1_add_listener(g_om.manager, &k_manager_listener, nullptr);
    }
}
void reg_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener k_registry_listener = {reg_global, reg_remove};

void c_succeeded(void*, zwlr_output_configuration_v1*) { g_om.result = 0; }
void c_failed(void*, zwlr_output_configuration_v1*) { g_om.result = 1; }
void c_cancelled(void*, zwlr_output_configuration_v1*) { g_om.result = 2; }
const zwlr_output_configuration_v1_listener k_config_listener = {c_succeeded, c_failed, c_cancelled};

om_head* head_named(const std::string& n) {
    for (auto* h : g_om.heads)
        if (!h->finished && h->name == n)
            return h;
    return nullptr;
}

struct head_spec {
    std::string name;
    bool off = false;
    int mode_w = 0, mode_h = 0, mode_r = -1;
    int custom_w = 0, custom_h = 0, custom_r = 0;
    bool pos = false;
    int x = 0, y = 0;
    double scale = 0.0;
    int transform = -1;
    int vrr = -1;
};

bool parse_size(const char* v, int& w, int& h, int& r) {
    r = -1;
    int n = std::sscanf(v, "%dx%d@%d", &w, &h, &r);
    return n >= 2;
}

bool parse_spec(const char* text, std::vector<head_spec>& out) {
    std::string s = text;
    std::size_t start = 0;
    while (start < s.size()) {
        std::size_t end = s.find(';', start);
        if (end == std::string::npos)
            end = s.size();
        const std::string part = s.substr(start, end - start);
        start = end + 1;
        if (part.empty())
            continue;
        const std::size_t colon = part.find(':');
        head_spec h{};
        h.name = part.substr(0, colon);
        if (colon != std::string::npos) {
            const std::string props = part.substr(colon + 1);
            if (props == "off") {
                h.off = true;
            } else {
                std::size_t p = 0;
                while (p < props.size()) {
                    // Keys are separated by ',' — but pos=X,Y holds one: split on ',' followed by
                    // a key name.
                    std::size_t q = p;
                    while (true) {
                        q = props.find(',', q);
                        if (q == std::string::npos)
                            break;
                        if (q + 1 < props.size() && std::isalpha(static_cast<unsigned char>(props[q + 1])))
                            break;
                        ++q;
                    }
                    const std::string kv = props.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    p = q == std::string::npos ? props.size() : q + 1;
                    const std::size_t eq = kv.find('=');
                    if (eq == std::string::npos)
                        return false;
                    const std::string k = kv.substr(0, eq);
                    const char* v = kv.c_str() + eq + 1;
                    if (k == "mode") {
                        if (!parse_size(v, h.mode_w, h.mode_h, h.mode_r))
                            return false;
                    } else if (k == "custom") {
                        if (!parse_size(v, h.custom_w, h.custom_h, h.custom_r))
                            return false;
                        if (h.custom_r < 0)
                            h.custom_r = 0;
                    } else if (k == "pos") {
                        h.pos = std::sscanf(v, "%d,%d", &h.x, &h.y) == 2;
                        if (!h.pos)
                            return false;
                    } else if (k == "scale") {
                        h.scale = std::strtod(v, nullptr);
                    } else if (k == "transform") {
                        h.transform = std::atoi(v);
                    } else if (k == "vrr") {
                        h.vrr = std::atoi(v);
                    } else {
                        return false;
                    }
                }
            }
        }
        out.push_back(h);
    }
    return true;
}

bool connect() {
    g_om.display = wl_display_connect(nullptr);
    if (!g_om.display) {
        std::fprintf(stderr, "output-mgmt: cannot connect\n");
        return false;
    }
    g_om.registry = wl_display_get_registry(g_om.display);
    wl_registry_add_listener(g_om.registry, &k_registry_listener, nullptr);
    if (wl_display_roundtrip(g_om.display) < 0 || !g_om.manager) {
        std::fprintf(stderr, "output-mgmt: no zwlr_output_manager_v1\n");
        return false;
    }
    while (g_om.dones == 0)
        if (wl_display_dispatch(g_om.display) < 0)
            return false;
    return true;
}

int run_error(const char* kind) {
    struct known {
        const char* name;
        const wl_interface* iface;
        uint32_t code;
    };
    const known table[] = {
        {"already_configured_head", &zwlr_output_configuration_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_CONFIGURED_HEAD},
        {"unconfigured_head", &zwlr_output_configuration_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_UNCONFIGURED_HEAD},
        {"already_used", &zwlr_output_configuration_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_V1_ERROR_ALREADY_USED},
        {"already_set", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET},
        {"already_set_custom", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_ALREADY_SET},
        {"invalid_mode", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_MODE},
        {"invalid_custom_mode", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_CUSTOM_MODE},
        {"invalid_transform", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_TRANSFORM},
        {"invalid_scale", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_SCALE},
        {"invalid_adaptive_sync_state", &zwlr_output_configuration_head_v1_interface,
         ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_ERROR_INVALID_ADAPTIVE_SYNC_STATE},
    };
    const known* want = nullptr;
    for (const auto& k : table)
        if (!std::strcmp(k.name, kind))
            want = &k;
    if (!want) {
        std::fprintf(stderr, "output-mgmt: unknown error kind %s\n", kind);
        return 2;
    }
    std::vector<om_head*> live;
    for (auto* h : g_om.heads)
        if (!h->finished)
            live.push_back(h);
    if (live.empty())
        return 2;
    auto* cfg = zwlr_output_manager_v1_create_configuration(g_om.manager, g_om.serial);
    zwlr_output_configuration_v1_add_listener(cfg, &k_config_listener, nullptr);
    const std::string k = kind;
    if (k == "already_configured_head") {
        (void)zwlr_output_configuration_v1_enable_head(cfg, live[0]->head);
        zwlr_output_configuration_v1_disable_head(cfg, live[0]->head);
    } else if (k == "unconfigured_head") {
        if (live.size() < 2) {
            // One head: leaving it out is the only way to leave one out.
            zwlr_output_configuration_v1_test(cfg);
        } else {
            (void)zwlr_output_configuration_v1_enable_head(cfg, live[0]->head);
            zwlr_output_configuration_v1_test(cfg);
        }
    } else if (k == "already_used") {
        for (auto* h : live) {
            if (h->enabled)
                (void)zwlr_output_configuration_v1_enable_head(cfg, h->head);
            else
                zwlr_output_configuration_v1_disable_head(cfg, h->head);
        }
        zwlr_output_configuration_v1_test(cfg);
        (void)zwlr_output_configuration_v1_enable_head(cfg, live[0]->head);
    } else {
        auto* ch = zwlr_output_configuration_v1_enable_head(cfg, live[0]->head);
        if (k == "already_set") {
            zwlr_output_configuration_head_v1_set_position(ch, 0, 0);
            zwlr_output_configuration_head_v1_set_position(ch, 1, 1);
        } else if (k == "already_set_custom") {
            zwlr_output_configuration_head_v1_set_custom_mode(ch, 800, 600, 0);
            if (!live[0]->modes.empty())
                zwlr_output_configuration_head_v1_set_mode(ch, live[0]->modes[0]->mode);
        } else if (k == "invalid_mode") {
            // A mode of another head, or a mode object this head never had.
            om_head* other = live.size() > 1 ? live[1] : nullptr;
            if (other && !other->modes.empty())
                zwlr_output_configuration_head_v1_set_mode(ch, other->modes[0]->mode);
            else
                std::fprintf(stderr, "output-mgmt: invalid_mode needs two heads\n");
        } else if (k == "invalid_custom_mode") {
            zwlr_output_configuration_head_v1_set_custom_mode(ch, 0, 600, 0);
        } else if (k == "invalid_transform") {
            zwlr_output_configuration_head_v1_set_transform(ch, 8);
        } else if (k == "invalid_scale") {
            zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_int(0));
        } else if (k == "invalid_adaptive_sync_state") {
            zwlr_output_configuration_head_v1_set_adaptive_sync(ch, 7);
        }
    }
    for (int i = 0; i < 100 && wl_display_get_error(g_om.display) == 0; ++i)
        if (wl_display_roundtrip(g_om.display) < 0)
            break;
    const wl_interface* iface = nullptr;
    uint32_t id = 0;
    const uint32_t code = wl_display_get_protocol_error(g_om.display, &iface, &id);
    if (wl_display_get_error(g_om.display) != EPROTO) {
        std::printf("no protocol error (result %d)\n", g_om.result);
        return 1;
    }
    std::printf("protocol error interface=%s code=%u\n", iface ? iface->name : "?", code);
    return iface == want->iface && code == want->code ? 0 : 1;
}

int run_config(const char* spec_text, bool test, bool stale) {
    std::vector<head_spec> specs;
    if (!parse_spec(spec_text, specs)) {
        std::fprintf(stderr, "output-mgmt: bad spec %s\n", spec_text);
        return 2;
    }
    for (const auto& s : specs)
        if (!head_named(s.name)) {
            std::fprintf(stderr, "output-mgmt: no head %s\n", s.name.c_str());
            return 2;
        }
    const uint32_t serial = stale ? g_om.serial - 1 : g_om.serial;
    auto* cfg = zwlr_output_manager_v1_create_configuration(g_om.manager, serial);
    zwlr_output_configuration_v1_add_listener(cfg, &k_config_listener, nullptr);
    for (auto* h : g_om.heads) {
        if (h->finished)
            continue;
        const head_spec* s = nullptr;
        for (const auto& sp : specs)
            if (sp.name == h->name)
                s = &sp;
        if ((s && s->off) || (!s && !h->enabled)) {
            zwlr_output_configuration_v1_disable_head(cfg, h->head);
            continue;
        }
        auto* ch = zwlr_output_configuration_v1_enable_head(cfg, h->head);
        if (!s)
            continue;
        if (s->mode_w > 0) {
            om_mode* found = nullptr;
            for (auto* m : h->modes)
                if (!m->finished && m->w == s->mode_w && m->h == s->mode_h &&
                    (s->mode_r < 0 || m->refresh == s->mode_r) &&
                    (!found || m->refresh > found->refresh))
                    found = m;
            if (!found) {
                std::fprintf(stderr, "output-mgmt: %s has no mode %dx%d\n", h->name.c_str(),
                             s->mode_w, s->mode_h);
                return 2;
            }
            zwlr_output_configuration_head_v1_set_mode(ch, found->mode);
        }
        if (s->custom_w > 0)
            zwlr_output_configuration_head_v1_set_custom_mode(ch, s->custom_w, s->custom_h, s->custom_r);
        if (s->pos)
            zwlr_output_configuration_head_v1_set_position(ch, s->x, s->y);
        if (s->transform >= 0)
            zwlr_output_configuration_head_v1_set_transform(ch, s->transform);
        if (s->scale > 0.0)
            zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(s->scale));
        if (s->vrr >= 0)
            zwlr_output_configuration_head_v1_set_adaptive_sync(ch, static_cast<uint32_t>(s->vrr));
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (test)
        zwlr_output_configuration_v1_test(cfg);
    else
        zwlr_output_configuration_v1_apply(cfg);
    while (g_om.result < 0)
        if (wl_display_dispatch(g_om.display) < 0)
            return 3;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("result %s after=%.1fms\n",
                g_om.result == 0 ? "succeeded" : g_om.result == 1 ? "failed" : "cancelled", ms);
    zwlr_output_configuration_v1_destroy(cfg);
    // The new state, if anything changed.
    (void)wl_display_roundtrip(g_om.display);
    return 0;
}

} // namespace

int output_mgmt_main(int argc, char** argv) {
    double watch = 0.0;
    const char* apply = nullptr;
    const char* test = nullptr;
    const char* error = nullptr;
    bool stale = false;
    for (int i = 2; i < argc; ++i) {
        const auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
        if (arg("--watch"))
            watch = std::strtod(argv[++i], nullptr);
        else if (arg("--apply"))
            apply = argv[++i];
        else if (arg("--test"))
            test = argv[++i];
        else if (arg("--error"))
            error = argv[++i];
        else if (!std::strcmp(argv[i], "--stale-serial"))
            stale = true;
        else {
            std::fprintf(stderr, "output-mgmt: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (!connect())
        return 1;
    if (error) {
        g_print_done = false;
        return run_error(error);
    }
    if (apply || test)
        return run_config(apply ? apply : test, test != nullptr, stale);
    if (watch > 0.0) {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(static_cast<long long>(watch * 1000));
        while (std::chrono::steady_clock::now() < end) {
            (void)wl_display_flush(g_om.display);
            if (wl_display_prepare_read(g_om.display) != 0) {
                (void)wl_display_dispatch_pending(g_om.display);
                continue;
            }
            pollfd pfd{wl_display_get_fd(g_om.display), POLLIN, 0};
            if (::poll(&pfd, 1, 50) > 0)
                (void)wl_display_read_events(g_om.display);
            else
                wl_display_cancel_read(g_om.display);
            (void)wl_display_dispatch_pending(g_om.display);
        }
    }
    return 0;
}

#endif
