module;

// PNG encoding for xdg-desktop-portal-lumen's screenshots (libpng), and decoding for the
// unit test's round trip. Speed over size, measured on a 1914x924 desktop (GTK 4 widget
// factory and foot, this VM): zlib level 1 with the Sub filter 29.6 ms / 347 KB; grim's
// default (level 6, adaptive filters) 113.9 ms / 299 KB; level 1 adaptive 72.0 ms; level 6
// Sub 71.4 ms. A screenshot is written while the user waits for it, so 4x faster for 16 %
// more bytes is the better trade. Without libpng (LUMEN_PORTAL_PNG unset) both return
// not_implemented.

#include <cstdint>

export module lx.portal:png;

import lx.foundation;
import :compose;

export namespace lx::portal {

/// Writes `img` as an 8-bit RGB PNG to `fd` (not closed). `level` is zlib's 0–9.
[[nodiscard]] lx::result<void> write_png(int fd, const image& img, int level = 1);

/// Reads an RGB(A) PNG into 0xAARRGGBB pixels (tests and tools).
[[nodiscard]] lx::result<image> read_png(const char* path);

} // namespace lx::portal
