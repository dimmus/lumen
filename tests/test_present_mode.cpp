// Present-mode policy (AGENTS.md §5: "discrete → mailbox, integrated → fifo_relaxed (unless
// overridden)"; docs/invariants.md, the "Present mode" row): `resolve_present_mode` per GPU
// tier, every explicit mode returned unchanged, and `output_manager` resolving an output
// against the compositing device's tier — an explicit per-output mode winning on every tier.
//
// What this drives is the selection. What the frame path does with the selected mode is not
// observable from a test today: `output_manager::present_mode_for` has no caller outside
// output_manager.cppm (the compositor hands over the tier through `set_gpu_tier`; nothing
// reads the mode back).
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.drm;
import lx.gfx;
import lx.compositor;

namespace {

using lx::compositor::output_manager;
using lx::compositor::present_mode;
using lx::gfx::gpu_tier;

// Every tier the enum has, and every mode a caller can name explicitly.
constexpr gpu_tier k_tiers[]{gpu_tier::discrete, gpu_tier::integrated, gpu_tier::embedded};
constexpr present_mode k_explicit[]{present_mode::mailbox, present_mode::fifo,
                                    present_mode::fifo_relaxed};

const char* name(present_mode mode) {
    switch (mode) {
    case present_mode::auto_select:
        return "auto_select";
    case present_mode::mailbox:
        return "mailbox";
    case present_mode::fifo:
        return "fifo";
    case present_mode::fifo_relaxed:
        return "fifo_relaxed";
    }
    return "?";
}

const char* name(gpu_tier tier) {
    switch (tier) {
    case gpu_tier::discrete:
        return "discrete";
    case gpu_tier::integrated:
        return "integrated";
    case gpu_tier::embedded:
        return "embedded";
    }
    return "?";
}

present_mode resolve(present_mode requested, gpu_tier tier) {
    return lx::compositor::resolve_present_mode(requested, tier);
}

/// `LUMEN_CHECK` repeats its expression; a table row has to say which row it was.
void expect(bool ok, const std::string& row) {
    if (!ok)
        throw lumen_test::assertion_error(row);
}

/// Adds one output, which starts in `auto_select` (`output_state::present`), and returns it.
lx::output_id add_auto_output(output_manager& m, const char* connector) {
    lx::compositor::output_desc d{};
    d.name = connector;
    d.mode = {1920, 1080};
    const int slot = m.add_output(d);
    LUMEN_CHECK(slot >= 0);
    const lx::output_id out = m.nth(static_cast<unsigned>(slot));
    LUMEN_CHECK(m.state(out).present == present_mode::auto_select);
    return out;
}

} // namespace

LUMEN_TEST(present_mode_auto_select_is_mailbox_on_discrete) {
    expect(resolve(present_mode::auto_select, gpu_tier::discrete) == present_mode::mailbox,
           "auto_select on a discrete device must be mailbox");
}

LUMEN_TEST(present_mode_auto_select_is_fifo_relaxed_on_integrated) {
    expect(resolve(present_mode::auto_select, gpu_tier::integrated) == present_mode::fifo_relaxed,
           "auto_select on an integrated device must be fifo_relaxed");
}

LUMEN_TEST(present_mode_auto_select_always_resolves) {
    // `embedded` is a third tier (lx.gfx.cppm:39) that neither AGENTS.md §5 nor the invariants
    // row names, so its mode is reported here, not pinned. What is pinned for every tier is
    // that the policy answers with a real mode — never `auto_select` again.
    for (const gpu_tier tier : k_tiers) {
        const present_mode got = resolve(present_mode::auto_select, tier);
        std::printf("    auto_select on %-10s -> %s\n", name(tier), name(got));
        expect(got != present_mode::auto_select,
               std::format("auto_select on {} came back unresolved", name(tier)));
    }
}

LUMEN_TEST(present_mode_explicit_modes_are_returned_unchanged_on_every_tier) {
    for (const gpu_tier tier : k_tiers)
        for (const present_mode mode : k_explicit) {
            const present_mode got = resolve(mode, tier);
            expect(got == mode, std::format("explicit {} on {} came back as {}", name(mode),
                                            name(tier), name(got)));
        }
}

LUMEN_TEST(present_mode_output_manager_follows_the_gpu_tier) {
    output_manager m;
    const lx::output_id out = add_auto_output(m, "DP-1");
    // Integrated until a device says otherwise (`output_manager::gpu_tier_`).
    expect(m.present_mode_for(out) == present_mode::fifo_relaxed,
           "an output with no tier set yet must take the integrated policy");

    m.set_gpu_tier(gpu_tier::discrete);
    expect(m.present_mode_for(out) == present_mode::mailbox,
           "an auto_select output must be mailbox once the tier is discrete");

    m.set_gpu_tier(gpu_tier::integrated);
    expect(m.present_mode_for(out) == present_mode::fifo_relaxed,
           "an auto_select output must be fifo_relaxed once the tier is integrated");

    // The tier is handed over again whenever the graphics device is (re)chosen: the latest
    // call is the one the outputs resolve against.
    m.set_gpu_tier(gpu_tier::discrete);
    expect(m.present_mode_for(out) == present_mode::mailbox,
           "a later set_gpu_tier must replace the earlier tier");
}

LUMEN_TEST(present_mode_output_manager_explicit_mode_wins_on_every_tier) {
    output_manager m;
    const lx::output_id out = add_auto_output(m, "eDP-1");
    for (const gpu_tier tier : k_tiers) {
        m.set_gpu_tier(tier);
        for (const present_mode mode : k_explicit) {
            m.set_present_mode(out, mode);
            const present_mode got = m.present_mode_for(out);
            expect(got == mode, std::format("an explicit {} on {} resolved as {}", name(mode),
                                            name(tier), name(got)));
        }
        // Back in `auto_select`, the tier decides again.
        m.set_present_mode(out, present_mode::auto_select);
        expect(m.present_mode_for(out) == resolve(present_mode::auto_select, tier),
               std::format("auto_select must follow the tier again on {}", name(tier)));
    }
}

LUMEN_TEST(present_mode_output_manager_override_is_per_output) {
    output_manager m;
    const lx::output_id a = add_auto_output(m, "eDP-1");
    const lx::output_id b = add_auto_output(m, "DP-1");
    m.set_gpu_tier(gpu_tier::discrete);
    LUMEN_CHECK(a != b);

    m.set_present_mode(a, present_mode::fifo);
    expect(m.present_mode_for(a) == present_mode::fifo,
           "the output given an explicit mode must keep it");
    expect(m.present_mode_for(b) == present_mode::mailbox,
           "a sibling output in auto_select must still take the tier's policy");

    m.set_gpu_tier(gpu_tier::integrated);
    expect(m.present_mode_for(a) == present_mode::fifo,
           "an explicit mode must outlive a tier change");
    expect(m.present_mode_for(b) == present_mode::fifo_relaxed,
           "a sibling output in auto_select must follow the new tier");
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
