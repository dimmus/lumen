// The C headers lx.wayland.client's implementation units include in their global module
// fragment: libwayland-client, the generated client protocol headers, xkbcommon. Not a
// module interface; nothing outside modules/lx.wayland.client includes it.
//
// LX_WLC_REAL is 1 when libwayland-client and every protocol the wrapper speaks were built
// (LUMEN_PROTOCOL_<XML_STEM> from the manifest); otherwise the implementation compiles to
// not_implemented stubs, so the build succeeds with any subset present.
#pragma once

#include <errno.h>
#include <stdint.h>
#include <string.h>

#if defined(LUMEN_HAS_WAYLAND) && defined(LUMEN_HAS_PROTOCOL_GLUE) &&                          \
    defined(LUMEN_PROTOCOL_XDG_SHELL) && defined(LUMEN_PROTOCOL_XDG_DECORATION_UNSTABLE_V1) && \
    defined(LUMEN_PROTOCOL_LINUX_DMABUF_V1) && defined(LUMEN_PROTOCOL_LINUX_DRM_SYNCOBJ_V1) &&  \
    defined(LUMEN_PROTOCOL_VIEWPORTER) && defined(LUMEN_PROTOCOL_FRACTIONAL_SCALE_V1) &&        \
    defined(LUMEN_PROTOCOL_PRESENTATION_TIME) &&                                                 \
    defined(LUMEN_PROTOCOL_TEXT_INPUT_UNSTABLE_V3) && defined(LUMEN_PROTOCOL_CURSOR_SHAPE_V1) &&  \
    defined(LUMEN_PROTOCOL_SINGLE_PIXEL_BUFFER_V1) && defined(LUMEN_PROTOCOL_XDG_ACTIVATION_V1) && \
    defined(LUMEN_PROTOCOL_XDG_OUTPUT_UNSTABLE_V1) &&                                             \
    defined(LUMEN_PROTOCOL_PRIMARY_SELECTION_UNSTABLE_V1)
#define LX_WLC_REAL 1
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
// Every proxy this wrapper creates carries this tag (wl_proxy_set_tag), so a wl_surface in
// an event maps back to its lx::wayland::surface only when it is one of ours.
inline const char* const lx_wlc_tag = "lx.wayland.client";
#else
#define LX_WLC_REAL 0
// The stubs name the proxy types the partitions forward-declare in their global fragments.
struct wl_display;
struct wl_registry;
struct wl_output;
struct wl_seat;
struct zxdg_output_v1;
#endif

#if defined(LUMEN_HAS_XKBCOMMON)
#include <xkbcommon/xkbcommon.h>
#endif

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
