// Overflow caps (AGENTS.md §5: "Overflow caps: use `overflow_action` — never silent
// unbounded growth on fixed arrays"; docs/invariants.md, the "Overflow caps" row). Every
// fixed-capacity structure of the engine that a test can reach through an exported API is
// driven past its capacity here, under each `overflow_action` that API lets a caller choose,
// and checked for the three things a cap owes:
//   (i)   the count stays at the capacity, however many further pushes come — never growth;
//   (ii)  the action's visible outcome — `reject` refuses the push, `drop_newest` ignores the
//         newest, `drop_oldest` evicts the oldest and takes the newest, `warn_and_drop` is
//         `drop_newest` plus one warning (counted where a counter exists, and on the log);
//   (iii) where the policy is a parameter, changing it changes the outcome.
//
// A structure whose policy is fixed offers only its own action: the outcome checked is that
// one's. What a structure documents for itself decides which actions apply to it — where the
// partition's contract names fewer than the enum's four, only those are pinned.
//
// Two things this test found are reported by its last case and pinned only as "no growth":
// a site whose code departs from its own documented contract, and one whose action the
// partition never names. Nothing here is bent to fit them. (A third — text_input's
// drop_oldest refusing like reject — was fixed to the enum's semantics; its case now pins
// all four actions.)
//
// The last case also prints every `overflow_action::` site under modules/ that this test
// does not drive, by name, with what it would take, and holds that report to the tree: a
// module file that names an action and that no case drives and the report does not name
// fails it.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.trace;
import lx.gfx;
import lx.scene;
import lx.text;
import lx.ui;

namespace {

using lx::runtime::overflow_action;

// Past the capacity, this many further pushes: a cap that grew "a little" would show.
constexpr unsigned k_further = 64;

constexpr overflow_action k_actions[]{overflow_action::reject, overflow_action::drop_newest,
                                      overflow_action::drop_oldest, overflow_action::warn_and_drop};

const char* name(overflow_action action) {
    switch (action) {
    case overflow_action::reject:
        return "reject";
    case overflow_action::drop_newest:
        return "drop_newest";
    case overflow_action::drop_oldest:
        return "drop_oldest";
    case overflow_action::warn_and_drop:
        return "warn_and_drop";
    }
    return "?";
}

/// `LUMEN_CHECK` repeats its expression; these checks say which structure, action and row.
void expect(bool ok, const std::string& what) {
    if (!ok)
        throw lumen_test::assertion_error(what);
}

std::string where(overflow_action action, const char* what) {
    return std::format("{} under {}", what, name(action));
}

// ── What this test drove, observed and could not ────────────────────────────────────────

/// The sites driven past their capacity.
std::vector<std::string>& driven() {
    static std::vector<std::string> sites;
    return sites;
}
/// A structure departing from its action: reported, never pinned either way.
std::vector<std::string>& observed() {
    static std::vector<std::string> sites;
    return sites;
}
/// The sites no case can reach.
std::vector<std::string>& not_driven() {
    static std::vector<std::string> sites;
    return sites;
}

void drove(const char* site) {
    driven().push_back(site);
    std::println("    drove   {}", site);
}

/// A row that needs a face: without one the text caches cannot be filled.
bool skip_without_face(bool have, const char* site) {
    if (have)
        return false;
    std::println("    SKIP: {} — no font face on this host, nothing to put in the cache", site);
    not_driven().push_back(std::format("{} (SKIP: no font face on this host)", site));
    return true;
}

// ── The log ────────────────────────────────────────────────────────────────────────────

const char* name(lx::trace::level lvl) {
    switch (lvl) {
    case lx::trace::level::trace:
        return "trace";
    case lx::trace::level::debug:
        return "debug";
    case lx::trace::level::info:
        return "info";
    case lx::trace::level::warn:
        return "warn";
    case lx::trace::level::error:
        return "error";
    case lx::trace::level::critical:
        return "critical";
    }
    return "?";
}

/// What the categories log while it lives, from `warn` up whatever `LUMEN_LOG` says. The
/// sink is process-wide, so it is installed for the shortest scope and restored after.
struct log_capture {
    std::vector<std::string> lines;
    lx::trace::level saved_min = lx::trace::level::trace;

    log_capture() {
        auto& log = lx::trace::logger::global();
        saved_min = log.min_level();
        log.set_min_level(lx::trace::level::warn);
        log.set_sink(&on_line, this);
    }
    ~log_capture() {
        auto& log = lx::trace::logger::global();
        log.set_sink(nullptr);
        log.set_min_level(saved_min);
    }
    log_capture(const log_capture&) = delete;
    log_capture& operator=(const log_capture&) = delete;

    static void on_line(lx::trace::level lvl, const char* category, const char* message,
                        void* user) {
        static_cast<log_capture*>(user)->lines.push_back(
            std::format("{} {}: {}", name(lvl), category ? category : "", message ? message : ""));
    }
    /// Warning lines carrying `text`.
    std::size_t warns(std::string_view text) const {
        std::size_t n = 0;
        for (const std::string& line : lines)
            n += line.starts_with("warn ") && line.find(text) != std::string::npos ? 1u : 0u;
        return n;
    }
};

// ── lx.scene: the draw list ────────────────────────────────────────────────────────────

lx::scene::draw_command tagged(unsigned i) {
    lx::scene::draw_command cmd{};
    cmd.dst = {static_cast<int>(i), 0, 1, 1};
    return cmd;
}
/// The tag of the `i`-th draw.
unsigned tag_of(const lx::scene::draw_list& list, unsigned i) {
    return static_cast<unsigned>(list.data()[i].dst.x);
}

// ── lx.ui: the smallest node ───────────────────────────────────────────────────────────

/// A `ui_node` that is nothing but a type: enough to be a timer's or a trap's subject.
struct probe_node final : lx::ui::ui_node {
    [[nodiscard]] lx::ui::element_type_id type() const override {
        static const lx::ui::element_type_id id = lx::ui::next_element_type_id();
        return id;
    }
};

// A leaf box.
lx::ui::child describe_leaf(lx::ui::build_context&) {
    return lx::ui::div(lx::ui::style{}, {});
}

/// A list row that is a leaf box.
lx::ui::child bare_row(void*, const lx::ui::list_row&) {
    return lx::ui::div(lx::ui::style{}, {});
}

/// A root on its own context and arena, ticked once so it has a tree.
struct one_root {
    std::unique_ptr<lx::ui::ui_context> ctx = std::make_unique<lx::ui::ui_context>();
    std::unique_ptr<lx::runtime::memory_arena> arena =
        std::make_unique<lx::runtime::memory_arena>(256 * 1024);
    std::unique_ptr<lx::ui::ui_root> root = std::make_unique<lx::ui::ui_root>(*arena, *ctx);
    lx::scene::draw_list list;

    explicit one_root(lx::ui::describe_fn describer = &describe_leaf,
                      lx::rect2i bounds = {0, 0, 64, 64}) {
        root->set_describer(describer);
        root->set_bounds(bounds);
        expect(root->tick(list), "the root's first tick must produce a frame");
    }
};

/// The first node of `tree` of type `want`.
lx::ui::ui_node* find_type(lx::ui::ui_node* tree, lx::ui::element_type_id want) {
    if (!tree)
        return nullptr;
    if (tree->type() == want)
        return tree;
    for (unsigned i = 0; i < tree->child_count(); ++i)
        if (lx::ui::ui_node* hit = find_type(tree->child_at(i), want))
            return hit;
    return nullptr;
}

} // namespace

// ── lx.scene ───────────────────────────────────────────────────────────────────────────

// `draw_list::push` (snapshot_buffer.cppm:791-798): a draw past `k_max_draw_commands`.
LUMEN_TEST(overflow_draw_commands_each_action) {
    constexpr unsigned cap = lx::scene::k_max_draw_commands;
    for (const overflow_action action : k_actions) {
        lx::scene::draw_list list;
        {
            log_capture log;
            for (unsigned i = 0; i < cap + k_further; ++i)
                list.push(tagged(i), action);
            // Once per overflow, not once per push (ADR-010 §4.4).
            expect(log.warns("draw command cap reached") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the \"draw command cap reached\" warning must come exactly "
                                 "once under warn_and_drop and never otherwise"));
        }
        expect(list.size() == cap, where(action, "the draw count must stay at the capacity"));
        const unsigned oldest = tag_of(list, 0);
        const unsigned newest = tag_of(list, cap - 1);
        const unsigned drops = list.overflow_drops();
        switch (action) {
        case overflow_action::reject:
            // The push is refused: the list is exactly what it was when it filled.
            expect(oldest == 0 && newest == cap - 1,
                   where(action, "a refused draw must keep the oldest and drop the newest"));
            expect(drops == 0, where(action, "a refused draw is not a drop"));
            break;
        case overflow_action::drop_newest:
        case overflow_action::warn_and_drop:
            expect(oldest == 0 && newest == cap - 1,
                   where(action, "the newest draw must be the one dropped"));
            expect(drops == k_further, where(action, "every dropped draw must be counted"));
            break;
        case overflow_action::drop_oldest:
            expect(oldest == k_further && newest == cap + k_further - 1,
                   where(action, "the oldest draw must go and the newest be taken"));
            for (unsigned i = 0; i < cap; ++i)
                expect(tag_of(list, i) == k_further + i,
                       where(action, "the survivors must keep their order"));
            break;
        }
    }
    // The default action of every push overload is warn_and_drop (snapshot_buffer.cppm:124-173).
    {
        lx::scene::draw_list list;
        log_capture log;
        for (unsigned i = 0; i < cap + 1; ++i)
            list.push(tagged(i));
        expect(list.size() == cap && list.overflow_drops() == 1 &&
                   log.warns("draw command cap reached") == 1,
               "a push with no action given must behave as warn_and_drop");
    }
    drove("lx.scene/snapshot_buffer.cppm:791-798 draw_list::push (draw commands, all four "
          "actions + the default)");
}

// `draw_list::admit_primitive` (snapshot_buffer.cppm:825-842): a primitive past
// `gfx::k_max_primitives`. `drop_oldest` is not offered for instances — it drops too.
LUMEN_TEST(overflow_primitives_each_action) {
    constexpr unsigned cap = lx::gfx::k_max_primitives;
    for (const overflow_action action : k_actions) {
        lx::scene::draw_list list;
        unsigned taken = 0;
        {
            log_capture log;
            for (unsigned i = 0; i < cap + k_further; ++i)
                if (list.append_quad({static_cast<int>(i), 0, 1, 1}, i, action))
                    ++taken;
            expect(log.warns("primitive cap reached") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the \"primitive cap reached\" warning must come exactly "
                                 "once under warn_and_drop and never otherwise"));
        }
        expect(taken == cap, where(action, "only the first k_max_primitives may be stored"));
        expect(list.primitive_count() == cap,
               where(action, "the primitive count must stay at the capacity"));
        expect(list.overflow_drops() == (action == overflow_action::reject ? 0u : k_further),
               where(action, "every dropped primitive must be counted, a refused one not"));
    }
    {
        // The default again: the other primitive pushes take the same action argument.
        lx::scene::draw_list list;
        log_capture log;
        for (unsigned i = 0; i < cap + 1; ++i)
            list.push_quad(lx::gfx::quad_instance{}, {static_cast<int>(i), 0, 1, 1}, i);
        expect(list.primitive_count() == cap && list.overflow_drops() == 1 &&
                   log.warns("primitive cap reached") == 1,
               "a push_quad with no action given must behave as warn_and_drop");
    }
    drove("lx.scene/snapshot_buffer.cppm:825-842 draw_list::admit_primitive (primitives, all "
          "four actions + the default)");
}

// `container_node::add` (lx.scene.cppm:546-559): a child past the 128 inline slots. The
// policy is `set_child_overflow_policy` (drop_newest until set).
LUMEN_TEST(overflow_container_children_each_policy) {
    constexpr unsigned cap = 128; // container_node::children_ (lx.scene.cppm:232)
    auto kids = std::make_unique<lx::scene::container_node[]>(cap + k_further);
    for (const overflow_action action : k_actions) {
        lx::scene::container_node parent;
        expect(parent.child_overflow_policy() == overflow_action::drop_newest,
               "a container must start in drop_newest");
        parent.set_child_overflow_policy(action);
        for (unsigned i = 0; i < cap; ++i)
            expect(parent.add(&kids[i]),
                   where(action, "a child must be taken while there is room"));
        expect(parent.child_count() == cap,
               where(action, "the container must fill to its capacity"));

        unsigned taken = 0;
        std::size_t warns = 0;
        {
            log_capture log;
            for (unsigned i = cap; i < cap + k_further; ++i)
                taken += parent.add(&kids[i]) ? 1u : 0u;
            warns = log.warns("");
        }
        expect(parent.child_count() == cap,
               where(action, "the child count must stay at the capacity"));
        const bool evicts = action == overflow_action::drop_oldest;
        expect(taken == (evicts ? k_further : 0u),
               where(action, "only drop_oldest reports a full container's push as taken"));
        expect(parent.child_at(cap - 1) == (evicts ? &kids[cap + k_further - 1] : &kids[cap - 1]),
               where(action, "the newest child must be present under drop_oldest and only then"));
        expect(parent.child_at(0) == (evicts ? &kids[k_further] : &kids[0]),
               where(action, "the oldest child must be gone under drop_oldest and only then"));
        if (action == overflow_action::warn_and_drop && warns == 0)
            observed().push_back(
                "container_node::add under warn_and_drop refuses and logs nothing: the enum's "
                "warn_and_drop is \"Log warn, then drop_newest\" (lx.runtime/memory.cppm:43; "
                "lx.scene/lx.scene.cppm:546-559). Nothing documents the container's policy, and "
                "set_child_overflow_policy has no caller under modules/ or executables/");

        // (iii): the same full container, the policy changed, the same push.
        if (evicts) {
            parent.set_child_overflow_policy(overflow_action::reject);
            expect(!parent.add(&kids[cap + k_further]),
                   "a container switched to reject must refuse the push it was taking");
            expect(parent.child_at(cap - 1) == &kids[cap + k_further - 1],
                   "a refused push must leave the children as they were");
        } else {
            parent.set_child_overflow_policy(overflow_action::drop_oldest);
            expect(parent.add(&kids[cap + k_further]),
                   "a container switched to drop_oldest must take the push it was refusing");
            expect(parent.child_at(cap - 1) == &kids[cap + k_further] &&
                       parent.child_at(0) == &kids[1],
                   "the push a container takes after the switch must evict its oldest child");
        }
    }
    drove("lx.scene/lx.scene.cppm:546-559 container_node::add (children, all four policies)");
}

// ── lx.ui: the structures with a fixed policy ──────────────────────────────────────────

// `overlay_layer::add` (overlay.cppm:585-595) through `overlay_node::open`: an overlay past
// `k_capacity` stays closed and is counted. Fixed `reject`.
LUMEN_TEST(overflow_overlay_layer_rejects_past_capacity) {
    constexpr unsigned cap = lx::ui::overlay_layer::k_capacity;
    auto overlays = std::make_unique<lx::ui::overlay_node[]>(cap + k_further);
    lx::ui::overlay_layer& layer = lx::ui::ui_overlays();
    expect(layer.open_count() == 0, "the overlay layer must be empty before the case");
    const std::uint64_t rejected_before = layer.rejected();

    for (unsigned i = 0; i < cap; ++i)
        expect(overlays[i].open() && overlays[i].is_open(),
               "an overlay must open while the layer has room");
    expect(layer.open_count() == cap, "the overlay layer must fill to its capacity");

    unsigned opened = 0;
    for (unsigned i = cap; i < cap + k_further; ++i) {
        opened += overlays[i].open() ? 1u : 0u;
        expect(!overlays[i].is_open(), "an overlay the full layer refused must stay closed");
    }
    expect(opened == 0, "no overlay may open past the capacity");
    expect(layer.open_count() == cap, "the open count must stay at the capacity");
    expect(layer.rejected() - rejected_before == k_further,
           "every refused overlay must be counted");
    expect(layer.top() == &overlays[cap - 1], "the last overlay taken must be the top one");

    // The room one close makes goes to the next opener, and to no other.
    overlays[0].close();
    expect(layer.open_count() == cap - 1, "a closed overlay must leave its slot");
    expect(overlays[cap].open() && overlays[cap].is_open() && layer.top() == &overlays[cap],
           "the overlay the full layer refused must open once there is room");
    expect(layer.open_count() == cap && !overlays[cap + 1].open(), "the layer must be full again");

    for (unsigned i = 0; i < cap + k_further; ++i)
        overlays[i].close();
    expect(layer.open_count() == 0, "every overlay must be closed again at the end");
    drove("lx.ui/overlay.cppm:585-595 overlay_layer::add (open overlays, reject)");
}

// `focus_manager::trap` (focus.cppm:421-434): a trap past `k_max_traps` is refused and
// leaves the scope as it was. Fixed `reject`.
LUMEN_TEST(overflow_focus_traps_reject_past_capacity) {
    constexpr unsigned cap = lx::ui::focus_manager::k_max_traps;
    auto scopes = std::make_unique<probe_node[]>(cap + k_further);
    lx::ui::focus_manager focus;

    for (unsigned i = 0; i < cap; ++i)
        expect(focus.trap(scopes[i]), "a trap must be taken while there is room");
    for (unsigned i = cap; i < cap + k_further; ++i) {
        expect(!focus.trap(scopes[i]), "a trap past the capacity must be refused");
        expect(!scopes[i].has_flag(lx::ui::node_flag::focus_scope),
               "a refused trap must not mark its scope a focus scope");
    }
    expect(focus.active_trap() == &scopes[cap - 1],
           "the last trap taken must be the active one, however many were refused");

    // The room one release makes goes to the next trap, and to no other.
    focus.release(scopes[0]);
    expect(focus.active_trap() == &scopes[cap - 1], "releasing the bottom trap must keep the top");
    expect(focus.trap(scopes[cap]), "the trap that was refused must be taken once there is room");
    expect(focus.active_trap() == &scopes[cap] && !focus.trap(scopes[cap + 1]),
           "the trap stack must be full again");

    for (unsigned i = cap; i >= 1; --i)
        focus.release(scopes[i]);
    expect(focus.active_trap() == nullptr, "every trap must be released at the end");
    drove("lx.ui/focus.cppm:421-434 focus_manager::trap (focus traps, reject)");
}

// `timer_queue::schedule` (interact.cppm:381-396): a timer past `k_capacity` is refused and
// counted. Fixed `reject`.
LUMEN_TEST(overflow_timers_reject_past_capacity) {
    constexpr unsigned cap = lx::ui::timer_queue::k_capacity;
    auto node = std::make_unique<probe_node>();
    lx::ui::timer_queue timers;
    constexpr std::uint64_t k_ms = 1'000'000ull;
    const auto tag = [](unsigned i) { return lx::ui::k_timer_user + i; };

    for (unsigned i = 0; i < cap; ++i)
        expect(timers.schedule(*node, tag(i), k_ms), "a timer must be taken while there is room");
    expect(timers.size() == cap && timers.rejected() == 0,
           "the timer table must fill to its capacity");

    for (unsigned i = cap; i < cap + k_further; ++i)
        expect(!timers.schedule(*node, tag(i), k_ms), "a timer past the capacity must be refused");
    expect(timers.size() == cap, "the timer count must stay at the capacity");
    expect(timers.rejected() == k_further, "every refused timer must be counted");

    // The same (node, tag) again is that timer moved, not another entry.
    expect(timers.schedule(*node, tag(5), 5 * k_ms), "a timer already set must be moved");
    expect(timers.size() == cap && timers.rejected() == k_further,
           "moving a timer must take no slot and count nothing");

    // The room one cancel makes goes to the next timer.
    timers.cancel(*node, tag(5));
    expect(timers.size() == cap - 1, "a cancelled timer must leave its slot");
    expect(timers.schedule(*node, tag(cap), k_ms),
           "the timer that was refused must be taken once there is room");
    expect(timers.size() == cap && !timers.schedule(*node, tag(cap + 1), k_ms),
           "the timer table must be full again");

    node.reset();
    expect(timers.size() == 0, "a node that dies must take its timers with it");
    drove("lx.ui/interact.cppm:381-396 timer_queue::schedule (timers, reject)");
}

// The thread's root table (context.cppm:220-231) through `ui_root`'s construction: a root
// past `k_max_roots` (32) is not registered — its nodes' out-of-scope invalidations are
// dropped, never routed to another root — and it is counted, with one warning. Fixed
// `warn_and_drop`. `k_max_roots` and the counter are `lx::ui::detail`'s (not exported);
// `context_of` is what tells a registered root from one the table could not take.
LUMEN_TEST(overflow_root_table_drops_past_capacity) {
    constexpr unsigned cap = 32; // lx::ui::detail::k_max_roots (context.cppm:104)
    std::vector<std::unique_ptr<one_root>> roots;
    {
        log_capture log;
        for (unsigned i = 0; i < cap + k_further; ++i) {
            roots.push_back(std::make_unique<one_root>());
            const bool registered =
                lx::ui::context_of(*roots.back()->root->tree()) == roots.back()->ctx.get();
            expect(registered == (i < cap),
                   std::format("root {} must {} be registered", i, i < cap ? "be" : "not be"));
        }
        // Warned once, however many roots the table could not take.
        expect(log.warns("root table full") == 1,
               "the root table must warn exactly once, however many roots it drops");
    }
    // (i): the table did not grow for any of them.
    for (unsigned i = cap; i < cap + k_further; ++i)
        expect(lx::ui::context_of(*roots[i]->root->tree()) == nullptr,
               std::format("root {} must still be unregistered", i));
    expect(lx::ui::context_of(*roots[cap - 1]->root->tree()) == roots[cap - 1]->ctx.get(),
           "the last registered root must keep its slot");

    // The room one root going frees goes to the next root constructed, and to no other:
    // nothing re-registers a root the table had refused.
    roots[0].reset();
    roots.push_back(std::make_unique<one_root>());
    expect(lx::ui::context_of(*roots.back()->root->tree()) == roots.back()->ctx.get(),
           "a root constructed after one went must take the freed slot");
    expect(lx::ui::context_of(*roots[cap]->root->tree()) == nullptr,
           "a root the table refused must stay unregistered");

    // All gone: the thread is as it was.
    roots.clear();
    {
        one_root fresh;
        expect(lx::ui::context_of(*fresh.root->tree()) == fresh.ctx.get(),
               "a root must register again once the table is empty");
    }
    drove("lx.ui/context.cppm:220-231 detail::register_root (the root table, warn_and_drop)");
}

// `text_input_node::insert` (text_input.cppm:918-987): an insertion that does not fit the
// capacity. The partition's contract (text_input.cppm:70-79) names all four actions:
// `reject` refuses the insertion whole, `drop_newest` / `warn_and_drop` keep what fits,
// cut at a character boundary, and `drop_oldest` — the enum's "Evict oldest, accept new" —
// evicts from the front of the text, at a character boundary, never the new bytes;
// `rejected()` counts every insertion the capacity refused, cut, or made evict.
LUMEN_TEST(overflow_text_input_each_documented_action) {
    for (const overflow_action action : k_actions) {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 8;
        props.overflow = action;
        (void)field.apply(props); // its bool is "the layout changed", not success
        expect(field.capacity() == 8, where(action, "the capacity must be max_bytes"));
        expect(field.insert("abcdef") && field.text() == "abcdef" && field.rejected() == 0,
               where(action, "text within the capacity must be taken whole"));

        // Six bytes in, an insertion of four: two fit.
        bool taken = false;
        {
            log_capture log;
            taken = field.insert("XYZW");
            expect(log.warns("text_input: an insertion was cut to the capacity") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the cut warning must come exactly once under warn_and_drop"));
        }
        // (i): whatever the action, the text never outgrows the capacity, and nothing is
        // silent: an insertion that did not fit is counted.
        expect(field.text().size() <= field.capacity(),
               where(action, "the text must never outgrow the capacity"));
        expect(field.rejected() == 1,
               where(action, "an insertion that did not fit must be counted"));
        // (ii): the action's own outcome. Two of the six bytes fit, so drop_newest keeps
        // "XY"; drop_oldest takes all four by evicting the two oldest ("ab"), the new bytes
        // (the newest) landing before what stayed.
        const char* const kept = action == overflow_action::reject        ? "abcdef"
                                 : action == overflow_action::drop_oldest ? "cdefXYZW"
                                                                          : "abcdefXY";
        expect(taken == (action != overflow_action::reject) && field.text() == kept,
               where(action, "reject must refuse the insertion whole; drop_newest and "
                             "warn_and_drop keep what fits; drop_oldest evicts the oldest "
                             "bytes and takes the insertion"));
        if (action == overflow_action::drop_oldest)
            expect(field.caret() == field.text().size() && !field.has_selection(),
                   where(action, "the caret must sit after the taken insertion, selecting "
                                 "nothing"));
        // A good many further pushes: still no growth, and each action keeps its own text.
        for (unsigned i = 0; i < k_further; ++i)
            (void)field.insert("0123456789");
        expect(field.text().size() <= field.capacity(),
               where(action, "the text must never outgrow the capacity"));
        // An insertion larger than the whole capacity: everything else refused or ignored
        // it; drop_oldest emptied the field for it, and it alone holds the last push's head.
        expect(field.text() == (action == overflow_action::drop_oldest ? "01234567" : kept),
               where(action, "the further pushes must leave each action's own text"));

        // (iii): the same field, the policy changed, the same insertion answers the new one.
        // Full, the field refuses under either policy, so its text goes back to six bytes
        // first — over the selection, which is room whatever the policy is.
        {
            lx::ui::text_input_props other{};
            other.max_bytes = 8;
            other.overflow = action == overflow_action::reject ? overflow_action::drop_newest
                                                               : overflow_action::reject;
            (void)field.apply(other);
            field.select_all();
            expect(field.insert("abcdef") && field.text() == "abcdef",
                   "the same field must take its six bytes back over the selection");
            const bool takes = other.overflow == overflow_action::drop_newest;
            const bool taken = field.insert("XYZW");
            expect(
                taken == takes && field.text() == (takes ? "abcdefXY" : "abcdef"),
                std::format("the same field must answer the insertion under {} as that action says",
                            name(other.overflow)));
            // And changed to drop_oldest, the same insertion evicts for itself.
            if (action == overflow_action::drop_oldest) {
                lx::ui::text_input_props oldest{};
                oldest.max_bytes = 8;
                oldest.overflow = overflow_action::drop_oldest;
                (void)field.apply(oldest);
                expect(field.insert("XYZW") && field.text() == "cdefXYZW" &&
                           field.rejected() == 3 + k_further,
                       where(action, "the same field under drop_oldest must evict the oldest "
                                     "bytes for the insertion"));
            }
        }
    }
    // The cut is at a character boundary: "ä" is two bytes, only one fits.
    {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 4;
        props.overflow = overflow_action::drop_newest;
        (void)field.apply(props);
        expect(field.insert("ab") && field.insert("äö") && field.text() == "abä",
               "an insertion cut at the capacity must be cut at a character boundary");
    }
    // drop_oldest's eviction is at a character boundary too: "ä" is two bytes, and evicting
    // "two bytes' worth" from "aäbX" takes the whole of "aä" — what stays starts on one.
    {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 5;
        props.overflow = overflow_action::drop_oldest;
        (void)field.apply(props);
        expect(field.insert("aäbX") && field.insert("YZ") && field.text() == "bXYZ",
               "the eviction must take whole characters from the front, never split one");
    }
    // The eviction shifts what it did not evict — the selection math above the cap check
    // (selection_begin/end feed the room) must not leave the caret clamped to the end:
    // a selection mid-text is replaced, the oldest bytes before it go, and the caret lands
    // after the insertion, mid-text.
    {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 8;
        props.overflow = overflow_action::drop_oldest;
        (void)field.apply(props);
        expect(field.insert("abcdefgh"), "the field must take its eight bytes");
        field.set_selection(5, 7); // "fg"
        expect(field.insert("1234") && field.text() == "cde1234h",
               "evicting ahead of a replaced selection must keep the bytes after it");
        expect(field.caret() == 7 && field.anchor() == 7 && !field.has_selection(),
               "the caret must sit after the insertion, shifted with the text, not clamped");
    }
    // The bytes before the selection are the oldest and go first; when they cannot make
    // room alone, the eviction continues past the selection — never into the new bytes.
    {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 8;
        props.overflow = overflow_action::drop_oldest;
        (void)field.apply(props);
        expect(field.insert("abcdefgh"), "the field must take its eight bytes");
        field.set_selection(1, 1); // the caret alone, one byte in
        expect(field.insert("XYZ") && field.text() == "XYZdefgh" && field.caret() == 3,
               "the eviction must run from the front — the prefix, then the suffix's oldest "
               "bytes — and keep the newest of the old text after the insertion");
    }
    // A multi-byte character can straddle the eviction point: the bump to a character
    // boundary then evicts more than the insertion adds, and the suffix shift's
    // destination reaches back over the prefix shift's source. The prefix shift must run
    // first — with the suffix's copy ahead of it, a byte that stayed was silently turned
    // into its neighbour. Two shapes: "ä" (two bytes) with the caret mid-text, and a
    // three-byte "€" whose selection it evicts past is one byte.
    {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 9;
        props.overflow = overflow_action::drop_oldest;
        (void)field.apply(props);
        expect(field.insert("abcäefgh"), // "ä" is two bytes: nine in all
               "the field must take its nine bytes");
        field.set_selection(6, 6); // the caret mid-text, just right of the "ä"
        expect(field.insert("ZZZZ") && field.text() == "eZZZZfgh",
               "a two-byte character straddling the eviction point must leave what stays "
               "intact, not swap a byte for its neighbour");
    }
    {
        lx::ui::text_input_node field;
        lx::ui::text_input_props props{};
        props.max_bytes = 9;
        props.overflow = overflow_action::drop_oldest;
        (void)field.apply(props);
        expect(field.insert("ab€cdef"), // "€" is three bytes: nine in all
               "the field must take its nine bytes");
        field.set_selection(6, 7); // one byte, the "d"
        expect(field.insert("ZZZZ") && field.text() == "cZZZZef",
               "a three-byte character straddling the eviction point must not corrupt the "
               "prefix that stays");
    }
    drove("lx.ui/text_input.cppm:918-987 text_input_node::insert (all four policies)");
}

// A virtualised list's chunk slots (list.cppm:392, `k_list_max_chunks` = 32 of 16 rows): a
// viewport taller than 512 rows shows the first 512. drop_newest by construction.
LUMEN_TEST(overflow_virtualised_list_drops_rows_past_capacity) {
    constexpr unsigned k_rows = 600;
    const auto describe = [](lx::ui::build_context&) -> lx::ui::child {
        return lx::ui::list(k_rows, &bare_row).layout(lx::ui::style{}.size_full());
    };
    // 600 rows at the dense height would need about 600 / 16 = 38 chunk slots.
    one_root r{describe, {0, 0, 400, 24000}};
    const auto* list = static_cast<const lx::ui::list_node*>(
        find_type(r.root->tree(), lx::ui::list_node::static_type()));
    expect(list != nullptr, "the list must be in the tree");
    expect(list->count() == k_rows && list->virtualised(), "the list must be virtualised");
    expect(list->live_rows() == 32 * 16,
           "a viewport taller than 512 rows must still get only the first 512 rows");
    expect(list->row_node(511) != nullptr && list->row_node(512) == nullptr,
           "row 511 must exist and row 512 must not");
    expect(list->first_live_row() == 0, "the rows shown must be the first ones");
    drove("lx.ui/list.cppm:392 list_node::rebuild chunk slots (a virtualised list's rows, "
          "drop_newest)");
}

// ── lx.text: the caches with a policy ──────────────────────────────────────────────────

// `shape_cache` (shape.cppm:539-547, 575-594, 731-738): an entry table of four, full. Only
// `drop_oldest` makes room — it evicts the least recently used; every other action keeps the
// run out of the cache (`stored` false: shaped again each time).
LUMEN_TEST(overflow_shape_cache_each_policy) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id face = faces.resolve(lx::text::font_desc{});
    if (skip_without_face(face.valid(), "lx.text/shape.cppm:539-594,731-738 shape_cache"))
        return;
    const char* const runs[]{"run zero",  "run one",  "run two", "run three",
                             "run four",  "run five", "run six", "run seven",
                             "run eight", "run nine", "run ten", "run eleven"};
    for (const overflow_action action : k_actions) {
        lx::text::shape_cache_config cfg{};
        cfg.entries = 4;
        cfg.glyph_capacity = 4096;
        cfg.max_run_glyphs = 64;
        cfg.overflow = action;
        lx::text::shape_cache cache{cfg};
        const auto shape = [&](const char* text) {
            lx::text::shape_request req{};
            req.face = face;
            req.size_26_6 = 12 * 64;
            req.utf8 = text;
            return cache.shape(faces, req);
        };
        for (unsigned i = 0; i < 4; ++i)
            expect(shape(runs[i])->stored,
                   where(action, "a run must be stored while there is room"));
        expect(cache.counters().entries == 4, where(action, "the entry table must fill"));

        const bool evicts = action == overflow_action::drop_oldest;
        {
            log_capture log;
            for (unsigned i = 4; i < 8; ++i) {
                const auto run = shape(runs[i]);
                expect(run.has_value() && !run->glyphs.empty(),
                       where(action, "a run the cache cannot keep must still be shaped"));
                expect(run->stored == evicts,
                       where(action, "only drop_oldest may store a run once the table is full"));
            }
            expect(log.warns("shape cache full") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the full warning must come exactly once under warn_and_drop"));
        }
        const lx::text::shape_cache_counters c = cache.counters();
        expect(c.entries == 4, where(action, "the entry count must stay at the capacity"));
        expect(c.evictions == (evicts ? 4u : 0u) && c.overflows == (evicts ? 0u : 4u),
               where(action,
                     "drop_oldest must evict and every other action must count a kept-out run"));
        if (evicts)
            expect(!shape(runs[0])->hit && shape(runs[7])->hit,
                   where(action, "the least recently used run must be gone and the newest kept"));
        else
            expect(shape(runs[0])->hit && !shape(runs[4])->hit,
                   where(action, "the oldest runs must stay and the new ones never be kept"));
    }
    drove("lx.text/shape.cppm:539-594,731-738 shape_cache (all four policies)");
}

// `glyph_cache` (raster.cppm:413-426): one set of eight ways, full. Only `drop_oldest`
// replaces a way; every other action keeps the set and fails the glyph.
LUMEN_TEST(overflow_glyph_cache_each_policy) {
    const std::unique_ptr<lx::text::face_cache> faces = std::make_unique<lx::text::face_cache>();
    (void)faces->add_font_dir(LUMEN_TEST_FONT_DIR);
    const lx::text::face_id face = faces->resolve(lx::text::font_desc{});
    if (skip_without_face(face.valid(), "lx.text/raster.cppm:413-426 glyph_cache"))
        return;
    const std::unique_ptr<lx::text::shape_cache> shapes = std::make_unique<lx::text::shape_cache>();
    const std::unique_ptr<lx::gfx::atlas> atlas = std::make_unique<lx::gfx::atlas>();
    constexpr std::int32_t k_14px = 14 * 64;
    std::vector<std::uint32_t> ids;
    {
        lx::text::shape_request req{};
        req.face = face;
        req.size_26_6 = k_14px;
        req.utf8 = "abcdefghijkl";
        const auto run = shapes->shape(*faces, req);
        expect(run.has_value() && run->glyphs.size() == 12,
               "the sample text must shape to 12 glyphs");
        for (const lx::text::shaped_glyph& g : run->glyphs)
            ids.push_back(g.glyph);
    }

    for (const overflow_action action : k_actions) {
        lx::text::glyph_cache_config cfg{};
        cfg.entries = lx::text::k_glyph_cache_ways; // one set
        cfg.overflow = action;
        const std::unique_ptr<lx::text::glyph_cache> glyphs =
            std::make_unique<lx::text::glyph_cache>(*atlas, cfg);
        const auto key = [&](unsigned i) { return lx::text::glyph_key{face, k_14px, ids[i], 0}; };
        atlas->begin_frame(1);
        for (unsigned i = 0; i < 8; ++i)
            expect(glyphs->get(*faces, key(i)).has_value(),
                   where(action, "a glyph must be rasterised while the set has room"));
        expect(glyphs->counters().live == 8, where(action, "the set must fill"));

        const bool evicts = action == overflow_action::drop_oldest;
        {
            log_capture log;
            for (unsigned i = 8; i < 12; ++i) {
                const auto entry = glyphs->get(*faces, key(i));
                expect(entry.has_value() == evicts,
                       where(action, "only drop_oldest may store a glyph once the set is full"));
            }
            expect(log.warns("glyph_cache: set full") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the full warning must come exactly once under warn_and_drop"));
        }
        const lx::text::glyph_cache_counters c = glyphs->counters();
        expect(c.live == 8, where(action, "the live way count must stay at the capacity"));
        expect(c.evictions == (evicts ? 4u : 0u) && c.overflows == (evicts ? 0u : 4u),
               where(action,
                     "drop_oldest must evict and every other action must count a refused glyph"));
        if (evicts)
            expect(glyphs->find(key(0)) == nullptr && glyphs->find(key(11)) != nullptr,
                   where(action, "the least recently used glyph must be gone and the newest kept"));
        else
            expect(glyphs->find(key(0)) != nullptr && glyphs->find(key(8)) == nullptr,
                   where(action, "the oldest glyphs must stay and the new ones never be kept"));
    }
    drove("lx.text/raster.cppm:413-426 glyph_cache (all four policies)");
}

// `text_layout` (lines.cppm:790-797, 834-840, 933-938, 1246-1248, 1274-1278): the caps of
// `text_layout_limits`. The partition's contract (lines.cppm:51-57) names two outcomes for
// every cap: `reject` makes the pass fail with an empty layout; every other action keeps what
// fits and sets `truncated()`. All five caps keep it — the line cap on both of the wrap pass's
// paths, the shaped one and the estimate one a 0 pt font (no face resolves) takes.
LUMEN_TEST(overflow_text_layout_each_policy) {
    constexpr const char* k_five_lines = "one\ntwo\nthree\nfour\nfive";

    // The line cap with no face (the wrap pass's estimate path, lines.cppm:1246-1248): a 0 pt
    // font resolves none, so the same five paragraphs are laid out as estimates. This leg
    // needs no face by construction, so it runs before the probe's skip can end the case —
    // on a host without fonts it is the line cap's only coverage.
    for (const overflow_action action : k_actions) {
        lx::text::text_layout layout;
        lx::text::text_layout_limits lim{};
        lim.max_lines = 2;
        lim.overflow = action;
        layout.set_limits(lim);
        layout.set_font({.size_pt = 0.f});
        layout.set_text(k_five_lines);
        lx::result<void> updated;
        {
            log_capture log;
            updated = layout.update();
            expect(log.warns("text_layout: a storage cap cut the text") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the cut warning must come exactly once under warn_and_drop "
                                 "and never otherwise"));
        }
        if (action == overflow_action::reject)
            // One line per paragraph whatever the wrap inputs: only `set_limits` (a shape
            // input) recovers this leg's refuse.
            expect(!updated && layout.lines().empty() && !layout.truncated(),
                   where(action, "the pass must fail and leave the layout empty and not flag "
                                 "what it refused to lay out"));
        else
            expect(updated.ok() && layout.lines().size() == lim.max_lines && layout.truncated(),
                   where(action, "what fits must be kept and the layout flagged"));
    }

    {
        // The layout needs a face: one shaped line says there is one.
        lx::text::text_layout probe;
        probe.set_font({});
        probe.set_text("one");
        if (skip_without_face(probe.update().ok() && probe.has_faces(),
                              "lx.text/lines.cppm:790-797,834-840,933-938,1246-1278 text_layout"))
            return;
    }
    for (const overflow_action action : k_actions) {
        const bool refuses = action == overflow_action::reject;
        const auto warns_like = [&](const log_capture& log) {
            return log.warns("text_layout: a storage cap cut the text") ==
                   (action == overflow_action::warn_and_drop ? 1u : 0u);
        };
        const auto warns_once = [&] {
            return where(action, "the cut warning must come exactly once under warn_and_drop "
                                 "and never otherwise");
        };

        // The byte cap: the text itself is cut, at a code point boundary.
        {
            lx::text::text_layout layout;
            lx::text::text_layout_limits lim{};
            lim.max_bytes = 8;
            lim.overflow = action;
            layout.set_limits(lim);
            layout.set_font({});
            layout.set_text(k_five_lines);
            lx::result<void> updated;
            {
                log_capture log;
                updated = layout.update();
                expect(warns_like(log), warns_once());
            }
            if (refuses)
                expect(!updated && layout.lines().empty(),
                       where(action, "the pass must fail and leave the layout empty"));
            else
                expect(updated.ok() && layout.truncated() && layout.text().size() <= lim.max_bytes,
                       where(action, "what fits of the text must be kept and the layout flagged"));
        }

        // The glyph cap and the run cap (one check, lines.cppm:834-840): "one" and "two" fit.
        for (const unsigned kind : {0u, 1u}) {
            lx::text::text_layout layout;
            lx::text::text_layout_limits lim{};
            if (kind == 0)
                lim.max_glyphs = 6;
            else
                lim.max_runs = 2;
            lim.overflow = action;
            layout.set_limits(lim);
            layout.set_font({});
            layout.set_text(k_five_lines);
            lx::result<void> updated;
            {
                log_capture log;
                updated = layout.update();
                expect(warns_like(log), warns_once());
            }
            if (refuses)
                expect(!updated && layout.lines().empty(),
                       where(action, "the pass must fail and leave the layout empty"));
            else
                expect(updated.ok() && layout.truncated() && layout.lines().size() <= 2,
                       where(action, "what fits must be kept and the layout flagged"));
        }

        // The line cap (the wrap pass's shaped path, lines.cppm:1274-1278).
        {
            lx::text::text_layout layout;
            lx::text::text_layout_limits lim{};
            lim.max_lines = 2;
            lim.overflow = action;
            layout.set_limits(lim);
            layout.set_font({});
            layout.set_text(k_five_lines);
            lx::result<void> updated;
            {
                log_capture log;
                updated = layout.update();
                expect(warns_like(log), warns_once());
            }
            if (refuses) {
                expect(!updated && layout.lines().empty() && !layout.truncated(),
                       where(action, "the pass must fail and leave the layout empty and not "
                                     "flag what it refused to lay out"));
                // A wrap input change retries the refused pass: the shaped text survived the
                // refuse, so capping the visible lines at the storage cap re-wraps it with
                // no shape input.
                layout.set_max_lines(lim.max_lines);
                updated = layout.update();
                expect(updated.ok() && layout.lines().size() == lim.max_lines,
                       where(action, "a wrap input change must recover the refused layout"));
            } else
                expect(updated.ok() && layout.lines().size() == lim.max_lines && layout.truncated(),
                       where(action, "what fits must be kept and the layout flagged"));
        }
    }
    drove("lx.text/lines.cppm:790-797,834-840,933-938,1246-1248,1274-1278 text_layout (byte, "
          "glyph, run and line caps, all four policies, the line cap on both wrap-pass paths)");
}

// `itemizer` (itemize.cppm:682-694): a paragraph with more runs than `max_runs`. The
// partition's contract (itemize.cppm:62-67): `reject` fails the call; every other action
// extends the last run over the rest of the paragraph and flags `runs_overflowed`.
LUMEN_TEST(overflow_itemizer_each_policy) {
    auto& faces = lx::text::face_cache::shared();
    const lx::text::face_id regular =
        faces.resolve(lx::text::k_ui_family, lx::text::weight::normal, lx::text::style::normal);
    const lx::text::face_id bold =
        faces.resolve(lx::text::k_ui_family, lx::text::weight::bold, lx::text::style::normal);
    if (skip_without_face(regular.valid() && bold.valid(), "lx.text/itemize.cppm:682-694 itemizer"))
        return;
    // Six characters, the face changing at every one: six runs.
    const lx::text::face_range ranges[6]{
        {0, 1, regular}, {1, 2, bold}, {2, 3, regular}, {3, 4, bold}, {4, 5, regular}, {5, 6, bold},
    };
    for (const overflow_action action : k_actions) {
        lx::text::itemizer_config cfg{};
        cfg.max_runs = 2;
        cfg.overflow = action;
        lx::text::itemizer items{cfg};
        lx::text::itemize_request req{};
        req.utf8 = "abcdef";
        req.face = regular;
        req.faces = ranges;
        lx::result<lx::text::itemized_paragraph> paragraph;
        {
            log_capture log;
            paragraph = items.itemize(faces, req);
            expect(log.warns("itemize: run cap reached") ==
                       (action == overflow_action::warn_and_drop ? 1u : 0u),
                   where(action, "the run cap warning must come exactly once under warn_and_drop"));
        }
        expect(items.counters().run_overflows == 1,
               where(action, "the paragraph that hit the cap must be counted"));
        if (action == overflow_action::reject)
            expect(!paragraph, where(action, "the call must fail"));
        else
            expect(paragraph.ok() && paragraph->runs.size() == cfg.max_runs &&
                       paragraph->runs_overflowed && paragraph->runs.back().end == 6,
                   where(action, "the run count must stay at the cap, the last run cover the "
                                 "rest of the paragraph, and the paragraph be flagged"));
    }
    drove("lx.text/itemize.cppm:682-694 itemizer (all four policies)");
}

// ── The report ──────────────────────────────────────────────────────────────────────────

LUMEN_TEST(overflow_caps_report_names_what_is_not_pinned_or_driven) {
    struct site {
        const char* where;
        const char* what;
        const char* needs;
    };
    // Every `overflow_action::` site under modules/ that no case above drives (grep -rn
    // 'overflow_action::' modules/). Of the files that name an action and are neither
    // driven nor listed here, each names it in a comment only (`commented`, below).
    const site rest[]{
        {"lx.ui/icon.cppm:518-520",
         "icon_cache: an async raster job's waiters (k_job_waiters = 4, reject)",
         "the raster job is posted to a worker, and librsvg and the bundled SVGs"},
        {"lx.gfx/swapchain.cpp:444-452",
         "client_swapchain::retire: images a resize left with the compositor "
         "(k_swapchain_retired = 6, warn_and_drop)",
         "a live Wayland connection and seven resized images a compositor still holds"},
        {"lx.wayland.client/dmabuf.cpp:109-112",
         "dmabuf_feedback: a tranche's formats (k_max_tranche_formats, warn_and_drop)",
         "zwp_linux_dmabuf_feedback_v1 events from a compositor over a live connection"},
        {"lx.wayland.client/surface.cpp:485-487",
         "surface: presentation feedback slots in flight (drop_newest)",
         "wp_presentation_feedback from a compositor over a live connection"},
    };
    std::println("  observed, not pinned ({}):", observed().size());
    for (const std::string& s : observed())
        std::println("    {}", s);
    std::println("  not driven, by name ({} + {} skipped):", sizeof(rest) / sizeof(rest[0]),
                 not_driven().size());
    for (const site& s : rest)
        std::println("    {}: {} — needs {}", s.where, s.what, s.needs);
    for (const std::string& s : not_driven())
        std::println("    {}", s);
    std::println("  driven: {} structure(s)", driven().size());
    for (const std::string& s : driven())
        std::println("    {}", s);
    // The report is held to the tree, by file. A module file that names an action is one a
    // case drove, one the list above names, or one that names it in a comment only.
    // Anything else is a site nobody has accounted for.
    const char* const commented[]{
        "lx.gfx/swapchain.cppm",        // `k_swapchain_retired`, the cap `retire` keeps
        "lx.wayland.client/types.cppm", // `dmabuf_tranche::dropped`, dmabuf.cpp's counter
    };
    const std::filesystem::path modules{LUMEN_TEST_MODULES_DIR};
    expect(std::filesystem::is_directory(modules),
           "the modules directory of the tree this test was built from must be there");
    unsigned naming = 0;
    std::vector<std::string> unmentioned;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{modules}) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream in{entry.path(), std::ios::binary};
        expect(in.is_open(), std::format("a module file must be readable: {}", entry.path()));
        const std::string text{std::istreambuf_iterator<char>{in}, {}};
        if (text.find("overflow_action::") == std::string::npos)
            continue;
        ++naming;
        const std::string file = entry.path().lexically_relative(modules).generic_string();
        const std::string prefix = file + ":";
        bool named =
            std::find(std::begin(commented), std::end(commented), file) != std::end(commented);
        for (const std::vector<std::string>* const list : {&driven(), &not_driven()})
            for (const std::string& s : *list)
                named = named || s.starts_with(prefix);
        for (const site& s : rest)
            named = named || std::string_view{s.where}.starts_with(prefix);
        if (!named)
            unmentioned.push_back(file);
    }
    std::println("  module files naming an action: {} ({} not accounted for)", naming,
                 unmentioned.size());
    std::ranges::sort(unmentioned);
    std::string files;
    for (const std::string& file : unmentioned)
        std::format_to(std::back_inserter(files), "{} ", file);
    expect(unmentioned.empty(),
           std::format("module file(s) naming an overflow_action that no case drives and the "
                       "report does not name — drive the site in a case above, or name it in "
                       "the report: {}",
                       files));
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
