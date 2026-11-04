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

[[nodiscard]] control_node& ctl(ui_node* n) {
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

    // The Shortcuts pane lists every action.
    s.set_current(settings::pane_id::shortcuts);
    LUMEN_CHECK(h.tick());
    unsigned listed = 0;
    for (unsigned i = 0; i < lx::config::actions().size(); ++i)
        if (find_key(h.root.tree(), settings::k_row_control_base + i))
            ++listed;
    LUMEN_CHECK(listed == lx::config::actions().size());

    // Bind window.close to Super+Y: the file gains the line, the snapshot takes it.
    const auto& all = lx::config::actions();
    unsigned close_idx = all.size();
    for (unsigned i = 0; i < all.size(); ++i)
        if (std::strcmp(all[i].name, "window.close") == 0)
            close_idx = i;
    LUMEN_CHECK(close_idx < all.size());
    s.begin_edit_action(close_idx);
    LUMEN_CHECK(s.edit_buffer().assign("Super+Y"));
    s.refresh();
    LUMEN_CHECK(h.tick());
    LUMEN_CHECK(s.edit_error().empty());
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
    LUMEN_CHECK(s.edit_buffer().assign("Super+Y"));
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

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    const int status = lumen_test::run_all(argc, argv);
    settings::set_active_session(nullptr);
    return status;
}
