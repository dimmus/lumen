// Vulkan composite → export_dmabuf → KMS framebuffer → atomic flip (vkms).
// Opt-in: LUMEN_TEST_VKMS=1 and modprobe vkms. Soft-skips without DRM master.
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
import lx.gfx;

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
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        drmVersion* version = drmGetVersion(fd);
        const bool is_vkms = version && version->name && std::strcmp(version->name, "vkms") == 0;
        if (version)
            drmFreeVersion(version);
        close(fd);
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

    auto selected = lx::gfx::device_selector::select_best();
    if (!selected) {
        std::printf("SKIP: no graphics device\n");
        return 0;
    }
    auto gpu = static_cast<decltype(selected)&&>(selected).value();
    if (!gpu.info().supports_dmabuf_import) {
        std::printf("SKIP: no dma-buf import\n");
        return 0;
    }

    lx::gfx::vulkan_compositor compositor{};
    if (auto ready = compositor.initialize(gpu.context()); !ready) {
        std::printf("SKIP: composite unavailable: %s\n", ready.get_error().message);
        return 0;
    }

    auto target = lx::gfx::render_target::create(gpu.context(), mode.value().width,
                                                 mode.value().height);
    if (!target) {
        std::fprintf(stderr, "FAIL: render target: %s\n", target.get_error().message);
        return 1;
    }
    auto rt = static_cast<decltype(target)&&>(target).value();

    lx::gfx::blit_command fill{};
    fill.texture_id = 0;
    fill.dst = {0, 0, static_cast<int>(mode.value().width), static_cast<int>(mode.value().height)};
    fill.opacity = 1.f;
    fill.blend = lx::blend_mode::opaque;

    if (auto composite = compositor.composite(rt, lx::color::rgb(0.2f, 0.4f, 0.9f), &fill, 0);
        !composite) {
        std::fprintf(stderr, "FAIL: composite: %s\n", composite.get_error().message);
        return 1;
    }

    auto exported = rt.export_dmabuf();
    if (!exported) {
        std::fprintf(stderr, "FAIL: export_dmabuf: %s\n", exported.get_error().message);
        return 1;
    }
    const auto dmabuf = static_cast<decltype(exported)&&>(exported).value();

    auto framebuffer = lx::drm::kms_framebuffer::import_dmabuf(
        dev, dmabuf.fd.get(), dmabuf.width, dmabuf.height, dmabuf.stride, dmabuf.offset,
        static_cast<lx::fourcc>(lx::pixel_format::xrgb8888), dmabuf.modifier);
    if (!framebuffer) {
        std::fprintf(stderr, "FAIL: kms_framebuffer import: %s\n", framebuffer.get_error().message);
        return 1;
    }
    const auto fb = static_cast<decltype(framebuffer)&&>(framebuffer).value();

    lx::drm::kms_atomic_commit atomic{dev};
    atomic.set_framebuffer(fb.id());
    bool flipped = false;
    atomic.set_page_flip_handler(
        [](lx::drm::page_flip_event ev, void* user) {
            *static_cast<bool*>(user) = ev.presented;
        },
        &flipped);

    lx::drm::atomic_commit_request req{};
    req.connector = 0;
    req.framebuffer_id = fb.id();
    req.request_page_flip = true;
    auto committed = atomic.commit(req);
    if (!committed) {
        std::fprintf(stderr, "WARN: atomic commit failed (may need DRM master)\n");
    }

    for (int i = 0; i < 50 && !flipped; ++i) {
        atomic.dispatch_events();
        usleep(10000);
    }

    if (!committed && !flipped) {
        if (geteuid() != 0) {
            std::printf("SKIP: no DRM master (run as root or via logind session)\n");
            return 0;
        }
        std::fprintf(stderr, "FAIL: expected page-flip on vkms\n");
        return 1;
    }

    const unsigned pixels = mode.value().width * mode.value().height * 4u;
    auto* rgba = new unsigned char[pixels];
    if (auto read = compositor.read_back(rt, rgba, pixels); !read) {
        std::fprintf(stderr, "FAIL: read_back: %s\n", read.get_error().message);
        delete[] rgba;
        return 1;
    }
    // The clear color round-trips as 8-bit RGBA, so assert every channel rather than a
    // loose "is it blue-ish": that would pass on a swizzled or partly-cleared target.
    const unsigned char expected[4] = {51, 102, 230, 255}; // rgb(0.2, 0.4, 0.9) * 255
    int worst = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int delta = static_cast<int>(rgba[i]) - static_cast<int>(expected[i]);
        const int magnitude = delta < 0 ? -delta : delta;
        if (magnitude > worst)
            worst = magnitude;
    }
    std::printf("clear color read back as %u %u %u %u (max delta %d)\n", rgba[0], rgba[1],
                rgba[2], rgba[3], worst);
    delete[] rgba;
    if (worst > 2) {
        std::fprintf(stderr, "FAIL: composited pixels do not match clear color\n");
        return 1;
    }

    std::printf("vkms vulkan scanout chain ok (flipped=%d)\n", flipped ? 1 : 0);
    return 0;
#endif
}
