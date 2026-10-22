module;

// Bodies of `:files`.

#include <errno.h>
#include <stdio.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

module lx.portal;

import std;

import lx.foundation;
import :files;

namespace lx::portal {

std::string file_uri(std::string_view path) {
    static constexpr char k_hex[] = "0123456789ABCDEF";
    std::string out = "file://";
    out.reserve(out.size() + path.size() * 3);
    for (const char ch : path) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        if (keep) {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(k_hex[c >> 4]);
            out.push_back(k_hex[c & 15u]);
        }
    }
    return out;
}

std::string parse_user_dir(std::string_view text, std::string_view key, std::string_view home) {
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.empty() || line.front() == '#' || !line.starts_with(key))
            continue;
        line.remove_prefix(key.size());
        if (line.empty() || line.front() != '=')
            continue;
        line.remove_prefix(1);
        if (line.size() < 2 || line.front() != '"')
            continue;
        // The closing quote is the first one not escaped by a backslash.
        std::size_t close = std::string_view::npos;
        for (std::size_t i = 1; i < line.size(); ++i) {
            if (line[i] == '\\') {
                ++i;
            } else if (line[i] == '"') {
                close = i;
                break;
            }
        }
        if (close == std::string_view::npos)
            continue;
        std::string_view value = line.substr(1, close - 1);
        std::string out;
        if (value.starts_with("$HOME")) {
            out.assign(home);
            value.remove_prefix(5);
        } else if (!value.starts_with("/")) {
            continue; // relative paths are not allowed by the format
        }
        // Backslash escapes (xdg-user-dirs writes "\"" and "\\").
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size())
                ++i;
            out.push_back(value[i]);
        }
        while (out.size() > 1 && out.back() == '/')
            out.pop_back();
        return out;
    }
    return {};
}

std::string pictures_dir(const char* home, const char* config_home, const char* env_pictures) {
    const std::string h = home && *home ? home : "/";
    std::string cfg = config_home && *config_home ? std::string(config_home) : h + "/.config";
    const std::string file = cfg + "/user-dirs.dirs";
    if (FILE* f = std::fopen(file.c_str(), "re")) {
        std::string text;
        char buf[1024];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < 65536)
            text.append(buf, n);
        std::fclose(f);
        std::string dir = parse_user_dir(text, "XDG_PICTURES_DIR", h);
        // xdg-user-dirs sets a directory to $HOME to disable it; a screenshot then goes to
        // the conventional place instead of the home directory itself.
        if (!dir.empty() && dir != h)
            return dir;
    }
    if (env_pictures && env_pictures[0] == '/')
        return env_pictures;
    return h + "/Pictures";
}

std::string screenshot_stem(const std::tm& t) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "Screenshot from %04d-%02d-%02d %02d-%02d-%02d", t.tm_year + 1900,
                  t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

lx::result<void> make_dirs(const std::string& dir, unsigned mode) {
    if (dir.empty() || dir.front() != '/')
        return lx::invalid_argument("make_dirs: not an absolute path");
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/')
            continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), static_cast<mode_t>(mode)) != 0 && errno != EEXIST)
            return lx::make_error(lx::error_domain::io,
                                  static_cast<int>(errno == EACCES ? lx::io_err::permission_denied
                                                                   : lx::io_err::not_found),
                                  "cannot create the screenshot directory");
    }
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return lx::make_error(lx::io_err::not_found, "the screenshot directory is not a directory");
    return {};
}

lx::result<int> create_unique(const std::string& dir, const std::string& stem, bool private_file,
                              std::string& path) {
    const mode_t mode = private_file ? 0600 : 0644;
    for (int n = 1; n < 1000; ++n) {
        path = dir + "/" + stem + (n == 1 ? std::string{} : "-" + std::to_string(n)) + ".png";
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return lx::make_error(lx::error_domain::io,
                                  static_cast<int>(errno == EACCES ? lx::io_err::permission_denied
                                                                   : lx::io_err::not_found),
                                  "cannot create the screenshot file");
    }
    return lx::make_error(lx::io_err::not_found, "no free screenshot file name");
}

unsigned prune_older(const std::string& dir, std::string_view prefix, long max_age_s) {
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return 0;
    const int dfd = ::dirfd(d);
    const std::time_t now = std::time(nullptr);
    unsigned removed = 0;
    while (dirent* e = ::readdir(d)) {
        const std::string_view name = e->d_name;
        if (!name.starts_with(prefix))
            continue;
        struct stat st{};
        if (::fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (now - st.st_mtime > max_age_s && ::unlinkat(dfd, e->d_name, 0) == 0)
            ++removed;
    }
    ::closedir(d);
    return removed;
}

} // namespace lx::portal
