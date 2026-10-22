// xdg-toplevel-icon-v1's icon store and xdg-foreign's handle registry (design-app-compat-
// capture B.2, B.4), Wayland-free: which buffers an icon keeps (edge cap, nearest the panel's
// 32 px, the pixel budget), the sealed memfds and their content (XRGB made opaque), the
// reference counts, and handles — 32 hex digits, unique, withdrawn on removal.
#include "lumen_test.hpp"


#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.compositor;

using namespace lx::compositor;

LUMEN_TEST(selection_prefers_the_panel_size_and_caps_edges) {
    const icon_candidate in[] = {{512, 1}, {16, 1}, {48, 1}, {32, 1}, {256, 1}, {24, 1}, {0, 1}};
    unsigned out[k_icon_max_images]{};
    const unsigned n = select_icon_images(in, 7, k_icon_pixel_budget, out);
    // 512 and 0 dropped; ranked by distance to 32: 32, 24, 48, 16 (256 is fifth).
    LUMEN_CHECK(n == 4);
    LUMEN_CHECK(in[out[0]].size == 32 && in[out[1]].size == 24 && in[out[2]].size == 48 &&
                in[out[3]].size == 16);
    // Equal distance: the larger edge first.
    const icon_candidate tie[] = {{24, 1}, {40, 1}};
    LUMEN_CHECK(select_icon_images(tie, 2, k_icon_pixel_budget, out) == 2 && tie[out[0]].size == 40);
}

LUMEN_TEST(selection_under_a_spent_budget_keeps_one_or_none) {
    const icon_candidate in[] = {{256, 1}, {32, 1}, {128, 1}};
    unsigned out[k_icon_max_images]{};
    // Room for the 32 px one only.
    LUMEN_CHECK(select_icon_images(in, 3, 32 * 32 * 4, out) == 1 && in[out[0]].size == 32);
    LUMEN_CHECK(select_icon_images(in, 3, 100, out) == 0);
}

LUMEN_TEST(table_copies_into_sealed_memfds_and_counts_references) {
    toplevel_icon_table t{};
    const unsigned id = t.create("org.example.App");
    LUMEN_CHECK(id != 0 && t.live() == 1);
    std::vector<std::uint32_t> px(16 * 20, 0x00123456u); // stride 20 px
    LUMEN_CHECK(t.add_image(id, px.data(), 20 * 4, 16, 2, true));
    LUMEN_CHECK(!t.add_image(id, px.data(), 8, 16, 1, false)); // stride too small
    LUMEN_CHECK(!t.add_image(id, px.data(), 20 * 4, k_icon_max_edge + 1, 1, false));
    const icon_entry* e = t.get(id);
    LUMEN_CHECK(e && e->count == 1 && e->images[0].size == 16 && e->images[0].scale == 2);
    LUMEN_CHECK(t.pixel_bytes() == 16 * 16 * 4);
    const int fd = e->images[0].fd;
    const int seals = fcntl(fd, F_GET_SEALS);
    LUMEN_CHECK(seals >= 0 && (seals & F_SEAL_WRITE) && (seals & F_SEAL_GROW) &&
                (seals & F_SEAL_SHRINK) && (seals & F_SEAL_SEAL));
    // Writable mappings are refused; the content is the image, opaque.
    LUMEN_CHECK(mmap(nullptr, 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED);
    auto* p = static_cast<const std::uint32_t*>(mmap(nullptr, 16 * 16 * 4, PROT_READ, MAP_SHARED, fd, 0));
    LUMEN_CHECK(p != MAP_FAILED && p[0] == 0xFF123456u && p[16 * 16 - 1] == 0xFF123456u);
    if (p != MAP_FAILED)
        munmap(const_cast<std::uint32_t*>(p), 16 * 16 * 4);
    t.ref(id);
    t.unref(id);
    LUMEN_CHECK(t.get(id) != nullptr);
    t.unref(id);
    LUMEN_CHECK(t.get(id) == nullptr && t.live() == 0 && t.pixel_bytes() == 0);
    LUMEN_CHECK(fcntl(fd, F_GETFD) == -1); // closed with the last reference
}

LUMEN_TEST(table_is_bounded) {
    toplevel_icon_table t{};
    unsigned last = 0;
    for (unsigned i = 0; i < k_icon_table_max; ++i)
        last = t.create("x");
    LUMEN_CHECK(last != 0 && t.create("one too many") == 0);
    // The budget: a full table of 256 px images stops at 16 MiB.
    std::vector<std::uint32_t> px(256 * 256, 0xFFFFFFFFu);
    unsigned added = 0;
    for (unsigned id = 1; id <= k_icon_table_max; ++id)
        added += t.add_image(id, px.data(), 256 * 4, 256, 1, false) ? 1u : 0u;
    LUMEN_CHECK(added == 64 && t.pixel_bytes() == k_icon_pixel_budget && t.budget_left() == 0);
}

LUMEN_TEST(foreign_handles_are_random_hex_and_withdrawn) {
    foreign_registry r{};
    char a[k_foreign_handle_bytes]{}, b[k_foreign_handle_bytes]{};
    int owner = 0;
    const unsigned ia = r.add(lx::toplevel_id{7}, &owner, a);
    const unsigned ib = r.add(lx::toplevel_id{8}, &owner, b);
    LUMEN_CHECK(ia && ib && ia != ib && std::strlen(a) == 32 && std::strcmp(a, b) != 0);
    bool hex = true;
    for (char ch : a)
        hex = hex && (ch == '\0' || (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'));
    LUMEN_CHECK(hex);
    LUMEN_CHECK(r.find(a) == ia && r.get(ia)->toplevel == lx::toplevel_id{7});
    LUMEN_CHECK(r.find("short") == 0 && r.find(nullptr) == 0);
    r.remove(ia);
    LUMEN_CHECK(r.find(a) == 0 && r.find(b) == ib && r.count() == 1);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
