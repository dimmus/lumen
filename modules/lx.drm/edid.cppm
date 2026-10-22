module;

// Monitor identity from EDID: make, model and serial — what wlroots puts on
// zwlr_output_head_v1 and in wl_output.description, and what kanshi and sway configs match
// ("make model serial", ADR 008 M1). libdisplay-info when the build has it (the parser
// wlroots, Weston, mpv and Mutter use: PNP vendor names, DisplayID, CTA quirks); otherwise
// a base-block reader that gives the PNP id, the product name or code, and the serial
// string or number — the same strings libdisplay-info falls back to, minus the vendor
// table.

#if defined(LUMEN_HAS_DISPLAY_INFO)
// A C library whose headers carry no `extern "C"` of their own.
extern "C" {
#include <libdisplay-info/info.h>
}
#endif

export module lx.drm:edid;

import std;

export namespace lx::drm {

struct edid_identity {
    char make[64]{};
    char model[64]{};
    char serial[64]{};
    /// A valid EDID was parsed (identity strings may still be empty).
    bool valid = false;
};

/// Parses `size` bytes of EDID. False (and an empty identity) for anything that is not one.
[[nodiscard]] bool parse_edid_identity(const unsigned char* data, unsigned size,
                                       edid_identity& out);
/// The built-in base-block reader alone (what `parse_edid_identity` uses without
/// libdisplay-info); exported for tests.
[[nodiscard]] bool parse_edid_base_block(const unsigned char* data, unsigned size,
                                         edid_identity& out);

} // namespace lx::drm

namespace {

void copy_trimmed(char (&dst)[64], const char* src, unsigned n) {
    unsigned len = 0;
    while (len < n && src[len] != '\0' && src[len] != '\n')
        ++len;
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\r'))
        --len;
    unsigned w = 0;
    for (unsigned i = 0; i < len && w + 1 < sizeof(dst); ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        // Control characters out (the strings are config keys and log text).
        if (c >= 0x20 && c != 0x7f)
            dst[w++] = static_cast<char>(c);
    }
    dst[w] = '\0';
}

} // namespace

bool lx::drm::parse_edid_base_block(const unsigned char* d, unsigned size, edid_identity& out) {
    out = {};
    static constexpr unsigned char k_header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (!d || size < 128 || std::memcmp(d, k_header, 8) != 0)
        return false;
    unsigned char sum = 0;
    for (unsigned i = 0; i < 128; ++i)
        sum = static_cast<unsigned char>(sum + d[i]);
    if (sum != 0)
        return false;
    // Manufacturer: three 5-bit letters, big-endian ('A' = 1).
    const unsigned m = (static_cast<unsigned>(d[8]) << 8) | d[9];
    const char pnp[4] = {static_cast<char>('@' + ((m >> 10) & 0x1f)),
                         static_cast<char>('@' + ((m >> 5) & 0x1f)),
                         static_cast<char>('@' + (m & 0x1f)), '\0'};
    if (pnp[0] > '@' && pnp[1] > '@' && pnp[2] > '@')
        std::snprintf(out.make, sizeof(out.make), "%s", pnp);
    const unsigned product = static_cast<unsigned>(d[10]) | (static_cast<unsigned>(d[11]) << 8);
    const unsigned serial_no = static_cast<unsigned>(d[12]) | (static_cast<unsigned>(d[13]) << 8) |
                               (static_cast<unsigned>(d[14]) << 16) |
                               (static_cast<unsigned>(d[15]) << 24);
    // Display descriptors: product name (0xFC) and serial string (0xFF).
    for (unsigned off = 54; off + 18 <= 126; off += 18) {
        const unsigned char* b = d + off;
        if (b[0] != 0 || b[1] != 0 || b[2] != 0)
            continue; // a detailed timing
        if (b[3] == 0xfc)
            copy_trimmed(out.model, reinterpret_cast<const char*>(b + 5), 13);
        else if (b[3] == 0xff)
            copy_trimmed(out.serial, reinterpret_cast<const char*>(b + 5), 13);
    }
    if (out.model[0] == '\0')
        std::snprintf(out.model, sizeof(out.model), "0x%04X", product);
    if (out.serial[0] == '\0' && serial_no != 0)
        std::snprintf(out.serial, sizeof(out.serial), "0x%08X", serial_no);
    out.valid = true;
    return true;
}

bool lx::drm::parse_edid_identity(const unsigned char* data, unsigned size, edid_identity& out) {
#if defined(LUMEN_HAS_DISPLAY_INFO)
    out = {};
    if (!data || size < 128)
        return false;
    di_info* info = di_info_parse_edid(data, size);
    if (!info)
        return parse_edid_base_block(data, size, out);
    const auto take = [](char (&dst)[64], char* s) {
        if (s) {
            copy_trimmed(dst, s, static_cast<unsigned>(std::strlen(s)));
            std::free(s);
        }
    };
    take(out.make, di_info_get_make(info));
    take(out.model, di_info_get_model(info));
    take(out.serial, di_info_get_serial(info));
    di_info_destroy(info);
    out.valid = true;
    return true;
#else
    return parse_edid_base_block(data, size, out);
#endif
}
