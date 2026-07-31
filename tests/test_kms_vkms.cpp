// Opt-in vkms KMS test. Enable with LUMEN_TEST_VKMS=1 and a loaded vkms module.
// Asserts connector enumeration + one atomic commit attempt against the virtual card.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(LUMEN_HAS_DRM)
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

import lx.foundation;
import lx.drm;

namespace {

bool env_enabled() {
    const char* v = std::getenv("LUMEN_TEST_VKMS");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

/// Asks each card for its driver name. Probing the sysfs `device/driver` symlink instead
/// is unreliable: kernels that register vkms on the faux bus report `faux_driver` there.
const char* find_vkms_card_path(char* out, std::size_t out_len) {
#if defined(LUMEN_HAS_DRM)
    DIR* d = opendir("/dev/dri");
    if (!d)
        return nullptr;
    while (dirent* ent = readdir(d)) {
        if (std::strncmp(ent->d_name, "card", 4) != 0)
            continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
        const int probe_fd = open(path, O_RDWR | O_CLOEXEC);
        if (probe_fd < 0)
            continue;
        drmVersion* version = drmGetVersion(probe_fd);
        const bool is_vkms = version && version->name && std::strcmp(version->name, "vkms") == 0;
        if (version)
            drmFreeVersion(version);
        close(probe_fd);
        if (is_vkms) {
            std::snprintf(out, out_len, "%s", path);
            closedir(d);
            return out;
        }
    }
    closedir(d);
#else
    (void)out;
    (void)out_len;
#endif
    return nullptr;
}

} // namespace

int main() {
    if (!env_enabled()) {
        std::printf("SKIP: set LUMEN_TEST_VKMS=1 to enable\n");
        return 0;
    }

#if !defined(LUMEN_HAS_DRM)
    std::printf("SKIP: LUMEN_HAS_DRM not set\n");
    return 0;
#else
    char card_path[64]{};
    if (!find_vkms_card_path(card_path, sizeof(card_path))) {
        std::fprintf(stderr, "FAIL: no vkms card found (modprobe vkms?)\n");
        return 1;
    }
    std::printf("using vkms card %s\n", card_path);

    auto opened = lx::drm::kms_device::open(card_path);
    if (!opened) {
        std::fprintf(stderr, "FAIL: open vkms card\n");
        return 1;
    }
    auto dev = static_cast<decltype(opened)&&>(opened).value();
    if (dev.connector_count() == 0) {
        std::fprintf(stderr, "FAIL: vkms reported 0 connectors\n");
        return 1;
    }
    auto mode = dev.active_mode(0);
    if (!mode) {
        std::fprintf(stderr, "FAIL: vkms has no active mode\n");
        return 1;
    }
    std::printf("vkms mode %ux%u\n", mode.value().width, mode.value().height);

    // Create a dumb FB for a real atomic page-flip.
    const int fd = dev.card_fd();
    struct drm_mode_create_dumb creq{};
    creq.width = mode.value().width;
    creq.height = mode.value().height;
    creq.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        std::fprintf(stderr, "FAIL: CREATE_DUMB\n");
        return 1;
    }
    uint32_t handles[4] = {creq.handle, 0, 0, 0};
    uint32_t pitches[4] = {creq.pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint32_t fb_id = 0;
    if (drmModeAddFB2(fd, creq.width, creq.height, DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                      &fb_id, 0) != 0) {
        std::fprintf(stderr, "FAIL: AddFB2\n");
        return 1;
    }

    lx::drm::kms_atomic_commit atomic{dev};
    atomic.set_framebuffer(fb_id);
    bool flipped = false;
    atomic.set_page_flip_handler(
        [](lx::drm::page_flip_event ev, void* user) {
            *static_cast<bool*>(user) = ev.presented;
        },
        &flipped);

    lx::drm::atomic_commit_request req{};
    req.connector = 0;
    req.framebuffer_id = fb_id;
    req.request_page_flip = true;
    auto committed = atomic.commit(req);
    if (!committed) {
        std::fprintf(stderr, "WARN: atomic commit failed (may need DRM master): continuing poll\n");
    }

    for (int i = 0; i < 50 && !flipped; ++i) {
        atomic.dispatch_events();
        usleep(10000);
    }

    drmModeRmFB(fd, fb_id);
    struct drm_mode_destroy_dumb dreq{};
    dreq.handle = creq.handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);

    if (!committed && !flipped) {
        // Without DRM master the commit fails — treat as soft skip when not root.
        if (geteuid() != 0) {
            std::printf("SKIP: no DRM master (run as root or via logind session)\n");
            return 0;
        }
        std::fprintf(stderr, "FAIL: expected page-flip on vkms\n");
        return 1;
    }
    std::printf("vkms atomic path ok (flipped=%d)\n", flipped ? 1 : 0);
    return 0;
#endif
}
