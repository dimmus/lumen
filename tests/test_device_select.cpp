// Which Vulkan device composites (`lx.gfx` `choose_physical_device`): the one behind the KMS
// card, matched on VK_EXT_physical_device_drm's primary (or render) node, so a two-GPU
// machine composites where it scans out. Laid out as real machines enumerate: a hybrid
// laptop whose loader lists the discrete GPU first while the panel is the integrated GPU's;
// a desktop with two discrete GPUs; drivers without the extension; a display-only card.
#include "lumen_test.hpp"

import std;
import lx.gfx;

namespace {

using lx::gfx::device_match;
using lx::gfx::drm_node;
using lx::gfx::physical_device_desc;
using lx::gfx::physical_device_kind;
using lx::gfx::physical_device_list;

constexpr unsigned k_drm = 226;

/// A GPU with VK_EXT_physical_device_drm: cardN primary, renderD(128+N) render node.
physical_device_desc gpu(physical_device_kind kind, unsigned card) {
    return {.kind = kind,
            .has_drm = true,
            .primary = {true, k_drm, card},
            .render = {true, k_drm, 128 + card}};
}

physical_device_desc no_ext(physical_device_kind kind) { return {.kind = kind}; }

/// lavapipe: no DRM device behind it, and no VK_EXT_physical_device_drm.
physical_device_desc lavapipe() { return no_ext(physical_device_kind::cpu); }

physical_device_list list_of(std::initializer_list<physical_device_desc> devices) {
    physical_device_list l{};
    for (const auto& d : devices)
        l.devices[l.count++] = d;
    return l;
}

drm_node card(unsigned minor) { return {true, k_drm, minor}; }

} // namespace

LUMEN_TEST(device_hybrid_laptop_composites_on_the_panel_gpu) {
    // NVIDIA enumerated first, the eDP panel on the Intel card1.
    const auto l = list_of({gpu(physical_device_kind::discrete_gpu, 0),
                            gpu(physical_device_kind::integrated_gpu, 1)});
    const auto c = lx::gfx::choose_physical_device(l, card(1));
    LUMEN_CHECK(c.index == 1);
    LUMEN_CHECK(c.match == device_match::primary_node);
    LUMEN_CHECK(lx::gfx::device_selector::probe_tier(l, card(1)) ==
                lx::gfx::gpu_tier::integrated);
    // The same machine with the display on the discrete GPU's HDMI.
    LUMEN_CHECK(lx::gfx::choose_physical_device(l, card(0)).index == 0);
    LUMEN_CHECK(lx::gfx::device_selector::probe_tier(l, card(0)) ==
                lx::gfx::gpu_tier::discrete);
}

LUMEN_TEST(device_two_discrete_gpus) {
    const auto l = list_of({gpu(physical_device_kind::discrete_gpu, 1),
                            gpu(physical_device_kind::discrete_gpu, 2)});
    const auto c = lx::gfx::choose_physical_device(l, card(2));
    LUMEN_CHECK(c.index == 1 && c.match == device_match::primary_node);
}

LUMEN_TEST(device_only_gpu_on_card1_is_discrete_by_type) {
    // simpledrm took card0 and is gone; the one GPU is card1. The old probe called that
    // "discrete" because card1 existed; the type says integrated.
    const auto l = list_of({gpu(physical_device_kind::integrated_gpu, 1), lavapipe()});
    LUMEN_CHECK(lx::gfx::choose_physical_device(l, card(1)).index == 0);
    LUMEN_CHECK(lx::gfx::device_selector::probe_tier(l, card(1)) ==
                lx::gfx::gpu_tier::integrated);
}

LUMEN_TEST(device_render_node_matches) {
    const auto l = list_of({gpu(physical_device_kind::discrete_gpu, 0),
                            gpu(physical_device_kind::integrated_gpu, 1)});
    const auto c = lx::gfx::choose_physical_device(l, card(129));
    LUMEN_CHECK(c.index == 1 && c.match == device_match::render_node);
}

LUMEN_TEST(device_without_the_extension_keeps_the_first_hardware_device) {
    const auto l = list_of({lavapipe(), no_ext(physical_device_kind::discrete_gpu),
                            no_ext(physical_device_kind::integrated_gpu)});
    const auto c = lx::gfx::choose_physical_device(l, card(1));
    LUMEN_CHECK(c.index == 1 && c.match == device_match::no_extension);
    // One device reporting nodes that are not the card's: no match, the extension exists.
    const auto mixed = list_of({no_ext(physical_device_kind::discrete_gpu),
                                gpu(physical_device_kind::integrated_gpu, 0)});
    const auto m = lx::gfx::choose_physical_device(mixed, card(1));
    LUMEN_CHECK(m.index == 0 && m.match == device_match::no_match);
}

LUMEN_TEST(device_display_only_card_matches_nothing) {
    // evdi/DisplayLink or vkms as the card: no Vulkan device behind it.
    const auto l = list_of({gpu(physical_device_kind::integrated_gpu, 0)});
    const auto c = lx::gfx::choose_physical_device(l, card(3));
    LUMEN_CHECK(c.index == 0 && c.match == device_match::no_match);
}

LUMEN_TEST(device_no_card_and_no_devices) {
    const auto l = list_of({lavapipe(), gpu(physical_device_kind::virtual_gpu, 0)});
    const auto c = lx::gfx::choose_physical_device(l, {});
    LUMEN_CHECK(c.index == 1 && c.match == device_match::no_card);
    // Every device a software rasterizer: the first.
    const auto sw = list_of({lavapipe(), lavapipe()});
    LUMEN_CHECK(lx::gfx::choose_physical_device(sw, card(0)).index == 0);
    // An absent node never matches, even with equal numbers.
    auto absent = list_of({gpu(physical_device_kind::discrete_gpu, 0)});
    absent.devices[0].primary.present = false;
    absent.devices[0].render.present = false;
    LUMEN_CHECK(lx::gfx::choose_physical_device(absent, card(0)).match ==
                device_match::no_match);
    const physical_device_list empty{};
    LUMEN_CHECK(lx::gfx::choose_physical_device(empty, card(0)).index == 0);
    LUMEN_CHECK(lx::gfx::device_selector::probe_tier(empty, card(0)) ==
                lx::gfx::gpu_tier::integrated);
}

LUMEN_TEST(device_tier_follows_type) {
    LUMEN_CHECK(lx::gfx::tier_of(physical_device_kind::discrete_gpu) ==
                lx::gfx::gpu_tier::discrete);
    LUMEN_CHECK(lx::gfx::tier_of(physical_device_kind::integrated_gpu) ==
                lx::gfx::gpu_tier::integrated);
    LUMEN_CHECK(lx::gfx::tier_of(physical_device_kind::virtual_gpu) ==
                lx::gfx::gpu_tier::integrated);
    LUMEN_CHECK(lx::gfx::tier_of(physical_device_kind::cpu) == lx::gfx::gpu_tier::integrated);
}

LUMEN_TEST(device_host_devices_match_their_own_nodes) {
    // On this host's Vulkan: every device that reports a primary node is chosen for it.
    // Vacuous without such a device (lavapipe has none).
    const physical_device_list l = lx::gfx::device_selector::probe_devices();
    LUMEN_CHECK(l.count <= lx::gfx::k_max_physical_devices);
    for (unsigned i = 0; i < l.count; ++i) {
        const physical_device_desc& d = l.devices[i];
        if (!d.has_drm || !d.primary.present)
            continue;
        const auto c = lx::gfx::choose_physical_device(l, d.primary);
        LUMEN_CHECK(c.match == device_match::primary_node);
        LUMEN_CHECK(l.devices[c.index].primary.minor == d.primary.minor);
    }
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
