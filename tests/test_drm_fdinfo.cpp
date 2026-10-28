// GPU memory from DRM fdinfo (`lx.drm:fdinfo`, drm-usage-stats): one fdinfo text parsed —
// regions, KiB/MiB units, the legacy drm-memory- spelling, junk skipped — and clients summed
// per region, the same client counted once however many fds reach it.
#include "lumen_test.hpp"

import std;
import lx.drm;

using lx::drm::add_fdinfo_client;
using lx::drm::fdinfo_client;
using lx::drm::gpu_memory;
using lx::drm::gpu_region;
using lx::drm::parse_fdinfo;

namespace {

const gpu_region* region(const auto& regions, std::string_view name) {
    for (const gpu_region& r : regions)
        if (name == r.name)
            return &r;
    return nullptr;
}

// amdgpu on 6.x: memory per region in KiB, the legacy keys alongside, engine times.
constexpr std::string_view k_amdgpu = "pos:\t0\n"
                                      "flags:\t02100002\n"
                                      "mnt_id:\t25\n"
                                      "ino:\t1067\n"
                                      "drm-driver:\tamdgpu\n"
                                      "drm-client-id:\t42\n"
                                      "drm-pdev:\t0000:03:00.0\n"
                                      "pasid:\t32772\n"
                                      "drm-memory-vram:\t4096 KiB\n"
                                      "drm-memory-gtt:\t 2048 KiB\n"
                                      "drm-total-vram:\t8 MiB\n"
                                      "drm-shared-vram:\t1024 KiB\n"
                                      "drm-resident-vram:\t6144 KiB\n"
                                      "drm-purgeable-vram:\t0\n"
                                      "drm-active-vram:\t512 KiB\n"
                                      "drm-total-gtt:\t2048 KiB\n"
                                      "drm-engine-gfx:\t123456789 ns\n"
                                      "amd-memory-visible-vram:\t4096 KiB\n";

} // namespace

LUMEN_TEST(fdinfo_parses_regions_and_units) {
    const fdinfo_client c = parse_fdinfo(k_amdgpu);
    LUMEN_CHECK(std::string_view{c.driver} == "amdgpu");
    LUMEN_CHECK(c.has_client_id && c.client_id == 42);
    LUMEN_CHECK(c.regions.size() == 2);
    const gpu_region* vram = region(c.regions, "vram");
    LUMEN_CHECK(vram != nullptr);
    LUMEN_CHECK(vram->has_total && vram->total == 8ull << 20);
    LUMEN_CHECK(vram->has_shared && vram->shared == 1ull << 20);
    // drm-resident- wins over its legacy alias drm-memory-.
    LUMEN_CHECK(vram->has_resident && vram->resident == 6144ull << 10);
    LUMEN_CHECK(vram->has_purgeable && vram->purgeable == 0);
    LUMEN_CHECK(vram->has_active && vram->active == 512ull << 10);
    // Only the legacy key for gtt's residency: it counts as resident.
    const gpu_region* gtt = region(c.regions, "gtt");
    LUMEN_CHECK(gtt != nullptr);
    LUMEN_CHECK(gtt->has_resident && gtt->resident == 2048ull << 10);
    LUMEN_CHECK(gtt->has_total && gtt->total == 2048ull << 10);
    LUMEN_CHECK(!gtt->has_shared && !gtt->has_purgeable && !gtt->has_active);
}

LUMEN_TEST(fdinfo_legacy_only_driver) {
    // panfrost/msm-style: one "memory" region, legacy key, bytes without a unit.
    const fdinfo_client c = parse_fdinfo("drm-driver:\tpanfrost\n"
                                         "drm-client-id:\t7\n"
                                         "drm-memory-memory:\t1000\n");
    LUMEN_CHECK(std::string_view{c.driver} == "panfrost");
    LUMEN_CHECK(c.regions.size() == 1);
    LUMEN_CHECK(c.regions[0].has_resident && c.regions[0].resident == 1000);
    LUMEN_CHECK(!c.regions[0].has_total);
}

LUMEN_TEST(fdinfo_skips_junk) {
    const fdinfo_client c = parse_fdinfo("garbage without a colon\n"
                                         "drm-driver:\ti915\n"
                                         "drm-client-id:\t12x\n"         // not a number
                                         "drm-total-system0:\tlots\n"    // not a size
                                         "drm-total-local0:\t5 TiB\n"    // unknown unit
                                         "drm-total-:\t5 KiB\n"          // no region
                                         "drm-total-stolen-system0:\t3 MiB\n"
                                         "drm-total-huge:\t99999999999999999999 MiB\n"
                                         ":\n\n"
                                         "drm-resident-system0:\t2 GiB");  // no final newline
    LUMEN_CHECK(std::string_view{c.driver} == "i915");
    LUMEN_CHECK(!c.has_client_id);
    LUMEN_CHECK(c.regions.size() == 2);
    const gpu_region* stolen = region(c.regions, "stolen-system0");
    LUMEN_CHECK(stolen && stolen->has_total && stolen->total == 3ull << 20);
    const gpu_region* sys = region(c.regions, "system0");
    LUMEN_CHECK(sys && !sys->has_total && sys->has_resident && sys->resident == 2ull << 30);
}

LUMEN_TEST(fdinfo_not_drm) {
    // vmwgfx (no usage stats) and any other fd: no driver, nothing to add.
    const fdinfo_client c = parse_fdinfo("pos:\t0\nflags:\t02100002\nmnt_id:\t25\nino:\t223\n");
    LUMEN_CHECK(c.driver[0] == '\0' && c.regions.empty());
    gpu_memory sum{};
    LUMEN_CHECK(!add_fdinfo_client(sum, c));
    LUMEN_CHECK(sum.clients == 0 && sum.driver[0] == '\0');
    LUMEN_CHECK(parse_fdinfo("").driver[0] == '\0');
}

LUMEN_TEST(fdinfo_region_cap) {
    std::string text = "drm-driver:\txe\n";
    for (unsigned i = 0; i < lx::drm::k_max_gpu_regions + 3; ++i)
        text += std::format("drm-total-vram{}:\t1 KiB\n", i);
    const fdinfo_client c = parse_fdinfo(text);
    LUMEN_CHECK(c.regions.size() == lx::drm::k_max_gpu_regions);
    LUMEN_CHECK(c.regions_dropped == 3);
}

LUMEN_TEST(fdinfo_sum_dedupes_clients) {
    gpu_memory sum{};
    // The card fd and Mesa's render node: two clients, summed.
    LUMEN_CHECK(add_fdinfo_client(sum, parse_fdinfo(k_amdgpu)));
    const fdinfo_client render = parse_fdinfo("drm-driver:\tamdgpu\n"
                                              "drm-client-id:\t43\n"
                                              "drm-total-vram:\t2 MiB\n"
                                              "drm-resident-vram:\t2 MiB\n");
    LUMEN_CHECK(add_fdinfo_client(sum, render));
    // The same render node reached through a dup'd fd: counted once.
    LUMEN_CHECK(!add_fdinfo_client(sum, render));
    LUMEN_CHECK(sum.clients == 2);
    LUMEN_CHECK(std::string_view{sum.driver} == "amdgpu");
    const gpu_region* vram = region(sum.regions, "vram");
    LUMEN_CHECK(vram && vram->total == 10ull << 20);
    LUMEN_CHECK(vram->resident == (6144ull << 10) + (2ull << 20));
    // Figures only one client printed stay present, with that client's value.
    LUMEN_CHECK(vram->has_active && vram->active == 512ull << 10);
    // A second GPU's regions are kept apart.
    LUMEN_CHECK(add_fdinfo_client(sum, parse_fdinfo("drm-driver:\ti915\n"
                                                     "drm-client-id:\t50\n"
                                                     "drm-total-vram:\t1 MiB\n")));
    const gpu_region* other = region(sum.regions, "i915:vram");
    LUMEN_CHECK(other && other->total == 1ull << 20);
    LUMEN_CHECK(region(sum.regions, "vram")->total == 10ull << 20);
}

LUMEN_TEST(fdinfo_read_self) {
    // This process: whatever it holds, the walk ends and sums consistently. With a render
    // node open, a driver with usage stats shows up; one without (vmwgfx) adds nothing.
    const gpu_memory before = lx::drm::read_gpu_memory();
    LUMEN_CHECK(before.clients == 0);
    std::FILE* f = std::fopen("/dev/dri/renderD128", "r+");
    const gpu_memory with = lx::drm::read_gpu_memory();
    if (f) {
        LUMEN_CHECK(with.clients <= 1);
        LUMEN_CHECK((with.clients == 1) == (with.driver[0] != '\0'));
        std::fclose(f);
    } else {
        LUMEN_CHECK(with.clients == 0);
    }
}

int main(int argc, char** argv) { return lumen_test::run_all(argc, argv); }
