module;

// Custom modes (zwlr_output_configuration_head_v1.set_custom_mode, ADR 008 §C.4): the timings
// of a size and refresh the connector does not list, from VESA's Coordinated Video Timings
// (CVT 2.0). Reduced blanking v2 for digital links — 80 pixels of horizontal blanking and
// the 460 µs minimum vertical blank, the lowest pixel clock a digital receiver accepts, so a
// custom mode fits the link where the classic formula's would not — and the classic formula
// (CRT-era blanking) for analog connectors, whose receivers lock onto sync pulses and may
// not take reduced blanking. wlroots computes the classic formula for every connector (its
// TODO is to pick reduced blanking from the EDID); KWin and Weston use reduced blanking.
//
// libdisplay-info's `di_cvt_compute` computes the timings when the build has it (wlroots
// uses it too); the built-in formulas below are the same arithmetic, kept for builds
// without it and unit-tested against it (tests/test_cvt.cpp). The kernel validates the
// result: a mode the connector or link cannot carry fails the TEST_ONLY commit, as it should.
// Pure, no allocation.

#if defined(LUMEN_HAS_DISPLAY_INFO)
extern "C" {
#include <libdisplay-info/cvt.h>
}
#endif

export module lx.drm:modes;

import std;

import :atomic;

export namespace lx::drm {

enum class cvt_blanking : unsigned char {
    /// CVT with standard (CRT) blanking.
    standard,
    /// CVT reduced blanking v2.
    reduced_v2,
};

/// The CVT timings of `width` × `height` at `refresh_mhz` (0: 60 Hz) as a mode a modeset can
/// program: `kms` holds the `drm_mode_modeinfo` bytes (sync polarity per CVT, type
/// DRM_MODE_TYPE_USERDEF, named "WxH"), `refresh_mhz` the refresh the timings really give
/// (CVT rounds the pixel clock). An invalid mode (`!valid()`) for a size outside
/// 1..16384 or a refresh past 1000 Hz.
[[nodiscard]] kms_mode cvt_mode(unsigned width, unsigned height, unsigned refresh_mhz,
                                cvt_blanking blanking);
/// The same from the built-in formulas only (what `cvt_mode` uses without libdisplay-info);
/// exported for tests.
[[nodiscard]] kms_mode cvt_mode_builtin(unsigned width, unsigned height, unsigned refresh_mhz,
                                        cvt_blanking blanking);
/// libdisplay-info's timings, when the build has it (false otherwise); exported for tests.
[[nodiscard]] bool cvt_mode_reference(unsigned width, unsigned height, unsigned refresh_mhz,
                                      cvt_blanking blanking, kms_mode& out);
/// The blanking a custom mode on a connector of DRM_MODE_CONNECTOR_* `type` gets: standard
/// for analog connectors (VGA, DVI-A, composite, S-video, component, DIN, TV), reduced v2
/// for the rest.
[[nodiscard]] cvt_blanking cvt_blanking_for_connector(unsigned connector_type);

/// The fields of a `drm_mode_modeinfo` (uapi, `drm_mode.h`) as `kms_mode::kms` stores them.
struct mode_timings {
    unsigned clock_khz = 0;
    unsigned hdisplay = 0, hsync_start = 0, hsync_end = 0, htotal = 0;
    unsigned vdisplay = 0, vsync_start = 0, vsync_end = 0, vtotal = 0;
    unsigned flags = 0;
};
/// Reads the timings back out of a mode's bytes (tests, logs).
[[nodiscard]] mode_timings timings_of(const kms_mode& mode);

} // namespace lx::drm

namespace {

/// `struct drm_mode_modeinfo` (include/uapi/drm/drm_mode.h): stable uapi, 68 bytes. Mirrored
/// here so this partition needs no libdrm header.
struct modeinfo {
    unsigned clock;
    unsigned short hdisplay, hsync_start, hsync_end, htotal, hskew;
    unsigned short vdisplay, vsync_start, vsync_end, vtotal, vscan;
    unsigned vrefresh;
    unsigned flags;
    unsigned type;
    char name[32];
};
static_assert(sizeof(modeinfo) == sizeof(lx::drm::drm_mode_blob::bytes));

constexpr unsigned k_flag_phsync = 1u << 0;
constexpr unsigned k_flag_nhsync = 1u << 1;
constexpr unsigned k_flag_pvsync = 1u << 2;
constexpr unsigned k_flag_nvsync = 1u << 3;
constexpr unsigned k_type_userdef = 1u << 5;
constexpr unsigned k_max_size = 16384;

/// Timings as the CVT spreadsheet gives them: active size, porches and syncs in pixels and
/// lines, the pixel clock in MHz.
struct cvt_result {
    double pixel_mhz = 0.0;
    unsigned h_active = 0, h_front = 0, h_sync = 0, h_back = 0;
    unsigned v_active = 0, v_front = 0, v_sync = 0, v_back = 0;
};

lx::drm::kms_mode to_mode(const cvt_result& t, lx::drm::cvt_blanking blanking) {
    lx::drm::kms_mode m{};
    modeinfo mi{};
    mi.clock = static_cast<unsigned>(std::lround(t.pixel_mhz * 1000.0));
    mi.hdisplay = static_cast<unsigned short>(t.h_active);
    mi.hsync_start = static_cast<unsigned short>(t.h_active + t.h_front);
    mi.hsync_end = static_cast<unsigned short>(mi.hsync_start + t.h_sync);
    mi.htotal = static_cast<unsigned short>(mi.hsync_end + t.h_back);
    mi.vdisplay = static_cast<unsigned short>(t.v_active);
    mi.vsync_start = static_cast<unsigned short>(t.v_active + t.v_front);
    mi.vsync_end = static_cast<unsigned short>(mi.vsync_start + t.v_sync);
    mi.vtotal = static_cast<unsigned short>(mi.vsync_end + t.v_back);
    // CVT's polarities: reduced blanking +H −V, standard −H +V (they tell a receiver which
    // formula made the timings).
    mi.flags = blanking == lx::drm::cvt_blanking::reduced_v2 ? (k_flag_phsync | k_flag_nvsync)
                                                              : (k_flag_nhsync | k_flag_pvsync);
    mi.type = k_type_userdef;
    if (mi.clock == 0 || mi.htotal == 0 || mi.vtotal == 0)
        return m;
    const unsigned long long mhz =
        (static_cast<unsigned long long>(mi.clock) * 1'000'000ull / mi.htotal + mi.vtotal / 2) /
        mi.vtotal;
    mi.vrefresh = static_cast<unsigned>((mhz + 500) / 1000);
    std::snprintf(mi.name, sizeof(mi.name), "%ux%u", t.h_active, t.v_active);
    m.width = t.h_active;
    m.height = t.v_active;
    m.refresh_mhz = static_cast<unsigned>(mhz);
    m.preferred = false;
    m.flags = mi.flags;
    std::memcpy(m.kms.bytes, &mi, sizeof(mi));
    return m;
}

[[nodiscard]] bool size_ok(unsigned w, unsigned h, unsigned mhz) {
    return w >= 1 && h >= 1 && w <= k_max_size && h <= k_max_size && mhz <= 1'000'000;
}

/// CVT 2.0 §3.4 (reduced blanking v2): fixed 80-pixel horizontal blank (front porch 8,
/// sync 32, back porch 40), vertical sync 8 lines and back porch 6, the vertical blank at
/// least 460 µs, pixel clock in 1 kHz steps.
cvt_result reduced_v2(unsigned w, unsigned h, double hz) {
    constexpr double k_min_vblank_us = 460.0;
    constexpr unsigned k_h_blank = 80, k_h_sync = 32, k_h_back = 40;
    constexpr unsigned k_v_sync = 8, k_v_back = 6, k_min_v_front = 1;
    constexpr double k_clock_step = 0.001;
    const double h_period_est = ((1'000'000.0 / hz) - k_min_vblank_us) / h;
    const unsigned vbi_lines = static_cast<unsigned>(std::floor(k_min_vblank_us / h_period_est)) + 1;
    const unsigned min_vbi = k_min_v_front + k_v_sync + k_v_back;
    const unsigned act_vbi = vbi_lines < min_vbi ? min_vbi : vbi_lines;
    const unsigned total_v = act_vbi + h;
    const unsigned total_h = k_h_blank + w;
    const double pixel = k_clock_step * std::floor((hz * total_v * total_h / 1'000'000.0) / k_clock_step);
    cvt_result r{};
    r.pixel_mhz = pixel;
    r.h_active = w;
    r.h_front = k_h_blank - k_h_sync - k_h_back;
    r.h_sync = k_h_sync;
    r.h_back = k_h_back;
    r.v_active = h;
    r.v_sync = k_v_sync;
    r.v_back = k_v_back;
    r.v_front = act_vbi - k_v_sync - k_v_back;
    return r;
}

/// CVT 2.0 §3.3 (standard blanking, progressive, no margins): the sync width from the
/// aspect ratio, a 550 µs minimum sync + back porch (and a back porch of at least 7 lines), the horizontal blank from the ideal
/// duty cycle (C' = 30, M' = 300), 8-pixel cells, pixel clock in 250 kHz steps.
cvt_result standard(unsigned w, unsigned h, double hz) {
    constexpr unsigned k_cell = 8;
    constexpr double k_min_vsync_bp_us = 550.0;
    constexpr unsigned k_min_v_porch = 3, k_min_v_back = 7; // CVT 2.0 (1.2 had 6)
    constexpr double k_c_prime = 30.0, k_m_prime = 300.0;
    constexpr double k_h_sync_pct = 8.0;
    constexpr double k_clock_step = 0.25;
    const unsigned h_rnd = (w / k_cell) * k_cell;
    // The vertical sync width encodes the aspect ratio (CVT table 3-2): exact ratios of the
    // cell-rounded width only (1366x768 is not 16:9 here, as in libdisplay-info and xorg).
    unsigned v_sync = 10;
    if (h % 3 == 0 && h * 4 / 3 == h_rnd)
        v_sync = 4;
    else if (h % 9 == 0 && h * 16 / 9 == h_rnd)
        v_sync = 5;
    else if (h % 10 == 0 && h * 16 / 10 == h_rnd)
        v_sync = 6;
    else if ((h % 4 == 0 && h * 5 / 4 == h_rnd) || (h % 9 == 0 && h * 15 / 9 == h_rnd))
        v_sync = 7;
    const double h_period_est =
        ((1.0 / hz) - k_min_vsync_bp_us / 1'000'000.0) / (h + k_min_v_porch) * 1'000'000.0;
    unsigned v_sync_bp = static_cast<unsigned>(std::floor(k_min_vsync_bp_us / h_period_est)) + 1;
    if (v_sync_bp < v_sync + k_min_v_back)
        v_sync_bp = v_sync + k_min_v_back;
    double duty = k_c_prime - (k_m_prime * h_period_est / 1000.0);
    if (duty < 20.0)
        duty = 20.0;
    const unsigned h_blank =
        static_cast<unsigned>(std::floor(h_rnd * duty / (100.0 - duty) / (2 * k_cell))) * 2 * k_cell;
    const unsigned total_h = h_rnd + h_blank;
    const double pixel = k_clock_step * std::floor((total_h / h_period_est) / k_clock_step);
    const unsigned h_sync =
        static_cast<unsigned>(std::floor(k_h_sync_pct / 100.0 * total_h / k_cell)) * k_cell;
    cvt_result r{};
    r.pixel_mhz = pixel;
    // The requested width, not the cell-rounded one (wlroots keeps it too): a custom mode is
    // the size the client asked for; htotal grows by the remainder.
    r.h_active = w;
    r.h_back = h_blank / 2;
    r.h_sync = h_sync;
    r.h_front = h_blank - h_sync - r.h_back;
    r.v_active = h;
    r.v_sync = v_sync;
    r.v_front = k_min_v_porch;
    r.v_back = v_sync_bp - v_sync;
    return r;
}

[[nodiscard]] double hz_of(unsigned refresh_mhz) {
    return refresh_mhz > 0 ? refresh_mhz / 1000.0 : 60.0;
}

} // namespace

lx::drm::kms_mode lx::drm::cvt_mode_builtin(unsigned width, unsigned height, unsigned refresh_mhz,
                                            cvt_blanking blanking) {
    if (!size_ok(width, height, refresh_mhz))
        return {};
    const double hz = hz_of(refresh_mhz);
    return to_mode(blanking == cvt_blanking::reduced_v2 ? reduced_v2(width, height, hz)
                                                        : standard(width, height, hz),
                   blanking);
}

bool lx::drm::cvt_mode_reference(unsigned width, unsigned height, unsigned refresh_mhz,
                                 cvt_blanking blanking, kms_mode& out) {
#if defined(LUMEN_HAS_DISPLAY_INFO)
    if (!size_ok(width, height, refresh_mhz))
        return false;
    di_cvt_options opt{};
    opt.red_blank_ver = blanking == cvt_blanking::reduced_v2 ? DI_CVT_REDUCED_BLANKING_V2
                                                             : DI_CVT_REDUCED_BLANKING_NONE;
    opt.h_pixels = static_cast<int>(width);
    opt.v_lines = static_cast<int>(height);
    opt.ip_freq_rqd = hz_of(refresh_mhz);
    di_cvt_timing t{};
    di_cvt_compute(&t, &opt);
    cvt_result r{};
    r.pixel_mhz = t.act_pixel_freq;
    r.h_active = width; // as the built-in formulas: the requested width

    r.h_front = static_cast<unsigned>(std::lround(t.h_front_porch));
    r.h_sync = static_cast<unsigned>(std::lround(t.h_sync));
    r.h_back = static_cast<unsigned>(std::lround(t.h_back_porch));
    r.v_active = static_cast<unsigned>(std::lround(t.v_lines_rnd));
    r.v_front = static_cast<unsigned>(std::lround(t.v_front_porch));
    r.v_sync = static_cast<unsigned>(std::lround(t.v_sync));
    r.v_back = static_cast<unsigned>(std::lround(t.v_back_porch));
    out = to_mode(r, blanking);
    return out.valid();
#else
    (void)width;
    (void)height;
    (void)refresh_mhz;
    (void)blanking;
    out = {};
    return false;
#endif
}

lx::drm::kms_mode lx::drm::cvt_mode(unsigned width, unsigned height, unsigned refresh_mhz,
                                    cvt_blanking blanking) {
    kms_mode m{};
    if (cvt_mode_reference(width, height, refresh_mhz, blanking, m))
        return m;
    return cvt_mode_builtin(width, height, refresh_mhz, blanking);
}

lx::drm::cvt_blanking lx::drm::cvt_blanking_for_connector(unsigned type) {
    // DRM_MODE_CONNECTOR_*: VGA 1, DVII 2, DVIA 4, Composite 5, SVIDEO 6, Component 8,
    // 9PinDIN 9, TV 13 (uapi values). DVI-I carries analog too: a VGA adapter on it is
    // common, so it counts as analog.
    switch (type) {
    case 1: case 2: case 4: case 5: case 6: case 8: case 9: case 13:
        return cvt_blanking::standard;
    default:
        return cvt_blanking::reduced_v2;
    }
}

lx::drm::mode_timings lx::drm::timings_of(const kms_mode& m) {
    modeinfo mi{};
    std::memcpy(&mi, m.kms.bytes, sizeof(mi));
    mode_timings t{};
    t.clock_khz = mi.clock;
    t.hdisplay = mi.hdisplay;
    t.hsync_start = mi.hsync_start;
    t.hsync_end = mi.hsync_end;
    t.htotal = mi.htotal;
    t.vdisplay = mi.vdisplay;
    t.vsync_start = mi.vsync_start;
    t.vsync_end = mi.vsync_end;
    t.vtotal = mi.vtotal;
    t.flags = mi.flags;
    return t;
}
