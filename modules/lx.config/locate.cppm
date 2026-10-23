module;

// Where the configuration lives (design-session-and-wm C.1): `$XDG_CONFIG_DIRS/lumen/
// config.toml` for each directory (lowest priority first; `/etc/xdg` when unset), then
// `$XDG_CONFIG_HOME/lumen/config.toml` (`~/.config`), each layer over the previous key by
// key. `LUMEN_CONFIG=PATH` uses only that file (tests, smokes); `LUMEN_CONFIG=0` none (the
// defaults). Reading is bounded: a layer over 256 KiB, or one that cannot be read, rejects
// the configuration like a syntax error does.

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

export module lx.config:locate;

import std;
import :diagnostics;
import :parse;

export namespace lx::config {

struct config_path {
    char path[512]{};
};

struct config_location {
    /// LUMEN_CONFIG=0: no file is read or watched.
    bool disabled = false;
    /// LUMEN_CONFIG=PATH.
    bool explicit_file = false;
    /// Lowest priority first; the user's file last.
    std::inplace_vector<config_path, k_max_layers> files{};
    /// The user's file (where an editor writes; what `lumen-ctl` names). Empty when disabled.
    config_path user_file{};
};

/// `getenv` stand-in for tests; null: the process environment.
using env_lookup = const char* (*)(const char* name);

[[nodiscard]] config_location locate_config(env_lookup env = nullptr);

/// Reads the layers that exist into `out` (missing files are skipped: no layer). False when
/// one cannot be read or is too large — an error in `diag`, and the configuration must not
/// be used.
[[nodiscard]] bool read_config(const config_location& where, config_text& out, diagnostics& diag);

/// Reads one file (≤ k_max_layer_bytes). False with `why` set (a static string); a missing
/// file is false with `missing` true.
[[nodiscard]] bool read_file(const char* path, std::string& out, const char*& why, bool& missing);

} // namespace lx::config

namespace {

const char* lookup(lx::config::env_lookup env, const char* name) {
    const char* v = env ? env(name) : std::getenv(name);
    return v && *v ? v : nullptr;
}

} // namespace

lx::config::config_location lx::config::locate_config(env_lookup env) {
    config_location loc{};
    if (const char* forced = lookup(env, "LUMEN_CONFIG")) {
        if (std::strcmp(forced, "0") == 0) {
            loc.disabled = true;
            return loc;
        }
        loc.explicit_file = true;
        config_path p{};
        std::snprintf(p.path, sizeof(p.path), "%s", forced);
        loc.files.push_back(p);
        loc.user_file = p;
        return loc;
    }
    // XDG_CONFIG_DIRS is most important first; layers go least important first. Relative
    // entries are ignored (the Base Directory spec).
    std::inplace_vector<config_path, k_max_layers - 1> system{};
    const char* dirs = lookup(env, "XDG_CONFIG_DIRS");
    std::string_view list{dirs ? dirs : "/etc/xdg"};
    while (!list.empty()) {
        const std::size_t colon = list.find(':');
        const std::string_view dir = list.substr(0, colon);
        list = colon == std::string_view::npos ? std::string_view{} : list.substr(colon + 1);
        if (dir.empty() || dir.front() != '/' || system.size() == system.capacity())
            continue;
        config_path p{};
        std::snprintf(p.path, sizeof(p.path), "%.*s/lumen/config.toml",
                      static_cast<int>(dir.size()), dir.data());
        system.push_back(p);
    }
    for (auto it = system.rbegin(); it != system.rend(); ++it)
        loc.files.push_back(*it);
    config_path user{};
    if (const char* home_cfg = lookup(env, "XDG_CONFIG_HOME"); home_cfg && home_cfg[0] == '/')
        std::snprintf(user.path, sizeof(user.path), "%s/lumen/config.toml", home_cfg);
    else if (const char* home = lookup(env, "HOME"))
        std::snprintf(user.path, sizeof(user.path), "%s/.config/lumen/config.toml", home);
    if (user.path[0]) {
        loc.files.push_back(user);
        loc.user_file = user;
    }
    return loc;
}

bool lx::config::read_file(const char* path, std::string& out, const char*& why, bool& missing) {
    out.clear();
    missing = false;
    why = nullptr;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        missing = errno == ENOENT || errno == ENOTDIR;
        why = missing ? "no such file" : "cannot be opened (permissions?)";
        return false;
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        why = "is not a regular file";
        return false;
    }
    if (static_cast<std::size_t>(st.st_size) > k_max_layer_bytes) {
        ::close(fd);
        why = "is larger than 256 KiB";
        return false;
    }
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t got = 0;
    while (got < out.size()) {
        const ssize_t n = ::read(fd, out.data() + got, out.size() - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += static_cast<std::size_t>(n);
    }
    // A file that grew meanwhile is read up to its old size; one that shrank, to its end.
    out.resize(got);
    ::close(fd);
    return true;
}

bool lx::config::read_config(const config_location& where, config_text& out, diagnostics& diag) {
    out = config_text{};
    if (where.disabled)
        return true;
    std::string content;
    for (const config_path& p : where.files) {
        const char* why = nullptr;
        bool missing = false;
        if (!read_file(p.path, content, why, missing)) {
            if (missing)
                continue;
            // A layer exists but cannot be used: so is the configuration (the previous one
            // stays), rather than silently running without the user's settings.
            (void)out.add_layer(p.path, "");
            diag.addf(severity::error, static_cast<unsigned>(out.layers.size() - 1), 0, "",
                      "{}; the configuration is not used", why);
            return false;
        }
        if (!out.add_layer(p.path, content)) {
            diag.add(severity::error, k_no_layer, 0, "", "too many configuration layers");
            return false;
        }
    }
    return true;
}
