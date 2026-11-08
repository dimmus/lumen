// ADR-013 Phase B's gate: the settings surface renders every schema key, the search
// reaches each of them, the rail switches panes, and the value controls edit the session's
// snapshot. The catalog totality is the mechanical half of §5.5's section→pane matrix.
#include "lumen_test.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.a11y;
import lx.config;
import lx.runtime;
import lx.scene;
import lx.ui;
import lumen.settings_ui;

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace lx::ui;

struct harness {
    lx::runtime::memory_arena arena{512 * 1024};
    ui_root root{arena};
    std::unique_ptr<lx::scene::draw_list> list = std::make_unique<lx::scene::draw_list>();

    harness() {
        ui_damage().clear();
        root.set_describer(&settings::describe);
        root.set_bounds({0, 0, 960, 680});
        LUMEN_CHECK(root.tick(*list));
    }
    bool tick() { return root.tick(*list); }
};

[[nodiscard]] ui_node* find(ui_node* n, lx::a11y::role role, const char* name) {
    if (!n)
        return nullptr;
    const lx::a11y::node a = n->accessibility_node();
    if (a.role_ == role && (!name || (a.name && std::strcmp(a.name, name) == 0)))
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ui_node* hit = find(n->child_at(i), role, name))
            return hit;
    return nullptr;
}

[[nodiscard]] ui_node* find_key(ui_node* n, element_key key) {
    if (!n)
        return nullptr;
    if (n->key() == key)
        return n;
    for (unsigned i = 0; i < n->child_count(); ++i)
        if (ui_node* hit = find_key(n->child_at(i), key))
            return hit;
    return nullptr;
}

[[maybe_unused]] [[nodiscard]] control_node& ctl(ui_node* n) {
    LUMEN_CHECK(n && n->type() == control_node::static_type());
    return *static_cast<control_node*>(n);
}

[[nodiscard]] lx::point2i centre(const ui_node& n) {
    const lx::rect2i b = n.bounds();
    return {b.x + b.width / 2, b.y + b.height / 2};
}

[[nodiscard]] std::string lower(std::string_view s) {
    std::string out{s};
    for (char& c : out)
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

} // namespace

LUMEN_TEST(settings_catalog_covers_every_schema_key_and_the_search_reaches_it) {
    const std::vector<settings::row_ref>& rows = settings::catalog();
    LUMEN_CHECK(rows.size() > 60); // the schema's ~70 keys are in

    // Totality over the non-pointer schema: every descriptor appears exactly once.
    for (const lx::config::key_desc& d : lx::config::schema_keys()) {
        unsigned hits = 0;
        for (const settings::row_ref& r : rows) {
            if (r.desc != &d)
                continue;
            ++hits;
            LUMEN_CHECK(r.label && r.label[0]);
            LUMEN_CHECK(r.group && r.group[0]);
            // The search text carries the section, the key and the label; the key alone
            // (lower case) is a subsequence of it.
            LUMEN_CHECK(settings::row_matches(r, lower(d.key)));
        }
        if (hits != 1)
            std::printf("      %s.%s: %u rows\n", d.section, d.key, hits);
        LUMEN_CHECK(hits == 1);
    }
    // The pointer sections: every accepted key appears once per class that takes it.
    for (const lx::config::pointer_key_desc& p : lx::config::pointer_keys()) {
        unsigned hits = 0;
        for (const settings::row_ref& r : rows) {
            if (r.pdesc != &p)
                continue;
            ++hits;
            LUMEN_CHECK(settings::row_matches(r, lower(p.key)));
        }
        if (hits != 1) // exactly one class shows every key except the masked ones
            LUMEN_CHECK(hits > 0);
    }
    // Every owner pane is one of the eight.
    for (const settings::row_ref& r : rows)
        LUMEN_CHECK(static_cast<unsigned>(r.owner) < settings::k_pane_count);

    // Every string key edits its own buffer: a slot each, none shared (a schema past the
    // session's cap gets k_no_row — read-only — not another row's buffer).
    unsigned slot_mask = 0;
    for (unsigned i = 0; i < rows.size(); ++i) {
        if (!rows[i].desc || rows[i].desc->kind != lx::config::value_kind::string)
            continue;
        const unsigned slot = settings::string_slot(i);
        if (slot == settings::session::k_no_row || slot >= settings::session::k_max_string_rows)
            continue;
        const unsigned bit = 1u << slot;
        if (slot_mask & bit)
            std::printf("      row %u reuses slot %u\n", i, slot);
        LUMEN_CHECK(!(slot_mask & bit));
        slot_mask |= bit;
    }
    LUMEN_CHECK(slot_mask != 0); // the schema's string keys all have buffers
}

LUMEN_TEST(settings_window_renders_rail_panes_and_switches_between_them) {
    settings::session s{};
    settings::set_active_session(&s);
    harness h{};
    h.root.set_describer(&settings::describe);
    ui_node* root = h.root.tree();
    LUMEN_CHECK(root);

    // The sidebar: a button per pane, the current pane's category the expanded one (Zed's
    // list highlights only the focused group leaf — never the category).
    for (unsigned i = 0; i < settings::k_pane_count; ++i) {
        ui_node* b = find(root, lx::a11y::role::button,
                          std::string(settings::k_pane_names[i]).c_str());
        LUMEN_CHECK(b);
        const bool expanded = (b->accessibility_node().state &
                               static_cast<unsigned>(lx::a11y::state_flag::expanded)) != 0u;
        LUMEN_CHECK(expanded == (i == 0));
    }
    // The search field and the content exist; the content carries rows (Appearance's).
    LUMEN_CHECK(find_key(root, settings::k_search_key));
    LUMEN_CHECK(find_key(root, settings::k_content_key));
    const unsigned rows_before = [&] {
        unsigned n = 0;
        for (const settings::row_ref& r : settings::catalog())
            if (r.owner == s.current())
                ++n;
        return n;
    }();
    LUMEN_CHECK(rows_before > 0);

    // Switch to the Input pane through the rail button (a click activates it).
    ui_node* input_btn = find(root, lx::a11y::role::button, "Input");
    LUMEN_CHECK(input_btn);
    LUMEN_CHECK(h.root.pointer_move(centre(*input_btn)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(s.current() == settings::pane_id::input);
    LUMEN_CHECK(h.tick());
    // The touchpad rows now render (their value controls carry the section's keys).
    unsigned input_rows = 0;
    for (unsigned i = 0; i < settings::catalog().size(); ++i) {
        const settings::row_ref& r = settings::catalog()[i];
        if (r.owner != settings::pane_id::input)
            continue;
        ++input_rows;
        LUMEN_CHECK(find_key(root, settings::k_row_control_base + i) != nullptr);
    }
    LUMEN_CHECK(input_rows > 6); // the keyboard's eight, at least
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_value_controls_edit_the_session_snapshot) {
    settings::session s{};
    settings::set_active_session(&s);
    harness h{};
    s.root = &h.root;
    ui_node* root = h.root.tree();

    // Every control of the current pane exists and is the kind its value kind implies.
    const auto& rows = settings::catalog();
    for (unsigned i = 0; i < rows.size(); ++i) {
        const settings::row_ref& r = rows[i];
        if (r.owner != s.current())
            continue;
        ui_node* c = find_key(root, settings::k_row_control_base + i);
        LUMEN_CHECK(c);
        const lx::config::value_kind kind = r.desc ? r.desc->kind : r.pdesc->kind;
        switch (kind) {
        case lx::config::value_kind::boolean:
            LUMEN_CHECK(find(root, lx::a11y::role::switch_control, r.label) != nullptr);
            break;
        case lx::config::value_kind::enumeration:
            LUMEN_CHECK(r.desc->name_count > 0);
            break;
        default:
            break;
        }
    }

    // A boolean edit: find a switch in the tree, click it, the snapshot flips.
    bool flipped = false;
    for (unsigned i = 0; i < rows.size() && !flipped; ++i) {
        const settings::row_ref& r = rows[i];
        if (r.owner != settings::pane_id::appearance && r.owner != settings::pane_id::idle)
            continue;
        if (!r.desc || r.desc->kind != lx::config::value_kind::boolean)
            continue;
        ui_node* sw = find(root, lx::a11y::role::switch_control, r.label);
        if (!sw)
            continue;
        void* f = r.desc->field(s.config());
        const bool before = *static_cast<bool*>(f);
        LUMEN_CHECK(h.root.pointer_move(centre(*sw)));
        LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
        LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
        const bool after = *static_cast<bool*>(f);
        LUMEN_CHECK(after != before);
        // No writable user layer in a test environment: the edit stays in memory.
        LUMEN_CHECK(s.state() == settings::session::save_state::memory_only);
        flipped = true;
    }
    LUMEN_CHECK(flipped);
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_search_filters_rows_across_panes) {
    settings::session s{};
    settings::set_active_session(&s);
    harness h{};
    s.root = &h.root;
    ui_node* root = h.root.tree();

    // Type into the search buffer: the pane's rows collapse to the matches, labelled by
    // their pane.
    LUMEN_CHECK(s.query_buffer().assign("tap"));
    s.refresh();
    LUMEN_CHECK(h.tick());
    unsigned visible = 0;
    for (unsigned i = 0; i < settings::catalog().size(); ++i) {
        const settings::row_ref& r = settings::catalog()[i];
        const bool should = settings::row_matches(r, "tap");
        if (find_key(root, settings::k_row_control_base + i))
            ++visible;
        LUMEN_CHECK(should || find_key(root, settings::k_row_control_base + i) == nullptr);
    }
    LUMEN_CHECK(visible > 0); // subsequence rules: the touchpad's rows plus any t-a-p text
    // Nothing matches a query no row's search text carries.
    LUMEN_CHECK(s.query_buffer().assign("zzqq"));
    s.refresh();
    LUMEN_CHECK(h.tick());
    unsigned none = 0;
    for (unsigned i = 0; i < settings::catalog().size(); ++i)
        if (find_key(root, settings::k_row_control_base + i))
            ++none;
    LUMEN_CHECK(none == 0);

    // Zed's clear button is there only while there is a query, and clears it.
    ui_node* clear = find(root, lx::a11y::role::button, "Clear");
    LUMEN_CHECK(clear);
    LUMEN_CHECK(h.root.pointer_move(centre(*clear)));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, true));
    LUMEN_CHECK(h.root.pointer_button(mouse_button::left, false));
    LUMEN_CHECK(s.query_buffer().size == 0);
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(find(h.root.tree(), lx::a11y::role::button, "Clear") == nullptr);
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_bindings_editor_validates_conflicts_and_writes) {
    // A real temp file: the editor's writes land and re-parse.
    char path[] = "/tmp/lumen-settings-bindings-XXXXXX";
    const int fd = ::mkstemp(path);
    LUMEN_CHECK(fd >= 0);
    ::close(fd);
    std::filesystem::remove(path);

    settings::session s{};
    s.force_user_file(path);
    settings::set_active_session(&s);
    harness h{};
    s.root = &h.root;
    h.root.set_describer(&settings::describe);

    // The Shortcuts pane lists every action a key can be given — all but `none`, the
    // unbinding sentinel (a row's editor unbinds with an empty field).
    s.set_current(settings::pane_id::shortcuts);
    LUMEN_CHECK(h.tick());
    unsigned listed = 0;
    unsigned bindable = 0;
    for (unsigned i = 0; i < lx::config::actions().size(); ++i) {
        const bool row = find_key(h.root.tree(), settings::k_row_control_base + i) != nullptr;
        const bool sentinel = lx::config::actions()[i].owner == lx::config::action_owner::none;
        LUMEN_CHECK(row != sentinel);
        listed += row ? 1u : 0u;
        bindable += sentinel ? 0u : 1u;
    }
    LUMEN_CHECK(listed == bindable && bindable > 0);

    // Bind window.close to Super+Y: the file gains the line, the snapshot takes it.
    const auto& all = lx::config::actions();
    unsigned close_idx = all.size();
    for (unsigned i = 0; i < all.size(); ++i)
        if (std::strcmp(all[i].name, "window.close") == 0)
            close_idx = i;
    LUMEN_CHECK(close_idx < all.size());
    s.begin_edit_action(close_idx);
    s.begin_binding_capture();
    LUMEN_CHECK(s.capture_binding_key('y', lx::config::mod_super, true));
    s.stop_binding_capture();
    LUMEN_CHECK(s.edit_buffer().view() == "Super+y");
    s.refresh();
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(s.edit_error().empty());
    LUMEN_CHECK(find(h.root.tree(), lx::a11y::role::button, "Record shortcut"));
    s.apply_edit_action();
    if (!s.edit_error().empty())
        std::printf("      apply said: %s\n", std::string(s.edit_error()).c_str());
    LUMEN_CHECK(s.edit_error().empty());

    {
        std::ifstream f{path};
        std::stringstream buf;
        buf << f.rdbuf();
        const std::string text = buf.str();
        // format_combo's canonical spelling lowercases the key; the editor writes it.
        LUMEN_CHECK(text.find("\"Super+y\" = \"window.close\"") != std::string::npos);
    }
    bool found = false;
    for (const lx::config::binding& b : s.config().bindings)
        if (b.action == all[close_idx].id && b.layer != lx::config::k_no_layer)
            found = true;
    LUMEN_CHECK(found);

    // Reserved keys refuse with an inline error; the file stands.
    s.begin_edit_action(close_idx);
    LUMEN_CHECK(s.edit_buffer().assign("Ctrl+Alt+F1"));
    s.refresh();
    (void)h.tick();
    s.apply_edit_action();
    LUMEN_CHECK(!s.edit_error().empty());
    {
        std::ifstream f{path};
        std::stringstream buf;
        buf << f.rdbuf();
        LUMEN_CHECK(buf.str().find("Ctrl+Alt+F1") == std::string::npos);
    }

    // A conflict: another action on the taken combo is refused.
    unsigned swap_idx = all.size();
    for (unsigned i = 0; i < all.size(); ++i)
        if (std::strcmp(all[i].name, "window.minimize") == 0)
            swap_idx = i;
    LUMEN_CHECK(swap_idx < all.size());
    s.begin_edit_action(swap_idx);
    s.begin_binding_capture();
    LUMEN_CHECK(s.capture_binding_key('y', lx::config::mod_super, true));
    s.stop_binding_capture();
    LUMEN_CHECK(s.edit_buffer().view() == "Super+y");
    s.refresh();
    (void)h.tick();
    s.apply_edit_action();
    LUMEN_CHECK(!s.edit_error().empty());

    // Reset: the user line goes (the binding falls back to the default).
    s.reset_binding(close_idx);
    {
        std::ifstream f{path};
        std::stringstream buf;
        buf << f.rdbuf();
        LUMEN_CHECK(buf.str().find("Super+y") == std::string::npos);
    }
    std::filesystem::remove(path);
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_session_loads_the_located_layers_and_writes_them) {
    // The app's own wiring (main constructs the session with load_files): the located
    // layers seed the snapshot, and an edit splices the user layer, verifies and renames
    // it — the write path ADR-013 Phase C claims, through the session the app really builds.
    char dir[] = "/tmp/lumen-settings-load-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir) != nullptr);
    const std::string file = std::string(dir) + "/config.toml";
    {
        std::ofstream out{file};
        out << "[focus]\nmode = \"sloppy\"\n";
    }
    LUMEN_CHECK(::setenv("LUMEN_CONFIG", file.c_str(), 1) == 0);
    settings::session s{true};
    LUMEN_CHECK(s.config().focus.mode == lx::config::focus_mode::sloppy); // the file's value
    LUMEN_CHECK(s.config().focus.raise_on_click); // an unset key: the default

    // Flip a boolean: the file gains the line, the snapshot re-reads it.
    const std::vector<settings::row_ref>& rows = settings::catalog();
    unsigned raise = settings::session::k_no_row;
    for (unsigned i = 0; i < rows.size(); ++i)
        if (rows[i].desc && std::strcmp(rows[i].desc->section, "focus") == 0 &&
            std::strcmp(rows[i].desc->key, "raise-on-click") == 0)
            raise = i;
    LUMEN_CHECK(raise != settings::session::k_no_row);
    s.set_bool(raise, false);
    LUMEN_CHECK(s.state() == settings::session::save_state::written);
    LUMEN_CHECK(!s.config().focus.raise_on_click);
    LUMEN_CHECK(s.config().focus.mode == lx::config::focus_mode::sloppy); // still the file's
    {
        std::ifstream f{file};
        std::stringstream buf;
        buf << f.rdbuf();
        LUMEN_CHECK(buf.str().find("raise-on-click = false") != std::string::npos);
        LUMEN_CHECK(buf.str().find("mode = \"sloppy\"") != std::string::npos);
    }

    ::unsetenv("LUMEN_CONFIG");
    std::filesystem::remove_all(dir);
}

LUMEN_TEST(settings_clock_rows_are_searchable_and_persist_to_user_layer) {
    char dir[] = "/tmp/lumen-settings-clock-XXXXXX";
    LUMEN_CHECK(::mkdtemp(dir) != nullptr);
    const std::string file = std::string(dir) + "/config.toml";
    {
        std::ofstream out{file};
        out << "[clock]\nformat = \"24h\"\nshow-seconds = false\n"
               "show-date = true\nshow-weekday = true\nfirst-day = \"monday\"\n"
               "week-numbers = false\n";
    }
    LUMEN_CHECK(::setenv("LUMEN_CONFIG", file.c_str(), 1) == 0);
    settings::session s{true};
    const auto& rows = settings::catalog();
    const auto row_of = [&](const char* key) {
        for (unsigned i = 0; i < rows.size(); ++i)
            if (rows[i].desc && std::strcmp(rows[i].desc->section, "clock") == 0 &&
                std::strcmp(rows[i].desc->key, key) == 0)
                return i;
        return settings::session::k_no_row;
    };
    const unsigned format = row_of("format");
    const unsigned seconds = row_of("show-seconds");
    const unsigned first_day = row_of("first-day");
    const unsigned weeks = row_of("week-numbers");
    LUMEN_CHECK(format != settings::session::k_no_row && seconds != settings::session::k_no_row &&
                first_day != settings::session::k_no_row && weeks != settings::session::k_no_row);
    const auto search_hits = [&](std::string_view term) {
        unsigned hits = 0;
        for (const settings::row_ref& row : rows)
            if (row.desc && std::strcmp(row.desc->section, "clock") == 0 &&
                settings::row_matches(row, term))
                ++hits;
        return hits;
    };
    for (const char* term : {"clock", "format", "seconds", "weekday", "calendar", "week"})
        LUMEN_CHECK(search_hits(term) > 0);
    for (unsigned i = 0; i < rows.size(); ++i) {
        if (!rows[i].desc || std::strcmp(rows[i].desc->section, "clock") != 0)
            continue;
        LUMEN_CHECK(rows[i].owner == settings::pane_id::appearance);
        LUMEN_CHECK(std::string_view(rows[i].group) == "Clock");
        LUMEN_CHECK(rows[i].label && rows[i].description);
    }
    LUMEN_CHECK(s.config().clock.format == lx::config::clock_format::h24);
    s.set_enum(format, 1);
    s.set_bool(seconds, true);
    s.set_enum(first_day, 1);
    s.set_bool(weeks, true);
    LUMEN_CHECK(s.state() == settings::session::save_state::written);
    LUMEN_CHECK(s.config().clock.format == lx::config::clock_format::h12 &&
                s.config().clock.show_seconds &&
                s.config().clock.first_day == lx::config::clock_first_day::sunday &&
                s.config().clock.week_numbers);
    {
        std::ifstream in{file};
        std::stringstream contents;
        contents << in.rdbuf();
        const std::string text = contents.str();
        LUMEN_CHECK(text.find("format = \"12h\"") != std::string::npos);
        LUMEN_CHECK(text.find("show-seconds = true") != std::string::npos);
        LUMEN_CHECK(text.find("first-day = \"sunday\"") != std::string::npos);
        LUMEN_CHECK(text.find("week-numbers = true") != std::string::npos);
    }
    ::unsetenv("LUMEN_CONFIG");
    std::filesystem::remove_all(dir);
}

LUMEN_TEST(settings_string_rows_carry_examples_and_refuse_blank_spec) {
    settings::session s{};
    settings::set_active_session(&s);
    harness h{};
    s.root = &h.root;
    h.root.set_describer(&settings::describe);
    LUMEN_CHECK(h.tick());

    // Every string row's field is there by name (its placeholder names what belongs in
    // the empty field) — pane by pane, only the current one renders.
    const auto& rows = settings::catalog();
    unsigned strings = 0;
    for (unsigned pane = 0; pane < settings::k_pane_count; ++pane) {
        s.set_current(static_cast<settings::pane_id>(pane));
        invalidate_layout(*h.root.tree());
        LUMEN_CHECK(h.tick());
        for (unsigned i = 0; i < rows.size(); ++i) {
            if (rows[i].owner != s.current() || !rows[i].desc ||
                rows[i].desc->kind != lx::config::value_kind::string)
                continue;
            LUMEN_CHECK(find(h.root.tree(), lx::a11y::role::text_field, rows[i].label) != nullptr);
            ++strings;
        }
    }
    LUMEN_CHECK(strings >= 8);

    // A blank commit where "" is not a value the key takes (the background's spec)
    // refills the field with the running value and writes nothing.
    for (unsigned i = 0; i < rows.size(); ++i) {
        if (!rows[i].desc || std::strcmp(rows[i].desc->section, "background") != 0 ||
            std::strcmp(rows[i].desc->key, "spec") != 0)
            continue;
        const unsigned slot = settings::string_slot(i);
        LUMEN_CHECK(slot != settings::session::k_no_row);
        s.string_buffer(slot).assign("");
        s.commit_string(i);
        LUMEN_CHECK(std::string_view(s.string_buffer(slot).storage.data()) ==
                    std::string_view("lumen"));
        LUMEN_CHECK(s.state() != settings::session::save_state::written);
        break;
    }
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_labels_survive_the_catalog_build) {
    // The derived labels were interned in a growing vector of short strings: each one's
    // characters lived inline (the small-string buffer), so the next push moved them and
    // every earlier label dangled — the Tiling layout row read "ile", the [ui] appearance
    // row a fragment of freed memory. Every label is printable now, capitalised, and these
    // two read what their keys say.
    const auto& rows = settings::catalog();
    unsigned checked = 0;
    for (const settings::row_ref& r : rows) {
        LUMEN_CHECK(r.label && r.label[0] >= 'A' && r.label[0] <= 'Z');
        for (const char* c = r.label; *c; ++c)
            LUMEN_CHECK(*c >= 0x20 && *c < 0x7f);
        if (r.desc && std::strcmp(r.desc->section, "tiling") == 0 &&
            std::strcmp(r.desc->key, "layout") == 0) {
            LUMEN_CHECK(std::string_view(r.label) == "Layout");
            ++checked;
        }
        if (r.desc && std::strcmp(r.desc->section, "ui") == 0 &&
            std::strcmp(r.desc->key, "appearance") == 0) {
            LUMEN_CHECK(std::string_view(r.label) == "Appearance");
            ++checked;
        }
    }
    LUMEN_CHECK(checked == 2);
}

LUMEN_TEST(settings_number_keys_step_on_their_grid_and_clamp) {
    settings::session s{};
    settings::set_active_session(&s);
    const auto& rows = settings::catalog();
    const auto row_of = [&](const char* section, const char* key) {
        for (unsigned i = 0; i < rows.size(); ++i)
            if (rows[i].desc && std::strcmp(rows[i].desc->section, section) == 0 &&
                std::strcmp(rows[i].desc->key, key) == 0)
                return i;
        return settings::session::k_no_row;
    };

    // The master ratio (0.1..0.9) steps by 0.05 on a spinner. The slider it replaced had
    // `min(hi − lo, 120)` steps — one step here — so any touch wrote 0.1 or 0.9.
    const unsigned ratio = row_of("tiling", "master-ratio");
    LUMEN_CHECK(ratio != settings::session::k_no_row);
    const unsigned slot = settings::string_slot(ratio);
    LUMEN_CHECK(slot != settings::session::k_no_row);
    double& v = *static_cast<double*>(rows[ratio].desc->field(s.config()));
    s.set_number(ratio, 0.55);
    s.step_number(ratio, +1);
    LUMEN_CHECK(std::abs(v - 0.60) < 1e-9);
    LUMEN_CHECK(std::string_view(s.string_buffer(slot).storage.data()) == "0.60");
    s.step_number(ratio, -1);
    s.step_number(ratio, -1);
    LUMEN_CHECK(std::abs(v - 0.50) < 1e-9);
    for (int k = 0; k < 40; ++k)
        s.step_number(ratio, +1);
    LUMEN_CHECK(std::abs(v - 0.9) < 1e-9);

    // Typed text commits (rounded onto the step's grid); junk puts the value back.
    s.string_buffer(slot).assign("0.31");
    s.commit_number(ratio);
    LUMEN_CHECK(std::abs(v - 0.30) < 1e-9);
    s.string_buffer(slot).assign("abc");
    s.commit_number(ratio);
    LUMEN_CHECK(std::abs(v - 0.30) < 1e-9);
    LUMEN_CHECK(std::string_view(s.string_buffer(slot).storage.data()) == "0.30");

    // A large number prints whole, never as an exponent (`%.2g` showed 800 as "8e+02").
    const unsigned stiffness = row_of("animations", "stiffness");
    LUMEN_CHECK(stiffness != settings::session::k_no_row);
    s.set_number(stiffness, 800.0);
    LUMEN_CHECK(std::string_view(
                    s.string_buffer(settings::string_slot(stiffness)).storage.data()) == "800");

    // An integer typed past `int` clamps to the key's range instead of wrapping
    // (4294967336 cast to int is 40).
    const unsigned history = row_of("notifications", "history");
    LUMEN_CHECK(history != settings::session::k_no_row);
    s.string_buffer(settings::string_slot(history)).assign("4294967336");
    s.commit_int(history);
    LUMEN_CHECK(*static_cast<int*>(rows[history].desc->field(s.config())) ==
                static_cast<int>(rows[history].desc->hi));
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_shortcuts_show_the_bindings_lumen_runs) {
    // Without a config file the session holds Lumen's defaults, and a default binding is
    // what the Shortcuts row shows: the launcher on Super. An unbound action reads as
    // unbound — the old test took a `combo{}` (kind `key`) for a binding and printed
    // keysym 0's name, "NoSymbol", on every row.
    settings::session s{};
    const auto all = lx::config::actions();
    const lx::config::action_info* launcher = lx::config::find_action("shell.launcher");
    LUMEN_CHECK(launcher != nullptr);
    const lx::config::combo c =
        s.binding_of_action(static_cast<unsigned>(launcher - all.data()));
    LUMEN_CHECK(!settings::combo_empty(c));
    char text[64]{};
    lx::config::format_combo(c, text, sizeof text);
    LUMEN_CHECK(std::string_view(text) == "Super");

    unsigned unbound = 0;
    for (unsigned i = 0; i < all.size(); ++i) {
        bool has_default = false;
        for (const lx::config::default_binding& d : lx::config::default_bindings())
            has_default = has_default || d.action == all[i].id;
        if (has_default || all[i].owner == lx::config::action_owner::none)
            continue;
        LUMEN_CHECK(settings::combo_empty(s.binding_of_action(i)));
        ++unbound;
    }
    LUMEN_CHECK(unbound > 0);
}

LUMEN_TEST(settings_ctrl_shift_e_moves_the_focus_between_nav_and_page) {
    // Zed's `ToggleFocusNav`, the shortcut the sidebar's footer names.
    settings::session s{};
    settings::set_active_session(&s);
    harness h{};
    s.root = &h.root;
    key_event e{};
    e.keycode = 18; // KEY_E
    e.codepoint = U'E';
    e.pressed = true;
    e.modifiers = k_mod_ctrl | k_mod_shift;
    const auto inside = [](const ui_node* n, element_key k) {
        for (; n; n = n->parent())
            if (n->key() == k)
                return true;
        return false;
    };

    // Nothing focused: to the open page's entry in the navigation.
    LUMEN_CHECK(h.root.key(e));
    const ui_node* f = h.root.focus().focused();
    LUMEN_CHECK(f && f->key() ==
                         settings::k_nav_key_base + static_cast<unsigned>(s.current()));
    LUMEN_CHECK(h.tick());

    // From the navigation: to the page's first control.
    LUMEN_CHECK(h.root.key(e));
    f = h.root.focus().focused();
    LUMEN_CHECK(f && inside(f, settings::k_page_key) && !inside(f, settings::k_rail_key));
    LUMEN_CHECK(h.tick());

    // And back.
    LUMEN_CHECK(h.root.key(e));
    f = h.root.focus().focused();
    LUMEN_CHECK(f && inside(f, settings::k_rail_key));

    // Ctrl+E alone is not the shortcut.
    key_event plain = e;
    plain.modifiers = k_mod_ctrl;
    const ui_node* before = h.root.focus().focused();
    (void)h.root.key(plain);
    LUMEN_CHECK(h.root.focus().focused() == before);
    settings::set_active_session(nullptr);
}

LUMEN_TEST(settings_shortcut_capture_records_keys_and_modifier_taps) {
    settings::session s{};
    const auto actions = lx::config::actions();
    const auto* action = lx::config::find_action("window.close");
    LUMEN_CHECK(action);
    const unsigned index = static_cast<unsigned>(action - actions.data());
    LUMEN_CHECK(!s.capture_binding_key('a', 0, true));
    s.begin_edit_action(index);
    LUMEN_CHECK(!s.capture_binding_key('a', 0, true)); // explicit opt-in
    s.begin_binding_capture();
    const auto check = [&](std::string_view expected) {
        LUMEN_CHECK(s.edit_buffer().view() == expected);
    };
    // Modifiers are config bits, independent from lock state and text/UTF-8.
    LUMEN_CHECK(s.capture_binding_key('1', lx::config::mod_shift, true));
    check("Shift+1");
    LUMEN_CHECK(s.capture_binding_key('1', 0, false));
    check("Shift+1");
    LUMEN_CHECK(s.capture_binding_key('2', 0, true, true));
    check("Shift+1"); // repeat does not overwrite
    LUMEN_CHECK(s.capture_binding_key(0xffe5, 0, true)); // Caps Lock
    LUMEN_CHECK(s.capture_binding_key(0, 0, true)); // no keymap
    check("Shift+1");
    LUMEN_CHECK(s.capture_binding_key(0xffeb, lx::config::mod_super, true));
    check("Shift+1"); // modifier press waits for a tap or a chord
    // Wayland modifiers commonly follow the release: Super can still be set.
    LUMEN_CHECK(s.capture_binding_key(0xffeb, lx::config::mod_super, false));
    check("Super");
    LUMEN_CHECK(s.capture_binding_key(0xffeb, lx::config::mod_super, true));
    LUMEN_CHECK(s.capture_binding_key('y', lx::config::mod_super | lx::config::mod_shift, true));
    LUMEN_CHECK(s.capture_binding_key(0xffeb, 0, false));
    check("Super+Shift+y"); // release cannot replace the chord with a tap
    s.stop_binding_capture();
    LUMEN_CHECK(!s.capture_binding_key('z', 0, true));
    check("Super+Shift+y");
    s.begin_binding_capture();
    s.set_current(settings::pane_id::appearance);
    LUMEN_CHECK(!s.capturing_binding());
}

LUMEN_TEST(settings_shortcut_capture_cancel_clear_and_reserved) {
    settings::session s{};
    const auto* action = lx::config::find_action("window.close");
    LUMEN_CHECK(action);
    const unsigned index = static_cast<unsigned>(action - lx::config::actions().data());
    s.begin_edit_action(index);
    s.begin_binding_capture();
    LUMEN_CHECK(s.capture_binding_key(lx::config::keysym::f1,
                                     lx::config::mod_ctrl | lx::config::mod_alt, true));
    LUMEN_CHECK(s.edit_buffer().view() == "Ctrl+Alt+F1");
    LUMEN_CHECK(!s.edit_error().empty());
    LUMEN_CHECK(s.state() != settings::session::save_state::written);
    s.apply_edit_action(); // capture uses the existing reserved-key validation on save
    LUMEN_CHECK(!s.edit_error().empty());
    LUMEN_CHECK(s.editing_action() == index);
    LUMEN_CHECK(!s.capturing_binding());
    s.begin_binding_capture();
    LUMEN_CHECK(s.capture_binding_key(lx::config::keysym::backspace, 0, true));
    LUMEN_CHECK(s.edit_buffer().view().empty());
    LUMEN_CHECK(s.edit_error().empty());
    LUMEN_CHECK(s.capture_binding_key(lx::config::keysym::backspace,
                                     lx::config::mod_ctrl, true));
    LUMEN_CHECK(s.edit_buffer().view() == "Ctrl+BackSpace");
    LUMEN_CHECK(s.capture_binding_key(lx::config::keysym::escape,
                                     lx::config::mod_super, true));
    LUMEN_CHECK(!s.edit_error().empty()); // modified Escape is reserved, not cancel
    LUMEN_CHECK(s.editing_action() == index);
    LUMEN_CHECK(s.capture_binding_key(lx::config::keysym::escape, 0, true));
    LUMEN_CHECK(s.editing_action() == settings::session::k_no_row);
    LUMEN_CHECK(!s.capturing_binding());
    LUMEN_CHECK(s.state() != settings::session::save_state::written);
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const int status = lumen_test::run_all(argc, argv);
    settings::set_active_session(nullptr);
    return status;
}
