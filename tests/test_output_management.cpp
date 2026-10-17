// wlr-output-management's wire-free model (ADR 008 §C.1–C.3, `lx.compositor:output_management`):
// what a bound manager is told when heads change (mode tuple identity, a disabled head's
// hidden properties), every protocol error of a configuration as a model result, the order
// of the checks at apply/test (`already_used`, then the serial → `cancelled`, then
// `unconfigured_head`), validation into a complete target (custom modes, restated values,
// scale rounding, adaptive sync, all-disabled, CRTC count), and the job FIFO.
#include <cstdio>
#include <cstring>

import lx.compositor;

namespace {

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

namespace mgmt = lx::compositor::mgmt;

mgmt::head make_head(unsigned key, const char* name, bool enabled) {
    mgmt::head h{};
    h.key = key;
    std::snprintf(h.name, sizeof(h.name), "%s", name);
    std::snprintf(h.description, sizeof(h.description), "%s", name);
    h.modes[0] = {1920, 1080, 60000, true};
    h.modes[1] = {1920, 1080, 50000, false};
    h.modes[2] = {1280, 720, 60000, false};
    h.mode_count = 3;
    h.enabled = enabled;
    h.current = 0;
    h.scale = 1.0;
    return h;
}

mgmt::head_set two_heads() {
    mgmt::head_set s{};
    s.heads[0] = make_head(44, "Virtual-1", true);
    s.heads[1] = make_head(53, "Virtual-2", true);
    s.heads[1].x = 1920;
    s.count = 2;
    return s;
}

} // namespace

int main() {
    std::printf("test_output_management\n");

    // ── what a manager is told ──
    {
        mgmt::head_set a = two_heads();
        mgmt::head_set b = a;
        mgmt::heads_diff d{};
        mgmt::diff_heads(a, b, d);
        check(d.empty(), "identical heads: nothing to send (and no new serial)");

        b.heads[1].x = 1280;
        b.heads[0].scale = 1.5;
        mgmt::diff_heads(a, b, d);
        check(d.changed_count == 2 && d.removed_count == 0 && d.added_count == 0,
              "two heads changed: two change entries");
        check(d.changed_bits[0] == mgmt::changed_scale && d.changed_bits[1] == mgmt::changed_position,
              "only the changed properties are flagged (scale; position)");

        b = a;
        b.heads[0].modes[0].preferred = false;
        b.heads[0].modes[2].preferred = true;
        check((mgmt::head_changes(a.heads[0], b.heads[0]) & mgmt::changed_modes) != 0,
              "a mode whose preferred flag flipped changes the mode list (finished + new)");

        b = a;
        b.heads[1].enabled = false;
        b.heads[1].x = 5;
        check(mgmt::head_changes(a.heads[1], b.heads[1]) == mgmt::changed_enabled,
              "disabling sends `enabled` only (a disabled head shows nothing else)");
        mgmt::head_set c = b;
        c.heads[1].x = 77;
        c.heads[1].scale = 2.0;
        check(mgmt::head_changes(b.heads[1], c.heads[1]) == 0,
              "a disabled head's hidden properties are not changes");

        b = a;
        std::snprintf(b.heads[1].serial, sizeof(b.heads[1].serial), "XYZ");
        mgmt::diff_heads(a, b, d);
        check(d.removed_count == 1 && d.removed[0] == 53 && d.added_count == 1 && d.added[0] == 1,
              "another monitor on the connector: the head is finished and a new one introduced");

        b = a;
        b.count = 1;
        mgmt::diff_heads(a, b, d);
        check(d.removed_count == 1 && d.removed[0] == 53, "an unplugged head is finished");

        b = a;
        b.heads[0].current = -1;
        b.heads[0].custom = {1600, 900, 59990, false};
        check((mgmt::head_changes(a.heads[0], b.heads[0]) & mgmt::changed_mode) != 0,
              "a custom mode becomes the current mode");
    }

    // ── configuration-head errors ──
    {
        mgmt::config_head h{};
        check(mgmt::set_mode(h, {1920, 1080, 60000, true}) == mgmt::config_error::none, "set_mode");
        check(mgmt::set_mode(h, {1920, 1080, 60000, true}) == mgmt::config_error::already_set,
              "set_mode twice: already_set");
        check(mgmt::set_custom_mode(h, 800, 600, 0) == mgmt::config_error::already_set,
              "set_custom_mode after set_mode: already_set");
        mgmt::config_head k{};
        check(mgmt::set_custom_mode(k, 0, 600, 0) == mgmt::config_error::invalid_custom_mode &&
                  mgmt::set_custom_mode(k, 800, 600, -1) == mgmt::config_error::invalid_custom_mode,
              "a custom mode with w ≤ 0 or refresh < 0: invalid_custom_mode");
        check(mgmt::set_custom_mode(k, 800, 600, 0) == mgmt::config_error::none &&
                  mgmt::set_mode(k, {1920, 1080, 60000, true}) == mgmt::config_error::already_set,
              "set_mode after set_custom_mode: already_set");
        check(mgmt::set_position(k, 1, 2) == mgmt::config_error::none &&
                  mgmt::set_position(k, 1, 2) == mgmt::config_error::already_set,
              "set_position twice: already_set");
        check(mgmt::set_transform(k, 8) == mgmt::config_error::invalid_transform &&
                  mgmt::set_transform(k, -1) == mgmt::config_error::invalid_transform,
              "transform outside 0..7: invalid_transform");
        check(mgmt::set_transform(k, 7) == mgmt::config_error::none &&
                  mgmt::set_transform(k, 1) == mgmt::config_error::already_set,
              "set_transform twice: already_set");
        check(mgmt::set_scale(k, 0.0) == mgmt::config_error::invalid_scale &&
                  mgmt::set_scale(k, -1.0) == mgmt::config_error::invalid_scale,
              "scale ≤ 0: invalid_scale");
        check(mgmt::set_scale(k, 1.5) == mgmt::config_error::none &&
                  mgmt::set_scale(k, 1.5) == mgmt::config_error::already_set,
              "set_scale twice: already_set");
        check(mgmt::set_adaptive_sync(k, 2) == mgmt::config_error::invalid_adaptive_sync_state,
              "adaptive sync state 2: invalid_adaptive_sync_state");
        check(mgmt::set_adaptive_sync(k, 0) == mgmt::config_error::none &&
                  mgmt::set_adaptive_sync(k, 1) == mgmt::config_error::already_set,
              "set_adaptive_sync twice: already_set");
        check(mgmt::error_code(mgmt::config_error::already_set) == 1 &&
                  mgmt::error_code(mgmt::config_error::invalid_adaptive_sync_state) == 6 &&
                  mgmt::error_code(mgmt::config_error::already_used) == 3 &&
                  mgmt::error_code(mgmt::config_error::unconfigured_head) == 2,
              "error codes are the protocol's");
    }

    // ── configuration errors and the order of checks ──
    {
        const mgmt::head_set heads = two_heads();
        mgmt::configuration c{};
        c.serial = 7;
        mgmt::config_head* e = nullptr;
        check(mgmt::add_head(c, 44, true, &e) == mgmt::config_error::none && e && e->enabled,
              "enable_head");
        check(mgmt::add_head(c, 44, false) == mgmt::config_error::already_configured_head,
              "disable_head of a configured head: already_configured_head");
        // Head 53 left out, serial current: unconfigured_head.
        mgmt::configuration u = c;
        check(mgmt::submit(u, 7, heads) == mgmt::submit_verdict::unconfigured_head,
              "a head left out: unconfigured_head");
        // The same with a stale serial: cancelled first.
        mgmt::configuration s = c;
        check(mgmt::submit(s, 8, heads) == mgmt::submit_verdict::cancelled,
              "stale serial: cancelled, before unconfigured_head");
        check(mgmt::submit(s, 8, heads) == mgmt::submit_verdict::already_used,
              "apply after apply: already_used");
        check(mgmt::add_head(s, 53, true) == mgmt::config_error::already_used,
              "enable_head after apply: already_used");
        check(mgmt::add_head(c, 53, false) == mgmt::config_error::none &&
                  mgmt::submit(c, 7, heads) == mgmt::submit_verdict::ok,
              "every head configured, current serial: ok");
    }

    // ── resolve ──
    {
        const mgmt::head_set heads = two_heads();
        const auto base = [&](bool second) {
            mgmt::configuration c{};
            c.serial = 1;
            (void)mgmt::add_head(c, 44, true);
            (void)mgmt::add_head(c, 53, second);
            return c;
        };
        mgmt::target t{};
        {
            const auto c = base(true);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::none && t.count == 2,
                  "unset properties: the heads' current state");
            const auto* a = t.find(44);
            check(a && a->enabled && a->mode.width == 1920 && a->mode.refresh_mhz == 60000 &&
                      !a->mode_explicit && !a->position_explicit && a->scale == 1.0,
                  "… kept as they are, nothing explicit");
        }
        {
            mgmt::configuration c{};
            c.serial = 1;
            (void)mgmt::add_head(c, 44, false);
            (void)mgmt::add_head(c, 53, false);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::no_head_enabled,
                  "every head disabled: failed (niri; power is zwlr_output_power_v1's)");
        }
        {
            auto c = base(true);
            (void)mgmt::set_mode(*c.find(44), {1280, 720, 60000, false});
            (void)mgmt::set_position(*c.find(53), 1280, 0);
            (void)mgmt::set_scale(*c.find(53), 1.3);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::none,
                  "mode, position and scale changes resolve");
            const auto* a = t.find(44);
            const auto* b = t.find(53);
            check(a && a->mode.width == 1280 && a->mode_explicit && !a->custom,
                  "set_mode: that mode, explicit");
            check(b && b->x == 1280 && b->position_explicit, "set_position: explicit");
            check(b && b->scale == 156.0 / 120.0, "scale 1.3 rounds to 156/120");
        }
        {
            auto c = base(true);
            (void)mgmt::set_mode(*c.find(44), {1920, 1080, 60000, true});
            (void)mgmt::set_position(*c.find(44), 0, 0);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::none &&
                      !t.find(44)->mode_explicit && !t.find(44)->position_explicit,
                  "restating the current mode and position keeps them automatic (wlr-randr)");
        }
        {
            auto c = base(true);
            (void)mgmt::set_mode(*c.find(44), {1024, 768, 60000, false});
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::mode_gone,
                  "a mode the head does not have: failed");
        }
        {
            auto c = base(true);
            (void)mgmt::set_custom_mode(*c.find(44), 1600, 900, 0);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::none &&
                      t.find(44)->custom && t.find(44)->mode.width == 1600 &&
                      t.find(44)->mode.refresh_mhz == 0,
                  "a custom mode not in the list stays custom (refresh 0: unspecified)");
            auto d = base(true);
            (void)mgmt::set_custom_mode(*d.find(44), 1920, 1080, 0);
            check(mgmt::resolve(d, heads, 8, t) == mgmt::resolve_failure::none &&
                      !t.find(44)->custom && t.find(44)->mode.refresh_mhz == 60000,
                  "a custom mode of a listed size is that mode (the highest refresh)");
            auto e = base(true);
            (void)mgmt::set_custom_mode(*e.find(44), 20000, 900, 0);
            check(mgmt::resolve(e, heads, 8, t) == mgmt::resolve_failure::bad_custom_mode,
                  "a custom mode over 16384: failed");
        }
        {
            auto c = base(true);
            (void)mgmt::set_scale(*c.find(44), 11.0);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::bad_scale,
                  "scale over 10: failed");
        }
        {
            auto c = base(true);
            (void)mgmt::set_adaptive_sync(*c.find(44), 1);
            check(mgmt::resolve(c, heads, 8, t) == mgmt::resolve_failure::adaptive_sync_unsupported,
                  "adaptive sync on a head without vrr_capable: failed");
            mgmt::head_set vrr = heads;
            vrr.heads[0].vrr_capable = true;
            auto d = base(true);
            (void)mgmt::set_adaptive_sync(*d.find(44), 1);
            check(mgmt::resolve(d, vrr, 8, t) == mgmt::resolve_failure::none &&
                      t.find(44)->adaptive_sync,
                  "… and on one with it: enabled");
            auto e = base(true);
            (void)mgmt::set_adaptive_sync(*e.find(44), 0);
            check(mgmt::resolve(e, heads, 8, t) == mgmt::resolve_failure::none,
                  "turning it off is always fine");
        }
        {
            const auto c = base(true);
            check(mgmt::resolve(c, heads, 1, t) == mgmt::resolve_failure::too_many_enabled,
                  "two heads enabled on one CRTC: failed");
        }
        {
            mgmt::head_set h = heads;
            h.heads[1].enabled = false;
            h.heads[1].current = -1; // never enabled
            mgmt::configuration c{};
            c.serial = 1;
            (void)mgmt::add_head(c, 44, true);
            (void)mgmt::add_head(c, 53, true);
            check(mgmt::resolve(c, h, 8, t) == mgmt::resolve_failure::none &&
                      t.find(53)->enabled && t.find(53)->mode.width == 1920 &&
                      t.find(53)->mode.refresh_mhz == 60000 && !t.find(53)->position_explicit,
                  "enabling a disabled head without a mode: its preferred mode, placed automatically");
        }
        {
            mgmt::target cur{};
            mgmt::current_target(heads, cur);
            check(cur.count == 2 && cur.find(53)->x == 1920 && cur.find(53)->enabled,
                  "current_target: the heads as they are");
        }
    }

    // ── job FIFO ──
    {
        mgmt::job_queue q{};
        // Tokens are configuration ids, never addresses: one destroyed while its job runs
        // and a new one allocated at the same address do not share a token.
        const unsigned long long tokens[6]{1, 2, 3, 4, 5, 6};
        bool all = true;
        for (int i = 0; i < 4; ++i) {
            mgmt::job j{};
            j.token = tokens[i];
            all = all && q.push(j);
        }
        mgmt::job extra{};
        extra.token = tokens[4];
        check(all && !q.push(extra) && q.size() == 4, "four jobs wait; a fifth is refused (cancelled)");
        q.detach(tokens[1]);
        check(q.front().token == tokens[0], "first in, first out");
        q.pop();
        check(q.front().token == 0, "a destroyed configuration's job is detached (result dropped)");
        q.pop();
        q.pop();
        q.pop();
        check(q.empty(), "drained");
    }

    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}
