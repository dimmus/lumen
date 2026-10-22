module;

// wlr-output-management's wire-free model (`:output_management`). See the partition.

module lx.compositor;

import std;

import :output;
import :output_management;

namespace lx::compositor::mgmt {

int head::preferred_index() const {
    for (unsigned i = 0; i < mode_count; ++i)
        if (modes[i].preferred)
            return static_cast<int>(i);
    return mode_count > 0 ? 0 : -1;
}

const head* head_set::find(unsigned key) const {
    for (unsigned i = 0; i < count; ++i)
        if (heads[i].key == key)
            return &heads[i];
    return nullptr;
}

bool same_identity(const head& a, const head& b) {
    return a.key == b.key && !std::strcmp(a.name, b.name) &&
           !std::strcmp(a.description, b.description) && !std::strcmp(a.make, b.make) &&
           !std::strcmp(a.model, b.model) && !std::strcmp(a.serial, b.serial) &&
           a.phys_w_mm == b.phys_w_mm && a.phys_h_mm == b.phys_h_mm;
}

unsigned head_changes(const head& a, const head& b) {
    unsigned bits = 0;
    if (a.mode_count != b.mode_count) {
        bits |= changed_modes;
    } else {
        for (unsigned i = 0; i < a.mode_count; ++i)
            if (!a.modes[i].same(b.modes[i])) {
                bits |= changed_modes;
                break;
            }
    }
    if (a.enabled != b.enabled)
        bits |= changed_enabled;
    if (!b.enabled)
        return bits; // nothing else of a disabled head is shown
    const mode ma = a.current_mode();
    const mode mb = b.current_mode();
    if (!ma.same(mb) || (a.current < 0) != (b.current < 0))
        bits |= changed_mode;
    if (a.x != b.x || a.y != b.y)
        bits |= changed_position;
    if (a.transform != b.transform)
        bits |= changed_transform;
    if (a.scale != b.scale)
        bits |= changed_scale;
    if (a.adaptive_sync != b.adaptive_sync)
        bits |= changed_adaptive_sync;
    return bits;
}

void diff_heads(const head_set& before, const head_set& after, heads_diff& out) {
    out = heads_diff{};
    for (unsigned i = 0; i < before.count; ++i) {
        const head* now = after.find(before.heads[i].key);
        if (!now || !same_identity(before.heads[i], *now))
            out.removed[out.removed_count++] = before.heads[i].key;
    }
    for (unsigned i = 0; i < after.count; ++i) {
        const head& h = after.heads[i];
        const head* was = before.find(h.key);
        if (!was || !same_identity(*was, h)) {
            out.added[out.added_count++] = i;
            continue;
        }
        const unsigned bits = head_changes(*was, h);
        if (bits) {
            out.changed[out.changed_count] = i;
            out.changed_bits[out.changed_count] = bits;
            ++out.changed_count;
        }
    }
}

unsigned error_code(config_error e) {
    // zwlr_output_configuration_v1.error and zwlr_output_configuration_head_v1.error values.
    switch (e) {
    case config_error::none: return 0;
    case config_error::already_configured_head: return 1;
    case config_error::unconfigured_head: return 2;
    case config_error::already_used: return 3;
    case config_error::already_set: return 1;
    case config_error::invalid_mode: return 2;
    case config_error::invalid_custom_mode: return 3;
    case config_error::invalid_transform: return 4;
    case config_error::invalid_scale: return 5;
    case config_error::invalid_adaptive_sync_state: return 6;
    }
    return 0;
}

const char* error_text(config_error e) {
    switch (e) {
    case config_error::none: return "";
    case config_error::already_configured_head: return "head has already been configured";
    case config_error::unconfigured_head: return "a head was left out of the configuration";
    case config_error::already_used: return "configuration object has already been used";
    case config_error::already_set: return "property has already been set";
    case config_error::invalid_mode: return "mode doesn't belong to head";
    case config_error::invalid_custom_mode: return "invalid custom mode";
    case config_error::invalid_transform: return "transform value outside wl_output.transform";
    case config_error::invalid_scale: return "scale negative or zero";
    case config_error::invalid_adaptive_sync_state: return "invalid adaptive sync state";
    }
    return "";
}

const char* failure_text(resolve_failure f) {
    switch (f) {
    case resolve_failure::none: return "";
    case resolve_failure::no_head_enabled: return "every head disabled";
    case resolve_failure::bad_custom_mode: return "custom mode outside 1..16384";
    case resolve_failure::mode_gone: return "the mode is no longer the head's";
    case resolve_failure::bad_scale: return "scale out of range";
    case resolve_failure::adaptive_sync_unsupported: return "adaptive sync not supported";
    case resolve_failure::too_many_enabled: return "more enabled heads than CRTCs";
    }
    return "";
}

config_error set_mode(config_head& h, const mode& m) {
    if (h.mode_set)
        return config_error::already_set;
    h.mode_set = true;
    h.custom = false;
    h.mode = m;
    return config_error::none;
}

config_error set_custom_mode(config_head& h, int width, int height, int refresh_mhz) {
    if (h.mode_set)
        return config_error::already_set;
    if (width <= 0 || height <= 0 || refresh_mhz < 0)
        return config_error::invalid_custom_mode;
    h.mode_set = true;
    h.custom = true;
    h.mode = mode{width, height, refresh_mhz, false};
    return config_error::none;
}

config_error set_position(config_head& h, int x, int y) {
    if (h.position_set)
        return config_error::already_set;
    h.position_set = true;
    h.x = x;
    h.y = y;
    return config_error::none;
}

config_error set_transform(config_head& h, int transform) {
    if (h.transform_set)
        return config_error::already_set;
    if (transform < 0 || transform > 7)
        return config_error::invalid_transform;
    h.transform_set = true;
    h.transform = static_cast<unsigned>(transform);
    return config_error::none;
}

config_error set_scale(config_head& h, double scale) {
    if (h.scale_set)
        return config_error::already_set;
    if (!(scale > 0.0))
        return config_error::invalid_scale;
    h.scale_set = true;
    h.scale = scale;
    return config_error::none;
}

config_error set_adaptive_sync(config_head& h, unsigned state) {
    if (h.adaptive_sync_set)
        return config_error::already_set;
    if (state > 1)
        return config_error::invalid_adaptive_sync_state;
    h.adaptive_sync_set = true;
    h.adaptive_sync = state == 1;
    return config_error::none;
}

config_head* configuration::find(unsigned key) {
    for (unsigned i = 0; i < count; ++i)
        if (heads[i].key == key)
            return &heads[i];
    return nullptr;
}

const config_head* configuration::find(unsigned key) const {
    for (unsigned i = 0; i < count; ++i)
        if (heads[i].key == key)
            return &heads[i];
    return nullptr;
}

config_error add_head(configuration& c, unsigned key, bool enabled, config_head** out) {
    if (out)
        *out = nullptr;
    if (c.used)
        return config_error::already_used;
    if (c.find(key))
        return config_error::already_configured_head;
    if (c.count >= k_max_heads)
        return config_error::already_configured_head; // more heads than exist: one is twice
    config_head& h = c.heads[c.count++];
    h = config_head{};
    h.key = key;
    h.enabled = enabled;
    if (out)
        *out = &h;
    return config_error::none;
}

submit_verdict submit(configuration& c, unsigned current_serial, const head_set& heads) {
    if (c.used)
        return submit_verdict::already_used;
    c.used = true;
    if (c.serial != current_serial)
        return submit_verdict::cancelled;
    for (unsigned i = 0; i < heads.count; ++i)
        if (!c.find(heads.heads[i].key))
            return submit_verdict::unconfigured_head;
    return submit_verdict::ok;
}

const head_target* target::find(unsigned key) const {
    for (unsigned i = 0; i < count; ++i)
        if (heads[i].key == key)
            return &heads[i];
    return nullptr;
}

namespace {

void from_head(const head& h, head_target& t) {
    t.key = h.key;
    t.enabled = h.enabled;
    t.mode = h.current_mode();
    t.custom = h.current < 0;
    t.mode_explicit = false;
    t.x = h.x;
    t.y = h.y;
    // A disabled head keeps the position it had (wlroots keeps the head's last state).
    t.position_explicit = !h.auto_position;
    t.transform = h.transform;
    t.scale = h.scale;
    t.adaptive_sync = h.adaptive_sync;
}

} // namespace

void current_target(const head_set& heads, target& out) {
    out = target{};
    for (unsigned i = 0; i < heads.count; ++i)
        from_head(heads.heads[i], out.heads[out.count++]);
}

resolve_failure resolve(const configuration& c, const head_set& heads, unsigned max_enabled,
                        target& out) {
    out = target{};
    unsigned enabled = 0;
    for (unsigned i = 0; i < heads.count; ++i) {
        const head& h = heads.heads[i];
        head_target& t = out.heads[out.count++];
        from_head(h, t);
        const config_head* ch = c.find(h.key);
        if (!ch) // `submit` passed, so only a head that came after it
            continue;
        t.enabled = ch->enabled;
        if (!ch->enabled)
            continue;
        ++enabled;
        if (ch->mode_set) {
            mode m = ch->mode;
            bool listed = false;
            if (!ch->custom) {
                for (unsigned k = 0; k < h.mode_count && !listed; ++k)
                    if (h.modes[k].same(m)) {
                        m = h.modes[k];
                        listed = true;
                    }
                if (!listed)
                    return resolve_failure::mode_gone;
            } else {
                if (m.width < 1 || m.height < 1 || m.width > 16384 || m.height > 16384)
                    return resolve_failure::bad_custom_mode;
                // A custom mode the head lists after all (wlr-randr --custom-mode with a listed
                // size) is that mode; without a refresh, the size's highest listed refresh.
                int best = -1;
                for (unsigned k = 0; k < h.mode_count; ++k) {
                    const mode& lm = h.modes[k];
                    if (lm.width != m.width || lm.height != m.height)
                        continue;
                    if (m.refresh_mhz != 0 && lm.refresh_mhz != m.refresh_mhz)
                        continue;
                    if (best < 0 || lm.refresh_mhz > h.modes[best].refresh_mhz ||
                        (lm.refresh_mhz == h.modes[best].refresh_mhz && lm.preferred))
                        best = static_cast<int>(k);
                }
                if (best >= 0) {
                    m = h.modes[best];
                    listed = true;
                }
            }
            t.mode = m;
            t.custom = !listed;
            // Restating the current mode (wlr-randr and kanshi send every property) keeps an
            // output that follows its preferred mode following it.
            const mode cur = h.current_mode();
            t.mode_explicit = !(h.enabled && cur.same_size_refresh(m) && (h.current < 0) == t.custom);
        } else if (!h.enabled) {
            const int p = h.current >= 0 ? h.current : h.preferred_index();
            if (p >= 0) {
                t.mode = h.modes[p];
                t.custom = false;
            }
        }
        if (ch->position_set) {
            // Restating the current position keeps an automatically placed output automatic.
            const bool restated = h.enabled && h.auto_position && ch->x == h.x && ch->y == h.y;
            t.x = ch->x;
            t.y = ch->y;
            t.position_explicit = !restated;
        }
        if (ch->transform_set)
            t.transform = ch->transform;
        if (ch->scale_set) {
            if (!(ch->scale > 0.0) || ch->scale > 10.0)
                return resolve_failure::bad_scale;
            const double s = lx::compositor::representable_scale(ch->scale);
            if (!(s > 0.0) || s > 10.0)
                return resolve_failure::bad_scale;
            t.scale = s;
        }
        if (ch->adaptive_sync_set) {
            if (ch->adaptive_sync && !h.vrr_capable)
                return resolve_failure::adaptive_sync_unsupported;
            t.adaptive_sync = ch->adaptive_sync;
        }
    }
    if (enabled == 0)
        return resolve_failure::no_head_enabled;
    if (enabled > max_enabled)
        return resolve_failure::too_many_enabled;
    return resolve_failure::none;
}

bool job_queue::push(const job& j) {
    if (count_ >= k_max)
        return false;
    jobs_[(head_ + count_) % k_max] = j;
    ++count_;
    return true;
}

void job_queue::pop() {
    if (count_ == 0)
        return;
    jobs_[head_] = job{};
    head_ = (head_ + 1) % k_max;
    --count_;
}

void job_queue::detach(unsigned long long token) {
    if (!token)
        return;
    for (unsigned i = 0; i < count_; ++i) {
        job& j = jobs_[(head_ + i) % k_max];
        if (j.token == token)
            j.token = 0;
    }
}

} // namespace lx::compositor::mgmt
