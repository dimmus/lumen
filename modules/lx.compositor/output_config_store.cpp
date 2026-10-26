module;

// The persistent output configuration (`:output_config_store`): the model's TOML mapping and
// the file. See the partition for the format and the policy.

#include <errno.h>
#include <stdio.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

module lx.compositor;

import std;

import lx.foundation;
import lx.trace;
import lx.config;
import :output_config_store;

namespace lx::compositor {

namespace {

constexpr lx::trace::category log_outputcfg{"compositor.outputcfg"};

constexpr const char* k_transform_names[8] = {"normal",  "90",         "180",         "270",
                                              "flipped", "flipped-90", "flipped-180", "flipped-270"};

void copy_id(char (&dst)[monitor_config::k_id], const char* src) {
    std::snprintf(dst, sizeof(dst), "%s", src ? src : "");
}

/// What the reader is filling.
struct load_state {
    output_config* cfg = nullptr;
    enum class section : unsigned char { top, output, setup, setup_output, unknown } where = section::top;
    monitor_config* monitor = nullptr;
    setup_config* setup = nullptr;
    setup_output_config* setup_output = nullptr;
    /// The `outputs` list of the current setup (checked against its entries at the end).
    char listed[setup_config::k_max_outputs][monitor_config::k_id]{};
    unsigned listed_count = 0;
    bool listed_given = false;
    bool version_seen = false;
};

[[nodiscard]] bool parse_mode(const char* s, monitor_config& m) {
    if (!std::strcmp(s, "preferred")) {
        m.has_mode = true;
        m.mode_preferred = true;
        return true;
    }
    unsigned w = 0, h = 0, r = 0;
    char tail = '\0';
    const int n = std::sscanf(s, "%ux%u@%u%c", &w, &h, &r, &tail);
    if (n == 2 || n == 3) {
        if (w == 0 || h == 0 || w > 16384 || h > 16384)
            return false;
        m.has_mode = true;
        m.mode_preferred = false;
        m.width = w;
        m.height = h;
        m.refresh_mhz = n == 3 ? r : 0;
        return true;
    }
    return false;
}

/// Closes the setup being read: its `outputs` list (when given) must name exactly its
/// entries.
bool finish_setup(load_state& st, toml::parse_error& err) {
    if (!st.setup)
        return true;
    if (st.listed_given) {
        bool ok = st.listed_count == st.setup->count;
        for (unsigned i = 0; ok && i < st.listed_count; ++i) {
            bool found = false;
            for (unsigned k = 0; k < st.setup->count && !found; ++k)
                found = !std::strcmp(st.listed[i], st.setup->outputs[k].identifier);
            ok = found;
        }
        if (!ok) {
            toml::fail(err, "a setup's `outputs` must name exactly its [[setup.output]] entries");
            return false;
        }
    }
    st.setup = nullptr;
    st.setup_output = nullptr;
    st.listed_count = 0;
    st.listed_given = false;
    return true;
}

bool on_table(void* user, const char* name, bool array, toml::parse_error& err) {
    auto& st = *static_cast<load_state*>(user);
    output_config& cfg = *st.cfg;
    st.monitor = nullptr;
    st.setup_output = nullptr;
    if (array && !std::strcmp(name, "output")) {
        if (!finish_setup(st, err))
            return false;
        st.where = load_state::section::output;
        if (cfg.monitor_count >= output_config::k_max_monitors) {
            ++cfg.dropped;
            st.monitor = nullptr;
            return true;
        }
        st.monitor = &cfg.monitors[cfg.monitor_count++];
        *st.monitor = monitor_config{};
        return true;
    }
    if (array && !std::strcmp(name, "setup")) {
        if (!finish_setup(st, err))
            return false;
        st.where = load_state::section::setup;
        if (cfg.setup_count >= output_config::k_max_setups) {
            ++cfg.dropped;
            st.setup = nullptr;
            return true;
        }
        st.setup = &cfg.setups[cfg.setup_count++];
        *st.setup = setup_config{};
        return true;
    }
    if (array && !std::strcmp(name, "setup.output")) {
        if (st.where != load_state::section::setup && st.where != load_state::section::setup_output) {
            toml::fail(err, "[[setup.output]] outside a [[setup]]");
            return false;
        }
        st.where = load_state::section::setup_output;
        if (!st.setup)
            return true; // a dropped setup
        if (st.setup->count >= setup_config::k_max_outputs) {
            toml::fail(err, "a setup has more than 16 outputs");
            return false;
        }
        st.setup_output = &st.setup->outputs[st.setup->count++];
        *st.setup_output = setup_output_config{};
        return true;
    }
    // Unknown tables are skipped (a newer Lumen's file read by an older one).
    if (!finish_setup(st, err))
        return false;
    st.where = load_state::section::unknown;
    return true;
}

bool want_string(const toml::value& v, toml::parse_error& err, const char* key) {
    if (v.type == toml::value::kind::string)
        return true;
    char msg[96];
    std::snprintf(msg, sizeof(msg), "`%s` must be a string", key);
    toml::fail(err, msg);
    return false;
}

bool on_key(void* user, const char* key, const toml::value& v, toml::parse_error& err) {
    auto& st = *static_cast<load_state*>(user);
    switch (st.where) {
    case load_state::section::top:
        if (!std::strcmp(key, "version")) {
            if (v.type != toml::value::kind::integer) {
                toml::fail(err, "`version` must be an integer");
                return false;
            }
            if (v.integer > output_config::k_version) {
                toml::fail(err, "written by a newer Lumen (version > 1)");
                return false;
            }
            st.version_seen = true;
        }
        return true; // unknown top-level keys are ignored
    case load_state::section::unknown:
        return true;
    case load_state::section::output: {
        monitor_config* m = st.monitor;
        if (!std::strcmp(key, "identifier")) {
            if (!want_string(v, err, key))
                return false;
            if (m)
                copy_id(m->identifier, v.str);
        } else if (!std::strcmp(key, "mode")) {
            if (!want_string(v, err, key))
                return false;
            monitor_config tmp{};
            if (!parse_mode(v.str, tmp)) {
                toml::fail(err, "`mode` must be \"preferred\" or \"WxH\" / \"WxH@mHz\"");
                return false;
            }
            if (m) {
                m->has_mode = tmp.has_mode;
                m->mode_preferred = tmp.mode_preferred;
                m->width = tmp.width;
                m->height = tmp.height;
                m->refresh_mhz = tmp.refresh_mhz;
            }
        } else if (!std::strcmp(key, "scale")) {
            if (!v.is_number() || !(v.number() > 0.0) || v.number() > 10.0) {
                toml::fail(err, "`scale` must be a number in (0, 10]");
                return false;
            }
            if (m) {
                m->has_scale = true;
                m->scale = v.number();
            }
        } else if (!std::strcmp(key, "transform")) {
            if (!want_string(v, err, key))
                return false;
            const int t = transform_from_name(v.str);
            if (t < 0) {
                toml::fail(err, "`transform` must be normal, 90, 180, 270 or flipped[-90|-180|-270]");
                return false;
            }
            if (m) {
                m->has_transform = true;
                m->transform = static_cast<unsigned>(t);
            }
        } else if (!std::strcmp(key, "adaptive_sync")) {
            if (v.type != toml::value::kind::boolean) {
                toml::fail(err, "`adaptive_sync` must be true or false");
                return false;
            }
            if (m) {
                m->has_adaptive_sync = true;
                m->adaptive_sync = v.boolean;
            }
        }
        return true;
    }
    case load_state::section::setup:
        if (!std::strcmp(key, "outputs")) {
            if (v.type != toml::value::kind::array) {
                toml::fail(err, "`outputs` must be an array of identifiers");
                return false;
            }
            if (v.count > setup_config::k_max_outputs) {
                toml::fail(err, "a setup has more than 16 outputs");
                return false;
            }
            st.listed_given = true;
            st.listed_count = 0;
            for (unsigned i = 0; i < v.count; ++i) {
                if (v.items[i].type != toml::value::kind::string) {
                    toml::fail(err, "`outputs` must be an array of identifiers");
                    return false;
                }
                copy_id(st.listed[st.listed_count++], v.items[i].str);
            }
        }
        return true;
    case load_state::section::setup_output: {
        setup_output_config* o = st.setup_output;
        if (!std::strcmp(key, "identifier")) {
            if (!want_string(v, err, key))
                return false;
            if (o)
                copy_id(o->identifier, v.str);
        } else if (!std::strcmp(key, "enabled")) {
            if (v.type != toml::value::kind::boolean) {
                toml::fail(err, "`enabled` must be true or false");
                return false;
            }
            if (o)
                o->enabled = v.boolean;
        } else if (!std::strcmp(key, "position")) {
            if (v.type != toml::value::kind::array || v.count != 2 ||
                v.items[0].type != toml::value::kind::integer ||
                v.items[1].type != toml::value::kind::integer ||
                v.items[0].integer < -1000000 || v.items[0].integer > 1000000 ||
                v.items[1].integer < -1000000 || v.items[1].integer > 1000000) {
                toml::fail(err, "`position` must be [x, y] (integers)");
                return false;
            }
            if (o) {
                o->has_position = true;
                o->x = static_cast<int>(v.items[0].integer);
                o->y = static_cast<int>(v.items[1].integer);
            }
        }
        return true;
    }
    }
    return true;
}

/// The setup's identifiers, sorted (the key a setup is found by).
unsigned sorted_ids(const setup_config& s, const char* (&out)[setup_config::k_max_outputs]) {
    unsigned n = 0;
    for (unsigned i = 0; i < s.count; ++i)
        out[n++] = s.outputs[i].identifier;
    for (unsigned i = 1; i < n; ++i)
        for (unsigned j = i; j > 0 && std::strcmp(out[j], out[j - 1]) < 0; --j) {
            const char* t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}

[[nodiscard]] bool same_monitor_config(const monitor_config& a, const monitor_config& b) {
    return !std::strcmp(a.identifier, b.identifier) && a.has_mode == b.has_mode &&
           a.mode_preferred == b.mode_preferred && a.width == b.width && a.height == b.height &&
           a.refresh_mhz == b.refresh_mhz && a.has_scale == b.has_scale &&
           (!a.has_scale || a.scale == b.scale) && a.has_transform == b.has_transform &&
           a.transform == b.transform && a.has_adaptive_sync == b.has_adaptive_sync &&
           a.adaptive_sync == b.adaptive_sync;
}

[[nodiscard]] bool same_setup_output(const setup_output_config& a, const setup_output_config& b) {
    return !std::strcmp(a.identifier, b.identifier) && a.enabled == b.enabled &&
           a.has_position == b.has_position && (!a.has_position || (a.x == b.x && a.y == b.y));
}

} // namespace

int transform_from_name(const char* name) {
    if (!name)
        return -1;
    for (unsigned i = 0; i < 8; ++i)
        if (!std::strcmp(name, k_transform_names[i]))
            return static_cast<int>(i);
    return -1;
}

const char* transform_name(unsigned transform) {
    return transform < 8 ? k_transform_names[transform] : "normal";
}

bool parse_output_config(const char* text, std::size_t length, output_config& out,
                         toml::parse_error& err) {
    out.monitor_count = 0;
    out.setup_count = 0;
    out.dropped = 0;
    load_state st{};
    st.cfg = &out;
    toml::handler h{};
    h.user = &st;
    h.table = &on_table;
    h.key_value = &on_key;
    if (!toml::parse(text, length, h, err))
        return false;
    if (!finish_setup(st, err)) {
        err.line = 0;
        return false;
    }
    // Entries without an identifier say nothing about any monitor: dropped.
    unsigned w = 0;
    for (unsigned i = 0; i < out.monitor_count; ++i)
        if (out.monitors[i].identifier[0])
            out.monitors[w++] = out.monitors[i];
    out.monitor_count = w;
    w = 0;
    for (unsigned i = 0; i < out.setup_count; ++i) {
        setup_config& s = out.setups[i];
        bool ok = s.count > 0;
        for (unsigned k = 0; ok && k < s.count; ++k)
            ok = s.outputs[k].identifier[0] != '\0';
        if (ok)
            out.setups[w++] = s;
    }
    out.setup_count = w;
    return true;
}

std::string serialize_output_config(const output_config& cfg) {
    toml::writer w{};
    w.comment("Written by lumen-compositor after each output configuration change (wlr-randr,");
    w.comment("kanshi, a settings app). Safe to edit while the compositor is not running.");
    w.comment("[[output]]: one per monitor (mode, scale, transform, adaptive sync).");
    w.comment("[[setup]]: one per set of monitors connected together (enabled, position).");
    w.key("version", static_cast<long long>(output_config::k_version));
    for (unsigned i = 0; i < cfg.monitor_count; ++i) {
        const monitor_config& m = cfg.monitors[i];
        w.blank();
        w.table_array("output");
        w.key("identifier", m.identifier);
        if (m.has_mode) {
            if (m.mode_preferred) {
                w.key("mode", "preferred");
            } else {
                char mode[48];
                if (m.refresh_mhz)
                    std::snprintf(mode, sizeof(mode), "%ux%u@%u", m.width, m.height, m.refresh_mhz);
                else
                    std::snprintf(mode, sizeof(mode), "%ux%u", m.width, m.height);
                w.key("mode", mode);
            }
        }
        if (m.has_scale)
            w.key("scale", m.scale);
        if (m.has_transform)
            w.key("transform", transform_name(m.transform));
        if (m.has_adaptive_sync)
            w.key_bool("adaptive_sync", m.adaptive_sync);
    }
    for (unsigned i = 0; i < cfg.setup_count; ++i) {
        const setup_config& s = cfg.setups[i];
        w.blank();
        w.table_array("setup");
        const char* ids[setup_config::k_max_outputs];
        const unsigned n = sorted_ids(s, ids);
        w.key_strings("outputs", ids, n);
        w.indent(true);
        for (unsigned k = 0; k < s.count; ++k) {
            const setup_output_config& o = s.outputs[k];
            w.table_array("setup.output");
            w.key("identifier", o.identifier);
            w.key_bool("enabled", o.enabled);
            if (o.has_position) {
                const long long pos[2] = {o.x, o.y};
                w.key_ints("position", pos, 2);
            }
        }
        w.indent(false);
    }
    return w.text();
}

const monitor_config* find_monitor(const output_config& cfg, const char* identifier) {
    if (!identifier || !*identifier)
        return nullptr;
    for (unsigned i = cfg.monitor_count; i-- > 0;)
        if (!std::strcmp(cfg.monitors[i].identifier, identifier))
            return &cfg.monitors[i];
    return nullptr;
}

const setup_config* find_setup(const output_config& cfg, const char* const* ids, unsigned count) {
    if (count == 0 || count > setup_config::k_max_outputs)
        return nullptr;
    const char* want[setup_config::k_max_outputs];
    for (unsigned i = 0; i < count; ++i)
        want[i] = ids[i] ? ids[i] : "";
    for (unsigned i = 1; i < count; ++i)
        for (unsigned j = i; j > 0 && std::strcmp(want[j], want[j - 1]) < 0; --j) {
            const char* t = want[j];
            want[j] = want[j - 1];
            want[j - 1] = t;
        }
    for (unsigned i = cfg.setup_count; i-- > 0;) {
        const setup_config& s = cfg.setups[i];
        if (s.count != count)
            continue;
        const char* have[setup_config::k_max_outputs];
        (void)sorted_ids(s, have);
        bool same = true;
        for (unsigned k = 0; same && k < count; ++k)
            same = !std::strcmp(have[k], want[k]);
        if (same)
            return &s;
    }
    return nullptr;
}

bool record_monitor(output_config& cfg, const monitor_config& m) {
    if (!m.identifier[0])
        return false;
    for (unsigned i = 0; i < cfg.monitor_count; ++i) {
        if (std::strcmp(cfg.monitors[i].identifier, m.identifier) != 0)
            continue;
        const bool changed = !same_monitor_config(cfg.monitors[i], m);
        // Most recently used last.
        for (unsigned k = i; k + 1 < cfg.monitor_count; ++k)
            cfg.monitors[k] = cfg.monitors[k + 1];
        cfg.monitors[cfg.monitor_count - 1] = m;
        return changed || i + 1 != cfg.monitor_count;
    }
    if (cfg.monitor_count >= output_config::k_max_monitors) {
        for (unsigned k = 0; k + 1 < cfg.monitor_count; ++k)
            cfg.monitors[k] = cfg.monitors[k + 1];
        --cfg.monitor_count;
    }
    cfg.monitors[cfg.monitor_count++] = m;
    return true;
}

bool record_setup(output_config& cfg, const setup_config& s) {
    if (s.count == 0)
        return false;
    const char* ids[setup_config::k_max_outputs];
    for (unsigned i = 0; i < s.count; ++i)
        ids[i] = s.outputs[i].identifier;
    if (const setup_config* found = find_setup(cfg, ids, s.count)) {
        const auto i = static_cast<unsigned>(found - cfg.setups);
        bool changed = false;
        for (unsigned k = 0; k < s.count && !changed; ++k) {
            const setup_output_config* old = nullptr;
            for (unsigned j = 0; j < found->count && !old; ++j)
                if (!std::strcmp(found->outputs[j].identifier, s.outputs[k].identifier))
                    old = &found->outputs[j];
            changed = !old || !same_setup_output(*old, s.outputs[k]);
        }
        for (unsigned k = i; k + 1 < cfg.setup_count; ++k)
            cfg.setups[k] = cfg.setups[k + 1];
        cfg.setups[cfg.setup_count - 1] = s;
        return changed || i + 1 != cfg.setup_count;
    }
    if (cfg.setup_count >= output_config::k_max_setups) {
        for (unsigned k = 0; k + 1 < cfg.setup_count; ++k)
            cfg.setups[k] = cfg.setups[k + 1];
        --cfg.setup_count;
    }
    cfg.setups[cfg.setup_count++] = s;
    return true;
}

bool output_config_path(char* out, std::size_t size) {
    if (!out || size == 0)
        return false;
    out[0] = '\0';
    if (const char* env = std::getenv("LUMEN_OUTPUT_CONFIG"); env && *env) {
        if (!std::strcmp(env, "0"))
            return false;
        std::snprintf(out, size, "%s", env);
        return true;
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] == '/') {
        std::snprintf(out, size, "%s/lumen/outputs.toml", xdg);
        return true;
    }
    if (const char* home = std::getenv("HOME"); home && home[0] == '/') {
        std::snprintf(out, size, "%s/.config/lumen/outputs.toml", home);
        return true;
    }
    return false;
}

output_config_file::output_config_file() : config_(new output_config{}) {}

output_config_file::~output_config_file() {
    flush();
    delete config_;
}

bool output_config_file::load() {
    if (!output_config_path(path_, sizeof(path_))) {
        path_[0] = '\0';
        log_outputcfg.info("output configuration is not persisted "
                           "(LUMEN_OUTPUT_CONFIG=0, or no home directory)");
        return false;
    }
    const int fd = ::open(path_, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            log_outputcfg.warn("output configuration {} unreadable: {}", path_,
                               std::strerror(errno));
        }
        return true; // nothing remembered yet
    }
    std::string text;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() > (1u << 20))
            break; // far beyond any real file: a mistake, not a configuration
    }
    ::close(fd);
    toml::parse_error err{};
    if (!parse_output_config(text.data(), text.size(), *config_, err)) {
        *config_ = output_config{};
        bad_ = true;
        log_outputcfg.warn("output configuration {}:{}: {} — ignored (it is renamed "
                           "to .bad at the next change)",
                           path_, err.line, err.message);
        return true;
    }
    if (config_->dropped > 0)
        log_outputcfg.warn("output configuration: entries past 64 monitors or "
                           "setups were dropped");
    log_outputcfg.info("output configuration {}: {} monitors, {} setups", path_,
                       config_->monitor_count, config_->setup_count);
    return true;
}

void output_config_file::schedule_save(unsigned long long now_ns) {
    if (!enabled())
        return;
    save_due_ns_ = now_ns + k_debounce_ns;
}

bool output_config_file::run_due_save(unsigned long long now_ns,
                                      bool (*post_worker)(void (*fn)(void*), void* arg)) {
    if (!enabled() || save_due_ns_ == 0 || now_ns < save_due_ns_)
        return false;
    if (writing_.load(std::memory_order_acquire))
        return false; // the previous write is still going: the next tick tries again
    save_due_ns_ = 0;
    pending_ = serialize_output_config(*config_);
    writing_.store(true, std::memory_order_release);
    if (post_worker && post_worker(&output_config_file::write_task, this))
        return true;
    write_now();
    return true;
}

void output_config_file::flush() {
    if (!enabled())
        return;
    // A write on the worker finishes first (it owns `pending_` meanwhile).
    while (writing_.load(std::memory_order_acquire))
        ::usleep(1000);
    if (save_due_ns_ == 0)
        return;
    save_due_ns_ = 0;
    pending_ = serialize_output_config(*config_);
    writing_.store(true, std::memory_order_release);
    write_now();
}

void output_config_file::write_task(void* self) { static_cast<output_config_file*>(self)->write_now(); }

void output_config_file::write_now() {
    const auto report = [this](const char* what) {
        log_outputcfg.error("output configuration {}: {} failed: {}", path_, what,
                            std::strerror(errno));
    };
    // The directory, made on demand (only the last level: $XDG_CONFIG_HOME exists).
    char dir[sizeof(path_)];
    std::snprintf(dir, sizeof(dir), "%s", path_);
    if (char* slash = std::strrchr(dir, '/'); slash && slash != dir) {
        *slash = '\0';
        if (::mkdir(dir, 0700) != 0 && errno != EEXIST)
            report("mkdir");
    }
    if (bad_) {
        char bad[sizeof(path_) + 8];
        std::snprintf(bad, sizeof(bad), "%s.bad", path_);
        if (::rename(path_, bad) != 0 && errno != ENOENT)
            report("rename to .bad");
        bad_ = false;
    }
    char tmp[sizeof(path_) + 8];
    std::snprintf(tmp, sizeof(tmp), "%s.tmp", path_);
    const int fd = ::open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    bool ok = fd >= 0;
    if (!ok)
        report("open");
    std::size_t off = 0;
    while (ok && off < pending_.size()) {
        const ssize_t n = ::write(fd, pending_.data() + off, pending_.size() - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            report("write");
            ok = false;
            break;
        }
        off += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) {
        report("fsync");
        ok = false;
    }
    if (fd >= 0)
        ::close(fd);
    if (ok && ::rename(tmp, path_) != 0) {
        report("rename");
        ok = false;
    }
    if (!ok)
        (void)::unlink(tmp);
    else
        saves_.fetch_add(1, std::memory_order_acq_rel);
    writing_.store(false, std::memory_order_release);
}

} // namespace lx::compositor
