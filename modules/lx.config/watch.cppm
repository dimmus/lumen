module;

// Watching the configuration files (design-session-and-wm C.4): inotify on the directory of
// each layer (an editor replaces a file by renaming over it, which a watch on the file itself
// would lose), on the directory a symlinked file points into (dotfile managers), and, for a
// directory that does not exist yet, on its nearest existing ancestor for the component on
// the way to it (`~/.config` for `lumen`). One fd; it sleeps until something changes. Any
// change to a watched name re-arms every watch (a handful of syscalls), so a directory that
// appears, goes or is replaced is followed without special cases.

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

export module lx.config:watch;

import std;
import :locate;

export namespace lx::config {

class config_watch {
public:
    static constexpr unsigned k_max_watches = 24;

    config_watch() = default;
    ~config_watch() { close(); }
    config_watch(const config_watch&) = delete;
    config_watch& operator=(const config_watch&) = delete;

    /// Creates the inotify fd and arms the watches for `where`. False when inotify is not
    /// available (the configuration is then read at start-up and on request only).
    [[nodiscard]] bool open(const config_location& where);
    /// Readable when something happened. -1 when closed.
    [[nodiscard]] int fd() const { return fd_; }
    /// Reads every pending event (non-blocking). True when a configuration file may have
    /// changed: the caller re-reads (after its debounce). Watches are re-armed as needed.
    [[nodiscard]] bool drain();
    void close();
    /// How many (directory, name) interests are armed (tests, the log).
    [[nodiscard]] unsigned watch_count() const { return static_cast<unsigned>(entries_.size()); }

private:
    struct entry {
        int wd = -1;
        /// The name in that directory that matters ("config.toml", or the next directory on
        /// the way to it).
        char name[256]{};
    };
    void arm();
    void watch_path(const char* file);
    void add(const char* dir, const char* name);

    int fd_ = -1;
    config_location where_{};
    std::inplace_vector<entry, k_max_watches> entries_{};
};

} // namespace lx::config

namespace {

constexpr std::uint32_t k_dir_mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
                                     IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

/// `path`'s directory into `dir` and last component into `name`.
void split_path(const char* path, char* dir, std::size_t dir_cap, char* name, std::size_t name_cap) {
    const char* slash = std::strrchr(path, '/');
    if (!slash) {
        std::snprintf(dir, dir_cap, ".");
        std::snprintf(name, name_cap, "%s", path);
        return;
    }
    const auto n = static_cast<int>(slash - path);
    std::snprintf(dir, dir_cap, "%.*s", n == 0 ? 1 : n, path);
    std::snprintf(name, name_cap, "%s", slash + 1);
}

[[nodiscard]] bool is_dir(const char* path) {
    struct stat st {};
    return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

bool lx::config::config_watch::open(const config_location& where) {
    close();
    where_ = where;
    if (where.disabled)
        return true;
    fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0)
        return false;
    arm();
    return true;
}

void lx::config::config_watch::close() {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    entries_.clear();
}

void lx::config::config_watch::add(const char* dir, const char* name) {
    if (entries_.size() == k_max_watches)
        return;
    const int wd = ::inotify_add_watch(fd_, dir, k_dir_mask | IN_MASK_ADD);
    if (wd < 0)
        return;
    for (const entry& e : entries_)
        if (e.wd == wd && std::strcmp(e.name, name) == 0)
            return;
    entry e{};
    e.wd = wd;
    std::snprintf(e.name, sizeof(e.name), "%s", name);
    entries_.push_back(e);
}

void lx::config::config_watch::watch_path(const char* file) {
    char dir[512];
    char name[256];
    split_path(file, dir, sizeof(dir), name, sizeof(name));
    // The directory may not exist yet: watch the nearest ancestor that does, for the next
    // component on the way down (its creation re-arms everything).
    while (!is_dir(dir)) {
        char parent[512];
        char child[256];
        split_path(dir, parent, sizeof(parent), child, sizeof(child));
        if (std::strcmp(parent, dir) == 0)
            return; // "/" missing: nothing to watch
        std::snprintf(dir, sizeof(dir), "%s", parent);
        std::snprintf(name, sizeof(name), "%s", child);
    }
    add(dir, name);
}

void lx::config::config_watch::arm() {
    if (fd_ < 0)
        return;
    for (const entry& e : entries_)
        (void)::inotify_rm_watch(fd_, e.wd);
    entries_.clear();
    for (const config_path& p : where_.files) {
        watch_path(p.path);
        // A symlinked file: its target's directory is where an editor writes.
        struct stat st {};
        if (::lstat(p.path, &st) == 0 && S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            if (::realpath(p.path, target))
                watch_path(target);
        }
    }
}

bool lx::config::config_watch::drain() {
    if (fd_ < 0)
        return false;
    alignas(inotify_event) char buf[4096];
    bool changed = false;
    bool rearm = false;
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(inotify_event)) <= n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
            off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            if (ev->mask & IN_Q_OVERFLOW) {
                changed = rearm = true;
                continue;
            }
            if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                // A watched directory went (or was renamed). Watches `arm` removed itself
                // report IN_IGNORED too: those are no longer current and mean nothing.
                if (std::ranges::any_of(entries_, [&](const entry& e) { return e.wd == ev->wd; }))
                    changed = rearm = true;
                continue;
            }
            if (ev->len == 0)
                continue;
            for (const entry& e : entries_) {
                if (e.wd == ev->wd && std::strcmp(e.name, ev->name) == 0) {
                    changed = rearm = true;
                    break;
                }
            }
        }
    }
    if (rearm)
        arm();
    return changed;
}
