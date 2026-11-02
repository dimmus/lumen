// ADR-013 Phase C's gate: lx.config:edit — the user layer's splice editor. Every splice
// preserves the other bytes, lands where the reader expects it, and re-parses to a
// snapshot that changed only in the intended key; `deploy/lumen/config.toml` (the
// documented defaults, comments included) is the adversarial corpus.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.config;

namespace {

namespace cfg = lx::config;

/// Parses `text` as one layer over the defaults (the settings application's verification).
[[nodiscard]] bool parse_one(std::string_view text, cfg::config_snapshot& out,
                             cfg::diagnostics& diag) {
    cfg::config_text t{};
    LUMEN_CHECK(t.add_layer("test", text));
    return cfg::parse_config(t, out, diag);
}

} // namespace

LUMEN_TEST(config_edit_replaces_an_existing_key_in_place) {
    std::string text = "# my settings\n"
                       "\n"
                       "[focus]\n"
                       "mode = \"click\"      # kept comment on the line goes\n"
                       "raise-on-click = true\n"
                       "\n"
                       "[tiling]\n"
                       "gaps = 8\n"
                       "layout = \"floating\"\n"
                       "\n"
                       "[general]\n"
                       "default-bindings = true\n";
    cfg::edit_report r{};
    const std::string edited =
        cfg::splice_key(text, "focus", "mode", cfg::edit_value::word("sloppy"), r);
    LUMEN_CHECK(r.status == cfg::edit_status::ok);
    LUMEN_CHECK(r.line == 4);
    // The line changed; nothing else did.
    LUMEN_CHECK(edited.find("mode = \"sloppy\"") != std::string::npos);
    LUMEN_CHECK(edited.find("raise-on-click = true") != std::string::npos);
    LUMEN_CHECK(edited.find("[tiling]") != std::string::npos);
    LUMEN_CHECK(edited.find("gaps = 8") != std::string::npos);
    LUMEN_CHECK(edited.find("# my settings") == 0);
    // The re-parse takes the word.
    cfg::config_snapshot a{};
    cfg::diagnostics d{};
    LUMEN_CHECK(parse_one(text, a, d));
    cfg::config_snapshot b{};
    LUMEN_CHECK(parse_one(edited, b, d));
    LUMEN_CHECK(a.focus.mode == cfg::focus_mode::click);
    LUMEN_CHECK(b.focus.mode == cfg::focus_mode::sloppy);
    LUMEN_CHECK(b.focus.raise_on_click == a.focus.raise_on_click);

    // Quoted strings keep their quoting and their escapes.
    const std::string quoted =
        cfg::splice_key(text, "focus", "mode", cfg::edit_value::string("us(intl)"), r);
    LUMEN_CHECK(quoted.find("mode = \"us(intl)\"") != std::string::npos);

    // Booleans and numbers.
    const std::string unchecked =
        cfg::splice_key(text, "focus", "raise-on-click", cfg::edit_value::boolean(false), r);
    LUMEN_CHECK(unchecked.find("raise-on-click = false") != std::string::npos);
    const std::string gapped =
        cfg::splice_key(text, "tiling", "gaps", cfg::edit_value::integer(16), r);
    LUMEN_CHECK(gapped.find("gaps = 16") != std::string::npos);
    const std::string ratio =
        cfg::splice_key(text, "tiling", "master-ratio", cfg::edit_value::number(0.6), r);
    LUMEN_CHECK(ratio.find("master-ratio = 0.6") != std::string::npos);

    // Splicing the same value again changes nothing (idempotence).
    const std::string again =
        cfg::splice_key(gapped, "tiling", "gaps", cfg::edit_value::integer(16), r);
    LUMEN_CHECK(again == gapped);
}

LUMEN_TEST(config_edit_adds_missing_keys_and_sections_and_removes_keys) {
    std::string text = "[focus]\nmode = \"click\"\n";
    cfg::edit_report r{};

    // A key the section lacks lands under its header, marked.
    const std::string added =
        cfg::splice_key(text, "focus", "raise-on-click", cfg::edit_value::boolean(false), r);
    LUMEN_CHECK(r.status == cfg::edit_status::ok);
    const std::size_t header = added.find("[focus]");
    const std::size_t marker = added.find("# set by lumen-settings");
    const std::size_t key = added.find("raise-on-click = false");
    LUMEN_CHECK(header != std::string::npos && marker != std::string::npos &&
                key != std::string::npos && header < marker && marker < key);
    LUMEN_CHECK(added.find("mode = \"click\"") != std::string::npos);

    // A section the file lacks appends at the end.
    const std::string sectioned =
        cfg::splice_key(added, "cursor", "size", cfg::edit_value::integer(32), r);
    const std::size_t cursor = sectioned.find("[cursor]");
    LUMEN_CHECK(cursor != std::string::npos && cursor > added.size() - 2);
    LUMEN_CHECK(sectioned.find("size = 32") != std::string::npos);
    LUMEN_CHECK(sectioned.find("mode = \"click\"") != std::string::npos);

    // Adding the same key again replaces the line it added (no growth).
    const std::string again =
        cfg::splice_key(sectioned, "cursor", "size", cfg::edit_value::integer(48), r);
    LUMEN_CHECK(again.find("size = 48") != std::string::npos);
    LUMEN_CHECK(again.find("size = 32") == std::string::npos);
    LUMEN_CHECK(again.size() == sectioned.size());

    // Removal takes the line away and leaves the rest.
    const std::string removed = cfg::remove_key(again, "cursor", "size", r);
    LUMEN_CHECK(r.status == cfg::edit_status::ok && r.line > 0);
    LUMEN_CHECK(removed.find("size") == std::string::npos);
    LUMEN_CHECK(removed.find("[cursor]") != std::string::npos); // the user's header stays
    LUMEN_CHECK(removed.find("mode = \"click\"") != std::string::npos);
    // splice's marker goes with its line — exactly one remains (the [focus] one, whose
    // key stands).
    std::size_t markers = 0;
    for (std::size_t at = removed.find("# set by lumen-settings"); at != std::string::npos;
         at = removed.find("# set by lumen-settings", at + 1))
        ++markers;
    LUMEN_CHECK(markers == 1);
    // Removing what is not there changes nothing.
    const std::string unchanged = cfg::remove_key(removed, "cursor", "size", r);
    LUMEN_CHECK(r.line == 0 && unchanged == removed);

    // A file without a trailing newline still appends.
    const std::string no_newline = "focus_mode = 0"; // a stray top-level key, no newline
    const std::string appended =
        cfg::splice_key(no_newline, "idle", "policy", cfg::edit_value::word("none"), r);
    LUMEN_CHECK(appended.find("focus_mode = 0\n") != std::string::npos);
    LUMEN_CHECK(appended.find("[idle]") != std::string::npos);
    LUMEN_CHECK(appended.find("policy = \"none\"") != std::string::npos);
}

LUMEN_TEST(config_edit_refuses_bad_values_and_array_sections) {
    const std::string text = "[focus]\nmode = \"click\"\n";
    cfg::edit_report r{};
    // A bare word must be a word.
    const std::string bad = cfg::splice_key(text, "focus", "mode",
                                            cfg::edit_value::word("not a word!"), r);
    LUMEN_CHECK(r.status == cfg::edit_status::bad_word && bad == text);
    // The array-of-table sections are not splices.
    const std::string arr = cfg::splice_key(text, "input.device", "tap",
                                            cfg::edit_value::boolean(true), r);
    LUMEN_CHECK(r.status == cfg::edit_status::array_section && arr == text);
}

LUMEN_TEST(config_edit_over_the_documented_defaults_changes_only_its_keys) {
    // The corpus: the shipped example, comments and all.
    std::ifstream file{std::string(LUMEN_SOURCE_DIR) + "/deploy/lumen/config.toml"};
    if (!file) {
        std::printf("      (deploy/lumen/config.toml not found; skipped)\n");
        return;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    LUMEN_CHECK(text.size() > 1000);

    cfg::config_snapshot base{};
    cfg::diagnostics d{};
    LUMEN_CHECK(parse_one(text, base, d));

    cfg::edit_report r{};
    std::string edited =
        cfg::splice_key(text, "tiling", "layout", cfg::edit_value::word("master-stack"), r);
    edited = cfg::splice_key(edited, "input.keyboard", "repeat-rate",
                             cfg::edit_value::integer(40), r);
    edited = cfg::splice_key(edited, "input.touchpad", "natural-scroll",
                             cfg::edit_value::boolean(false), r);
    edited = cfg::splice_key(edited, "notifications", "position",
                             cfg::edit_value::word("bottom-left"), r);

    cfg::config_snapshot after{};
    LUMEN_CHECK(parse_one(edited, after, d));
    LUMEN_CHECK(d.errors() == 0);

    // Exactly the four keys moved.
    LUMEN_CHECK(after.tiling.layout == cfg::tiling_layout::master_stack &&
                base.tiling.layout == cfg::tiling_layout::floating);
    LUMEN_CHECK(after.keyboard.repeat_rate == 40 && base.keyboard.repeat_rate == 25);
    LUMEN_CHECK(after.touchpad.natural_scroll == false && base.touchpad.natural_scroll == true);
    LUMEN_CHECK(after.notifications.position ==
                    cfg::notification_position::bottom_left &&
                base.notifications.position == cfg::notification_position::top_right);
    // And a spot-check of untouched groups across the file.
    LUMEN_CHECK(after.tiling.gaps == base.tiling.gaps);
    LUMEN_CHECK(after.focus.mode == base.focus.mode);
    LUMEN_CHECK(after.workspaces.count == base.workspaces.count);
    LUMEN_CHECK(after.idle.blank_after == base.idle.blank_after);
    LUMEN_CHECK(after.bindings.size() == base.bindings.size());
    LUMEN_CHECK(after.default_bindings == base.default_bindings);
}

LUMEN_TEST(config_edit_string_lists_and_the_round_trip_through_the_parser) {
    std::string text = "[workspaces]\nmode = \"static\"\ncount = 3\n";
    cfg::edit_report r{};
    const std::string_view names[] = {"main", "web", "comms"};
    const std::string edited = cfg::splice_key(text, "workspaces", "names",
                                               cfg::edit_value::string_list(names), r);
    LUMEN_CHECK(r.status == cfg::edit_status::ok);
    LUMEN_CHECK(edited.find("names = [\"main\", \"web\", \"comms\"]") != std::string::npos);
    cfg::config_snapshot s{};
    cfg::diagnostics d{};
    LUMEN_CHECK(parse_one(edited, s, d));
    LUMEN_CHECK(s.workspaces.name_count == 3);
    LUMEN_CHECK(std::string_view(s.workspaces.names[0]) == "main");
    LUMEN_CHECK(std::string_view(s.workspaces.names[2]) == "comms");
}

int main(int argc, char** argv) {
    const int status = lumen_test::run_all(argc, argv);
    return status;
}
