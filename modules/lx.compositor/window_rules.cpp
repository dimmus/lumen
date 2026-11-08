module;
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

module lx.compositor;
import std;
import lx.foundation;
import lx.config;
import :window_rules;

namespace lx::compositor {
namespace {
bool valid_text(std::string_view s, std::size_t cap) {
    return !s.empty() && s.size() <= cap &&
           std::ranges::none_of(s, [](unsigned char c) { return c < 32 || c == 127; });
}
lx::error io_failure() {
    return lx::make_error(lx::io_err::not_found, "window rules file operation failed");
}
}

lx::result<void> window_rule_store::validate(const window_rule& r) {
    if (!valid_text(r.app_id, 127))
        return lx::invalid_argument("window rules: invalid app_id");
    if (r.workspace_index && (*r.workspace_index == 0 || *r.workspace_index > 64))
        return lx::invalid_argument("window rules: workspace_index must be 1..64");
    if (r.output_name && !valid_text(*r.output_name, 31))
        return lx::invalid_argument("window rules: invalid output_name");
    if (r.geometry) {
        const auto& g = *r.geometry;
        if (g.width <= 0 || g.height <= 0 ||
            static_cast<long long>(g.x) + g.width > std::numeric_limits<int>::max() ||
            static_cast<long long>(g.y) + g.height > std::numeric_limits<int>::max())
            return lx::invalid_argument("window rules: invalid geometry");
    }
    if (r.state_flags && ((*r.state_flags & ~15u) ||
        ((*r.state_flags & 3u) == 3u) || ((*r.state_flags & 12u) == 12u)))
        return lx::invalid_argument("window rules: conflicting or unknown state_flags");
    return {};
}
const window_rule* window_rule_store::find(std::string_view id) const {
    const auto it = std::ranges::find(rules_, id, &window_rule::app_id);
    return it == rules_.end() ? nullptr : &*it;
}
lx::result<void> window_rule_store::set(window_rule r) {
    if (auto v = validate(r); !v) return v.get_error();
    auto next = *this;
    auto it = std::ranges::find(next.rules_, r.app_id, &window_rule::app_id);
    if (it != next.rules_.end()) *it = std::move(r);
    else {
        if (next.rules_.size() == k_capacity)
            return lx::invalid_argument("window rules: capacity exceeded");
        next.rules_.push_back(std::move(r));
    }
    if (auto v = next.save(); !v) return v.get_error();
    rules_ = std::move(next.rules_);
    return {};
}
lx::result<void> window_rule_store::clear(std::string_view id) {
    if (!valid_text(id, 127)) return lx::invalid_argument("window rules: invalid app_id");
    auto next = *this;
    std::erase_if(next.rules_, [id](const auto& r) { return r.app_id == id; });
    if (auto v = next.save(); !v) return v.get_error();
    rules_ = std::move(next.rules_);
    return {};
}
lx::result<void> window_rule_store::clear_all() {
    auto next = *this;
    next.rules_.clear();
    if (auto v = next.save(); !v) return v.get_error();
    rules_.clear();
    return {};
}

std::string window_rule_store::serialize() const {
    lx::config::toml::writer w;
    for (const auto& r : rules_) {
        w.table_array("rule");
        w.key("app_id", r.app_id.c_str());
        if (r.workspace_index) w.key("workspace_index", static_cast<long long>(*r.workspace_index));
        if (r.output_name) w.key("output_name", r.output_name->c_str());
        if (r.geometry) {
            const long long values[]{r.geometry->x, r.geometry->y, r.geometry->width, r.geometry->height};
            w.key_ints("geometry", values, 4);
        }
        if (r.state_flags) w.key("state_flags", static_cast<long long>(*r.state_flags));
        if (r.server_side) w.key_bool("server_side", *r.server_side);
        w.blank();
    }
    return w.text();
}
lx::result<void> window_rule_store::parse(std::string_view text) {
    if (text.size() > k_max_file_size) return lx::invalid_argument("window rules: file too large");
    struct parser {
        std::vector<window_rule> rules;
        unsigned seen = 0;
    } p;
    lx::config::toml::handler h{};
    h.user = &p;
    h.table = [](void* u, const char* name, bool array, lx::config::toml::parse_error&) {
        auto& p = *static_cast<parser*>(u);
        if (!array || std::string_view{name} != "rule" || p.rules.size() == k_capacity) return false;
        p.rules.emplace_back(); p.seen = 0; return true;
    };
    h.key_value = [](void* u, const char* key, const lx::config::toml::value& v,
                     lx::config::toml::parse_error&) {
        auto& p = *static_cast<parser*>(u);
        if (p.rules.empty()) return false;
        auto& r = p.rules.back();
        using kind = lx::config::toml::value::kind;
        const std::string_view k{key};
        unsigned bit = k == "app_id" ? 1 : k == "workspace_index" ? 2 : k == "output_name" ? 4 :
                       k == "geometry" ? 8 : k == "state_flags" ? 16 : k == "server_side" ? 32 : 0;
        if (!bit || (p.seen & bit)) return false;
        p.seen |= bit;
        if (bit == 1 && v.type == kind::string) r.app_id = v.str;
        else if (bit == 4 && v.type == kind::string) r.output_name = v.str;
        else if ((bit == 2 || bit == 16) && v.type == kind::integer && v.integer >= 0 &&
                 v.integer <= std::numeric_limits<unsigned>::max()) {
            if (bit == 2) r.workspace_index = static_cast<unsigned>(v.integer);
            else r.state_flags = static_cast<unsigned>(v.integer);
        } else if (bit == 32 && v.type == kind::boolean) r.server_side = v.boolean;
        else if (bit == 8 && v.type == kind::array && v.count == 4) {
            int a[4];
            for (unsigned i = 0; i < 4; ++i) {
                if (v.items[i].type != kind::integer || v.items[i].integer < std::numeric_limits<int>::min() ||
                    v.items[i].integer > std::numeric_limits<int>::max()) return false;
                a[i] = static_cast<int>(v.items[i].integer);
            }
            r.geometry = lx::rect2i{a[0], a[1], a[2], a[3]};
        } else return false;
        return true;
    };
    lx::config::toml::parse_error err{};
    if (!lx::config::toml::parse(text.data(), text.size(), h, err))
        return lx::invalid_argument("window rules: malformed TOML");
    for (unsigned i = 0; i < p.rules.size(); ++i) {
        if (auto v = validate(p.rules[i]); !v) return v.get_error();
        for (unsigned j = 0; j < i; ++j)
            if (p.rules[i].app_id == p.rules[j].app_id)
                return lx::invalid_argument("window rules: duplicate app_id");
    }
    rules_ = std::move(p.rules);
    return {};
}
lx::result<void> window_rule_store::load_default() {
    const char* base = std::getenv("XDG_CONFIG_HOME");
    if (base && base[0] == '/') return load(std::string{base} + "/lumen/window-rules.toml");
    base = std::getenv("HOME");
    if (base && base[0] == '/') return load(std::string{base} + "/.config/lumen/window-rules.toml");
    return lx::invalid_argument("window rules: no configuration directory");
}
lx::result<void> window_rule_store::load(std::string path) {
    path_ = std::move(path);
    return reload();
}
lx::result<void> window_rule_store::reload() {
    if (path_.empty()) return lx::invalid_argument("window rules: no file path");
    const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) { rules_.clear(); return {}; }
        return io_failure();
    }
    struct stat st{};
    // `st_size` is signed (`off_t`) and a negative size means "unknown" on some filesystems;
    // the cast happens after that is ruled out, so the comparison cannot invert.
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<std::uintmax_t>(st.st_size) > k_max_file_size) {
        ::close(fd); return lx::invalid_argument("window rules: invalid file");
    }
    std::string text;
    char buf[4096];
    for (;;) {
        const auto n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ::close(fd); return io_failure(); }
        if (n == 0) break;
        text.append(buf, static_cast<std::size_t>(n));
        if (text.size() > k_max_file_size) { ::close(fd); return lx::invalid_argument("window rules: file too large"); }
    }
    ::close(fd);
    return parse(text);
}
lx::result<void> window_rule_store::save() const {
    if (path_.empty()) return {};
    const auto slash = path_.find_last_of('/');
    if (slash == std::string::npos) return lx::invalid_argument("window rules: invalid file path");
    const auto dir = path_.substr(0, slash);
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return io_failure();
    auto tmp = path_ + ".XXXXXX";
    const int fd = ::mkstemp(tmp.data());
    if (fd < 0) return io_failure();
    (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const auto text = serialize();
    bool ok = true;
    std::size_t off = 0;
    while (off < text.size()) {
        const auto n = ::write(fd, text.data() + off, text.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        off += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::rename(tmp.c_str(), path_.c_str()) != 0) ok = false;
    if (!ok) { (void)::unlink(tmp.c_str()); return io_failure(); }
    return {};
}
} // namespace lx::compositor
