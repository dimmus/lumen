module;

// Where xdg-desktop-portal-lumen puts screenshots, and how it names them (pure parts are
// unit-tested in test_portal_compose).
//
//   - Interactive screenshots are the user's: `$XDG_PICTURES_DIR/Screenshots/Screenshot
//     from YYYY-MM-DD HH-MM-SS.png` (GNOME's naming, so file managers and galleries file
//     them with GNOME's), never overwriting (`-2`, `-3`, … on a clash; O_EXCL).
//   - Non-interactive ones are what an application asked for programmatically: a private
//     file in `$XDG_RUNTIME_DIR/lumen-portal/` (0700 directory, 0600 file), which the
//     frontend hands to a sandboxed caller through the document portal. Files older than an
//     hour are removed when the next one is written, so the tmpfs does not fill up.

#include <ctime>
#include <string>
#include <string_view>

export module lx.portal:files;

import lx.foundation;

export namespace lx::portal {

/// `file://` URI of an absolute path, percent-encoding everything but RFC 3986's
/// unreserved characters and '/'.
[[nodiscard]] std::string file_uri(std::string_view path);

/// The value of `key` (e.g. "XDG_PICTURES_DIR") in a user-dirs.dirs text, `$HOME`
/// expanded; empty when absent. Only the forms xdg-user-dirs writes: "$HOME/…" or "/…".
[[nodiscard]] std::string parse_user_dir(std::string_view text, std::string_view key,
                                         std::string_view home);

/// The pictures directory: user-dirs.dirs (in `config_home`, else `home`/.config), then
/// `env_pictures` ($XDG_PICTURES_DIR, which xdg-user-dir also honours), then `home`/Pictures.
[[nodiscard]] std::string pictures_dir(const char* home, const char* config_home,
                                       const char* env_pictures);

/// "Screenshot from 2026-10-22 13-45-07" (no extension).
[[nodiscard]] std::string screenshot_stem(const std::tm& local);

/// Creates `dir` and its parents (`mode` for the ones created).
[[nodiscard]] lx::result<void> make_dirs(const std::string& dir, unsigned mode);

/// Creates a new file `dir`/`stem``.png` (or `stem-N.png` on a clash), 0600 when
/// `private_file`, else 0644; returns the fd and sets `path`.
[[nodiscard]] lx::result<int> create_unique(const std::string& dir, const std::string& stem,
                                            bool private_file, std::string& path);

/// Removes regular files in `dir` whose name starts with `prefix` and that are older than
/// `max_age_s`; returns how many went.
unsigned prune_older(const std::string& dir, std::string_view prefix, long max_age_s);

} // namespace lx::portal
