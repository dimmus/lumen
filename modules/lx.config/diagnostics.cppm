module;

// What reading a configuration found wrong (design-session-and-wm C.6): one record per
// problem, with the layer (file) and line it came from and the key it concerns, in fixed
// storage — a configuration is read on a reload, never per frame, but a file full of
// mistakes must not grow memory without bound either: past `k_max` the rest are counted,
// not kept.

export module lx.config:diagnostics;

import std;

export namespace lx::config {

enum class severity : unsigned char { error, warning };

[[nodiscard]] constexpr const char* severity_name(severity s) {
    return s == severity::error ? "error" : "warning";
}

struct diagnostic {
    severity sev = severity::error;
    /// Index of the layer (`config_text::layers`); `k_no_layer` for one that concerns no
    /// file (an apply-time problem: a keymap that does not compile).
    unsigned layer = 0;
    /// 1-based line in that file; 0: the file as a whole.
    unsigned line = 0;
    /// The key ("input.touchpad.accel-speed", a binding's combination); empty for syntax.
    char key[64]{};
    char message[192]{};
};

inline constexpr unsigned k_no_layer = ~0u;

class diagnostics {
public:
    static constexpr unsigned k_max = 64;

    /// Records one. Past `k_max` it is counted (`dropped`) but not kept.
    void add(severity sev, unsigned layer, unsigned line, std::string_view key,
             const char* message);
    template <typename... Args>
    void addf(severity sev, unsigned layer, unsigned line, std::string_view key,
              std::format_string<Args...> fmt, Args&&... args) {
        char text[sizeof(diagnostic{}.message)];
        auto res = std::format_to_n(text, sizeof(text) - 1, fmt, std::forward<Args>(args)...);
        *res.out = '\0';
        add(sev, layer, line, key, text);
    }
    void clear() {
        items_.clear();
        errors_ = warnings_ = dropped_ = 0;
    }
    /// Appends `other`'s records (and counts).
    void append(const diagnostics& other);

    [[nodiscard]] std::span<const diagnostic> items() const { return items_; }
    [[nodiscard]] unsigned errors() const { return errors_; }
    [[nodiscard]] unsigned warnings() const { return warnings_; }
    [[nodiscard]] unsigned dropped() const { return dropped_; }
    [[nodiscard]] bool empty() const { return errors_ == 0 && warnings_ == 0; }
    /// Same records (what decides whether a notification is shown again).
    [[nodiscard]] bool same_as(const diagnostics& other) const;

private:
    std::inplace_vector<diagnostic, k_max> items_{};
    unsigned errors_ = 0;
    unsigned warnings_ = 0;
    unsigned dropped_ = 0;
};

/// "config.toml:12: input.touchpad.tapp: unknown key (did you mean tap?)" — the file's last
/// path component (or the whole path with `full_path`), the line when there is one, the key
/// when there is one. `file` may be null (no layer).
void format_diagnostic(const diagnostic& d, const char* file, bool full_path, char* out,
                       std::size_t cap);

/// Damerau–Levenshtein distance (optimal string alignment), case-insensitive, capped at
/// `limit + 1`: what "did you mean" uses (≤ 2).
[[nodiscard]] unsigned edit_distance(std::string_view a, std::string_view b, unsigned limit = 2);

} // namespace lx::config


void lx::config::diagnostics::add(severity sev, unsigned layer, unsigned line,
                                  std::string_view key, const char* message) {
    if (sev == severity::error)
        ++errors_;
    else
        ++warnings_;
    if (items_.size() == k_max) {
        ++dropped_;
        return;
    }
    diagnostic d{};
    d.sev = sev;
    d.layer = layer;
    d.line = line;
    const std::size_t kn = std::min(key.size(), sizeof(d.key) - 1);
    std::memcpy(d.key, key.data(), kn);
    d.key[kn] = '\0';
    std::snprintf(d.message, sizeof(d.message), "%s", message ? message : "");
    items_.push_back(d);
}

void lx::config::diagnostics::append(const diagnostics& other) {
    unsigned kept_errors = 0;
    unsigned kept_warnings = 0;
    for (const diagnostic& d : other.items_) {
        add(d.sev, d.layer, d.line, d.key, d.message);
        ++(d.sev == severity::error ? kept_errors : kept_warnings);
    }
    // `add` counted the kept ones; those `other` had to drop count too.
    dropped_ += other.dropped_;
    errors_ += other.errors_ - kept_errors;
    warnings_ += other.warnings_ - kept_warnings;
}

bool lx::config::diagnostics::same_as(const diagnostics& other) const {
    if (items_.size() != other.items_.size() || errors_ != other.errors_ ||
        warnings_ != other.warnings_)
        return false;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const diagnostic& a = items_[i];
        const diagnostic& b = other.items_[i];
        if (a.sev != b.sev || a.layer != b.layer || a.line != b.line ||
            std::strcmp(a.key, b.key) != 0 || std::strcmp(a.message, b.message) != 0)
            return false;
    }
    return true;
}

void lx::config::format_diagnostic(const diagnostic& d, const char* file, bool full_path,
                                   char* out, std::size_t cap) {
    if (!out || cap == 0)
        return;
    const char* name = file ? file : "";
    if (!full_path && file) {
        if (const char* slash = std::strrchr(file, '/'))
            name = slash + 1;
    }
    // Straight into `out`: an intermediate buffer for "file:line" cut paths longer than it
    // (a configuration under a long $XDG_CONFIG_HOME).
    const bool at_line = *name && d.line;
    if (!*name)
        name = "lumen";
    if (at_line && d.key[0])
        std::snprintf(out, cap, "%s:%u: %s: %s", name, d.line, d.key, d.message);
    else if (at_line)
        std::snprintf(out, cap, "%s:%u: %s", name, d.line, d.message);
    else if (d.key[0])
        std::snprintf(out, cap, "%s: %s: %s", name, d.key, d.message);
    else
        std::snprintf(out, cap, "%s: %s", name, d.message);
}

unsigned lx::config::edit_distance(std::string_view a, std::string_view b, unsigned limit) {
    constexpr std::size_t k_max_len = 48;
    if (a.size() > k_max_len || b.size() > k_max_len)
        return limit + 1;
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    if ((n > m ? n - m : m - n) > limit)
        return limit + 1;
    const auto lower = [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    };
    // Three rows of the optimal-string-alignment table.
    unsigned rows[3][k_max_len + 1];
    unsigned* prev2 = rows[0];
    unsigned* prev = rows[1];
    unsigned* cur = rows[2];
    for (std::size_t j = 0; j <= m; ++j)
        prev[j] = static_cast<unsigned>(j);
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = static_cast<unsigned>(i);
        unsigned best = cur[0];
        for (std::size_t j = 1; j <= m; ++j) {
            const unsigned cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0u : 1u;
            unsigned v = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && lower(a[i - 1]) == lower(b[j - 2]) &&
                lower(a[i - 2]) == lower(b[j - 1]))
                v = std::min(v, prev2[j - 2] + 1);
            cur[j] = v;
            best = std::min(best, v);
        }
        if (best > limit)
            return limit + 1;
        unsigned* t = prev2;
        prev2 = prev;
        prev = cur;
        cur = t;
    }
    return std::min(prev[m], limit + 1);
}
