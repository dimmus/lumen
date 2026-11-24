#include "lumen_test.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

import std;
import lx.runtime;

// The event loop's source table is process-wide and fixed-size (event_loop::
// source_capacity()) while event_loops come and go — the compositor creates one per
// start() in one process. A loop that dies must return the slots it filled: otherwise a
// later loop inherits a table of dead sources (dispatch walks them) and its add_source
// silently drops once the table is full — the "no event source slot for theme
// completion" start failure. No test here runs a loop: the add/remove/destroy
// bookkeeping is the behavior under test, and a leaked slot's source is often already
// destroyed (declared after its loop), which run() would dispatch through.

LUMEN_TEST(remove_source_still_frees_a_slot_while_the_loop_lives) {
    lx::runtime::event_loop loop;
    lx::runtime::timer_source first{1'000'000, {}};
    lx::runtime::timer_source second{1'000'000, {}};
    loop.add_source(&first);
    loop.add_source(&second);
    LUMEN_CHECK(loop.source_count() == 2);
    loop.remove_source(&first);
    LUMEN_CHECK(loop.source_count() == 1);
    // A removed slot is reusable right away: the re-add lands again.
    loop.add_source(&first);
    LUMEN_CHECK(loop.source_count() == 2);
}

LUMEN_TEST(a_destroyed_loop_returns_its_source_table_slots) {
    // Sources declared after their loop die first: the destructor reclaims slots whose
    // sources are already gone, so it must match them without touching them.
    {
        lx::runtime::event_loop early;
        lx::runtime::timer_source first{1'000'000, {}};
        lx::runtime::timer_source second{1'000'000, {}};
        early.add_source(&first);
        early.add_source(&second);
        LUMEN_CHECK(early.source_count() == 2);
    }
    lx::runtime::event_loop later;
    // The table is process-wide: only early's destruction can have returned those two
    // slots. On the leak, later inherits them — and would dispatch them.
    LUMEN_CHECK(later.source_count() == 0);
}

LUMEN_TEST(a_dying_loop_does_not_take_a_live_loops_sources) {
    lx::runtime::timer_source kept{1'000'000, {}};
    lx::runtime::event_loop keeper;
    keeper.add_source(&kept);
    {
        lx::runtime::event_loop victim;
        lx::runtime::timer_source doomed{1'000'000, {}};
        victim.add_source(&doomed);
        LUMEN_CHECK(keeper.source_count() == 2);
    }
    // keeper's own source survives victim's death: the table is shared, so a dying loop
    // reclaims exactly its own slots, never the table's.
    LUMEN_CHECK(keeper.source_count() == 1);
    keeper.remove_source(&kept);
    LUMEN_CHECK(keeper.source_count() == 0);
}

LUMEN_TEST(a_full_table_must_not_be_inherited_from_dead_loops) {
    // Exhaust the table across loop lifetimes: capacity()'s worth of loops, four sources
    // each. Only if each loop's death returns its slots does every add land — otherwise
    // add_source's table-full branch silently drops, the in-process restart failure.
    unsigned rounds = 1;
    {
        lx::runtime::event_loop probe;
        rounds = probe.source_capacity() / 4;
    }
    for (unsigned round = 0; round < rounds; ++round) {
        lx::runtime::event_loop loop;
        lx::runtime::timer_source s0{1'000'000, {}};
        lx::runtime::timer_source s1{1'000'000, {}};
        lx::runtime::timer_source s2{1'000'000, {}};
        lx::runtime::timer_source s3{1'000'000, {}};
        for (lx::runtime::event_source* source : {&s0, &s1, &s2, &s3})
            loop.add_source(source);
        LUMEN_CHECK(loop.source_count() == 4);
    }
    lx::runtime::event_loop fresh;
    LUMEN_CHECK(fresh.source_count() == 0);
    lx::runtime::timer_source extra{1'000'000, {}};
    fresh.add_source(&extra);
    // The slot existed to take: the add did not silently drop.
    LUMEN_CHECK(fresh.source_count() == 1);
}

LUMEN_TEST(a_source_outliving_its_loop_joins_a_later_one) {
    // An fd source whose loop dies while it lives: the slot comes back, so the later
    // loop takes it fresh — one source, one slot, never a duplicate.
    lx::runtime::fd_source watched{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), {}};
    LUMEN_CHECK(watched.poll_fd() >= 0);
    {
        lx::runtime::event_loop first;
        first.add_source(&watched);
        LUMEN_CHECK(first.source_count() == 1);
    }
    lx::runtime::event_loop second;
    second.add_source(&watched);
    LUMEN_CHECK(second.source_count() == 1);
    second.remove_source(&watched);
    LUMEN_CHECK(second.source_count() == 0);
    ::close(watched.poll_fd());
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
