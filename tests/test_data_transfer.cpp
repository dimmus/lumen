// Data-transfer pieces with no Wayland in them: the inline mime list every selection and
// drag source keeps, and drag-and-drop action negotiation (wl_data_device_manager
// dnd_action), including the modifier-chosen user action and version-2 sides.
#include "lumen_test.hpp"

import std;
import lx.compositor;

using lx::compositor::dnd_ask;
using lx::compositor::dnd_copy;
using lx::compositor::dnd_move;
using lx::compositor::dnd_none;
using lx::compositor::mime_list;
using lx::compositor::negotiate_dnd_action;
using lx::compositor::single_dnd_action;
using lx::compositor::user_dnd_action;
using lx::compositor::valid_dnd_mask;

namespace {
// mime_list is 4 KiB: keep instances off the test's stack frames.
mime_list g_list;
} // namespace

LUMEN_TEST(mime_list_add_contains_order) {
    g_list.clear();
    LUMEN_CHECK(g_list.count() == 0);
    LUMEN_CHECK(g_list.add("text/plain;charset=utf-8"));
    LUMEN_CHECK(g_list.add("UTF8_STRING"));
    LUMEN_CHECK(g_list.add("text/plain"));
    LUMEN_CHECK(g_list.count() == 3);
    LUMEN_CHECK(std::strcmp(g_list.at(0), "text/plain;charset=utf-8") == 0);
    LUMEN_CHECK(std::strcmp(g_list.at(2), "text/plain") == 0);
    LUMEN_CHECK(g_list.at(3) == nullptr);
    LUMEN_CHECK(g_list.contains("UTF8_STRING"));
    // Exact match only: a prefix of an offered type is not offered.
    LUMEN_CHECK(!g_list.contains("text/plain;charset"));
    LUMEN_CHECK(!g_list.contains("text/html"));
    LUMEN_CHECK(!g_list.contains(nullptr));
}

LUMEN_TEST(mime_list_duplicates_and_empty) {
    g_list.clear();
    LUMEN_CHECK(g_list.add("text/plain"));
    LUMEN_CHECK(g_list.add("text/plain")); // accepted, not stored twice
    LUMEN_CHECK(g_list.count() == 1);
    LUMEN_CHECK(!g_list.add(""));
    LUMEN_CHECK(!g_list.add(nullptr));
    LUMEN_CHECK(g_list.count() == 1);
}

LUMEN_TEST(mime_list_overflow_by_count_and_bytes) {
    g_list.clear();
    char name[32];
    for (unsigned i = 0; i < mime_list::k_max; ++i) {
        std::snprintf(name, sizeof(name), "type/%u", i);
        LUMEN_CHECK(g_list.add(name));
    }
    LUMEN_CHECK(!g_list.add("type/one-too-many"));
    LUMEN_CHECK(g_list.count() == mime_list::k_max);
    LUMEN_CHECK(g_list.contains("type/63"));

    // Bytes: a string that does not fit what is left is refused; earlier ones stay intact.
    g_list.clear();
    static char big[3000];
    std::memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    LUMEN_CHECK(g_list.add(big));
    big[0] = 'b';
    LUMEN_CHECK(!g_list.add(big)); // 3000 more bytes do not fit in the 1096 left
    LUMEN_CHECK(g_list.add("text/plain"));
    LUMEN_CHECK(g_list.count() == 2);
    LUMEN_CHECK(g_list.at(0)[0] == 'a' && std::strlen(g_list.at(0)) == sizeof(big) - 1);

    // A reset is a counter reset: the list is reusable at full capacity.
    g_list.clear();
    LUMEN_CHECK(g_list.count() == 0 && !g_list.contains("text/plain"));
    LUMEN_CHECK(g_list.add(big));
}

LUMEN_TEST(dnd_mask_validation) {
    LUMEN_CHECK(valid_dnd_mask(0) && valid_dnd_mask(7) && valid_dnd_mask(dnd_ask));
    LUMEN_CHECK(!valid_dnd_mask(8) && !valid_dnd_mask(0x80000000u));
    LUMEN_CHECK(single_dnd_action(0) && single_dnd_action(dnd_copy) &&
                single_dnd_action(dnd_move) && single_dnd_action(dnd_ask));
    LUMEN_CHECK(!single_dnd_action(dnd_copy | dnd_move) && !single_dnd_action(8) &&
                !single_dnd_action(7));
}

LUMEN_TEST(dnd_user_action_from_modifiers) {
    LUMEN_CHECK(user_dnd_action(false, false, false) == dnd_none);
    LUMEN_CHECK(user_dnd_action(true, false, false) == dnd_move);
    LUMEN_CHECK(user_dnd_action(false, true, false) == dnd_copy);
    LUMEN_CHECK(user_dnd_action(false, false, true) == dnd_ask);
    // Checked in order: Shift wins over Ctrl, Ctrl over Alt.
    LUMEN_CHECK(user_dnd_action(true, true, true) == dnd_move);
    LUMEN_CHECK(user_dnd_action(false, true, true) == dnd_copy);
}

LUMEN_TEST(dnd_negotiation_table) {
    const unsigned cm = dnd_copy | dnd_move;
    const unsigned all = dnd_copy | dnd_move | dnd_ask;
    struct row {
        unsigned source, offer, preferred, user, want;
    };
    const row rows[] = {
        // Nothing shared → none, whatever is preferred or held.
        {dnd_copy, dnd_move, dnd_move, dnd_none, dnd_none},
        {0, all, dnd_copy, dnd_copy, dnd_none},
        {all, 0, 0, dnd_move, dnd_none},
        // The destination's preferred action, when both sides allow it.
        {cm, cm, dnd_move, dnd_none, dnd_move},
        {cm, cm, dnd_copy, dnd_none, dnd_copy},
        // Preferred not available → lowest shared bit.
        {cm, cm, dnd_ask, dnd_none, dnd_copy},
        {dnd_move | dnd_ask, all, dnd_copy, dnd_none, dnd_move},
        // No preference → lowest shared bit.
        {all, all, dnd_none, dnd_none, dnd_copy},
        {dnd_ask, all, dnd_none, dnd_none, dnd_ask},
        // The user's modifier wins when available …
        {cm, cm, dnd_move, dnd_copy, dnd_copy},
        {all, all, dnd_copy, dnd_ask, dnd_ask},
        // … and is ignored when not.
        {cm, cm, dnd_move, dnd_ask, dnd_move},
        {dnd_copy, cm, dnd_move, dnd_move, dnd_copy},
        // A version-2 side counts as copy (the caller passes dnd_copy).
        {dnd_copy, cm, dnd_move, dnd_none, dnd_copy},
        {cm, dnd_copy, dnd_none, dnd_move, dnd_copy},
        {dnd_copy, dnd_copy, dnd_none, dnd_none, dnd_copy},
        // Undefined bits are never negotiated.
        {8 | dnd_copy, 8 | dnd_copy, 8, 8, dnd_copy},
    };
    for (const row& r : rows) {
        const unsigned got = negotiate_dnd_action(r.source, r.offer, r.preferred, r.user);
        if (got != r.want)
            std::printf("    source=%u offer=%u preferred=%u user=%u: got %u want %u\n",
                        r.source, r.offer, r.preferred, r.user, got, r.want);
        LUMEN_CHECK(got == r.want);
    }
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
