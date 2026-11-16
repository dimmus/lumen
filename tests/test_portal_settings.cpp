// xdg-desktop-portal-lumen's Settings backend (org.freedesktop.impl.portal.Settings): the
// [ui] appearance → org.freedesktop.appearance mapping (dark→1, light→2, contrast always 0),
// ReadAll's namespace filter (empty list/pattern, exact name, trailing glob), and the
// reload state machine over a temp configuration directory: an editor's write-by-rename
// that flips the appearance is a flip, a rewrite of the same values is not, and
// LUMEN_CONFIG=0 degrades to the defaults with no watch.
#include "lumen_test.hpp"

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <unistd.h>

import std;
import lx.config;
import lx.portal;

using namespace lx::portal;
namespace cfg = lx::config;

namespace {

const char* g_lumen_config = nullptr;
const char* g_home = nullptr;

/// The test's environment: LUMEN_CONFIG (a temp file, or "0"), nothing else.
const char* test_env(const char* name) {
    if (std::strcmp(name, "LUMEN_CONFIG") == 0)
        return g_lumen_config;
    if (std::strcmp(name, "HOME") == 0)
        return g_home;
    return nullptr;
}

std::string temp_dir() {
    char tmpl[] = "/tmp/lumen-portal-settings-test-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "";
}

void write_file(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return;
    std::fputs(text, f);
    std::fclose(f);
}

/// `content` parsed as one layer (the value of [ui] appearance is all the tests need).
cfg::config_snapshot parse(const char* content) {
    cfg::config_text t{};
    (void)t.add_layer("/tmp/lumen-portal-settings-test/config.toml", content);
    cfg::config_snapshot c{};
    cfg::diagnostics d{};
    (void)cfg::parse_config(t, c, d);
    return c;
}

} // namespace

LUMEN_TEST(appearance_maps_to_the_portal_color_scheme) {
    LUMEN_CHECK(appearance_of(cfg::default_config()).color_scheme == k_color_scheme_dark);
    LUMEN_CHECK(appearance_of(cfg::default_config()).contrast == 0);
    LUMEN_CHECK(appearance_of(parse("[ui]\nappearance = \"dark\"\n")).color_scheme == k_color_scheme_dark);
    LUMEN_CHECK(appearance_of(parse("[ui]\nappearance = \"light\"\n")).color_scheme ==
                k_color_scheme_light);
    // contrast is 0 whatever the file says (nothing maps to it yet).
    LUMEN_CHECK(appearance_of(parse("[ui]\nappearance = \"light\"\n")).contrast == 0);
}

LUMEN_TEST(readall_filters_namespaces_by_empty_exact_or_trailing_glob) {
    LUMEN_CHECK(namespace_matches(k_appearance_namespace, {}));
    LUMEN_CHECK(namespace_matches(k_appearance_namespace, {""}));
    LUMEN_CHECK(namespace_matches(k_appearance_namespace, {"org.freedesktop.appearance"}));
    LUMEN_CHECK(namespace_matches(k_appearance_namespace, {"org.freedesktop.*"}));
    LUMEN_CHECK(namespace_matches(k_appearance_namespace, {"*"}));
    LUMEN_CHECK(namespace_matches(k_appearance_namespace, {"org.example", "org.freedesktop.app*"}));
    LUMEN_CHECK(!namespace_matches(k_appearance_namespace, {"org.example"}));
    // A glob only for trailing sections: the exact namespace does not end in the dot.
    LUMEN_CHECK(!namespace_matches(k_appearance_namespace, {"org.freedesktop.appearance.*"}));
    // A pattern longer than the namespace is never a prefix of it.
    LUMEN_CHECK(!namespace_matches(k_appearance_namespace, {"org.freedesktop.appearance.color*"}));
}

LUMEN_TEST(reload_flips_only_when_the_served_values_change) {
    const std::string dir = temp_dir();
    LUMEN_CHECK(!dir.empty());
    if (dir.empty())
        return;
    const std::string path = dir + "/config.toml";
    write_file(path, "[ui]\nappearance = \"dark\"\n");
    g_lumen_config = path.c_str();
    g_home = dir.c_str();
    settings_state st{test_env};
    LUMEN_CHECK(st.fd() >= 0); // the watch is armed on the temp directory
    LUMEN_CHECK(st.values().color_scheme == k_color_scheme_dark);
    LUMEN_CHECK(!st.poll_changed()); // nothing happened yet

    // An editor's save: a temporary file, then a rename over the configuration. The
    // temporary itself is not a change; the rename is; and it flips the appearance.
    write_file(path + ".new", "[ui]\nappearance = \"light\"\n");
    LUMEN_CHECK(!st.poll_changed());
    LUMEN_CHECK(::rename((path + ".new").c_str(), path.c_str()) == 0);
    const appearance_flip f = st.poll_changed();
    LUMEN_CHECK(f.color_scheme);
    LUMEN_CHECK(!f.contrast);
    LUMEN_CHECK(st.values().color_scheme == k_color_scheme_light);

    // A rewrite with the same values: a change on disk, no flip.
    write_file(path + ".new", "[ui]\nappearance = \"light\"\n");
    (void)st.poll_changed();
    LUMEN_CHECK(::rename((path + ".new").c_str(), path.c_str()) == 0);
    LUMEN_CHECK(!st.poll_changed());
    LUMEN_CHECK(st.values().color_scheme == k_color_scheme_light);

    // And back: light → dark flips again.
    write_file(path + ".new", "[ui]\nappearance = \"dark\"\n");
    (void)st.poll_changed();
    LUMEN_CHECK(::rename((path + ".new").c_str(), path.c_str()) == 0);
    LUMEN_CHECK(st.poll_changed().color_scheme);
    LUMEN_CHECK(st.values().color_scheme == k_color_scheme_dark);
}

LUMEN_TEST(disabled_config_serves_the_defaults_without_a_watch) {
    g_lumen_config = "0";
    settings_state st{test_env};
    LUMEN_CHECK(st.fd() < 0);
    LUMEN_CHECK(st.values().color_scheme == k_color_scheme_dark);
    LUMEN_CHECK(st.values().contrast == 0);
    LUMEN_CHECK(!st.poll_changed());
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
