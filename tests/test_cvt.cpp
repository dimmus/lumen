// Custom-mode timings (ADR 008 §C.4, `lx.drm:modes`): the built-in CVT formulas against
// published reference timings (`cvt` from xorg-x11-utils, VESA's CVT 2.0 spreadsheet), and
// against libdisplay-info's `di_cvt_compute` when the build has it — the two must agree to the
// pixel for every size a client may ask for.

import std;
import lx.drm;

namespace {

int g_failed = 0;
int g_ran = 0;

void check(bool ok, const char* what) {
    ++g_ran;
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

bool same(const lx::drm::mode_timings& a, const lx::drm::mode_timings& b) {
    return a.clock_khz == b.clock_khz && a.hdisplay == b.hdisplay &&
           a.hsync_start == b.hsync_start && a.hsync_end == b.hsync_end && a.htotal == b.htotal &&
           a.vdisplay == b.vdisplay && a.vsync_start == b.vsync_start &&
           a.vsync_end == b.vsync_end && a.vtotal == b.vtotal && a.flags == b.flags;
}

void print(const char* tag, const lx::drm::kms_mode& m) {
    const auto t = lx::drm::timings_of(m);
    std::printf("    %s %ux%u@%u: %u kHz  %u %u %u %u  %u %u %u %u  flags=%u\n", tag, m.width,
                m.height, m.refresh_mhz, t.clock_khz, t.hdisplay, t.hsync_start, t.hsync_end,
                t.htotal, t.vdisplay, t.vsync_start, t.vsync_end, t.vtotal, t.flags);
}

} // namespace

int main() {
    using lx::drm::cvt_blanking;
    std::printf("test_cvt\n");

    // Reduced blanking v2, 1920x1080@60 (VESA CVT 2.0 example): 133.320 MHz, htotal 2000
    // (blank 80: front 8, sync 32, back 40), vtotal 1111 (sync 8, back 6).
    {
        const auto m = lx::drm::cvt_mode_builtin(1920, 1080, 60000, cvt_blanking::reduced_v2);
        const auto t = lx::drm::timings_of(m);
        print("rb2", m);
        check(t.clock_khz == 133320 && t.htotal == 2000 && t.hsync_start == 1928 &&
                  t.hsync_end == 1960 && t.vtotal == 1111 && t.vsync_end - t.vsync_start == 8 &&
                  t.vtotal - t.vsync_end == 6,
              "RB v2 1920x1080@60: 133.32 MHz, 2000 x 1111");
        check(m.refresh_mhz >= 59990 && m.refresh_mhz <= 60010 && (t.flags & 1u) && (t.flags & 8u),
              "RB v2: refresh within 10 mHz of 60 Hz, +hsync -vsync");
    }
    // Standard blanking, 1920x1080@60 — `cvt 1920 1080 60`:
    //   173.00  1920 2048 2248 2576  1080 1083 1088 1120 -hsync +vsync
    {
        const auto m = lx::drm::cvt_mode_builtin(1920, 1080, 60000, cvt_blanking::standard);
        const auto t = lx::drm::timings_of(m);
        print("cvt", m);
        check(t.clock_khz == 173000 && t.hsync_start == 2048 && t.hsync_end == 2248 &&
                  t.htotal == 2576 && t.vsync_start == 1083 && t.vsync_end == 1088 &&
                  t.vtotal == 1120 && (t.flags & 2u) && (t.flags & 4u),
              "CVT 1920x1080@60 matches `cvt`: 173.00 2048 2248 2576 / 1083 1088 1120");
    }
    // `cvt 1024 768 60`: 63.50  1024 1072 1176 1328  768 771 775 798 -hsync +vsync
    {
        const auto t = lx::drm::timings_of(
            lx::drm::cvt_mode_builtin(1024, 768, 60000, cvt_blanking::standard));
        check(t.clock_khz == 63500 && t.hsync_start == 1072 && t.hsync_end == 1176 &&
                  t.htotal == 1328 && t.vsync_start == 771 && t.vsync_end == 775 && t.vtotal == 798,
              "CVT 1024x768@60 matches `cvt` (4:3: 4-line sync)");
    }
    // A width that is not a multiple of 8 keeps its size (wlroots keeps the requested width).
    {
        const auto m = lx::drm::cvt_mode_builtin(1366, 768, 60000, cvt_blanking::standard);
        check(m.width == 1366 && lx::drm::timings_of(m).hdisplay == 1366, "1366 wide stays 1366");
        const auto r = lx::drm::cvt_mode_builtin(1366, 768, 60000, cvt_blanking::reduced_v2);
        check(r.width == 1366 && r.height == 768, "RB v2 1366x768 keeps its size");
    }
    // 0 mHz is 60 Hz; sizes outside 1..16384 are refused.
    {
        const auto a = lx::drm::cvt_mode_builtin(800, 600, 0, cvt_blanking::reduced_v2);
        const auto b = lx::drm::cvt_mode_builtin(800, 600, 60000, cvt_blanking::reduced_v2);
        check(same(lx::drm::timings_of(a), lx::drm::timings_of(b)), "refresh 0 means 60 Hz");
        check(!lx::drm::cvt_mode(0, 600, 60000, cvt_blanking::reduced_v2).valid() &&
                  !lx::drm::cvt_mode(16385, 600, 60000, cvt_blanking::reduced_v2).valid(),
              "a size outside 1..16384 gives no mode");
    }
    // The mode carries a name and the user-defined type; same_timings tells sizes apart.
    {
        const auto a = lx::drm::cvt_mode(1600, 900, 60000, cvt_blanking::reduced_v2);
        const auto b = lx::drm::cvt_mode(1600, 900, 75000, cvt_blanking::reduced_v2);
        check(a.valid() && b.valid() && !a.same_timings(b) && a.same_timings(a),
              "60 and 75 Hz modes of one size differ in timings");
    }
    check(lx::drm::cvt_blanking_for_connector(1) == cvt_blanking::standard &&
              lx::drm::cvt_blanking_for_connector(10) == cvt_blanking::reduced_v2 &&
              lx::drm::cvt_blanking_for_connector(15) == cvt_blanking::reduced_v2,
          "VGA gets standard blanking, DisplayPort and Virtual reduced v2");

    // Built-in == libdisplay-info, over sizes and refreshes clients ask for.
    lx::drm::kms_mode ref{};
    if (!lx::drm::cvt_mode_reference(1920, 1080, 60000, cvt_blanking::reduced_v2, ref)) {
        std::printf("  (libdisplay-info not in this build: reference comparison skipped)\n");
    } else {
        static constexpr unsigned k_sizes[][2] = {
            {640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 800},
            {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900},  {1680, 1050},
            {1914, 924},  {1918, 928},  {1920, 1080}, {1920, 1200}, {2560, 1440},
            {2560, 1600}, {3440, 1440}, {3840, 2160}, {5120, 2880}, {7680, 4320}};
        static constexpr unsigned k_rates[] = {30000, 50000, 59940, 60000, 75000, 120000, 144000, 240000};
        unsigned mismatches = 0;
        unsigned compared = 0;
        for (const auto& s : k_sizes) {
            for (unsigned hz : k_rates) {
                for (auto b : {cvt_blanking::reduced_v2, cvt_blanking::standard}) {
                    lx::drm::kms_mode r{};
                    if (!lx::drm::cvt_mode_reference(s[0], s[1], hz, b, r))
                        continue;
                    const auto m = lx::drm::cvt_mode_builtin(s[0], s[1], hz, b);
                    ++compared;
                    if (!same(lx::drm::timings_of(m), lx::drm::timings_of(r))) {
                        if (++mismatches <= 6) {
                            print("builtin", m);
                            print("libdisplay-info", r);
                        }
                    }
                }
            }
        }
        std::printf("    %u timings compared\n", compared);
        check(compared > 0 && mismatches == 0,
              "the built-in formulas equal libdisplay-info's for every size and refresh");
    }

    std::printf("%d cases, %d failed\n", g_ran, g_failed);
    return g_failed == 0 ? 0 : 1;
}
