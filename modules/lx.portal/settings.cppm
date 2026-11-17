module;

// org.freedesktop.impl.portal.Settings (the Settings portal's backend half): the read-only
// host settings toolkits ask for — org.freedesktop.appearance color-scheme and contrast —
// derived from the layered configuration ([ui] appearance), re-read when the files change on
// disk (lx.config's directory watch), the flipped values becoming SettingChanged signals.
//
// The D-Bus surface itself (the vtable, the signal) lives in :service beside the others;
// this partition is the pure half — the mapping, ReadAll's namespace filter, and the
// reload/flip state machine — unit-tested in test_portal_settings without a bus.
//
// Mapping ([ui] appearance → color-scheme, the interface doc's values): dark→1 (prefer
// dark), light→2 (prefer light). 0 (no preference) is never served: Lumen always has an
// appearance. contrast is always 0: Lumen has no high-contrast mode. accent-color and
// reduced-motion stay unserved — Read of them answers the portal's not-found error, like
// every unknown namespace or key.

export module lx.portal:settings;

import std;

import lx.config;

export namespace lx::portal {

/// The only namespace served.
inline constexpr const char* k_appearance_namespace = "org.freedesktop.appearance";
/// The interface doc's color-scheme values (0 no preference is never served).
inline constexpr unsigned k_color_scheme_dark = 1;
inline constexpr unsigned k_color_scheme_light = 2;

/// The values served today.
struct appearance_values {
    unsigned color_scheme = k_color_scheme_dark;
    unsigned contrast = 0;
};

/// The values of a parsed configuration ([ui] appearance → color-scheme; contrast 0).
[[nodiscard]] appearance_values appearance_of(const lx::config::config_snapshot& c);

/// ReadAll's namespace filter (the interface doc): an empty list or an empty pattern
/// matches everything, an exact name matches, and a trailing `*` matches its prefix
/// ("org.freedesktop.*" but not a glob in the middle).
[[nodiscard]] bool namespace_matches(std::string_view ns, const std::vector<std::string>& patterns);

/// Which served keys flipped between two value sets.
struct appearance_flip {
    bool color_scheme = false;
    bool contrast = false;
    [[nodiscard]] explicit operator bool() const { return color_scheme || contrast; }
};

[[nodiscard]] inline appearance_flip flips(const appearance_values& before,
                                           const appearance_values& after) {
    return {before.color_scheme != after.color_scheme, before.contrast != after.contrast};
}

/// The Settings state: the served values, and the watch that re-reads the layered
/// configuration when its files change on disk. Single-threaded (the service's thread).
class settings_state {
public:
    /// Locates (`env`; null: the process environment), reads and parses the layers, and
    /// opens the watch. Layers that are missing, unreadable or invalid serve the defaults,
    /// as the compositor does (it keeps its previous configuration).
    explicit settings_state(lx::config::env_lookup env = nullptr);
    /// The watch's fd (-1: nothing is watched — LUMEN_CONFIG=0, or no inotify).
    [[nodiscard]] int fd() const { return watch_.fd(); }
    [[nodiscard]] appearance_values values() const { return values_; }
    /// Drains the watch and, when a configuration file may have changed, re-reads and
    /// re-parses the layers. The flip of the served values (SettingChanged's payload);
    /// empty when nothing is readable, nothing changed, or the new content does not
    /// change what is served.
    [[nodiscard]] appearance_flip poll_changed();

private:
    void load();

    lx::config::env_lookup env_ = nullptr;
    lx::config::config_location where_{};
    lx::config::config_watch watch_{};
    appearance_values values_{};
};

} // namespace lx::portal
