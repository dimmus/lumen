#include "lumen_test.hpp"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

import lx.foundation;
import lx.compositor;
using namespace lx::compositor;

LUMEN_TEST(window_rules_replace_and_clear_exact_app_ids) {
    window_rule_store s;
    window_rule r; r.app_id = "org.example.Editor"; r.workspace_index = 2;
    LUMEN_CHECK(s.set(r).ok());
    LUMEN_CHECK(s.find("org.example.editor") == nullptr);
    r.workspace_index.reset(); r.server_side = false;
    LUMEN_CHECK(s.set(r).ok());
    LUMEN_CHECK(s.rules().size() == 1);
    LUMEN_CHECK(!s.find(r.app_id)->workspace_index);
    LUMEN_CHECK(s.find(r.app_id)->server_side == false);
    LUMEN_CHECK(s.clear("unknown").ok());
    LUMEN_CHECK(s.clear(r.app_id).ok());
    LUMEN_CHECK(s.rules().empty());
}

LUMEN_TEST(window_rules_roundtrip_all_optional_fields) {
    window_rule_store s, copy;
    window_rule r;
    r.app_id = "a\"b"; r.output_name = "DP-1"; r.workspace_index = 3;
    r.geometry = lx::rect2i{-200, 30, 640, 480}; r.state_flags = 4; r.server_side = false;
    LUMEN_CHECK(s.set(r).ok());
    LUMEN_CHECK(copy.parse(s.serialize()).ok());
    const auto* got = copy.find(r.app_id);
    LUMEN_CHECK(got && got->output_name == r.output_name && got->workspace_index == 3);
    LUMEN_CHECK(got->geometry->x == -200 && got->geometry->width == 640);
    LUMEN_CHECK(got->state_flags == 4 && got->server_side == false);
}

LUMEN_TEST(window_rules_reject_invalid_and_duplicate_values_transactionally) {
    window_rule_store s;
    window_rule r; r.app_id = "keep";
    LUMEN_CHECK(s.set(r).ok());
    for (const auto text : {
        "[[rule]]\napp_id = \"x\"\nworkspace_index = 0\n",
        "[[rule]]\napp_id = \"x\"\nstate_flags = 12\n",
        "[[rule]]\napp_id = \"x\"\ngeometry = [0, 0, -1, 2]\n",
        "[[rule]]\napp_id = \"x\"\nunknown = true\n",
        "[[rule]]\napp_id = \"x\"\n[[rule]]\napp_id = \"x\"\n",
        "[[rule]]\napp_id = \"x\"\napp_id = \"y\"\n"}) {
        LUMEN_CHECK(!s.parse(text).ok());
        LUMEN_CHECK(s.rules().size() == 1 && s.find("keep"));
    }
    r.app_id = std::string(128, 'a'); LUMEN_CHECK(!s.set(r).ok());
    r.app_id = "bad\napp"; LUMEN_CHECK(!s.set(r).ok());
    r.app_id = "ok"; r.state_flags = 16; LUMEN_CHECK(!s.set(r).ok());
}

LUMEN_TEST(window_rules_capacity_rejects_new_entry_but_allows_replacement) {
    window_rule_store s;
    for (unsigned i = 0; i < window_rule_store::k_capacity; ++i) {
        window_rule r; r.app_id = std::to_string(i); LUMEN_CHECK(s.set(r).ok());
    }
    window_rule r; r.app_id = "overflow"; LUMEN_CHECK(!s.set(r).ok());
    r.app_id = "0"; r.state_flags = 0; LUMEN_CHECK(s.set(r).ok());
    LUMEN_CHECK(s.rules().size() == window_rule_store::k_capacity);
}

LUMEN_TEST(window_rules_persistence_reload_and_failed_save_preserve_memory) {
    char dir[] = "/tmp/lumen-rules-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir));
    const std::string path = std::string{dir} + "/window-rules.toml";
    window_rule_store s, copy;
    LUMEN_CHECK(s.load(path).ok());
    window_rule r; r.app_id = "persist"; r.server_side = true;
    LUMEN_CHECK(s.set(r).ok());
    struct stat st{}; LUMEN_CHECK(::stat(path.c_str(), &st) == 0);
    LUMEN_CHECK((st.st_mode & 0777) == 0600);
    LUMEN_CHECK(copy.load(path).ok() && copy.find("persist"));
    { std::ofstream file{path}; file << "[[rule]]\napp_id = \"broken\"\ngeometry = [0, 0, 0, 5]\n"; }
    LUMEN_CHECK(!copy.reload().ok());
    LUMEN_CHECK(copy.find("persist") && !copy.find("broken"));
    LUMEN_CHECK(::unlink(path.c_str()) == 0);
    LUMEN_CHECK(::rmdir(dir) == 0);
    // A regular file in place of the parent makes persistence fail even as root.
    { std::ofstream file{dir}; file << "blocked"; }
    r.app_id = "not-saved";
    LUMEN_CHECK(!s.set(r).ok());
    LUMEN_CHECK(!s.find("not-saved") && s.find("persist"));
    LUMEN_CHECK(::unlink(dir) == 0);
}

LUMEN_TEST(window_rules_decoration_requires_client_protocol_and_resets_on_remap) {
    toplevel_record r;
    r.rule_server_side = true;
    LUMEN_CHECK(toplevel_manager::wanted_decoration(r) == decoration_mode::client);
    int object = 0; r.deco_resource = &object;
    r.deco_requested = decoration_request::client;
    LUMEN_CHECK(toplevel_manager::wanted_decoration(r) == decoration_mode::server);
    r.rule_server_side = false;
    LUMEN_CHECK(toplevel_manager::wanted_decoration(r) == decoration_mode::client);
    r.rule_workspace_index = 3;
    toplevel_manager::reset_for_remap(r);
    LUMEN_CHECK(!r.rule_server_side && r.rule_workspace_index == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
