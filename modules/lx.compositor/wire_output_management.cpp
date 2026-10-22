module;

// zwlr_output_manager_v1 v4 (wlr-output-management-unstable-v1.xml, ADR 008 Part C): heads
// and modes on the wire, configurations and their protocol errors. Part of the protocol wire
// split; shared declarations live in the :wire partition (wire.cppm), the model — what
// changed, every error, validation, the job queue — in :output_management (pure, unit-tested
// on its own), and what an apply does in compositor_impl.cpp.
//
// Objects. Per bound manager there is one head object per head, and per head object one mode
// object per mode (plus one for a custom mode in use — wlroots' virtual mode). All are
// allocated per object and freed by their destructors, in intrusive lists like wl_output's
// records: a client that never releases cannot exhaust a shared table. A head that goes is
// sent `finished` (its modes first) and its objects go inert; a mode whose tuple changes or
// disappears likewise, and a new object is introduced. After any change every manager is
// sent `done` with a new serial, once per loop iteration (the compositor coalesces calls).
//
// Configurations. Requests are checked in the spec's order and every error is raised —
// `already_set` and `unconfigured_head` too, which wlroots and niri leave unchecked. A
// configuration whose serial is not current gets `cancelled`, checked before
// `unconfigured_head` (a head that appeared after the client's `done` is not its fault). A
// mode object that is no longer the head's is `invalid_mode` — unless the configuration's
// serial is stale anyway (the mode went with the change that moved the serial), where the
// apply is answered `cancelled` instead of killing a client that only raced a hotplug.
// Configuration head objects are destroyed at apply/test and with their configuration, as
// the protocol says (wlroots does the same). A passed apply/test is copied into the job
// queue: the client may destroy its configuration meanwhile; the job still runs, and its
// answer is dropped.
//
// Sandboxed clients do not see the global (lx.wayland.server's filter, ADR 008 C5).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if defined(LUMEN_HAS_WAYLAND)
#include <wayland-server.h>
#if defined(LUMEN_PROTOCOL_WLR_OUTPUT_MANAGEMENT_UNSTABLE_V1)
#include "wlr-output-management-unstable-v1-server-protocol.h"
#define LUMEN_HAS_OUTPUT_MANAGEMENT 1
#endif
#endif

module lx.compositor;

import std;

import lx.foundation;
import lx.wayland.server;
import lx.trace;
import :output_management;
import :wire;

namespace mg = lx::compositor::mgmt;

#if defined(LUMEN_HAS_WAYLAND)

namespace lx::compositor::p0 {

namespace {

#if defined(LUMEN_HAS_OUTPUT_MANAGEMENT)

constexpr const char* k_category = "compositor.outputcfg";

struct manager_obj;
struct head_obj;

struct mode_obj {
    wl_resource* resource = nullptr;
    /// The head object it was introduced on; null once that object went.
    head_obj* head = nullptr;
    /// The head it belongs to (outlives the head object: a released head's mode may still
    /// be named in a configuration).
    unsigned head_key = 0;
    mg::mode mode{};
    /// The virtual mode object of a custom mode in use.
    bool custom = false;
    /// False once `finished`.
    bool live = false;
    mode_obj* prev = nullptr;
    mode_obj* next = nullptr;
};

struct head_obj {
    wl_resource* resource = nullptr;
    manager_obj* manager = nullptr;
    unsigned key = 0;
    /// False once `finished`, or its manager stopped.
    bool live = false;
    head_obj* prev = nullptr;
    head_obj* next = nullptr;
};

struct manager_obj {
    wl_resource* resource = nullptr;
    manager_obj* prev = nullptr;
    manager_obj* next = nullptr;
};

struct config_obj {
    wl_resource* resource = nullptr;
    /// Its jobs' token (`mg::job::token`): unique for the compositor's life.
    unsigned long long id = 0;
    mg::configuration cfg{};
    /// succeeded / failed / cancelled was sent.
    bool answered = false;
    config_obj* prev = nullptr;
    config_obj* next = nullptr;
};

struct config_head_obj {
    wl_resource* resource = nullptr;
    config_obj* config = nullptr;
    unsigned key = 0;
    /// Made for a head that is gone (or after an error): its requests do nothing.
    bool inert = false;
    config_head_obj* prev = nullptr;
    config_head_obj* next = nullptr;
};

struct mgmt_state {
    manager_obj* managers = nullptr;
    head_obj* heads = nullptr;
    mode_obj* modes = nullptr;
    config_obj* configs = nullptr;
    config_head_obj* config_heads = nullptr;
    /// The heads every manager was last told about (heap: ~50 KB).
    mg::head_set* current = nullptr;
    unsigned serial = 0;
    mg::job_queue jobs{};
    /// The last configuration id handed out (`config_obj::id`; 64 bits never wrap).
    unsigned long long last_config_id = 0;
};

mgmt_state g{};

template <class T> void push(T*& head, T* n) {
    n->prev = nullptr;
    n->next = head;
    if (head)
        head->prev = n;
    head = n;
}

template <class T> void unlink(T*& head, T* n) {
    if (n->prev)
        n->prev->next = n->next;
    else
        head = n->next;
    if (n->next)
        n->next->prev = n->prev;
    n->prev = n->next = nullptr;
}

[[nodiscard]] bool debug_on() {
    return lx::trace::logger::global().min_level() <= lx::trace::level::debug;
}

const mg::head_set& heads_now() {
    static const mg::head_set k_empty{};
    return g.current ? *g.current : k_empty;
}

// ── modes ──

mode_obj* mode_from(wl_resource* r) {
    auto* m = r ? static_cast<mode_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return m && m->resource == r ? m : nullptr;
}

void mode_destroy_resource(wl_resource* r) {
    mode_obj* m = mode_from(r);
    if (!m)
        return;
    unlink(g.modes, m);
    delete m;
}

const struct zwlr_output_mode_v1_interface mode_impl = {
    .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

void send_mode_state(mode_obj* m) {
    zwlr_output_mode_v1_send_size(m->resource, m->mode.width, m->mode.height);
    if (m->mode.refresh_mhz > 0)
        zwlr_output_mode_v1_send_refresh(m->resource, m->mode.refresh_mhz);
    if (m->mode.preferred)
        zwlr_output_mode_v1_send_preferred(m->resource);
}

/// A new mode object on head object `h`, introduced with `head.mode` and its state.
mode_obj* introduce_mode(head_obj* h, const mg::mode& mode, bool custom) {
    wl_client* client = wl_resource_get_client(h->resource);
    wl_resource* r = wl_resource_create(client, &zwlr_output_mode_v1_interface,
                                        wl_resource_get_version(h->resource), 0);
    if (!r) {
        wl_client_post_no_memory(client);
        return nullptr;
    }
    auto* m = new (std::nothrow) mode_obj{};
    if (!m) {
        wl_resource_set_implementation(r, &mode_impl, nullptr, nullptr);
        wl_client_post_no_memory(client);
        return nullptr;
    }
    m->resource = r;
    m->head = h;
    m->head_key = h->key;
    m->mode = mode;
    m->custom = custom;
    m->live = true;
    push(g.modes, m);
    wl_resource_set_implementation(r, &mode_impl, m, &mode_destroy_resource);
    zwlr_output_head_v1_send_mode(h->resource, r);
    send_mode_state(m);
    return m;
}

void finish_mode(mode_obj* m) {
    if (!m->live)
        return;
    m->live = false;
    if (!client_dying(wl_resource_get_client(m->resource)))
        zwlr_output_mode_v1_send_finished(m->resource);
}

/// Head object `h`'s live mode object with `mode`'s tuple (custom or listed).
mode_obj* find_mode(head_obj* h, const mg::mode& mode, bool custom) {
    for (mode_obj* m = g.modes; m; m = m->next)
        if (m->head == h && m->live && m->custom == custom &&
            (custom ? m->mode.same_size_refresh(mode) : m->mode.same(mode)))
            return m;
    return nullptr;
}

/// The mode objects of `h` follow `hd`: tuples gone are finished, new ones introduced; the
/// custom mode object exists exactly while the head is enabled at a custom mode.
void sync_modes(head_obj* h, const mg::head& hd) {
    for (mode_obj* m = g.modes; m; m = m->next) {
        if (m->head != h || !m->live)
            continue;
        bool keep = false;
        if (m->custom) {
            keep = hd.enabled && hd.current < 0 && m->mode.same_size_refresh(hd.custom);
        } else {
            for (unsigned i = 0; i < hd.mode_count && !keep; ++i)
                keep = m->mode.same(hd.modes[i]);
        }
        if (!keep)
            finish_mode(m);
    }
    for (unsigned i = 0; i < hd.mode_count; ++i)
        if (!find_mode(h, hd.modes[i], false))
            (void)introduce_mode(h, hd.modes[i], false);
    if (hd.enabled && hd.current < 0 && !find_mode(h, hd.custom, true))
        (void)introduce_mode(h, hd.custom, true);
}

// ── heads ──

head_obj* head_from(wl_resource* r) {
    auto* h = r ? static_cast<head_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return h && h->resource == r ? h : nullptr;
}

void head_destroy_resource(wl_resource* r) {
    head_obj* h = head_from(r);
    if (!h)
        return;
    for (mode_obj* m = g.modes; m; m = m->next)
        if (m->head == h)
            m->head = nullptr;
    unlink(g.heads, h);
    delete h;
}

const struct zwlr_output_head_v1_interface head_impl = {
    .release = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

/// The head state events `bits` (`mgmt::changed_*`) call for. Enabling sends everything.
void send_head_state(head_obj* h, const mg::head& hd, unsigned bits) {
    if (bits & mg::changed_enabled) {
        zwlr_output_head_v1_send_enabled(h->resource, hd.enabled ? 1 : 0);
        bits = ~0u;
    }
    if (!hd.enabled)
        return;
    if (bits & (mg::changed_mode | mg::changed_modes)) {
        if (mode_obj* m = find_mode(h, hd.current_mode(), hd.current < 0))
            zwlr_output_head_v1_send_current_mode(h->resource, m->resource);
    }
    if (bits & mg::changed_position)
        zwlr_output_head_v1_send_position(h->resource, hd.x, hd.y);
    if (bits & mg::changed_transform)
        zwlr_output_head_v1_send_transform(h->resource, static_cast<int32_t>(hd.transform));
    if (bits & mg::changed_scale)
        zwlr_output_head_v1_send_scale(h->resource, wl_fixed_from_double(hd.scale));
    if ((bits & mg::changed_adaptive_sync) &&
        wl_resource_get_version(h->resource) >= ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_SINCE_VERSION)
        zwlr_output_head_v1_send_adaptive_sync(h->resource,
                                               hd.adaptive_sync
                                                   ? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
                                                   : ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
}

/// A new head object for manager `mgr`: `manager.head`, the properties sent once, the modes,
/// then the state.
void introduce_head(manager_obj* mgr, const mg::head& hd) {
    wl_client* client = wl_resource_get_client(mgr->resource);
    const int version = wl_resource_get_version(mgr->resource);
    wl_resource* r = wl_resource_create(client, &zwlr_output_head_v1_interface, version, 0);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* h = new (std::nothrow) head_obj{};
    if (!h) {
        wl_resource_set_implementation(r, &head_impl, nullptr, nullptr);
        wl_client_post_no_memory(client);
        return;
    }
    h->resource = r;
    h->manager = mgr;
    h->key = hd.key;
    h->live = true;
    push(g.heads, h);
    wl_resource_set_implementation(r, &head_impl, h, &head_destroy_resource);
    zwlr_output_manager_v1_send_head(mgr->resource, r);
    zwlr_output_head_v1_send_name(r, hd.name);
    zwlr_output_head_v1_send_description(r, hd.description[0] ? hd.description : "Unknown");
    if (hd.phys_w_mm > 0 && hd.phys_h_mm > 0)
        zwlr_output_head_v1_send_physical_size(r, hd.phys_w_mm, hd.phys_h_mm);
    if (version >= ZWLR_OUTPUT_HEAD_V1_MAKE_SINCE_VERSION) {
        // Only what the EDID gave (wlroots sends nothing for an absent string).
        if (hd.make[0])
            zwlr_output_head_v1_send_make(r, hd.make);
        if (hd.model[0])
            zwlr_output_head_v1_send_model(r, hd.model);
        if (hd.serial[0])
            zwlr_output_head_v1_send_serial_number(r, hd.serial);
    }
    sync_modes(h, hd);
    send_head_state(h, hd, ~0u);
}

void finish_head(head_obj* h) {
    if (!h->live)
        return;
    h->live = false;
    const bool dying = client_dying(wl_resource_get_client(h->resource));
    for (mode_obj* m = g.modes; m; m = m->next)
        if (m->head == h)
            finish_mode(m);
    if (!dying)
        zwlr_output_head_v1_send_finished(h->resource);
}

// ── configuration heads ──

config_head_obj* config_head_from(wl_resource* r) {
    auto* c = r ? static_cast<config_head_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return c && c->resource == r ? c : nullptr;
}

void config_head_destroy_resource(wl_resource* r) {
    config_head_obj* c = config_head_from(r);
    if (!c)
        return;
    unlink(g.config_heads, c);
    delete c;
}

/// The entry a configuration head edits; null for an inert one.
mg::config_head* entry_of(wl_resource* r) {
    config_head_obj* c = config_head_from(r);
    if (!c || c->inert || !c->config || c->config->cfg.used)
        return nullptr;
    return c->config->cfg.find(c->key);
}

void post(wl_resource* r, mg::config_error e) {
    if (e == mg::config_error::none)
        return;
    wl_resource_post_error(r, mg::error_code(e), "%s", mg::error_text(e));
}

void ch_set_mode(struct wl_client*, struct wl_resource* r, struct wl_resource* mode_res) {
    mg::config_head* e = entry_of(r);
    if (!e)
        return; // inert
    config_head_obj* c = config_head_from(r);
    mode_obj* m = mode_from(mode_res);
    if (!m || !m->live) {
        // A finished mode: the change that finished it moved the serial. With a stale
        // configuration that is a race the apply answers with `cancelled`; with a current
        // one the client used an object it was told is gone.
        if (c->config->cfg.serial != g.serial) {
            if (e->mode_set)
                post(r, mg::config_error::already_set);
            e->mode_set = true;
            return;
        }
        post(r, mg::config_error::invalid_mode);
        return;
    }
    if (m->head_key != c->key) {
        post(r, mg::config_error::invalid_mode);
        return;
    }
    // The virtual mode object of a custom mode stands for that mode (wlroots).
    post(r, m->custom ? mg::set_custom_mode(*e, m->mode.width, m->mode.height, m->mode.refresh_mhz)
                      : mg::set_mode(*e, m->mode));
}

void ch_set_custom_mode(struct wl_client*, struct wl_resource* r, int32_t w, int32_t h,
                        int32_t refresh) {
    if (mg::config_head* e = entry_of(r))
        post(r, mg::set_custom_mode(*e, w, h, refresh));
}

void ch_set_position(struct wl_client*, struct wl_resource* r, int32_t x, int32_t y) {
    if (mg::config_head* e = entry_of(r))
        post(r, mg::set_position(*e, x, y));
}

void ch_set_transform(struct wl_client*, struct wl_resource* r, int32_t t) {
    if (mg::config_head* e = entry_of(r))
        post(r, mg::set_transform(*e, t));
}

void ch_set_scale(struct wl_client*, struct wl_resource* r, wl_fixed_t s) {
    if (mg::config_head* e = entry_of(r))
        post(r, mg::set_scale(*e, wl_fixed_to_double(s)));
}

void ch_set_adaptive_sync(struct wl_client*, struct wl_resource* r, uint32_t state) {
    if (mg::config_head* e = entry_of(r))
        post(r, mg::set_adaptive_sync(*e, state));
}

const struct zwlr_output_configuration_head_v1_interface config_head_impl = {
    .set_mode = &ch_set_mode,
    .set_custom_mode = &ch_set_custom_mode,
    .set_position = &ch_set_position,
    .set_transform = &ch_set_transform,
    .set_scale = &ch_set_scale,
    .set_adaptive_sync = &ch_set_adaptive_sync,
};

// ── configurations ──

config_obj* config_from(wl_resource* r) {
    auto* c = r ? static_cast<config_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return c && c->resource == r ? c : nullptr;
}

/// The configuration heads of `c` go (apply/test, or the configuration's destruction): the
/// protocol gives them no destructor of their own.
void drop_config_heads(config_obj* c) {
    for (config_head_obj* h = g.config_heads; h;) {
        config_head_obj* next = h->next;
        if (h->config == c) {
            unlink(g.config_heads, h);
            wl_resource* r = h->resource;
            wl_resource_set_user_data(r, nullptr);
            wl_resource_set_destructor(r, nullptr);
            delete h;
            if (!client_dying(wl_resource_get_client(r)))
                wl_resource_destroy(r);
        }
        h = next;
    }
}

void config_destroy_resource(wl_resource* r) {
    config_obj* c = config_from(r);
    if (!c)
        return;
    g.jobs.detach(c->id);
    drop_config_heads(c);
    unlink(g.configs, c);
    delete c;
}

void cfg_enable_head(struct wl_client* client, struct wl_resource* r, uint32_t id,
                     struct wl_resource* head_res) {
    config_obj* c = config_from(r);
    if (!c || c->cfg.used) {
        post(r, mg::config_error::already_used);
        return;
    }
    head_obj* h = head_from(head_res);
    wl_resource* ch = wl_resource_create(client, &zwlr_output_configuration_head_v1_interface,
                                         wl_resource_get_version(r), id);
    if (!ch) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* o = new (std::nothrow) config_head_obj{};
    if (!o) {
        wl_resource_set_implementation(ch, &config_head_impl, nullptr, nullptr);
        wl_client_post_no_memory(client);
        return;
    }
    o->resource = ch;
    o->config = c;
    push(g.config_heads, o);
    wl_resource_set_implementation(ch, &config_head_impl, o, &config_head_destroy_resource);
    if (!h || !h->live) {
        // A head that is gone: an inert configuration head (its requests do nothing); the
        // serial moved with it, so the apply is `cancelled`.
        o->inert = true;
        return;
    }
    o->key = h->key;
    if (const auto e = mg::add_head(c->cfg, h->key, true); e != mg::config_error::none) {
        o->inert = true;
        post(r, e);
    }
}

void cfg_disable_head(struct wl_client*, struct wl_resource* r, struct wl_resource* head_res) {
    config_obj* c = config_from(r);
    if (!c || c->cfg.used) {
        post(r, mg::config_error::already_used);
        return;
    }
    head_obj* h = head_from(head_res);
    if (!h || !h->live)
        return;
    post(r, mg::add_head(c->cfg, h->key, false));
}

void submit(struct wl_resource* r, bool test) {
    config_obj* c = config_from(r);
    if (!c) {
        post(r, mg::config_error::already_used);
        return;
    }
    const mg::submit_verdict v = mg::submit(c->cfg, g.serial, heads_now());
    switch (v) {
    case mg::submit_verdict::already_used:
        post(r, mg::config_error::already_used);
        return;
    case mg::submit_verdict::unconfigured_head:
        post(r, mg::config_error::unconfigured_head);
        return;
    case mg::submit_verdict::cancelled:
        drop_config_heads(c);
        c->answered = true;
        zwlr_output_configuration_v1_send_cancelled(r);
        if (debug_on()) {
            lx::trace::logger::global().logf(lx::trace::level::debug, k_category,
                                             "{} cancelled: serial {}, current {}",
                                             test ? "test" : "apply", c->cfg.serial, g.serial);
        }
        return;
    case mg::submit_verdict::ok:
        break;
    }
    drop_config_heads(c);
    mg::job j{};
    j.config = c->cfg;
    j.test = test;
    j.token = c->id;
    if (!g.jobs.push(j)) {
        // Four already wait behind the one running: the client is far ahead of what can be
        // applied; this one is answered at once (§C.3 step 7).
        c->answered = true;
        zwlr_output_configuration_v1_send_cancelled(r);
        return;
    }
    if (g_p0_ctx && g_p0_ctx->output_mgmt_kick)
        g_p0_ctx->output_mgmt_kick(g_p0_ctx->output_mgmt_user);
}

const struct zwlr_output_configuration_v1_interface config_impl = {
    .enable_head = &cfg_enable_head,
    .disable_head = &cfg_disable_head,
    .apply = [](struct wl_client*, struct wl_resource* r) { submit(r, false); },
    .test = [](struct wl_client*, struct wl_resource* r) { submit(r, true); },
    .destroy = [](struct wl_client*, struct wl_resource* r) { wl_resource_destroy(r); },
};

// ── manager ──

manager_obj* manager_from(wl_resource* r) {
    auto* m = r ? static_cast<manager_obj*>(wl_resource_get_user_data(r)) : nullptr;
    return m && m->resource == r ? m : nullptr;
}

void manager_destroy_resource(wl_resource* r) {
    manager_obj* m = manager_from(r);
    if (!m)
        return;
    // Its head objects hear nothing more (the client stopped, or is gone).
    for (head_obj* h = g.heads; h; h = h->next)
        if (h->manager == m) {
            h->manager = nullptr;
            h->live = false;
        }
    unlink(g.managers, m);
    delete m;
}

void mgr_create_configuration(struct wl_client* client, struct wl_resource* r, uint32_t id,
                              uint32_t serial) {
    wl_resource* cr = wl_resource_create(client, &zwlr_output_configuration_v1_interface,
                                         wl_resource_get_version(r), id);
    if (!cr) {
        wl_client_post_no_memory(client);
        return;
    }
    auto* c = new (std::nothrow) config_obj{};
    if (!c) {
        wl_resource_set_implementation(cr, &config_impl, nullptr, nullptr);
        wl_client_post_no_memory(client);
        return;
    }
    c->resource = cr;
    c->id = ++g.last_config_id;
    c->cfg.serial = serial;
    push(g.configs, c);
    wl_resource_set_implementation(cr, &config_impl, c, &config_destroy_resource);
}

void mgr_stop(struct wl_client*, struct wl_resource* r) {
    // `finished` is the destructor event: the object is gone on both sides after it.
    zwlr_output_manager_v1_send_finished(r);
    wl_resource_destroy(r);
}

const struct zwlr_output_manager_v1_interface manager_impl = {
    .create_configuration = &mgr_create_configuration,
    .stop = &mgr_stop,
};

#endif // LUMEN_HAS_OUTPUT_MANAGEMENT

} // namespace

#if defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
void bind_output_manager(wayland::client_connection& client, unsigned id, int version) {
    auto* wc = static_cast<wl_client*>(client.native());
    wl_resource* r = wl_resource_create(wc, &zwlr_output_manager_v1_interface, version,
                                        static_cast<uint32_t>(id));
    if (!r) {
        wl_client_post_no_memory(wc);
        return;
    }
    auto* m = new (std::nothrow) manager_obj{};
    if (!m) {
        wl_resource_set_implementation(r, &manager_impl, nullptr, nullptr);
        wl_client_post_no_memory(wc);
        return;
    }
    m->resource = r;
    push(g.managers, m);
    wl_resource_set_implementation(r, &manager_impl, m, &manager_destroy_resource);
    const mg::head_set& hs = heads_now();
    for (unsigned i = 0; i < hs.count; ++i)
        introduce_head(m, hs.heads[i]);
    zwlr_output_manager_v1_send_done(r, g.serial);
}
#endif

} // namespace lx::compositor::p0

#endif // LUMEN_HAS_WAYLAND

void lx::compositor::output_heads_changed(p0_protocol_context& ctx, const mgmt::head_set& heads) {
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
    using namespace p0;
    if (!g.current) {
        g.current = new (std::nothrow) mg::head_set{};
        if (!g.current)
            return;
    }
    mg::heads_diff d{};
    mg::diff_heads(*g.current, heads, d);
    if (d.empty() && g.serial != 0)
        return;
    // Heads gone (or another monitor on their connector) first, then what changed, then
    // what is new — every object of every manager.
    for (unsigned i = 0; i < d.removed_count; ++i)
        for (head_obj* h = g.heads; h; h = h->next)
            if (h->live && h->key == d.removed[i])
                finish_head(h);
    for (unsigned i = 0; i < d.changed_count; ++i) {
        const mg::head& hd = heads.heads[d.changed[i]];
        unsigned bits = d.changed_bits[i];
        for (head_obj* h = g.heads; h; h = h->next) {
            if (!h->live || h->key != hd.key || client_dying(wl_resource_get_client(h->resource)))
                continue;
            // A new mode list or a custom mode coming or going: the current mode may be a new
            // object, so `current_mode` is sent again.
            if (bits & (mg::changed_modes | mg::changed_mode | mg::changed_enabled))
                sync_modes(h, hd);
            send_head_state(h, hd, bits);
        }
    }
    for (unsigned i = 0; i < d.added_count; ++i)
        for (manager_obj* m = g.managers; m; m = m->next)
            if (!client_dying(wl_resource_get_client(m->resource)))
                introduce_head(m, heads.heads[d.added[i]]);
    *g.current = heads;
    if (ctx.server && ctx.server->native_display())
        g.serial = wl_display_next_serial(static_cast<wl_display*>(ctx.server->native_display()));
    else
        ++g.serial;
    for (manager_obj* m = g.managers; m; m = m->next)
        if (!client_dying(wl_resource_get_client(m->resource)))
            zwlr_output_manager_v1_send_done(m->resource, g.serial);
    if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
        lx::trace::logger::global().logf(lx::trace::level::debug, k_category,
                                         "heads done serial={} heads={} added={} removed={} "
                                         "changed={}",
                                         g.serial, heads.count, d.added_count, d.removed_count,
                                         d.changed_count);
    }
#else
    (void)ctx;
    (void)heads;
#endif
}

const lx::compositor::mgmt::head_set&
lx::compositor::output_mgmt_heads(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
    return p0::heads_now();
#else
    static const mg::head_set k_empty{};
    return k_empty;
#endif
}

unsigned lx::compositor::output_mgmt_serial(const p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
    return p0::g.serial;
#else
    return 0;
#endif
}

lx::compositor::mgmt::job_queue& lx::compositor::output_mgmt_jobs(p0_protocol_context& ctx) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
    return p0::g.jobs;
#else
    static mg::job_queue k_none{};
    return k_none;
#endif
}

void lx::compositor::output_mgmt_finish(p0_protocol_context& ctx, unsigned long long token,
                                        mgmt_result result) {
    (void)ctx;
#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_OUTPUT_MANAGEMENT)
    using namespace p0;
    if (!token)
        return;
    for (config_obj* c = g.configs; c; c = c->next) {
        if (c->id != token || c->answered)
            continue;
        c->answered = true;
        if (client_dying(wl_resource_get_client(c->resource)))
            return;
        switch (result) {
        case mgmt_result::succeeded: zwlr_output_configuration_v1_send_succeeded(c->resource); break;
        case mgmt_result::failed: zwlr_output_configuration_v1_send_failed(c->resource); break;
        case mgmt_result::cancelled: zwlr_output_configuration_v1_send_cancelled(c->resource); break;
        }
        return;
    }
#else
    (void)token;
    (void)result;
#endif
}
