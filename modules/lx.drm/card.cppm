module;

// Which card to scan out on. /dev/dri/card0 is not "the GPU": simpledrm (the firmware
// framebuffer) takes card0 at boot and the real driver that replaces it gets the next minor
// (nouveau, amdgpu and i915 load after it on most desktops), a render-only GPU has a card node
// without connectors, and a hybrid laptop has two cards. The candidates are read from sysfs
// only — no device is opened, so nothing races a DRM master or needs a permission — and
// ranked as wlroots (`wlr_session_find_gpus`) and Mutter (`meta_kms_device` selection) rank
// them: a card with a connected display first, then the firmware's boot VGA device, then the
// lowest index.

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

export module lx.drm:card;

import std;

export namespace lx::drm {

/// One card node as sysfs describes it (`find_cards`).
struct card_candidate {
    /// N of /dev/dri/cardN.
    unsigned index = 0;
    /// "/dev/dri/cardN".
    char path[32]{};
    /// The kernel driver ("nouveau", "amdgpu", "simpledrm"); empty when sysfs has none.
    char driver[32]{};
    /// Connector entries (cardN-*). None: a render-only device, never a display.
    unsigned connectors = 0;
    /// Of them, status "connected".
    unsigned connected = 0;
    /// PCI boot_vga: the device the firmware drove the console with.
    bool boot_vga = false;
};

inline constexpr unsigned k_max_cards = 16;

struct card_list {
    card_candidate cards[k_max_cards]{};
    unsigned count = 0;
};

/// Pure: drops the cards without connectors and orders the rest best first — a connected
/// display, then boot VGA, then the lowest index.
void rank_cards(card_list& list);

/// The card nodes under `sys_drm` (sysfs' drm class; a directory laid out like it in tests),
/// ranked (`rank_cards`). Empty when there is no display device.
[[nodiscard]] card_list find_cards(const char* sys_drm = "/sys/class/drm");

/// What to do about a card whose `driver` refused atomic modesetting (`drm_err::no_atomic`):
/// one static line for the log. nouveau leaves atomic off before Linux 7.4 unless
/// `nouveau.atomic=1`; when `sys_module`'s `nouveau/parameters/atomic` shows it on already
/// (the file is root-only, so usually it cannot be read), the GPU predates NV50 and has
/// none. radeon has none at all; amdgpu drives its GCN 1.0/1.1 cards with atomic.
[[nodiscard]] const char* no_atomic_hint(const char* driver, const char* sys_module = "/sys/module");

} // namespace lx::drm


namespace {

/// `cardN` (not `cardN-DP-1`, not `renderD128`): N, -1 otherwise.
long card_index(const char* name) {
    if (std::strncmp(name, "card", 4) != 0 || name[4] == '\0')
        return -1;
    long n = 0;
    for (const char* p = name + 4; *p; ++p) {
        if (*p < '0' || *p > '9' || n > 1'000'000)
            return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

/// The first line of a small sysfs attribute, without its newline; false when unreadable.
bool read_attr(const char* path, char* out, std::size_t size) {
    FILE* f = std::fopen(path, "re");
    if (!f)
        return false;
    const bool ok = std::fgets(out, static_cast<int>(size), f) != nullptr;
    std::fclose(f);
    if (ok)
        out[std::strcspn(out, "\n")] = '\0';
    return ok;
}

} // namespace

void lx::drm::rank_cards(card_list& list) {
    card_candidate* const begin = list.cards;
    card_candidate* const end = std::remove_if(begin, begin + list.count, [](const card_candidate& c) {
        return c.connectors == 0;
    });
    list.count = static_cast<unsigned>(end - begin);
    std::stable_sort(begin, end, [](const card_candidate& a, const card_candidate& b) {
        if ((a.connected > 0) != (b.connected > 0))
            return a.connected > 0;
        if (a.boot_vga != b.boot_vga)
            return a.boot_vga;
        return a.index < b.index;
    });
}

const char* lx::drm::no_atomic_hint(const char* driver, const char* sys_module) {
    if (driver && std::strcmp(driver, "nouveau") == 0) {
        char path[512];
        char value[16];
        std::snprintf(path, sizeof(path), "%s/nouveau/parameters/atomic",
                      sys_module ? sys_module : "/sys/module");
        // 1, or -1 ("auto", Linux 7.4+): asked for and still refused.
        if (read_attr(path, value, sizeof(value)) && std::strcmp(value, "0") != 0)
            return "nouveau has no atomic modesetting on GPUs before NV50 (GeForce 8); Lumen "
                   "cannot scan out on this card";
        return "TODO: nouveau leaves atomic modesetting off before Linux 7.4 — add `options "
               "nouveau atomic=1` to /etc/modprobe.d (./scripts/install-deps.sh "
               "--nouveau-atomic does it) and reboot";
    }
    if (driver && std::strcmp(driver, "radeon") == 0)
        return "radeon has no atomic modesetting; a GCN 1.0/1.1 card can run on amdgpu instead "
               "(radeon.si_support=0 amdgpu.si_support=1, or the cik_support pair)";
    return "the driver has no atomic modesetting, which Lumen's KMS path requires";
}

lx::drm::card_list lx::drm::find_cards(const char* sys_drm) {
    card_list list{};
    if (!sys_drm)
        return list;
    DIR* dir = ::opendir(sys_drm);
    if (!dir)
        return list;
    // Pass 1: the card nodes.
    while (const dirent* e = ::readdir(dir)) {
        const long n = card_index(e->d_name);
        if (n < 0 || list.count >= k_max_cards)
            continue;
        card_candidate& c = list.cards[list.count++];
        c.index = static_cast<unsigned>(n);
        std::snprintf(c.path, sizeof(c.path), "/dev/dri/card%ld", n);
        char attr[512];
        char value[16];
        std::snprintf(attr, sizeof(attr), "%s/%s/device/boot_vga", sys_drm, e->d_name);
        c.boot_vga = read_attr(attr, value, sizeof(value)) && std::strcmp(value, "1") == 0;
        std::snprintf(attr, sizeof(attr), "%s/%s/device/driver", sys_drm, e->d_name);
        char target[256];
        const ssize_t len = ::readlink(attr, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            const char* base = std::strrchr(target, '/');
            std::snprintf(c.driver, sizeof(c.driver), "%s", base ? base + 1 : target);
        }
    }
    // Pass 2: their connectors, `cardN-<connector>/status`.
    ::rewinddir(dir);
    while (const dirent* e = ::readdir(dir)) {
        const char* dash = std::strchr(e->d_name, '-');
        if (!dash)
            continue;
        char card[32];
        const std::size_t len = static_cast<std::size_t>(dash - e->d_name);
        if (len >= sizeof(card))
            continue;
        std::memcpy(card, e->d_name, len);
        card[len] = '\0';
        const long n = card_index(card);
        if (n < 0)
            continue;
        for (unsigned i = 0; i < list.count; ++i) {
            card_candidate& c = list.cards[i];
            if (c.index != static_cast<unsigned>(n))
                continue;
            ++c.connectors;
            char attr[512];
            char value[16];
            std::snprintf(attr, sizeof(attr), "%s/%s/status", sys_drm, e->d_name);
            if (read_attr(attr, value, sizeof(value)) && std::strcmp(value, "connected") == 0)
                ++c.connected;
            break;
        }
    }
    ::closedir(dir);
    rank_cards(list);
    return list;
}
