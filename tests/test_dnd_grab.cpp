// dnd_grab: the drag-and-drop grab's state machine, driven as the window manager and the
// wire drive it, with fake wire ops — target changes vs motion, drop vs cancel and their
// reasons, Escape, the source-less drag that stays inside its client, the modifier-chosen
// user action, the icon following the pointer once per tick with its accumulated offset,
// and no allocation on the per-motion path. A real surface tree and scene, no Wayland.
// The last case widens that to the whole drag path through the window manager: the
// xdg-toplevel-drag-v1 window is marked by the motion and moved once per tick, alloc-free.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.scene;
import lx.input;
import lx.wayland.server;
import lx.compositor;

using lx::compositor::dnd_end_reason;
using lx::compositor::dnd_grab;
using lx::compositor::dnd_ops;
using lx::compositor::dnd_target;
using lx::compositor::root_kind;

// ── Allocation counter: every operator new in the process while `g_counting` is set ──

namespace {
std::atomic<bool> g_counting{false};
std::atomic<unsigned> g_allocs{0};
} // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed))
        g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

lx::surface_id sid(unsigned n) { return lx::surface_id{n}; }

/// What the fake wire saw.
struct wire_log {
    unsigned focus_calls = 0;
    void* focus_res = nullptr;
    double focus_sx = 0.0;
    unsigned motion_calls = 0;
    double motion_sx = 0.0;
    unsigned drop_calls = 0;
    bool drop_result = true;
    dnd_end_reason drop_why = dnd_end_reason::not_accepted;
    unsigned cancel_calls = 0;
    dnd_end_reason cancel_why = dnd_end_reason::drop;
    unsigned user_action_calls = 0;
    unsigned user_action = 0;
};

struct fixture {
    lx::compositor::surface_manager surfaces{};
    lx::scene::scene_graph scene{};
    lx::scene::container_node icon_layer{};
    lx::compositor::surface_tree tree{};
    dnd_grab grab{};
    wire_log w{};
    unsigned long long next_buffer = 1;
    int res_a = 0, res_b = 0; // stand-ins for wl_surface resources

    fixture() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        surfaces.set_scene(&scene);
        tree.bind(surfaces, scene);
        (void)scene.root().add(&icon_layer);
        grab.bind(tree, icon_layer, nullptr);
        dnd_ops ops{};
        ops.user = &w;
        ops.focus = [](void* u, void* res, double sx, double) {
            auto* l = static_cast<wire_log*>(u);
            ++l->focus_calls;
            l->focus_res = res;
            l->focus_sx = sx;
        };
        ops.motion = [](void* u, unsigned, double sx, double) {
            auto* l = static_cast<wire_log*>(u);
            ++l->motion_calls;
            l->motion_sx = sx;
        };
        ops.drop = [](void* u, dnd_end_reason& why) {
            auto* l = static_cast<wire_log*>(u);
            ++l->drop_calls;
            if (!l->drop_result)
                why = l->drop_why;
            return l->drop_result;
        };
        ops.cancel = [](void* u, dnd_end_reason why) {
            auto* l = static_cast<wire_log*>(u);
            ++l->cancel_calls;
            l->cancel_why = why;
        };
        ops.user_action = [](void* u, unsigned a) {
            auto* l = static_cast<wire_log*>(u);
            ++l->user_action_calls;
            l->user_action = a;
        };
        grab.set_ops(ops);
    }

    /// A root surface with w x h content, made a drag icon.
    lx::surface_id icon(unsigned n, int w, int h) {
        const lx::surface_id s = sid(n);
        LUMEN_CHECK(surfaces.register_surface(s, nullptr));
        LUMEN_CHECK(tree.add_surface(s));
        const lx::buffer_id b{next_buffer++};
        surfaces.register_shm(b, static_cast<unsigned>(w), static_cast<unsigned>(h),
                              static_cast<lx::fourcc>(lx::pixel_format::xrgb8888));
        lx::compositor::surface_commit c{};
        c.surface = s;
        c.buffer = b;
        c.damage = {0, 0, w, h};
        LUMEN_CHECK(static_cast<bool>(surfaces.on_commit(std::move(c))));
        tree.set_has_buffer(s, true);
        LUMEN_CHECK(tree.make_root(s, root_kind::dnd_icon));
        return s;
    }

    dnd_target over(int* res, unsigned surface, unsigned client, double sx) {
        dnd_target t{};
        t.surface_resource = res;
        t.surface = sid(surface);
        t.client = lx::client_id{client};
        t.sx = sx;
        t.sy = sx;
        return t;
    }
};

std::unique_ptr<fixture> make_fixture() { return std::make_unique<fixture>(); }

/// The drag with its window manager around it: the same surface tree and scene, the wire's
/// optional parts absent (no popups, frames or layer-shell; no outputs, so placement falls
/// back to the nominal 1920x1080 desktop) and a shell bridge over an unbound server, whose
/// emits are no-ops. What this adds over `fixture` is the WM side of
/// xdg-toplevel-drag-v1: the motion that marks the dragged toplevel pending and the tick
/// that moves it once — the paths `window_manager` runs per event and per frame.
struct wm_fixture : fixture {
    lx::wayland::server server{};
    lx::compositor::shell_bridge shell{server};
    lx::compositor::toplevel_manager tops{};
    lx::compositor::input_router router{};
    lx::input::seat seat{};
    lx::compositor::output_manager outputs{};
    lx::scene::container_node windows_layer{};
    lx::compositor::window_manager wm{};

    wm_fixture() {
        (void)scene.root().add(&windows_layer);
        wm.bind(tops, surfaces, tree, scene, windows_layer, router, seat, shell, outputs);
        wm.set_dnd(&grab);
    }
    ~wm_fixture() {
        // The tree is a base member, destroyed after the WM: its unmap hook must not point
        // at the WM anymore by then.
        tree.set_unmap_hook(nullptr, nullptr);
    }

    /// A mapped toplevel `w`x`h`, mapped the way the wire maps one: a committed buffer, a
    /// record, the initial commit and the buffer commit that places and maps it. Where it
    /// lands is the cascade's choice; read it back with `geometry`.
    lx::toplevel_id window(unsigned n, int w, int h) {
        const lx::surface_id s = sid(n);
        LUMEN_CHECK(surfaces.register_surface(s, &res_a));
        LUMEN_CHECK(tree.add_surface(s));
        const lx::buffer_id b{next_buffer++};
        surfaces.register_shm(b, static_cast<unsigned>(w), static_cast<unsigned>(h),
                              static_cast<lx::fourcc>(lx::pixel_format::xrgb8888));
        lx::compositor::surface_commit c{};
        c.surface = s;
        c.buffer = b;
        c.damage = {0, 0, w, h};
        LUMEN_CHECK(static_cast<bool>(surfaces.on_commit(std::move(c))));
        tree.set_has_buffer(s, true);
        LUMEN_CHECK(tree.make_root(s, root_kind::toplevel));
        auto created = tops.create(lx::client_id{1}, s, &res_a, &res_a, &res_a);
        LUMEN_CHECK(static_cast<bool>(created));
        const lx::toplevel_id id = created ? *created : lx::toplevel_id{};
        wm.on_initial_commit(id);
        lx::compositor::toplevel_commit_info info{};
        info.has_buffer = true;
        info.surface_size = {w, h};
        info.tree_bbox = {0, 0, w, h};
        wm.on_toplevel_commit(id, info);
        return id;
    }

    lx::rect2i geometry(lx::toplevel_id id) const {
        const auto* rec = tops.find(id);
        return rec ? rec->geometry : lx::rect2i{};
    }
};

} // namespace

LUMEN_TEST(dnd_target_changes_are_focus_same_target_is_motion) {
    auto f = make_fixture();
    f->grab.begin(lx::client_id{1}, false, {}, 10.0, 10.0);
    LUMEN_CHECK(f->grab.active());
    const dnd_target a = f->over(&f->res_a, 1, 1, 5.0);
    f->grab.on_motion(&a, 10.0, 10.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 1 && f->w.focus_res == &f->res_a && f->w.motion_calls == 0);
    const dnd_target a2 = f->over(&f->res_a, 1, 1, 7.0);
    f->grab.on_motion(&a2, 12.0, 12.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 1 && f->w.motion_calls == 1 && f->w.motion_sx == 7.0);
    // A re-hit after the scene changed sends no motion for the same target.
    f->grab.on_motion(&a2, 12.0, 12.0, 0, false);
    LUMEN_CHECK(f->w.motion_calls == 1);
    const dnd_target b = f->over(&f->res_b, 2, 2, 1.0);
    f->grab.on_motion(&b, 300.0, 12.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 2 && f->w.focus_res == &f->res_b);
    LUMEN_CHECK(f->grab.target() == sid(2));
    f->grab.on_motion(nullptr, 900.0, 900.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 3 && f->w.focus_res == nullptr);
    // Nothing → nothing: no call at all.
    f->grab.on_motion(nullptr, 901.0, 900.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 3 && f->w.motion_calls == 1);
}

LUMEN_TEST(dnd_release_drops_or_cancels_with_the_wire_reason) {
    auto f = make_fixture();
    f->grab.begin(lx::client_id{1}, false, {}, 0.0, 0.0);
    f->grab.on_released();
    LUMEN_CHECK(f->w.drop_calls == 1 && f->w.cancel_calls == 0 && !f->grab.active());

    f->w = {};
    f->w.drop_result = false;
    f->w.drop_why = dnd_end_reason::not_accepted;
    f->grab.begin(lx::client_id{1}, false, {}, 0.0, 0.0);
    f->grab.on_released();
    LUMEN_CHECK(f->w.drop_calls == 1 && f->w.cancel_calls == 1);
    LUMEN_CHECK(f->w.cancel_why == dnd_end_reason::not_accepted && !f->grab.active());
    // Inactive: nothing more reaches the wire.
    f->grab.on_released();
    f->grab.on_escape();
    LUMEN_CHECK(f->w.drop_calls == 1 && f->w.cancel_calls == 1);
}

LUMEN_TEST(dnd_escape_cancels_and_end_tears_down_without_the_wire) {
    auto f = make_fixture();
    f->grab.begin(lx::client_id{1}, false, {}, 0.0, 0.0);
    f->grab.on_escape();
    LUMEN_CHECK(f->w.cancel_calls == 1 && f->w.cancel_why == dnd_end_reason::escape);
    LUMEN_CHECK(!f->grab.active());

    f->w = {};
    f->grab.begin(lx::client_id{1}, false, {}, 0.0, 0.0);
    f->grab.end(dnd_end_reason::source_destroyed);
    LUMEN_CHECK(!f->grab.active() && f->w.cancel_calls == 0 && f->w.drop_calls == 0);
}

LUMEN_TEST(dnd_without_a_source_stays_inside_its_client) {
    auto f = make_fixture();
    f->grab.begin(lx::client_id{7}, true, {}, 0.0, 0.0);
    const dnd_target other = f->over(&f->res_a, 1, 8, 1.0);
    f->grab.on_motion(&other, 1.0, 1.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 0 && f->w.motion_calls == 0);
    const dnd_target own = f->over(&f->res_b, 2, 7, 1.0);
    f->grab.on_motion(&own, 2.0, 2.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 1 && f->w.focus_res == &f->res_b);
    // Back over the other client's surface: that is no target, so focus goes to none.
    f->grab.on_motion(&other, 3.0, 3.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 2 && f->w.focus_res == nullptr);
}

LUMEN_TEST(dnd_modifiers_choose_the_user_action_on_change_only) {
    auto f = make_fixture();
    f->grab.on_modifiers(true, false, false); // not active: ignored
    LUMEN_CHECK(f->w.user_action_calls == 0);
    f->grab.begin(lx::client_id{1}, false, {}, 0.0, 0.0);
    f->grab.on_modifiers(false, false, false); // none, as at begin
    LUMEN_CHECK(f->w.user_action_calls == 0);
    f->grab.on_modifiers(false, true, false);
    LUMEN_CHECK(f->w.user_action_calls == 1 && f->w.user_action == lx::compositor::dnd_copy);
    f->grab.on_modifiers(true, true, false); // Shift wins over Ctrl
    LUMEN_CHECK(f->w.user_action_calls == 2 && f->w.user_action == lx::compositor::dnd_move);
    f->grab.on_modifiers(true, true, false);
    LUMEN_CHECK(f->w.user_action_calls == 2);
    f->grab.on_modifiers(false, false, true);
    LUMEN_CHECK(f->w.user_action_calls == 3 && f->w.user_action == lx::compositor::dnd_ask);
}

LUMEN_TEST(dnd_target_unmapped_or_destroyed_is_left_at_once) {
    auto f = make_fixture();
    f->grab.begin(lx::client_id{1}, false, {}, 0.0, 0.0);
    const dnd_target a = f->over(&f->res_a, 1, 1, 1.0);
    f->grab.on_motion(&a, 1.0, 1.0, 0);
    f->grab.on_surface_unmapped(sid(2)); // not the target
    LUMEN_CHECK(f->w.focus_calls == 1);
    f->grab.on_surface_unmapped(sid(1));
    LUMEN_CHECK(f->w.focus_calls == 2 && f->w.focus_res == nullptr && !f->grab.target());
    // The next motion over it (mapped again) is a new focus.
    f->grab.on_motion(&a, 1.0, 1.0, 0);
    LUMEN_CHECK(f->w.focus_calls == 3 && f->w.focus_res == &f->res_a);
    f->grab.on_surface_destroyed(sid(1));
    LUMEN_CHECK(f->w.focus_calls == 4 && f->w.focus_res == nullptr && f->grab.active());
}

LUMEN_TEST(dnd_icon_follows_the_pointer_per_tick_with_its_offset) {
    auto f = make_fixture();
    const lx::surface_id icon = f->icon(5, 32, 32);
    f->grab.begin(lx::client_id{1}, false, icon, 100.4, 200.6);
    // Mapped in the icon layer at the hotspot: top-left at the rounded pointer.
    LUMEN_CHECK(f->icon_layer.child_count() == 1);
    LUMEN_CHECK(f->tree.root_mapped(icon));
    lx::point2i o = f->tree.root_origin(icon);
    LUMEN_CHECK(o.x == 100 && o.y == 201);
    LUMEN_CHECK(f->surfaces.node_rect(icon).x == 100 && f->surfaces.node_rect(icon).y == 201);

    // Motion only records; the tick moves it.
    f->grab.on_motion(nullptr, 150.0, 250.0, 0);
    o = f->tree.root_origin(icon);
    LUMEN_CHECK(o.x == 100 && o.y == 201);
    f->grab.on_tick();
    o = f->tree.root_origin(icon);
    LUMEN_CHECK(o.x == 150 && o.y == 250);
    LUMEN_CHECK(f->surfaces.node_rect(icon).x == 150 && f->surfaces.node_rect(icon).y == 250);

    // Offsets accumulate (attach dx/dy or wl_surface.offset): the hotspot moves into it.
    f->grab.on_icon_commit(icon, true, -16, -8);
    f->grab.on_icon_commit(icon, true, 0, 0);
    f->grab.on_icon_commit(icon, true, -4, 2);
    LUMEN_CHECK(f->grab.icon_offset().x == -20 && f->grab.icon_offset().y == -6);
    o = f->tree.root_origin(icon);
    LUMEN_CHECK(o.x == 130 && o.y == 244);
    // A commit for another surface is not the icon's.
    f->grab.on_icon_commit(sid(9), true, 100, 100);
    LUMEN_CHECK(f->grab.icon_offset().x == -20);

    // The end takes it out of the layer; the role (root kind) stays.
    f->grab.on_released();
    LUMEN_CHECK(f->icon_layer.child_count() == 0 && !f->tree.root_mapped(icon));
    LUMEN_CHECK(f->tree.kind(icon) == root_kind::dnd_icon);

    // Given to a second drag: the offset starts over at the hotspot.
    f->grab.begin(lx::client_id{1}, false, icon, 10.0, 10.0);
    LUMEN_CHECK(f->tree.root_mapped(icon) && f->grab.icon_offset().x == 0);
    o = f->tree.root_origin(icon);
    LUMEN_CHECK(o.x == 10 && o.y == 10);
    // The icon destroyed mid-drag: gone from the layer, the drag goes on.
    f->grab.on_surface_destroyed(icon);
    LUMEN_CHECK(f->grab.active() && !f->grab.icon() && f->icon_layer.child_count() == 0);
}

LUMEN_TEST(dnd_motion_and_tick_do_not_allocate) {
    auto f = make_fixture();
    const lx::surface_id icon = f->icon(5, 32, 32);
    f->grab.begin(lx::client_id{1}, false, icon, 0.0, 0.0);
    const dnd_target a = f->over(&f->res_a, 1, 1, 1.0);
    const dnd_target b = f->over(&f->res_b, 2, 2, 1.0);
    f->grab.on_motion(&a, 1.0, 1.0, 0);
    f->grab.on_tick(); // first move warms whatever the scene keeps
    g_allocs.store(0);
    g_counting.store(true);
    for (int i = 0; i < 2000; ++i) {
        const dnd_target* t = (i / 100) % 2 ? &b : &a;
        f->grab.on_motion(t, 1.0 + i, 2.0 + i, static_cast<unsigned>(i));
        if (i % 8 == 0)
            f->grab.on_modifiers(i % 16 == 0, false, false);
    }
    const unsigned motion_allocs = g_allocs.load();
    g_allocs.store(0);
    for (int i = 0; i < 200; ++i) {
        f->grab.on_motion(&a, 3.0 + i, 4.0 + i, 0);
        f->grab.on_tick();
    }
    g_counting.store(false);
    const unsigned tick_allocs = g_allocs.load();
    std::printf("    allocations: motion=%u tick=%u\n", motion_allocs, tick_allocs);
    LUMEN_CHECK(motion_allocs == 0);
    LUMEN_CHECK(tick_allocs == 0);
    LUMEN_CHECK(f->w.motion_calls > 1900);
}

LUMEN_TEST(dnd_dragged_toplevel_motion_and_tick_do_not_allocate) {
    auto f = std::make_unique<wm_fixture>();
    // The window the drag moves, and where the cascade placed it. The pointer starts 40,30
    // inside it — the attach offset — and every position below is relative to that anchor,
    // so the pointer stays at that point of the window wherever the drag carries it.
    const lx::toplevel_id win = f->window(7, 120, 80);
    const lx::rect2i g0 = f->geometry(win);
    LUMEN_CHECK(g0.width == 120 && g0.height == 80);
    f->seat.set_pointer_position(g0.x + 40.0, g0.y + 30.0);
    f->grab.begin(lx::client_id{1}, false, {}, g0.x + 40.0, g0.y + 30.0);
    f->wm.begin_drag();
    // begin_drag makes the surface under the pointer the first target (the origin window
    // itself); the attach takes it out of the target selection from here on.
    LUMEN_CHECK(f->w.focus_calls == 1 && f->w.focus_res == &f->res_a);
    f->wm.drag_toplevel_attach(win, {40, 30});
    f->wm.on_pointer_motion(0);
    f->wm.on_tick(1'000'000'000); // the first move warms whatever the scene keeps

    // Motion only records: the window does not move until a tick applies it, whole-delta.
    f->seat.set_pointer_position(g0.x + 60.0, g0.y + 50.0);
    f->wm.on_pointer_motion(1);
    LUMEN_CHECK(f->geometry(win).x == g0.x && f->geometry(win).y == g0.y);
    f->wm.on_tick(1'016'666'667);
    LUMEN_CHECK(f->geometry(win).x == g0.x + 20 && f->geometry(win).y == g0.y + 20);

    g_allocs.store(0);
    g_counting.store(true);
    for (int i = 0; i < 2000; ++i) {
        f->seat.set_pointer_position(g0.x + 60.0 + i * 0.05, g0.y + 50.0 + i * 0.05);
        f->wm.on_pointer_motion(static_cast<unsigned>(i));
    }
    const unsigned motion_allocs = g_allocs.load();
    g_allocs.store(0);
    for (int i = 0; i < 200; ++i) {
        f->seat.set_pointer_position(g0.x + 160.5 + i * 0.5, g0.y + 150.5 + i * 0.5);
        f->wm.on_pointer_motion(static_cast<unsigned>(i));
        f->wm.on_tick(2'000'000'000 + i * 16'666'667);
    }
    g_counting.store(false);
    const unsigned tick_allocs = g_allocs.load();
    std::printf("    allocations: motion=%u tick=%u\n", motion_allocs, tick_allocs);
    LUMEN_CHECK(motion_allocs == 0);
    LUMEN_CHECK(tick_allocs == 0);
    // It followed exactly: the whole delta from the anchor (pointer 220, 220 past it at the
    // last tick), rounded once per tick — never accumulated per event.
    LUMEN_CHECK(f->geometry(win).x == g0.x + 220 && f->geometry(win).y == g0.y + 220);
    // And the wire heard nothing of it: the focus begin_drag gave the window ended with the
    // attach, and no motion ever named the dragged window a target.
    LUMEN_CHECK(f->w.focus_calls == 2 && f->w.focus_res == nullptr);
    LUMEN_CHECK(f->w.motion_calls == 0);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
