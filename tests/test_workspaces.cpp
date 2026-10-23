// The workspace model (design-session-and-wm Part W, `:workspaces`): dynamic invariants (one
// trailing empty workspace, an empty inactive one goes), static counts and names, index
// bindings past the end, neighbours, focus memory, hotplug — an output that goes hands its
// active workspace's windows to the fallback's active one and appends the rest (static:
// merged by position), each remembering its place, and they come back with the output —
// assign, and stranded workspaces when no output is left. Pure: windows are counted by a fake
// sink here, as the window manager's records count them in the compositor.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.compositor;

using namespace lx::compositor;

namespace {

/// The windows of each workspace, as the window manager's records would hold them.
struct fake_windows {
    std::map<unsigned long long, unsigned> on; // workspace id → windows
    unsigned moved_calls = 0;
    std::vector<std::tuple<unsigned long long, int, int>> workspace_moves;

    workspace_sink sink() {
        workspace_sink s{};
        s.user = this;
        s.windows_moved = [](void* u, lx::workspace_id from, lx::workspace_id to) {
            auto* f = static_cast<fake_windows*>(u);
            f->on[to.id()] += f->on[from.id()];
            f->on.erase(from.id());
            ++f->moved_calls;
        };
        s.workspace_moved = [](void* u, lx::workspace_id ws, int from, int to) {
            static_cast<fake_windows*>(u)->workspace_moves.emplace_back(ws.id(), from, to);
        };
        return s;
    }
    void add(workspace_model& m, lx::workspace_id ws) {
        ++on[ws.id()];
        m.window_added(ws);
    }
    void remove(workspace_model& m, lx::workspace_id ws) {
        if (on[ws.id()] > 0)
            --on[ws.id()];
        m.window_removed(ws);
    }
};

workspace_config dynamic_config() { return {}; }

workspace_config static_config(unsigned count) {
    workspace_config c{};
    c.dynamic = false;
    c.count = count;
    return c;
}

/// Names of output `slot`'s workspaces, "*" after the active one: "1* 2 3".
std::string names(const workspace_model& m, unsigned slot) {
    std::string out;
    const workspace_group& g = m.group(slot);
    for (unsigned i = 0; i < g.count; ++i) {
        const workspace_record* w = m.find(g.order[i]);
        if (!w)
            continue;
        if (!out.empty())
            out += ' ';
        out += w->name;
        if (w->active)
            out += '*';
    }
    return out;
}

void add_output(workspace_model& m, fake_windows& f, unsigned slot, const char* identifier) {
    m.output_added(slot, 100 + slot, identifier, slot == 0 ? "A-1" : "B-1", f.sink());
    m.settle(slot);
}

} // namespace

LUMEN_TEST(dynamic_starts_with_one_empty_workspace) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    LUMEN_CHECK(names(m, 0) == "1*");
    LUMEN_CHECK(m.group(0).serial != 0 && m.count() == 1);
    // A window on it: a new empty one follows.
    f.add(m, m.active(0));
    LUMEN_CHECK(names(m, 0) == "1* 2");
    // The empty last one can be switched to, and another window there makes a third.
    const lx::workspace_id two = m.by_index(0, 2);
    LUMEN_CHECK(m.activate(two));
    LUMEN_CHECK(names(m, 0) == "1 2*");
    f.add(m, two);
    LUMEN_CHECK(names(m, 0) == "1 2* 3");
    // Index past the end: the last (empty) one, as in niri.
    LUMEN_CHECK(m.by_index(0, 9) == m.group(0).order[2]);
    LUMEN_CHECK(m.by_index(0, 0) == lx::workspace_id{});
}

LUMEN_TEST(dynamic_removes_an_empty_workspace_when_it_is_left) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    const lx::workspace_id one = m.active(0);
    f.add(m, one);
    const lx::workspace_id two = m.by_index(0, 2);
    m.activate(two);
    f.add(m, two);
    LUMEN_CHECK(names(m, 0) == "1 2* 3");
    // Its window closes while it is active: it stays (the user is on it).
    f.remove(m, two);
    LUMEN_CHECK(names(m, 0) == "1 2* 3");
    // Left: it goes, and the trailing empty one is renamed by position.
    m.activate(one);
    LUMEN_CHECK(names(m, 0) == "1* 2");
    LUMEN_CHECK(m.find(two) == nullptr);
    // A window on an inactive workspace that closes takes the workspace with it.
    const lx::workspace_id last = m.by_index(0, 2);
    f.add(m, last); // as if a window were moved there without following
    LUMEN_CHECK(names(m, 0) == "1* 2 3");
    f.remove(m, last);
    LUMEN_CHECK(names(m, 0) == "1* 2");
}

LUMEN_TEST(neighbours_do_not_wrap) {
    workspace_model m;
    fake_windows f;
    m.configure(static_config(3), f.sink());
    add_output(m, f, 0, "mon-a");
    LUMEN_CHECK(names(m, 0) == "1* 2 3");
    const lx::workspace_id one = m.by_index(0, 1);
    const lx::workspace_id three = m.by_index(0, 3);
    LUMEN_CHECK(m.neighbour(one, -1) == lx::workspace_id{});
    LUMEN_CHECK(m.neighbour(one, 1) == m.by_index(0, 2));
    LUMEN_CHECK(m.neighbour(three, 1) == lx::workspace_id{});
    // Static: past the count is nothing.
    LUMEN_CHECK(m.by_index(0, 4) == lx::workspace_id{});
}

LUMEN_TEST(static_count_names_and_reconfigure) {
    workspace_model m;
    fake_windows f;
    workspace_config c = static_config(4);
    std::snprintf(c.names[0], sizeof(c.names[0]), "web");
    std::snprintf(c.names[1], sizeof(c.names[1]), "mail");
    c.name_count = 2;
    m.configure(c, f.sink());
    add_output(m, f, 0, "mon-a");
    LUMEN_CHECK(names(m, 0) == "web* mail 3 4");
    LUMEN_CHECK(m.find(m.by_index(0, 1))->named && !m.find(m.by_index(0, 3))->named);
    // Empty static workspaces stay.
    m.activate(m.by_index(0, 4));
    m.activate(m.by_index(0, 1));
    LUMEN_CHECK(names(m, 0) == "web* mail 3 4");
    // Windows on 3 and 4, 4 active; down to 2: theirs go to the last kept, which is active.
    f.add(m, m.by_index(0, 3));
    f.add(m, m.by_index(0, 4));
    f.add(m, m.by_index(0, 4));
    const lx::workspace_id mail = m.by_index(0, 2);
    m.activate(m.by_index(0, 4));
    c.count = 2;
    m.configure(c, f.sink());
    LUMEN_CHECK(names(m, 0) == "web mail*");
    LUMEN_CHECK(m.find(mail)->windows == 3 && f.on[mail.id()] == 3);
    // Back to dynamic: the empty inactive one goes, an empty trailing one appears, names are
    // positions.
    m.configure(dynamic_config(), f.sink());
    LUMEN_CHECK(names(m, 0) == "1* 2" && m.by_index(0, 1) == mail);
}

LUMEN_TEST(focus_memory_and_urgent) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    const lx::workspace_id one = m.active(0);
    m.set_focus_memory(one, lx::toplevel_id{7});
    LUMEN_CHECK(m.focus_memory(one) == lx::toplevel_id{7});
    m.forget_window(lx::toplevel_id{7});
    LUMEN_CHECK(!m.focus_memory(one));
    const unsigned g0 = m.generation();
    m.set_urgent(one, true);
    LUMEN_CHECK(m.find(one)->urgent && m.generation() != g0);
}

LUMEN_TEST(hotplug_dynamic_merges_the_active_one_and_appends_the_rest) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    add_output(m, f, 1, "mon-b");
    // B: 1 (2 windows), 2 (1 window, active), 3 (1 window), 4 empty.
    const lx::workspace_id b1 = m.active(1);
    f.add(m, b1);
    f.add(m, b1);
    const lx::workspace_id b2 = m.by_index(1, 2);
    f.add(m, b2);
    const lx::workspace_id b3 = m.by_index(1, 3);
    f.add(m, b3);
    m.activate(b2);
    LUMEN_CHECK(names(m, 1) == "1 2* 3 4");
    // A: 1 (1 window, active), 2 empty.
    const lx::workspace_id a1 = m.active(0);
    f.add(m, a1);
    LUMEN_CHECK(names(m, 0) == "1* 2");

    m.output_removed(1, 0, f.sink());
    LUMEN_CHECK(!m.has_group(1));
    // B's active one's window joined what A shows; B's 1 and 3 were appended (before the
    // trailing empty one), remembering B and their places.
    LUMEN_CHECK(m.find(b2) == nullptr && f.on[a1.id()] == 2 && m.find(a1)->windows == 2);
    LUMEN_CHECK(names(m, 0) == "1* 2 3 4");
    const workspace_record* r1 = m.find(b1);
    const workspace_record* r3 = m.find(b3);
    LUMEN_CHECK(r1 && r3 && r1->output == 0 && r3->output == 0);
    LUMEN_CHECK(std::strcmp(r1->home, "mon-b") == 0 && r1->home_index == 1);
    LUMEN_CHECK(std::strcmp(r3->home, "mon-b") == 0 && r3->home_index == 3);

    // B comes back: its workspaces go home in their order; the merged window asks for its
    // old position (2), which is made between them; the one that was active (2) is again.
    m.output_added(1, 200, "mon-b", "B-1", f.sink());
    const lx::workspace_id back2 = m.home_workspace(1, 2);
    LUMEN_CHECK(back2 && back2 != b1 && back2 != b3);
    // The window goes there (the window manager moves its record).
    f.remove(m, a1);
    f.add(m, back2);
    m.settle(1);
    LUMEN_CHECK(names(m, 1) == "1 2* 3 4");
    LUMEN_CHECK(m.by_index(1, 1) == b1 && m.by_index(1, 2) == back2 && m.by_index(1, 3) == b3);
    LUMEN_CHECK(m.find(b1)->home[0] == '\0');
    LUMEN_CHECK(names(m, 0) == "1* 2");
    // Their windows went with them (the sink was told).
    bool moved_b1 = false;
    for (const auto& [id, from, to] : f.workspace_moves)
        if (id == b1.id() && from == 0 && to == 1)
            moved_b1 = true;
    LUMEN_CHECK(moved_b1);
}

LUMEN_TEST(hotplug_static_merges_by_position) {
    workspace_model m;
    fake_windows f;
    m.configure(static_config(3), f.sink());
    add_output(m, f, 0, "mon-a");
    add_output(m, f, 1, "mon-b");
    const lx::workspace_id b1 = m.by_index(1, 1);
    const lx::workspace_id b3 = m.by_index(1, 3);
    f.add(m, b1);
    f.add(m, b3);
    m.activate(b3);
    const lx::workspace_id a1 = m.by_index(0, 1);
    const lx::workspace_id a3 = m.by_index(0, 3);
    m.output_removed(1, 0, f.sink());
    // B's active 3 → A's active 1; B's 1 → A's 1 too (same position).
    LUMEN_CHECK(f.on[a1.id()] == 2 && f.on[a3.id()] == 0);
    LUMEN_CHECK(names(m, 0) == "1* 2 3");
    // Back: three fresh ones; a returning window asks for its position, active is 3 again.
    m.output_added(1, 300, "mon-b", "B-1", f.sink());
    const lx::workspace_id home3 = m.home_workspace(1, 3);
    LUMEN_CHECK(home3 == m.by_index(1, 3));
    m.settle(1);
    LUMEN_CHECK(names(m, 1) == "1 2 3*");
}

LUMEN_TEST(stranded_workspaces_wait_for_the_next_output) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    const lx::workspace_id one = m.active(0);
    f.add(m, one);
    const lx::workspace_id two = m.by_index(0, 2);
    f.add(m, two);
    m.activate(two);
    m.output_removed(0, -1, f.sink());
    LUMEN_CHECK(!m.has_group(0));
    // Still there, still flagged active (what was shown stays shown), on no output.
    LUMEN_CHECK(m.find(one) && m.find(one)->output == -1 && m.is_active(two));
    add_output(m, f, 2, "mon-c");
    LUMEN_CHECK(names(m, 2) == "1 2* 3");
    LUMEN_CHECK(m.by_index(2, 1) == one && m.by_index(2, 2) == two);
}

// Two outputs of 10 go at once (undocking with the lid shut): 20 wait, 16 fit on the next
// output. The other 4 are merged into the last; walking the stranded list by index while
// merge() shifted it left every other one in no group, its windows unreachable.
LUMEN_TEST(stranded_workspaces_past_a_full_output_all_merge) {
    workspace_model m;
    fake_windows f;
    m.configure(static_config(10), f.sink());
    add_output(m, f, 0, "mon-a");
    add_output(m, f, 1, "mon-b");
    for (unsigned slot = 0; slot < 2; ++slot)
        for (unsigned i = 1; i <= 10; ++i)
            f.add(m, m.by_index(slot, i));
    const lx::workspace_id shown = m.by_index(1, 9);
    m.activate(shown);
    m.output_removed(0, -1, f.sink());
    m.output_removed(1, -1, f.sink());
    add_output(m, f, 2, "mon-c");
    const workspace_group& g = m.group(2);
    LUMEN_CHECK(g.count == k_workspaces_per_output);
    LUMEN_CHECK(m.count() == k_workspaces_per_output);
    // Every window is on a workspace of the new output, 20 in all, 5 on the last.
    unsigned total = 0;
    for (const auto& [ws, n] : f.on) {
        bool listed = false;
        for (unsigned i = 0; i < g.count; ++i)
            listed = listed || g.order[i].id() == ws;
        LUMEN_CHECK(listed || n == 0);
        total += n;
    }
    LUMEN_CHECK(total == 20);
    LUMEN_CHECK(f.on[g.order[g.count - 1].id()] == 5);
    // The one shown was among those merged: the last one is shown now, and only it.
    unsigned active = 0;
    for (unsigned i = 0; i < g.count; ++i)
        active += m.is_active(g.order[i]) ? 1u : 0u;
    LUMEN_CHECK(active == 1 && m.is_active(g.order[g.count - 1]));
}

LUMEN_TEST(assign_moves_a_workspace_with_its_windows) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    add_output(m, f, 1, "mon-b");
    const lx::workspace_id a1 = m.active(0);
    f.add(m, a1);
    const lx::workspace_id a2 = m.by_index(0, 2);
    f.add(m, a2);
    m.activate(a2);
    LUMEN_CHECK(names(m, 0) == "1 2* 3");
    LUMEN_CHECK(m.assign(a2, 1, f.sink()));
    // A shows its previous one now; B has it before its trailing empty one, not active.
    LUMEN_CHECK(names(m, 0) == "1* 2");
    LUMEN_CHECK(names(m, 1) == "1* 2 3" && m.by_index(1, 2) == a2 && !m.is_active(a2));
    LUMEN_CHECK(f.workspace_moves.size() == 1 &&
                std::get<0>(f.workspace_moves[0]) == a2.id() &&
                std::get<1>(f.workspace_moves[0]) == 0 && std::get<2>(f.workspace_moves[0]) == 1);
    // To where it is, or nowhere: nothing.
    LUMEN_CHECK(!m.assign(a2, 1, f.sink()));
    LUMEN_CHECK(!m.assign(a2, 7, f.sink()));
}

LUMEN_TEST(ids_are_never_reused) {
    workspace_model m;
    fake_windows f;
    m.configure(dynamic_config(), f.sink());
    add_output(m, f, 0, "mon-a");
    std::set<unsigned long long> seen;
    const lx::workspace_id one = m.active(0);
    f.add(m, one);
    for (int round = 0; round < 50; ++round) {
        // The trailing one gets a window and loses it, and is left: it goes, and the new
        // trailing one has a new id.
        const lx::workspace_id two = m.by_index(0, 2);
        seen.insert(two.id());
        m.activate(two);
        f.add(m, two);
        f.remove(m, two);
        m.activate(one);
        LUMEN_CHECK(m.find(two) == nullptr);
    }
    LUMEN_CHECK(seen.size() == 50);
    LUMEN_CHECK(m.count() == 2);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
