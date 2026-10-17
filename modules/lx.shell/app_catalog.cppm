module;

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

import lx.foundation;
import lx.trace;

export module lx.shell:app_catalog;

import :desktop_entry;

export namespace lx::shell {

// ── Application catalog ───────────────────────────────────────────────────────
//
// The installed applications, from the XDG data directories (Desktop Entry Specification
// 1.5: $XDG_DATA_HOME/applications, then each $XDG_DATA_DIRS entry's, the first directory
// that has a desktop file ID winning even when that file says Hidden). One generation is
// one arena plus one entry vector, built whole by `scan()` and swapped in, so nothing is
// ever half-updated. inotify only marks the catalog stale; the shell rescans when the
// launcher next opens (or, while it is open, once a burst of changes settles).

struct catalog_config {
    /// $XDG_DATA_HOME, else $HOME/.local/share.
    std::string data_home;
    /// $XDG_DATA_DIRS, else /usr/local/share:/usr/share. Relative entries are dropped.
    std::vector<std::string> data_dirs;
    std::string path_env;
    /// $TERMINAL (empty: unset).
    std::string terminal_env;
    locale_keys locale{};
    desktop_list current{};

    [[nodiscard]] static catalog_config from_env();
};

/// The terminal for Super+Return and Terminal=true: $TERMINAL, xdg-terminal-exec, or the
/// preferred TerminalEmulator entry (NoDisplay ones included).
struct terminal_choice {
    bool found = false;
    /// The catalog entry index, or -1 for a raw command (`argv0`).
    int entry = -1;
    /// Raw command as written, and its absolute path.
    char argv0[256]{};
    char program[PATH_MAX]{};
    /// Inserted between the terminal and a Terminal=true program: "-e",
    /// X-TerminalArgExec, or empty (xdg-terminal-exec takes the command as is).
    char exec_flag[32]{};
    /// Where it came from, for logs: "$TERMINAL", "xdg-terminal-exec" or the desktop ID.
    char source[128]{};
};

struct scan_stats {
    unsigned entries = 0;
    unsigned listed = 0;
    unsigned files = 0;
    unsigned dirs = 0;
    double ms = 0.0;
};

/// Search tiers, best first: Name prefix, a word of Name, Name substring, GenericName or
/// Keywords, the Exec program's name.
inline constexpr unsigned k_rank_tiers = 5;

class app_catalog {
public:
    static constexpr std::size_t k_max_file = 64 * 1024;
    static constexpr unsigned k_max_depth = 8;

    app_catalog();
    ~app_catalog();
    app_catalog(const app_catalog&) = delete;
    app_catalog& operator=(const app_catalog&) = delete;

    void configure(catalog_config cfg);
    /// Builds a new generation from the directories and swaps it in; watches every
    /// directory it walked. Logs `apps: catalog …` at info.
    void scan();
    [[nodiscard]] bool scanned() const { return generation_ > 0; }
    [[nodiscard]] bool stale() const { return stale_; }
    void mark_stale() { stale_ = true; }
    /// inotify fd (-1 before the first scan or without inotify).
    [[nodiscard]] int watch_fd() const { return inotify_; }
    /// Drains inotify. True when the catalog became stale.
    bool on_watch_readable();

    [[nodiscard]] const std::vector<desktop_entry>& entries() const { return entries_; }
    [[nodiscard]] const desktop_entry* entry(unsigned i) const {
        return i < entries_.size() ? &entries_[i] : nullptr;
    }
    /// Listed entries' indices, by case-folded Name.
    [[nodiscard]] const std::vector<unsigned>& listed() const { return listed_; }
    [[nodiscard]] const desktop_entry* find(std::string_view id) const;
    /// Listed entries matching `query` (ASCII case-insensitive), best tier first and
    /// alphabetical within a tier; all of them alphabetically for an empty query. Returns
    /// how many were written (≤ cap).
    [[nodiscard]] unsigned rank(std::string_view query, unsigned* out, unsigned cap) const;
    [[nodiscard]] const terminal_choice& terminal() const { return terminal_; }
    /// 0 before the first scan; +1 per scan.
    [[nodiscard]] unsigned generation() const { return generation_; }
    [[nodiscard]] const scan_stats& stats() const { return stats_; }
    [[nodiscard]] const catalog_config& config() const { return cfg_; }

private:
    struct walk_state;
    void walk(walk_state& w, int dirfd, char* path, std::size_t path_len, char* prefix,
              std::size_t prefix_len, unsigned depth);
    void read_entry(walk_state& w, int dirfd, const char* name, std::string_view id,
                    std::string_view file);
    void choose_terminal();
    void close_watches();

    catalog_config cfg_{};
    string_arena arena_{};
    std::vector<desktop_entry> entries_{};
    std::vector<unsigned> listed_{};
    terminal_choice terminal_{};
    scan_stats stats_{};
    std::unique_ptr<char[]> file_buf_{};
    int inotify_ = -1;
    /// Watches on the data directories themselves (for `applications` appearing).
    std::vector<int> parent_watches_{};
    unsigned generation_ = 0;
    bool stale_ = false;
    mutable std::vector<unsigned> tier_scratch_[k_rank_tiers]{};
};

} // namespace lx::shell

// ── Implementation ───────────────────────────────────────────────────────────

namespace {

constexpr const char* k_apps_category = "shell.apps";
constexpr std::uint32_t k_dir_mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                     IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
constexpr std::uint32_t k_parent_mask = IN_CREATE | IN_MOVED_TO | IN_ONLYDIR;

void apps_log(lx::trace::level lvl, const char* line) {
    lx::trace::logger::global().log(lvl, k_apps_category, line);
}

[[nodiscard]] bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

[[nodiscard]] bool word_starts_with(std::string_view s, std::string_view p) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const bool boundary = i == 0 || s[i - 1] == ' ' || s[i - 1] == '-' || s[i - 1] == '_' ||
                              s[i - 1] == '.' || s[i - 1] == '/';
        if (boundary && starts_with(s.substr(i), p))
            return true;
    }
    return false;
}

[[nodiscard]] bool contains(std::string_view s, std::string_view p) {
    return s.find(p) != std::string_view::npos;
}

[[nodiscard]] bool sort_less(const lx::shell::desktop_entry& a, const lx::shell::desktop_entry& b) {
    if (const int c = a.name_fold.compare(b.name_fold); c != 0)
        return c < 0;
    if (const int c = a.name.compare(b.name); c != 0)
        return c < 0;
    return a.id < b.id;
}

/// Desktop IDs in the order the terminal is chosen from (§B.4).
constexpr std::string_view k_terminal_preference[] = {
    "foot.desktop",
    "Alacritty.desktop",
    "kitty.desktop",
    "org.wezfurlong.wezterm.desktop",
    "org.gnome.Ptyxis.desktop",
    "org.gnome.Console.desktop",
    "org.gnome.Terminal.desktop",
    "org.kde.konsole.desktop",
    "xterm.desktop",
};

} // namespace

lx::shell::catalog_config lx::shell::catalog_config::from_env() {
    catalog_config c{};
    const char* home = std::getenv("HOME");
    const char* dh = std::getenv("XDG_DATA_HOME");
    if (dh && dh[0] == '/')
        c.data_home = dh;
    else if (home && home[0] == '/')
        c.data_home = std::string(home) + "/.local/share";
    const char* dd = std::getenv("XDG_DATA_DIRS");
    std::string_view dirs = (dd && *dd) ? std::string_view{dd} : "/usr/local/share:/usr/share";
    while (!dirs.empty()) {
        const std::size_t colon = dirs.find(':');
        const std::string_view d = dirs.substr(0, colon);
        if (!d.empty() && d.front() == '/')
            c.data_dirs.emplace_back(d);
        dirs.remove_prefix(colon == std::string_view::npos ? dirs.size() : colon + 1);
    }
    if (const char* p = std::getenv("PATH"))
        c.path_env = p;
    if (const char* t = std::getenv("TERMINAL"))
        c.terminal_env = t;
    c.locale = locale_keys::from_env();
    c.current = desktop_list::from_env();
    return c;
}

lx::shell::app_catalog::app_catalog()
    : file_buf_{std::make_unique_for_overwrite<char[]>(k_max_file + 1)} {}

lx::shell::app_catalog::~app_catalog() { close_watches(); }

void lx::shell::app_catalog::configure(catalog_config cfg) { cfg_ = std::move(cfg); }

void lx::shell::app_catalog::close_watches() {
    if (inotify_ >= 0)
        close(inotify_);
    inotify_ = -1;
    parent_watches_.clear();
}

struct lx::shell::app_catalog::walk_state {
    string_arena* arena;
    std::vector<desktop_entry>* entries;
    std::unordered_set<std::string_view> seen;
    parse_context ctx;
    int inotify;
    unsigned files = 0;
    unsigned dirs = 0;
};

void lx::shell::app_catalog::read_entry(walk_state& w, int dirfd, const char* name,
                                        std::string_view id, std::string_view file) {
    desktop_entry e{};
    const int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return;
    std::size_t n = 0;
    bool too_large = false;
    for (;;) {
        const ssize_t r = read(fd, file_buf_.get() + n, k_max_file + 1 - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += static_cast<std::size_t>(r);
        if (n > k_max_file) {
            too_large = true;
            break;
        }
    }
    close(fd);
    ++w.files;
    if (too_large) {
        if (lx::trace::logger::global().min_level() <= lx::trace::level::debug) {
            char line[512];
            std::snprintf(line, sizeof(line), "desktop file %.*s: larger than 64 KiB: skipped",
                          static_cast<int>(file.size()), file.data());
            apps_log(lx::trace::level::debug, line);
        }
        e.id = w.arena->store(id);
        e.file = w.arena->store(file);
        e.skip = entry_skip::too_large;
    } else {
        parse_desktop_entry({file_buf_.get(), n}, id, file, w.ctx, *w.arena, e);
    }
    w.entries->push_back(e);
    w.seen.insert(w.entries->back().id);
}

void lx::shell::app_catalog::walk(walk_state& w, int dirfd, char* path, std::size_t path_len,
                                  char* prefix, std::size_t prefix_len, unsigned depth) {
    // Watched before it is read: a file created meanwhile is still an event.
    if (w.inotify >= 0)
        (void)inotify_add_watch(w.inotify, path, k_dir_mask);
    ++w.dirs;
    DIR* d = fdopendir(dirfd);
    if (!d) {
        close(dirfd);
        return;
    }
    // Names first, then work: entries are sorted so the walk is deterministic.
    std::vector<std::string> names;
    while (dirent* de = readdir(d)) {
        if (de->d_name[0] == '.')
            continue;
        names.emplace_back(de->d_name);
    }
    std::sort(names.begin(), names.end());
    const int dfd = dirfd;
    for (const std::string& nm : names) {
        struct stat st{};
        if (fstatat(dfd, nm.c_str(), &st, 0) != 0) // follows symlinks
            continue;
        if (path_len + 1 + nm.size() >= PATH_MAX || prefix_len + nm.size() + 1 >= 256)
            continue;
        std::memcpy(path + path_len, "/", 1);
        std::memcpy(path + path_len + 1, nm.data(), nm.size());
        const std::size_t sub_len = path_len + 1 + nm.size();
        path[sub_len] = '\0';
        if (S_ISDIR(st.st_mode)) {
            if (depth + 1 >= k_max_depth)
                continue;
            const int sub = openat(dfd, nm.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sub < 0)
                continue;
            // kde/foo.desktop has the ID kde-foo.desktop.
            std::memcpy(prefix + prefix_len, nm.data(), nm.size());
            prefix[prefix_len + nm.size()] = '-';
            walk(w, sub, path, sub_len, prefix, prefix_len + nm.size() + 1, depth + 1);
        } else if (S_ISREG(st.st_mode) && nm.size() > 8 &&
                   std::string_view{nm}.ends_with(".desktop")) {
            std::memcpy(prefix + prefix_len, nm.data(), nm.size());
            const std::string_view id{prefix, prefix_len + nm.size()};
            if (w.seen.contains(id))
                continue; // shadowed by a directory earlier in precedence
            read_entry(w, dfd, nm.c_str(), id, {path, sub_len});
        }
        path[path_len] = '\0';
    }
    closedir(d);
}

void lx::shell::app_catalog::scan() {
    const auto t0 = std::chrono::steady_clock::now();
    string_arena arena;
    std::vector<desktop_entry> entries;
    entries.reserve(entries_.empty() ? 256 : entries_.size() + 32);
    const int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    std::vector<int> parents;
    walk_state w{&arena, &entries, {}, {}, ino};
    w.ctx.locale = &cfg_.locale;
    w.ctx.current = &cfg_.current;
    w.ctx.path_env = cfg_.path_env;
    w.ctx.check_try_exec = true;

    std::vector<const std::string*> bases;
    if (!cfg_.data_home.empty())
        bases.push_back(&cfg_.data_home);
    for (const std::string& d : cfg_.data_dirs)
        bases.push_back(&d);
    char path[PATH_MAX];
    char prefix[256];
    for (const std::string* base : bases) {
        // The data directory itself: `applications` may be created later.
        if (ino >= 0) {
            const int wd = inotify_add_watch(ino, base->c_str(), k_parent_mask);
            if (wd >= 0)
                parents.push_back(wd);
        }
        const int n = std::snprintf(path, sizeof(path), "%s/applications", base->c_str());
        if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(path))
            continue;
        const int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
            continue;
        walk(w, fd, path, static_cast<std::size_t>(n), prefix, 0, 0);
    }

    std::vector<unsigned> listed;
    listed.reserve(entries.size());
    for (unsigned i = 0; i < entries.size(); ++i)
        if (entries[i].listed())
            listed.push_back(i);
    std::sort(listed.begin(), listed.end(),
              [&](unsigned a, unsigned b) { return sort_less(entries[a], entries[b]); });

    // Swap the generation in.
    close_watches();
    inotify_ = ino;
    parent_watches_ = std::move(parents);
    arena_ = std::move(arena);
    entries_ = std::move(entries);
    listed_ = std::move(listed);
    for (auto& t : tier_scratch_) {
        t.clear();
        t.reserve(listed_.size());
    }
    ++generation_;
    stale_ = false;
    choose_terminal();

    stats_.entries = static_cast<unsigned>(entries_.size());
    stats_.listed = static_cast<unsigned>(listed_.size());
    stats_.files = w.files;
    stats_.dirs = w.dirs;
    stats_.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
    char line[192];
    std::snprintf(line, sizeof(line),
                  "apps: catalog %u entries, %u listed (%u files, %u dirs) in %.2f ms; "
                  "terminal %s",
                  stats_.entries, stats_.listed, stats_.files, stats_.dirs, stats_.ms,
                  terminal_.found ? terminal_.source : "none");
    apps_log(lx::trace::level::info, line);
}

bool lx::shell::app_catalog::on_watch_readable() {
    if (inotify_ < 0)
        return false;
    bool became = false;
    alignas(inotify_event) char buf[4096];
    for (;;) {
        const ssize_t n = read(inotify_, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(inotify_event)) <= n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
            off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            bool relevant = true;
            if (ev->wd >= 0 && std::find(parent_watches_.begin(), parent_watches_.end(), ev->wd) !=
                                   parent_watches_.end())
                relevant = ev->len > 0 && std::strcmp(ev->name, "applications") == 0;
            if (relevant && !stale_) {
                stale_ = true;
                became = true;
            }
        }
    }
    if (became)
        apps_log(lx::trace::level::debug, "apps: catalog stale");
    return became;
}

const lx::shell::desktop_entry* lx::shell::app_catalog::find(std::string_view id) const {
    for (const desktop_entry& e : entries_)
        if (e.id == id)
            return &e;
    return nullptr;
}

unsigned lx::shell::app_catalog::rank(std::string_view query, unsigned* out, unsigned cap) const {
    unsigned n = 0;
    if (query.empty()) {
        for (unsigned i : listed_) {
            if (n == cap)
                break;
            out[n++] = i;
        }
        return n;
    }
    char qbuf[256];
    const std::size_t qn = query.size() < sizeof(qbuf) ? query.size() : sizeof(qbuf);
    for (std::size_t i = 0; i < qn; ++i)
        qbuf[i] = ascii_fold(query[i]);
    const std::string_view q{qbuf, qn};
    for (auto& t : tier_scratch_)
        t.clear();
    for (unsigned i : listed_) {
        const desktop_entry& e = entries_[i];
        int tier = -1;
        if (starts_with(e.name_fold, q))
            tier = 0;
        else if (word_starts_with(e.name_fold, q))
            tier = 1;
        else if (contains(e.name_fold, q))
            tier = 2;
        else if (contains(e.generic_fold, q) || contains(e.keywords_fold, q))
            tier = 3;
        else if (starts_with(e.exec_name, q))
            tier = 4;
        if (tier >= 0)
            tier_scratch_[tier].push_back(i);
    }
    for (const auto& t : tier_scratch_)
        for (unsigned i : t) {
            if (n == cap)
                return n;
            out[n++] = i;
        }
    return n;
}

void lx::shell::app_catalog::choose_terminal() {
    terminal_ = {};
    auto raw = [&](std::string_view argv0, const char* flag, const char* source) {
        char resolved[PATH_MAX];
        if (argv0.empty() || argv0.size() >= sizeof(terminal_.argv0) ||
            !resolve_program(argv0, cfg_.path_env, resolved))
            return false;
        terminal_.found = true;
        terminal_.entry = -1;
        std::snprintf(terminal_.argv0, sizeof(terminal_.argv0), "%.*s",
                      static_cast<int>(argv0.size()), argv0.data());
        std::memcpy(terminal_.program, resolved, sizeof(resolved));
        std::snprintf(terminal_.exec_flag, sizeof(terminal_.exec_flag), "%s", flag);
        std::snprintf(terminal_.source, sizeof(terminal_.source), "%s", source);
        return true;
    };
    if (raw(cfg_.terminal_env, "-e", "$TERMINAL"))
        return;
    if (raw("xdg-terminal-exec", "", "xdg-terminal-exec"))
        return;
    auto usable = [&](const desktop_entry& e) {
        if (!(e.skip == entry_skip::none || e.skip == entry_skip::no_display))
            return false;
        if (!list_contains(e.categories, "TerminalEmulator"))
            return false;
        // foot --server opens no window; footclient needs that server.
        if (e.exec.find("--server") != std::string_view::npos || e.id == "footclient.desktop")
            return false;
        return true;
    };
    auto choose = [&](unsigned i) {
        const desktop_entry& e = entries_[i];
        terminal_.found = true;
        terminal_.entry = static_cast<int>(i);
        const std::string_view flag = e.terminal_arg_exec.empty() ? std::string_view{"-e"}
                                                                   : e.terminal_arg_exec;
        std::snprintf(terminal_.exec_flag, sizeof(terminal_.exec_flag), "%.*s",
                      static_cast<int>(flag.size()), flag.data());
        std::snprintf(terminal_.source, sizeof(terminal_.source), "%.*s",
                      static_cast<int>(e.id.size()), e.id.data());
    };
    for (std::string_view want : k_terminal_preference)
        for (unsigned i = 0; i < entries_.size(); ++i)
            if (entries_[i].id == want && usable(entries_[i])) {
                choose(i);
                return;
            }
    int best = -1;
    for (unsigned i = 0; i < entries_.size(); ++i)
        if (usable(entries_[i]) &&
            (best < 0 || sort_less(entries_[i], entries_[static_cast<unsigned>(best)])))
            best = static_cast<int>(i);
    if (best >= 0)
        choose(static_cast<unsigned>(best));
}
