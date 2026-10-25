// Which card the compositor scans out on (`lx.drm:card`): read from a sysfs drm class laid
// out in a temporary directory, the layouts real machines have. simpledrm on card0 with the
// GPU's driver on card1 (nouveau, amdgpu, i915 replacing the firmware framebuffer); a
// render-only card0; a hybrid laptop whose panel is on the integrated GPU; no display at all.
#include "lumen_test.hpp"

#include <stdlib.h>
#include <unistd.h>

import std;
import lx.drm;

namespace {

namespace fs = std::filesystem;

/// A fake /sys/class/drm, removed with the object.
struct fake_sysfs {
    fs::path root;
    fake_sysfs() {
        char tmpl[] = "/tmp/lumen-card-select-XXXXXX";
        root = ::mkdtemp(tmpl);
    }
    ~fake_sysfs() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static void write(const fs::path& p, std::string_view text) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << text << '\n';
    }
    /// cardN with its device (driver link, optional boot_vga) and render node noise.
    void card(unsigned n, const char* driver, int boot_vga = -1) {
        const fs::path dev = root / ("card" + std::to_string(n)) / "device";
        fs::create_directories(dev);
        fs::create_directories(root / "drivers" / driver);
        fs::create_symlink(root / "drivers" / driver, dev / "driver");
        if (boot_vga >= 0)
            write(dev / "boot_vga", boot_vga ? "1" : "0");
    }
    void connector(unsigned n, const char* name, const char* status) {
        write(root / ("card" + std::to_string(n) + "-" + name) / "status", status);
    }
    [[nodiscard]] lx::drm::card_list find() const { return lx::drm::find_cards(root.c_str()); }
};

} // namespace

LUMEN_TEST(card_simpledrm_replaced_by_gpu_driver) {
    // The reported machine: GTX 1060 on nouveau as card1, card0 simpledrm's (still listed
    // with its virtual connector while it lingers).
    fake_sysfs s;
    s.card(0, "simple-framebuffer");
    s.connector(0, "Unknown-1", "connected");
    s.card(1, "nouveau", 1);
    s.connector(1, "DVI-D-1", "disconnected");
    s.connector(1, "HDMI-A-1", "connected");
    s.connector(1, "DP-1", "disconnected");
    fs::create_directories(s.root / "renderD128");
    const lx::drm::card_list l = s.find();
    LUMEN_CHECK(l.count == 2);
    LUMEN_CHECK(std::string_view(l.cards[0].path) == "/dev/dri/card1");
    LUMEN_CHECK(std::string_view(l.cards[0].driver) == "nouveau");
    LUMEN_CHECK(l.cards[0].connectors == 3 && l.cards[0].connected == 1 && l.cards[0].boot_vga);
    LUMEN_CHECK(std::string_view(l.cards[1].path) == "/dev/dri/card0");
}

LUMEN_TEST(card_only_card1) {
    // simpledrm gone: the GPU's card1 alone, card0 never comes back.
    fake_sysfs s;
    s.card(1, "amdgpu", 1);
    s.connector(1, "DP-1", "connected");
    const lx::drm::card_list l = s.find();
    LUMEN_CHECK(l.count == 1);
    LUMEN_CHECK(std::string_view(l.cards[0].path) == "/dev/dri/card1");
}

LUMEN_TEST(card_render_only_is_never_a_display) {
    fake_sysfs s;
    s.card(0, "nvidia", 0); // no connectors: render-only
    s.card(1, "i915", 1);
    s.connector(1, "eDP-1", "connected");
    const lx::drm::card_list l = s.find();
    LUMEN_CHECK(l.count == 1);
    LUMEN_CHECK(std::string_view(l.cards[0].path) == "/dev/dri/card1");
}

LUMEN_TEST(card_hybrid_laptop_prefers_the_connected_display) {
    // The discrete GPU drives no display here; the panel is the integrated GPU's.
    fake_sysfs s;
    s.card(0, "amdgpu", 0);
    s.connector(0, "HDMI-A-1", "disconnected");
    s.card(1, "i915", 1);
    s.connector(1, "eDP-1", "connected");
    const lx::drm::card_list l = s.find();
    LUMEN_CHECK(l.count == 2);
    LUMEN_CHECK(std::string_view(l.cards[0].driver) == "i915");
    LUMEN_CHECK(std::string_view(l.cards[1].driver) == "amdgpu");
}

LUMEN_TEST(card_nothing_connected_prefers_boot_vga_then_index) {
    fake_sysfs s;
    s.card(0, "amdgpu", 0);
    s.connector(0, "DP-1", "disconnected");
    s.card(1, "nouveau", 1);
    s.connector(1, "DP-2", "disconnected");
    s.card(2, "vkms");
    s.connector(2, "Virtual-1", "disconnected");
    const lx::drm::card_list l = s.find();
    LUMEN_CHECK(l.count == 3);
    LUMEN_CHECK(l.cards[0].index == 1);
    LUMEN_CHECK(l.cards[1].index == 0 && l.cards[2].index == 2);
}

LUMEN_TEST(card_none) {
    fake_sysfs s;
    fs::create_directories(s.root / "renderD128");
    LUMEN_CHECK(s.find().count == 0);
    LUMEN_CHECK(lx::drm::find_cards("/nonexistent/lumen").count == 0);
    LUMEN_CHECK(lx::drm::find_cards(nullptr).count == 0);
}

LUMEN_TEST(card_rank_is_pure) {
    lx::drm::card_list l{};
    l.count = 3;
    l.cards[0] = {.index = 0, .connectors = 1, .connected = 0};
    l.cards[1] = {.index = 2, .connectors = 2, .connected = 1};
    l.cards[2] = {.index = 1, .connectors = 0, .connected = 0};
    lx::drm::rank_cards(l);
    LUMEN_CHECK(l.count == 2);
    LUMEN_CHECK(l.cards[0].index == 2 && l.cards[1].index == 0);
}

LUMEN_TEST(card_no_atomic_hint) {
    // nouveau before Linux 7.4: atomic=0, or the parameter unreadable (it is root-only).
    fake_sysfs s;
    const std::string mod = (s.root / "module").string();
    const std::string_view off = lx::drm::no_atomic_hint("nouveau", mod.c_str());
    LUMEN_CHECK(off.starts_with("TODO:"));
    LUMEN_CHECK(off.find("options nouveau atomic=1") != std::string_view::npos);
    LUMEN_CHECK(off.find("--nouveau-atomic") != std::string_view::npos);
    fake_sysfs::write(s.root / "module/nouveau/parameters/atomic", "0");
    LUMEN_CHECK(std::string_view(lx::drm::no_atomic_hint("nouveau", mod.c_str())) == off);
    // Asked for (1, or -1 = auto on 7.4+) and still refused: a GPU before NV50.
    for (const char* on : {"1", "-1"}) {
        fake_sysfs::write(s.root / "module/nouveau/parameters/atomic", on);
        const std::string_view old = lx::drm::no_atomic_hint("nouveau", mod.c_str());
        LUMEN_CHECK(old.find("NV50") != std::string_view::npos);
        LUMEN_CHECK(old.find("TODO") == std::string_view::npos);
    }
    LUMEN_CHECK(std::string_view(lx::drm::no_atomic_hint("radeon")).find("amdgpu") !=
                std::string_view::npos);
    LUMEN_CHECK(std::string_view(lx::drm::no_atomic_hint("udl")).find("no atomic") !=
                std::string_view::npos);
    LUMEN_CHECK(lx::drm::no_atomic_hint(nullptr) != nullptr);
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
