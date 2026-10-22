// surface_tree: subsurface parentage, double-buffered child state, effective sync, mapping,
// paint-order flattening, bounding box, hit testing and destruction in any order. Pure
// bookkeeping over a real surface_manager and scene graph — no Wayland.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.scene;
import lx.compositor;

using lx::compositor::root_kind;
using lx::compositor::surface_tree;
using link_error = lx::compositor::surface_tree::link_error;
using restack_error = lx::compositor::surface_tree::restack_error;

namespace {

lx::surface_id sid(unsigned n) { return lx::surface_id{n}; }

bool same(lx::rect2i a, lx::rect2i b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

struct fixture {
    lx::compositor::surface_manager surfaces{};
    lx::scene::scene_graph scene{};
    lx::scene::container_node layer{};
    surface_tree tree{};
    unsigned long long next_buffer = 1;
    unsigned unmapped_hook_calls = 0;
    lx::surface_id last_unmapped{};

    fixture() {
        lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
        surfaces.set_scene(&scene);
        tree.bind(surfaces, scene);
        tree.set_unmap_hook(
            [](void* user, lx::surface_id s) {
                auto* f = static_cast<fixture*>(user);
                ++f->unmapped_hook_calls;
                f->last_unmapped = s;
            },
            this);
        (void)scene.root().add(&layer);
    }

    lx::surface_id make(unsigned n) {
        const lx::surface_id s = sid(n);
        LUMEN_CHECK(surfaces.register_surface(s, nullptr));
        LUMEN_CHECK(tree.add_surface(s));
        return s;
    }

    /// Content of w x h, as the protocol layer's apply path leaves it.
    void content(lx::surface_id s, int w, int h) {
        const lx::buffer_id b{next_buffer++};
        surfaces.register_shm(b, static_cast<unsigned>(w), static_cast<unsigned>(h),
                              static_cast<lx::fourcc>(lx::pixel_format::xrgb8888));
        lx::compositor::surface_commit c{};
        c.surface = s;
        c.buffer = b;
        c.damage = {0, 0, w, h};
        LUMEN_CHECK(static_cast<bool>(surfaces.on_commit(static_cast<lx::compositor::surface_commit&&>(c))));
        tree.set_has_buffer(s, true);
    }

    /// A parent commit that is applied right away (a root, or desynchronized).
    void commit_apply(lx::surface_id parent) {
        tree.cache_child_state(parent);
        tree.apply_child_state(parent);
    }

    lx::surface_id painted(unsigned index) {
        auto* box = tree.container(root);
        auto* n = box ? box->child_at(index) : nullptr;
        return n ? static_cast<lx::scene::surface_node*>(n)->surface() : lx::surface_id{};
    }

    lx::surface_id root{};
};

std::unique_ptr<fixture> make_fixture() { return std::make_unique<fixture>(); }

} // namespace

// ── linking ──────────────────────────────────────────────────────────────────

LUMEN_TEST(tree_link_rejects_self_and_descendant_parents) {
    auto f = make_fixture();
    const auto a = f->make(1), b = f->make(2), c = f->make(3);
    LUMEN_CHECK(f->tree.link(a, a) == link_error::bad_parent);
    LUMEN_CHECK(f->tree.link(b, a) == link_error::none);
    LUMEN_CHECK(f->tree.link(c, b) == link_error::none);
    // a is b's parent and c's grandparent: neither may become a's parent.
    LUMEN_CHECK(f->tree.link(a, b) == link_error::bad_parent);
    LUMEN_CHECK(f->tree.link(a, c) == link_error::bad_parent);
    // Unknown parent.
    const auto d = f->make(4);
    LUMEN_CHECK(f->tree.link(d, sid(99)) == link_error::bad_parent);
}

LUMEN_TEST(tree_link_rejects_linked_surfaces_and_roots) {
    auto f = make_fixture();
    const auto p = f->make(1), q = f->make(2), c = f->make(3), r = f->make(4);
    LUMEN_CHECK(f->tree.link(c, p) == link_error::none);
    // Already has a live wl_subsurface.
    LUMEN_CHECK(f->tree.link(c, q) == link_error::bad_surface);
    // A root (toplevel) is never a subsurface.
    LUMEN_CHECK(f->tree.make_root(r, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(r, p) == link_error::bad_surface);
    // A linked surface cannot become a root.
    LUMEN_CHECK(!f->tree.make_root(c, root_kind::toplevel));
    // After unlink it may be linked again, anywhere.
    f->tree.unlink(c);
    LUMEN_CHECK(f->tree.link(c, q) == link_error::none);
}

LUMEN_TEST(tree_link_is_double_buffered_on_the_parent) {
    auto f = make_fixture();
    const auto p = f->make(1), c = f->make(2);
    LUMEN_CHECK(f->tree.link(c, p) == link_error::none);
    // Pending only: the current order is still just the parent.
    LUMEN_CHECK(f->tree.order(p)->count == 1);
    LUMEN_CHECK(f->tree.order_pending(p)->count == 2);
    LUMEN_CHECK(f->tree.order_pending(p)->ids[1] == c);
    // Cached by the parent's commit, current once that is applied.
    f->tree.cache_child_state(p);
    LUMEN_CHECK(f->tree.order(p)->count == 1);
    f->tree.apply_child_state(p);
    LUMEN_CHECK(f->tree.order(p)->count == 2);
    LUMEN_CHECK(f->tree.order(p)->ids[0] == p && f->tree.order(p)->ids[1] == c);
}

LUMEN_TEST(tree_position_is_pending_then_cached_then_current) {
    auto f = make_fixture();
    const auto p = f->make(1), c = f->make(2);
    LUMEN_CHECK(f->tree.link(c, p) == link_error::none);
    f->tree.set_position_pending(c, {5, -7});
    LUMEN_CHECK(f->tree.position(c).x == 0 && f->tree.position(c).y == 0);
    f->tree.cache_child_state(p);
    // A later set_position stays pending: it belongs to the parent's next commit.
    f->tree.set_position_pending(c, {9, 9});
    LUMEN_CHECK(f->tree.position(c).x == 0);
    f->tree.apply_child_state(p);
    LUMEN_CHECK(f->tree.position(c).x == 5 && f->tree.position(c).y == -7);
    f->commit_apply(p);
    LUMEN_CHECK(f->tree.position(c).x == 9 && f->tree.position(c).y == 9);
}

// ── restacking ───────────────────────────────────────────────────────────────

LUMEN_TEST(tree_place_errors_and_order) {
    auto f = make_fixture();
    const auto p = f->make(1), a = f->make(2), b = f->make(3), other = f->make(4);
    const auto stranger = f->make(5);
    LUMEN_CHECK(f->tree.link(a, p) == link_error::none);
    LUMEN_CHECK(f->tree.link(b, p) == link_error::none);
    LUMEN_CHECK(f->tree.link(stranger, other) == link_error::none);
    // Itself, a non-sibling, an unrelated surface: bad_surface.
    LUMEN_CHECK(f->tree.place(a, a, true) == restack_error::bad_surface);
    LUMEN_CHECK(f->tree.place(a, stranger, true) == restack_error::bad_surface);
    LUMEN_CHECK(f->tree.place(a, other, false) == restack_error::bad_surface);
    // Pending order is self, a, b. a below the parent: a, self, b.
    LUMEN_CHECK(f->tree.place(a, p, false) == restack_error::none);
    const auto* o = f->tree.order_pending(p);
    LUMEN_CHECK(o->count == 3 && o->ids[0] == a && o->ids[1] == p && o->ids[2] == b);
    // b below a: b, a, self.
    LUMEN_CHECK(f->tree.place(b, a, false) == restack_error::none);
    LUMEN_CHECK(o->ids[0] == b && o->ids[1] == a && o->ids[2] == p);
    // a above the parent: b, self, a.
    LUMEN_CHECK(f->tree.place(a, p, true) == restack_error::none);
    LUMEN_CHECK(o->ids[0] == b && o->ids[1] == p && o->ids[2] == a);
    // Current order unchanged until the parent's state is applied.
    LUMEN_CHECK(f->tree.order(p)->count == 1);
    f->commit_apply(p);
    LUMEN_CHECK(f->tree.order(p)->ids[0] == b && f->tree.order(p)->ids[2] == a);
}

LUMEN_TEST(tree_place_on_an_orphan_is_inert) {
    auto f = make_fixture();
    const auto p = f->make(1), a = f->make(2), b = f->make(3);
    LUMEN_CHECK(f->tree.link(a, p) == link_error::none);
    LUMEN_CHECK(f->tree.link(b, p) == link_error::none);
    f->tree.remove_surface(p);
    // The parent is gone: requests on the wl_subsurface do nothing, and raise no error.
    LUMEN_CHECK(f->tree.place(a, b, true) == restack_error::none);
    LUMEN_CHECK(f->tree.parent_of(a) == lx::surface_id{});
    LUMEN_CHECK(f->tree.linked(a));
}

// ── synchronization ──────────────────────────────────────────────────────────

LUMEN_TEST(tree_effective_sync_through_nesting) {
    auto f = make_fixture();
    const auto r = f->make(1), c = f->make(2), g = f->make(3);
    LUMEN_CHECK(f->tree.make_root(r, root_kind::toplevel));
    LUMEN_CHECK(!f->tree.effectively_sync(r)); // roots never are
    LUMEN_CHECK(f->tree.link(c, r) == link_error::none);
    LUMEN_CHECK(f->tree.link(g, c) == link_error::none);
    LUMEN_CHECK(f->tree.effectively_sync(c)); // initially synchronized
    f->tree.set_sync(g, false);
    LUMEN_CHECK(f->tree.effectively_sync(g)); // desync, but its parent is sync
    f->tree.set_sync(c, false);
    LUMEN_CHECK(!f->tree.effectively_sync(c));
    LUMEN_CHECK(!f->tree.effectively_sync(g));
    f->tree.set_sync(g, true);
    LUMEN_CHECK(f->tree.effectively_sync(g));
    // Unlinked: no parent to synchronize with.
    f->tree.unlink(c);
    LUMEN_CHECK(!f->tree.effectively_sync(c));
    LUMEN_CHECK(f->tree.effectively_sync(g)); // still synchronized to c itself
}

LUMEN_TEST(tree_sync_children_in_pending_order_without_desync_ones) {
    auto f = make_fixture();
    const auto p = f->make(1), a = f->make(2), b = f->make(3), c = f->make(4);
    LUMEN_CHECK(f->tree.make_root(p, root_kind::toplevel));
    for (auto k : {a, b, c})
        LUMEN_CHECK(f->tree.link(k, p) == link_error::none);
    f->tree.set_sync(b, false);
    LUMEN_CHECK(f->tree.place(c, a, false) == restack_error::none); // self, c, a, b
    lx::surface_id out[8];
    const unsigned n = f->tree.sync_children(p, out, 8);
    LUMEN_CHECK(n == 2 && out[0] == c && out[1] == a);
}

LUMEN_TEST(tree_preorder_visits_parents_before_children) {
    auto f = make_fixture();
    const auto r = f->make(1), a = f->make(2), a1 = f->make(3), b = f->make(4);
    LUMEN_CHECK(f->tree.link(a, r) == link_error::none);
    LUMEN_CHECK(f->tree.link(a1, a) == link_error::none);
    LUMEN_CHECK(f->tree.link(b, r) == link_error::none);
    lx::surface_id seen[8];
    unsigned n = 0;
    f->tree.for_each_preorder(r, [&](lx::surface_id s) {
        seen[n++] = s;
        return true;
    });
    LUMEN_CHECK(n == 4 && seen[0] == r && seen[1] == a && seen[2] == a1 && seen[3] == b);
    // Not descending prunes the subtree.
    n = 0;
    f->tree.for_each_preorder(r, [&](lx::surface_id s) {
        seen[n++] = s;
        return !(s == a);
    });
    LUMEN_CHECK(n == 3 && seen[2] == b);
}

// ── mapping and the scene ────────────────────────────────────────────────────

LUMEN_TEST(tree_maps_children_with_content_under_a_mapped_root) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto a = f->make(2), b = f->make(3), a1 = f->make(4);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(a, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(b, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(a1, a) == link_error::none);
    f->tree.set_position_pending(a, {10, -20});
    f->tree.set_position_pending(b, {-5, 0});
    f->tree.set_position_pending(a1, {1, 2});
    // b paints under the root; a1 under a (a1 below a's self marker).
    LUMEN_CHECK(f->tree.place(b, f->root, false) == restack_error::none);
    LUMEN_CHECK(f->tree.place(a1, a, false) == restack_error::none);
    f->content(f->root, 100, 80);
    f->content(a, 30, 20);
    f->content(a1, 4, 4);
    // b has no content: unmapped, and so would its children be.
    f->commit_apply(a);
    f->commit_apply(f->root);

    f->tree.set_root_origin(f->root, {200, 300});
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));
    LUMEN_CHECK(f->layer.child_count() == 1);
    LUMEN_CHECK(f->tree.mapped(f->root) && f->tree.mapped(a) && f->tree.mapped(a1));
    LUMEN_CHECK(!f->tree.mapped(b));
    // Flattened paint order: (b skipped) root, a1, a.
    auto* box = f->tree.container(f->root);
    LUMEN_CHECK(box->child_count() == 3);
    LUMEN_CHECK(f->painted(0) == f->root && f->painted(1) == a1 && f->painted(2) == a);
    // Global origins: root + accumulated positions.
    LUMEN_CHECK(same(f->surfaces.node_rect(f->root), {200, 300, 100, 80}));
    LUMEN_CHECK(same(f->surfaces.node_rect(a), {210, 280, 30, 20}));
    LUMEN_CHECK(same(f->surfaces.node_rect(a1), {211, 282, 4, 4}));

    // b gets content: mapped at its place in the order, under the root.
    f->content(b, 6, 6);
    f->tree.sync_scene(f->root, "commit");
    LUMEN_CHECK(f->tree.mapped(b));
    LUMEN_CHECK(f->painted(0) == b && f->painted(1) == f->root);
    LUMEN_CHECK(same(f->surfaces.node_rect(b), {195, 300, 6, 6}));

    // A null buffer on a unmaps it and, recursively, a1.
    f->tree.set_has_buffer(a, false);
    f->tree.sync_scene(f->root, "commit");
    LUMEN_CHECK(!f->tree.mapped(a) && !f->tree.mapped(a1));
    LUMEN_CHECK(f->surfaces.node(a) == nullptr && f->surfaces.node(a1) == nullptr);
    LUMEN_CHECK(box->child_count() == 2);
}

LUMEN_TEST(tree_children_committed_before_the_root_maps_show_with_it) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto c = f->make(2);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(c, f->root) == link_error::none);
    f->content(c, 10, 10);
    f->content(f->root, 50, 50);
    f->commit_apply(f->root);
    // Nothing is drawn before the root maps…
    LUMEN_CHECK(f->surfaces.node(c) == nullptr);
    // …and the child is there in the very sync that maps it.
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));
    LUMEN_CHECK(f->surfaces.node(c) != nullptr);
}

LUMEN_TEST(tree_root_origin_moves_every_node) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto c = f->make(2);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(c, f->root) == link_error::none);
    f->tree.set_position_pending(c, {0, -30});
    f->content(f->root, 40, 40);
    f->content(c, 40, 30);
    f->commit_apply(f->root);
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));
    f->tree.set_root_origin(f->root, {100, 100});
    LUMEN_CHECK(same(f->surfaces.node_rect(f->root), {100, 100, 40, 40}));
    LUMEN_CHECK(same(f->surfaces.node_rect(c), {100, 70, 40, 30}));
    // A dirty tree only records the origin; the sync that follows places every node.
    f->tree.mark_dirty(c);
    f->tree.set_root_origin(f->root, {10, 20});
    f->tree.sync_scene(f->root, "move");
    LUMEN_CHECK(same(f->surfaces.node_rect(f->root), {10, 20, 40, 40}));
    LUMEN_CHECK(same(f->surfaces.node_rect(c), {10, -10, 40, 30}));
}

LUMEN_TEST(tree_bbox_reaches_negative_positions) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto title = f->make(2), left = f->make(3), hidden = f->make(4);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(title, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(left, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(hidden, f->root) == link_error::none);
    f->tree.set_position_pending(title, {0, -26});
    f->tree.set_position_pending(left, {-5, -26});
    f->tree.set_position_pending(hidden, {-500, -500});
    f->content(f->root, 696, 468);
    f->content(title, 696, 26);
    f->content(left, 5, 494);
    f->commit_apply(f->root);
    // `hidden` has no content and does not count.
    LUMEN_CHECK(same(f->tree.subtree_bbox(f->root), {-5, -26, 701, 494}));
    // No content on the root: nothing is mapped, so no box.
    f->tree.set_has_buffer(f->root, false);
    LUMEN_CHECK(same(f->tree.subtree_bbox(f->root), {}));
}

LUMEN_TEST(tree_hit_follows_paint_order_and_input_regions) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto strip = f->make(2), over = f->make(3);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(strip, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(over, f->root) == link_error::none);
    f->tree.set_position_pending(over, {80, -10}); // sticks out above-right of the root
    f->content(f->root, 100, 100);
    f->content(strip, 100, 20);
    f->content(over, 40, 40);
    lx::compositor::input_region none{};
    none.reset_empty();
    f->surfaces.set_input_region(strip, none); // takes no input
    f->commit_apply(f->root);
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));

    lx::compositor::tree_hit h{};
    // On the strip: input passes through to the root below.
    LUMEN_CHECK(f->tree.hit(f->root, 10.5, 5.5, h) && h.surface == f->root);
    // On `over`, inside the root too: the topmost wins, in its own coordinates.
    LUMEN_CHECK(f->tree.hit(f->root, 85.0, 5.0, h) && h.surface == over);
    LUMEN_CHECK(h.sx == 5.0 && h.sy == 15.0);
    // On `over` outside the root's extents: not clipped to the parent.
    LUMEN_CHECK(f->tree.hit(f->root, 110.0, -5.0, h) && h.surface == over);
    // Outside everything.
    LUMEN_CHECK(!f->tree.hit(f->root, 300.0, 300.0, h));
    // Unmapped root: nothing.
    f->tree.unmap_root(f->root);
    LUMEN_CHECK(!f->tree.hit(f->root, 10.0, 10.0, h));
    LUMEN_CHECK(f->layer.child_count() == 0);
}

// ── destruction in any order ─────────────────────────────────────────────────

LUMEN_TEST(tree_unlink_unmaps_and_forgets_position) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto c = f->make(2), g = f->make(3);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(c, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(g, c) == link_error::none);
    f->tree.set_position_pending(c, {3, 4});
    f->content(f->root, 10, 10);
    f->content(c, 5, 5);
    f->content(g, 2, 2);
    f->commit_apply(c);
    f->commit_apply(f->root);
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));
    LUMEN_CHECK(f->surfaces.node(g) != nullptr);
    const unsigned hooks = f->unmapped_hook_calls;
    f->tree.unlink(c);
    LUMEN_CHECK(f->surfaces.node(c) == nullptr && f->surfaces.node(g) == nullptr);
    LUMEN_CHECK(f->unmapped_hook_calls == hooks + 2);
    LUMEN_CHECK(f->tree.order(f->root)->count == 1);
    LUMEN_CHECK(f->tree.position(c).x == 0 && f->tree.position(c).y == 0);
    // g stays c's child; c can be linked again.
    LUMEN_CHECK(f->tree.parent_of(g) == c);
    LUMEN_CHECK(f->tree.link(c, f->root) == link_error::none);
}

LUMEN_TEST(tree_parent_destroyed_first_orphans_children) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto mid = f->make(2), leaf = f->make(3);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(mid, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(leaf, mid) == link_error::none);
    f->content(f->root, 10, 10);
    f->content(mid, 5, 5);
    f->content(leaf, 2, 2);
    f->commit_apply(mid);
    f->commit_apply(f->root);
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));

    // The middle surface goes first: its child is orphaned and unmapped, its own parent
    // keeps drawing.
    f->tree.remove_surface(mid);
    LUMEN_CHECK(!f->tree.contains(mid));
    LUMEN_CHECK(f->tree.parent_of(leaf) == lx::surface_id{});
    LUMEN_CHECK(f->tree.linked(leaf)); // its wl_subsurface still lives, inert
    LUMEN_CHECK(!f->tree.effectively_sync(leaf));
    LUMEN_CHECK(f->surfaces.node(leaf) == nullptr);
    LUMEN_CHECK(f->tree.order(f->root)->count == 1);
    LUMEN_CHECK(f->surfaces.node(f->root) != nullptr);
    // Then the root, mapped: its container leaves the layer.
    f->tree.remove_surface(f->root);
    LUMEN_CHECK(f->layer.child_count() == 0);
    // Then the orphan's wl_subsurface and surface, in that order.
    f->tree.unlink(leaf);
    f->tree.remove_surface(leaf);
    LUMEN_CHECK(!f->tree.contains(leaf));
}

LUMEN_TEST(tree_child_destroyed_first_leaves_the_parent_intact) {
    auto f = make_fixture();
    f->root = f->make(1);
    const auto a = f->make(2), b = f->make(3);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    LUMEN_CHECK(f->tree.link(a, f->root) == link_error::none);
    LUMEN_CHECK(f->tree.link(b, f->root) == link_error::none);
    f->content(f->root, 10, 10);
    f->content(a, 5, 5);
    f->content(b, 5, 5);
    f->commit_apply(f->root);
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));
    // A surface destroyed while its wl_subsurface lives (client teardown in id order).
    f->tree.remove_surface(a);
    LUMEN_CHECK(f->tree.order(f->root)->count == 2);
    LUMEN_CHECK(f->tree.order_pending(f->root)->index_of(a) < 0);
    LUMEN_CHECK(f->tree.container(f->root)->child_count() == 2);
    // Unlinking a removed surface is a no-op.
    f->tree.unlink(a);
    f->tree.remove_surface(b);
    f->tree.remove_surface(f->root);
    LUMEN_CHECK(f->layer.child_count() == 0);
}

// ── blending ─────────────────────────────────────────────────────────────────

LUMEN_TEST(surface_node_blends_unless_its_content_is_opaque) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    lx::scene::surface_node node{sid(1)};
    lx::gfx::imported_image img{};
    img.image_id = 7;
    img.width = 4;
    img.height = 4;
    node.attach(img);
    node.set_bounds({0, 0, 4, 4});
    auto out = std::make_unique<lx::scene::draw_list>();
    // ARGB content at full opacity still blends: its transparent pixels are not black.
    node.emit_draws(*out);
    LUMEN_CHECK(out->size() == 1 && out->data()[0].blend == lx::blend_mode::premultiplied);
    out->clear();
    node.set_opaque(true);
    node.emit_draws(*out);
    LUMEN_CHECK(out->size() == 1 && out->data()[0].blend == lx::blend_mode::opaque);
    // Opaque content under partial opacity blends again.
    out->clear();
    node.set_transform({0, 0}, 1.f, 0.5f);
    node.emit_draws(*out);
    LUMEN_CHECK(out->data()[0].blend == lx::blend_mode::premultiplied);
}

LUMEN_TEST(surface_manager_carries_content_opacity_into_its_node) {
    auto f = make_fixture();
    f->root = f->make(1);
    LUMEN_CHECK(f->tree.make_root(f->root, root_kind::toplevel));
    f->content(f->root, 10, 10); // fixture commits opaque = false (the default)
    LUMEN_CHECK(f->tree.map_root(f->root, f->layer, ~0u));
    LUMEN_CHECK(!f->surfaces.node(f->root)->opaque());
    f->surfaces.set_opaque(f->root, true);
    LUMEN_CHECK(f->surfaces.node(f->root)->opaque());
}

// ── the id index ─────────────────────────────────────────────────────────────

LUMEN_TEST(id_slot_index_survives_churn) {
    lx::compositor::id_slot_index<64> index{};
    // Fill to half, erase every other key, refill: every live key stays findable.
    for (unsigned long long k = 1; k <= 32; ++k)
        LUMEN_CHECK(index.insert(k * 64 + 7, static_cast<int>(k)));
    for (unsigned long long k = 1; k <= 32; k += 2)
        index.erase(k * 64 + 7);
    for (unsigned long long k = 2; k <= 32; k += 2)
        LUMEN_CHECK(index.find(k * 64 + 7) == static_cast<int>(k));
    for (unsigned long long k = 1; k <= 32; k += 2)
        LUMEN_CHECK(index.find(k * 64 + 7) == -1);
    for (unsigned long long k = 100; k < 116; ++k)
        LUMEN_CHECK(index.insert(k, static_cast<int>(k)));
    for (unsigned long long k = 100; k < 116; ++k)
        LUMEN_CHECK(index.find(k) == static_cast<int>(k));
    LUMEN_CHECK(index.find(0) == -1);
    LUMEN_CHECK(!index.insert(0, 1));
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
