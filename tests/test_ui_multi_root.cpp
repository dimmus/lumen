// ADR-010 §7 Phase 5: several ui_roots on one UI thread, each with its own ui_context. Damage,
// overlays, timers and reducers are per root: what happens in one root wakes, draws and
// hit-tests only that root, and asynchronous invalidations (a task posted to the UI strand,
// an icon raster landing) reach the root whose node asked. Headless: two roots A and B on the
// test's thread, ticked alternately.
//
// The icon case needs librsvg (dlopen'd) and the bundled SVGs; without them it prints SKIP and
// passes.
#include "lumen_test.hpp"

#include <unistd.h>

import std;
import lx.foundation;
import lx.a11y;
import lx.gfx;
import lx.layout;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.ui;

namespace {

using namespace lx::ui;
using lx::layout::px;

// ── Fixtures ──

/// The text and icon caches every root draws with (thread-wide by design).
struct resources {
    lx::text::face_cache faces{};
    lx::text::shape_cache shapes{};
    lx::text::itemizer items{};
    lx::gfx::atlas atlas{};
    lx::text::glyph_cache glyphs{atlas};
    icon_cache icons{atlas};

    resources() {
        ui_resources r{};
        r.text = {{&faces, &shapes, &items}, &glyphs, nullptr};
        r.icons = &icons;
        set_ui_resources(r);
    }
    ~resources() { set_ui_resources({}); }
};

[[nodiscard]] resources& env() {
    static const std::unique_ptr<resources> r = std::make_unique<resources>();
    return *r;
}

/// A root on its own context (declared first: it outlives the root), ticked once.
struct test_root {
    ui_context ctx{};
    lx::runtime::memory_arena arena{512 * 1024};
    ui_root root{arena, ctx};
    std::unique_ptr<lx::scene::draw_list> list = std::make_unique<lx::scene::draw_list>();
    unsigned frames = 0;

    test_root(describe_fn describer, lx::rect2i bounds) {
        (void)env();
        root.set_describer(describer);
        root.set_bounds(bounds);
        LUMEN_CHECK(tick());
    }
    bool tick() {
        const bool produced = root.tick(*list);
        frames += produced ? 1u : 0u;
        return produced;
    }
};

std::uint64_t g_now_ns = 1'000'000'000ull;
std::uint64_t test_clock() { return g_now_ns; }

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

[[nodiscard]] lx::point2i centre(const ui_node& n) {
    const lx::rect2i b = n.bounds();
    return {b.x + b.width / 2, b.y + b.height / 2};
}

[[nodiscard]] bool in_tree(const ui_node* n, const ui_node* tree) {
    while (n && n->parent())
        n = n->parent();
    return n == tree;
}

void click(ui_root& r, lx::point2i at) {
    (void)r.pointer_move(at);
    (void)r.pointer_button(mouse_button::left, true);
    (void)r.pointer_button(mouse_button::left, false);
}

// ── Describers (plain functions: each root its own) ──

child describe_a(build_context&) {
    const child kids[2] = {
        button("Save").name("a-save").tooltip("Save the file"),
        button("Open").name("a-open"),
    };
    return div(style{}.flex_row().items_center().gap(px(20.f)).p(px(20.f)), kids);
}

child describe_b(build_context&) {
    const child kids[2] = {
        button("Run").name("b-run").tooltip("Run it"),
        button("Stop").name("b-stop"),
    };
    return div(style{}.flex_row().items_center().gap(px(20.f)).p(px(20.f)), kids);
}

} // namespace

LUMEN_TEST(sizes) {
    // A context is the per-root state; a node is unchanged by it (no field: out-of-scope
    // damage finds its root through `parent()`).
    std::printf("    sizeof(ui_root) %zu B, sizeof(ui_context) %zu B, sizeof(ui_node) %zu B\n",
                sizeof(ui_root), sizeof(ui_context), sizeof(ui_node));
    LUMEN_CHECK(sizeof(ui_node) == 64);
    LUMEN_CHECK(sizeof(ui_context) <= 1536);
}

LUMEN_TEST(a_press_in_one_root_invalidates_only_that_root) {
    test_root a{describe_a, {0, 0, 400, 120}};
    test_root b{describe_b, {0, 0, 400, 120}};
    LUMEN_CHECK(!a.tick() && !b.tick());
    ui_node* save = find(a.root.tree(), lx::a11y::role::button, "a-save");
    LUMEN_CHECK(save != nullptr);

    (void)a.root.pointer_move(centre(*save));
    (void)a.root.pointer_button(mouse_button::left, true);
    LUMEN_CHECK(!a.root.damage().empty() && a.root.wants_frame());
    LUMEN_CHECK(b.root.damage().empty() && !b.root.wants_frame());
    // Neither root's damage reached the thread default (no root uses it).
    LUMEN_CHECK(thread_default_context().damage().empty());
    // B's tick has nothing to do; A's repaints the button.
    LUMEN_CHECK(!b.tick());
    LUMEN_CHECK(a.tick());
    LUMEN_CHECK(a.root.frame_damage().union_bounds().width > 0);
    (void)a.root.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(a.tick() && !b.tick());
    LUMEN_CHECK(!a.tick());
}

LUMEN_TEST(a_tooltip_timer_fires_only_in_its_roots_tick) {
    set_ui_clock(test_clock);
    set_reduced_motion(false);
    {
        test_root a{describe_a, {0, 0, 400, 200}};
        test_root b{describe_b, {0, 0, 400, 200}};
        ui_node* save = find(a.root.tree(), lx::a11y::role::button, "a-save");
        LUMEN_CHECK(a.root.pointer_move(centre(*save)));
        LUMEN_CHECK(a.root.next_deadline_ns() == g_now_ns + 500'000'000ull);
        LUMEN_CHECK(b.root.next_deadline_ns() == 0);
        LUMEN_CHECK(a.tick());
        g_now_ns += 500'000'000ull;
        // Due now: A wants its frame, B does not, and B's tick does not fire A's timer.
        LUMEN_CHECK(a.root.wants_frame() && !b.root.wants_frame());
        LUMEN_CHECK(!b.tick());
        LUMEN_CHECK(a.ctx.overlays().open_count() == 0);
        LUMEN_CHECK(a.tick());
        LUMEN_CHECK(a.ctx.overlays().open_count() == 1 && b.ctx.overlays().open_count() == 0);
        LUMEN_CHECK(a.ctx.overlays().top()->kind() == overlay_kind::tooltip);
        LUMEN_CHECK(b.root.next_deadline_ns() == 0);
    }
    set_ui_clock(nullptr);
}

namespace {

struct counter_state {
    int count = 0;
    int id = 0;
};
enum class counter_action { increment };
using counter_host = reducer_node<counter_state, counter_action>;

int g_builds[2] = {};
char g_count_text[2][16] = {};

void reduce_counter(counter_state& s, const counter_action&) { ++s.count; }

child build_counter(const counter_state& s, counter_host& host) {
    ++g_builds[s.id];
    std::snprintf(g_count_text[s.id], sizeof(g_count_text[s.id]), "n=%d", s.count);
    const child kids[2] = {
        button("+").on_press(bind(host, counter_action::increment)).name(s.id == 0 ? "inc-a" : "inc-b"),
        label(g_count_text[s.id]),
    };
    return div(style{}.flex_row().items_center().gap(px(8.f)).w(px(160.f)), kids);
}

child describe_counter_a(build_context&) {
    return reducer<counter_state, counter_action>({0, 0}, reduce_counter, build_counter, 1);
}
child describe_counter_b(build_context&) {
    return reducer<counter_state, counter_action>({0, 1}, reduce_counter, build_counter, 1);
}

} // namespace

LUMEN_TEST(a_reducer_dispatched_in_one_root_rebuilds_only_that_root) {
    g_builds[0] = g_builds[1] = 0;
    test_root a{describe_counter_a, {0, 0, 300, 60}};
    test_root b{describe_counter_b, {0, 0, 300, 60}};
    LUMEN_CHECK(g_builds[0] == 1 && g_builds[1] == 1);
    LUMEN_CHECK(!a.tick() && !b.tick());

    // A click in B: B's reducer, B's frame.
    click(b.root, centre(*find(b.root.tree(), lx::a11y::role::button, "inc-b")));
    auto* host_b = static_cast<counter_host*>(b.root.tree());
    LUMEN_CHECK(host_b->state().count == 1);
    LUMEN_CHECK(b.ctx.reducers_pending() && !a.ctx.reducers_pending());
    LUMEN_CHECK(!a.root.wants_frame() && b.root.wants_frame());
    LUMEN_CHECK(!a.tick());
    LUMEN_CHECK(b.tick());
    LUMEN_CHECK(g_builds[0] == 1 && g_builds[1] == 2);
    LUMEN_CHECK(b.root.last_stage_times().reducers_rebuilt == 1);
    LUMEN_CHECK(!a.tick() && !b.tick());

    // A dispatch outside any root's scope (app code, a posted task): it finds B by its tree.
    host_b->dispatch(counter_action::increment);
    LUMEN_CHECK(b.ctx.reducers_pending() && !a.ctx.reducers_pending());
    LUMEN_CHECK(!a.root.wants_frame() && b.root.wants_frame());
    LUMEN_CHECK(!a.tick() && b.tick());
    LUMEN_CHECK(g_builds[0] == 1 && g_builds[1] == 3 && host_b->state().count == 2);
}

namespace {

child describe_overlay_a(build_context&) {
    overlay_props p{};
    p.kind = overlay_kind::popover;
    p.open = true;
    p.placement.side = overlay_side::window_top;
    p.placement.window_top = 20;
    const child content =
        div(style{}.w(px(120.f)).h(px(60.f)).bg(tok::elevated_surface_background), {});
    const child kids[2] = {
        button("Anchor").name("a-anchor"),
        overlay(p, content),
    };
    return div(style{}.flex_col().p(px(10.f)), kids);
}

child describe_plain_b(build_context&) {
    const child kids[1] = {div(style{}.w(px(400.f)).h(px(200.f)).bg(tok::surface_background), {})};
    return div(style{}.flex_col(), kids);
}

} // namespace

LUMEN_TEST(an_overlay_opened_in_one_root_is_not_hit_in_another) {
    test_root a{describe_overlay_a, {0, 0, 400, 200}};
    LUMEN_CHECK(a.ctx.overlays().open_count() == 1);
    (void)a.tick(); // the fresh overlay's first paint
    test_root b{describe_plain_b, {0, 0, 400, 200}};
    // B's frames do not close A's overlay (a shared layer took the newest tree's side).
    LUMEN_CHECK(!b.tick());
    LUMEN_CHECK(a.ctx.overlays().open_count() == 1 && b.ctx.overlays().open_count() == 0);
    const lx::rect2i shown = a.ctx.overlays().top()->content()->bounds();
    LUMEN_CHECK(shown.width == 120 && shown.height == 60);
    const lx::point2i at{shown.x + shown.width / 2, shown.y + shown.height / 2};
    // In A the overlay takes the point; in B the point is B's own tree.
    ui_node* in_a = a.root.pointer().resolve(a.root.tree(), at, false);
    LUMEN_CHECK(in_a && in_tree(in_a, a.root.tree()));
    bool inside_content = false;
    for (const ui_node* n = in_a; n; n = n->parent())
        inside_content = inside_content || n == a.ctx.overlays().top()->content();
    LUMEN_CHECK(inside_content);
    ui_node* in_b = b.root.pointer().resolve(b.root.tree(), at, false);
    LUMEN_CHECK(in_b && in_tree(in_b, b.root.tree()));
    (void)b.root.pointer_move(at);
    LUMEN_CHECK(b.root.pointer().hovered() == nullptr || in_tree(b.root.pointer().hovered(), b.root.tree()));
    // A press there in B dismisses nothing of A's.
    (void)b.root.pointer_button(mouse_button::left, true);
    (void)b.root.pointer_button(mouse_button::left, false);
    LUMEN_CHECK(a.ctx.overlays().open_count() == 1);
    LUMEN_CHECK(!a.root.wants_frame());
}

namespace {

struct post_damage {
    ui_node* node = nullptr;
    bool ran = false;
};

void run_post_damage(void* raw) {
    auto* p = static_cast<post_damage*>(raw);
    invalidate_paint(*p->node);
    p->ran = true;
}

} // namespace

LUMEN_TEST(damage_posted_to_the_ui_strand_lands_in_the_nodes_root) {
    test_root a{describe_a, {0, 0, 400, 120}};
    test_root b{describe_b, {0, 0, 400, 120}};
    LUMEN_CHECK(!a.tick() && !b.tick());
    ui_node* stop = find(b.root.tree(), lx::a11y::role::button, "b-stop");
    LUMEN_CHECK(stop != nullptr);
    post_damage job{stop};
    lx::runtime::executor& ex = lx::runtime::executor::global();
    LUMEN_CHECK(ex.post(lx::runtime::affinity::ui, lx::runtime::task{run_post_damage, &job}));
    ex.drain(lx::runtime::affinity::ui); // outside any root's scope
    LUMEN_CHECK(job.ran);
    LUMEN_CHECK(a.root.damage().empty() && !a.root.wants_frame());
    LUMEN_CHECK(b.root.damage().count() == 1);
    const lx::rect2i d = b.root.damage().union_bounds();
    const lx::rect2i s = stop->bounds();
    LUMEN_CHECK(d.x <= s.x && d.y <= s.y && d.x + d.width >= s.x + s.width &&
                d.y + d.height >= s.y + s.height);
    LUMEN_CHECK(!a.tick() && b.tick());

    // A node in no root's tree, while every live root has its own context: no root draws
    // it, so its damage is dropped and counted (not given to the thread default).
    lx::runtime::memory_arena arena{64 * 1024};
    build_context bctx{arena};
    reconciler rec{};
    ui_node* loose = nullptr;
    {
        build_scope scope{bctx};
        loose = rec.apply(nullptr, label("loose"));
    }
    loose->arrange({0, 0, 50, 20});
    thread_default_context().damage().clear();
    const std::uint64_t dropped = ui_layout_counters().damage_dropped;
    invalidate_paint(*loose);
    LUMEN_CHECK(ui_layout_counters().damage_dropped == dropped + 1);
    LUMEN_CHECK(thread_default_context().damage().empty());
    LUMEN_CHECK(a.root.damage().empty() && b.root.damage().empty());
    // Inside a root's scope it is that root's (what a describe or a handler raises).
    {
        const context_scope scope{a.ctx};
        invalidate_paint(*loose);
    }
    LUMEN_CHECK(a.root.damage().count() == 1 && b.root.damage().empty());
    a.root.damage().clear();
    delete loose;
}

LUMEN_TEST(a_tree_with_no_root_keeps_the_thread_default) {
    // No root on the thread: a bare tree (a test, a tool) damages the thread default, as the
    // one ledger per thread did.
    lx::runtime::memory_arena arena{64 * 1024};
    build_context bctx{arena};
    reconciler rec{};
    ui_node* tree = nullptr;
    {
        build_scope scope{bctx};
        tree = rec.apply(nullptr, label("bare"));
    }
    tree->arrange({5, 6, 50, 20});
    ui_damage().clear();
    invalidate_paint(*tree);
    LUMEN_CHECK(&ui_damage() == &thread_default_context().damage());
    LUMEN_CHECK(ui_damage().count() == 1 && ui_damage().union_bounds().x == 5);
    ui_damage().clear();
    // A root on the thread default keeps it in use, for detached trees too.
    {
        lx::runtime::memory_arena root_arena{64 * 1024};
        ui_root legacy{root_arena};
        LUMEN_CHECK(&legacy.context() == &thread_default_context());
        invalidate_paint(*tree);
        LUMEN_CHECK(legacy.damage().count() == 1);
        ui_damage().clear();
    }
    delete tree;
}

namespace {

/// The worker affinity: a thread draining the executor's worker strand until stopped.
class worker_thread {
public:
    worker_thread()
        : thread_{[this] {
              while (!stop_.load(std::memory_order_acquire)) {
                  lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
                  ::usleep(200);
              }
              lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
          }} {}
    ~worker_thread() {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

template<typename F>
bool pump(F done) {
    for (int i = 0; i < 20000; ++i) {
        lx::runtime::executor::global().drain(lx::runtime::affinity::ui);
        if (done())
            return true;
        ::usleep(1000);
    }
    return false;
}

} // namespace

LUMEN_TEST(an_icon_landing_after_its_root_went_damages_no_other_root) {
    if (!icons_available()) {
        std::printf("    SKIP: librsvg or the bundled icons are not available\n");
        return;
    }
    auto atlas = std::make_unique<lx::gfx::atlas>();
    atlas->begin_frame(1);
    worker_thread worker{};
    test_root b{describe_b, {0, 0, 400, 120}};
    LUMEN_CHECK(!b.tick());
    {
        icon_cache cache{*atlas};
        cache.set_async(&lx::runtime::executor::global());
        lx::gfx::atlas_tile t{};
        {
            // A asks (what its icon node's emit does in A's tick), then goes before the raster
            // lands.
            auto a = std::make_unique<test_root>(describe_a, lx::rect2i{0, 0, 400, 120});
            const context_scope scope{a->ctx};
            LUMEN_CHECK(!cache.place(icon::bell, 16, t, {10, 20, 16, 16}));
        }
        // B asks for another icon: its landing is B's.
        {
            const context_scope scope{b.ctx};
            LUMEN_CHECK(!cache.place(icon::moon, 16, t, {40, 20, 16, 16}));
        }
        LUMEN_CHECK(cache.counters().async_posted == 2);
        LUMEN_CHECK(pump([&] { return cache.counters().async_landed == 2; }));
        // The bell's landing damaged nothing (its root is gone); the moon's damaged B alone.
        LUMEN_CHECK(b.root.damage().count() == 1);
        const lx::rect2i d = b.root.damage().union_bounds();
        LUMEN_CHECK(d.x == 40 && d.y == 20 && d.width == 16 && d.height == 16);
        LUMEN_CHECK(thread_default_context().damage().empty());
        LUMEN_CHECK(cache.place(icon::bell, 16, t) && t.w == 16);
        b.root.damage().clear();
    }
    LUMEN_CHECK(pump([] { return true; }));
}

namespace {

child describe_fading_a(build_context&) {
    overlay_props p{};
    p.kind = overlay_kind::popover;
    p.open = true;
    p.placement.side = overlay_side::window_top;
    p.fade_ms = 150;
    const child content =
        div(style{}.w(px(80.f)).h(px(40.f)).bg(tok::elevated_surface_background), {});
    const child kids[1] = {overlay(p, content)};
    return div(style{}.flex_col(), kids);
}

} // namespace

LUMEN_TEST(g1_per_root_one_animating_root_wakes_no_other) {
    set_ui_clock(test_clock);
    set_reduced_motion(false);
    {
        test_root b{describe_b, {0, 0, 400, 120}};
        test_root a{describe_fading_a, {0, 0, 400, 200}};
        LUMEN_CHECK(!b.tick());
        b.frames = 0;
        unsigned a_frames = 0;
        for (int i = 0; i < 100 && a.root.wants_frame(); ++i) {
            LUMEN_CHECK(!b.root.wants_frame() && b.root.next_deadline_ns() == 0);
            LUMEN_CHECK(!b.tick());
            g_now_ns += 16'666'667ull;
            a_frames += a.tick() ? 1u : 0u;
        }
        std::printf("    A animated %u frames, B produced %u\n", a_frames, b.frames);
        LUMEN_CHECK(a_frames >= 8 && a_frames <= 12);
        LUMEN_CHECK(b.frames == 0);
        LUMEN_CHECK(!a.root.wants_frame() && !a.tick());
    }
    set_ui_clock(nullptr);
}

int main(int argc, char** argv) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    return lumen_test::run_all(argc, argv);
}
