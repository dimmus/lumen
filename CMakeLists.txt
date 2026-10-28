cmake_minimum_required(VERSION 3.28)
project(lumen VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(LumenOptions)
include(LumenStdModule)
include(LumenCXXModules)
include(LumenPlatform)

# ── Codegen tool (must exist before protocol generation) ────────────────────
add_executable(wl-scanner-cpp
    tools/wl-scanner-cpp/main.cpp
)
target_include_directories(wl-scanner-cpp PRIVATE tools/wl-scanner-cpp)
target_compile_features(wl-scanner-cpp PRIVATE cxx_std_20)

include(LumenProtocols)

# ── Foundation ──────────────────────────────────────────────────────────────
lumen_add_module_library(lx.foundation modules/lx.foundation)
lumen_add_module_library(lx.sync modules/lx.sync DEPENDS lx.foundation)
lumen_add_module_library(lx.trace modules/lx.trace DEPENDS lx.foundation)
lumen_add_module_library(lx.runtime modules/lx.runtime DEPENDS lx.foundation lx.trace lx.sync)
# The session's health report (docs/subsystems/health.md): pure, filled on request.
lumen_add_module_library(lx.health modules/lx.health DEPENDS lx.foundation)
# The configuration file (design-session-and-wm Part C): schema, parser, layers, watch. Pure
# apart from reading files; keysym names from xkbcommon when it is there.
lumen_add_module_library(lx.config modules/lx.config DEPENDS lx.foundation)
if(XKBCOMMON_FOUND)
    target_include_directories(lx.config PRIVATE ${XKBCOMMON_INCLUDE_DIRS})
    target_link_libraries(lx.config PUBLIC ${XKBCOMMON_LIBRARIES})
    target_compile_definitions(lx.config PRIVATE LUMEN_HAS_XKBCOMMON=1)
endif()

# ── Platform ─────────────────────────────────────────────────────────────────
lumen_add_module_library(lx.input modules/lx.input DEPENDS lx.runtime lx.trace)
lumen_add_module_library(lx.drm modules/lx.drm DEPENDS lx.runtime)
lumen_add_module_library(lx.session modules/lx.session DEPENDS lx.runtime)
lumen_add_module_library(lx.scheduler modules/lx.scheduler DEPENDS lx.runtime lx.drm)

# ── Wayland ──────────────────────────────────────────────────────────────────
lumen_add_module_library(lx.wayland.protocols modules/lx.wayland.protocols
    GENERATED ${LUMEN_GENERATED_PROTOCOL_SOURCES})
add_dependencies(lx.wayland.protocols wl-scanner-cpp)

# Plain-C static lib of wayland-scanner private-code (wl_interface tables).
# Linked into lx.wayland.server / lx.compositor; not a C++ module.
if(LUMEN_HAS_WAYLAND_SCANNER AND LUMEN_PROTOCOL_GLUE_SOURCES)
    add_library(lumen-protocol-glue STATIC ${LUMEN_PROTOCOL_GLUE_SOURCES})
    # Headers are generated alongside .c — force generation via OBJECT deps.
    if(LUMEN_PROTOCOL_GLUE_HEADERS)
        # Touch a stamp so headers are built even when only .c are compiled.
        add_custom_target(lumen-protocol-glue-headers DEPENDS ${LUMEN_PROTOCOL_GLUE_HEADERS})
        add_dependencies(lumen-protocol-glue lumen-protocol-glue-headers)
    endif()
    target_include_directories(lumen-protocol-glue PUBLIC "${LUMEN_GENERATED_DIR}")
    # LUMEN_PROTOCOL_<XML_STEM>=1 per generated protocol: code gates on these, so a protocol
    # that appears or goes changes the compile commands and nothing stale survives a rebuild.
    target_compile_definitions(lumen-protocol-glue PUBLIC ${LUMEN_PROTOCOL_DEFINITIONS})
    if(WAYLAND_FOUND)
        target_include_directories(lumen-protocol-glue PUBLIC ${WAYLAND_INCLUDE_DIRS})
    endif()
    set_target_properties(lumen-protocol-glue PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_STANDARD 11
    )
endif()

lumen_add_module_library(lx.wayland.server modules/lx.wayland.server
    DEPENDS lx.runtime lx.wayland.protocols lx.input lx.session)
if(TARGET lumen-protocol-glue)
    target_link_libraries(lx.wayland.server PUBLIC lumen-protocol-glue)
    target_compile_definitions(lx.wayland.server PUBLIC LUMEN_HAS_PROTOCOL_GLUE=1)
    # Protocol handlers in lx.compositor include generated *-server-protocol.h
    target_include_directories(lx.wayland.server PUBLIC "${LUMEN_GENERATED_DIR}")
endif()
lumen_add_module_library(lx.wayland.client modules/lx.wayland.client
    DEPENDS lx.runtime lx.wayland.protocols)

# ── Graphics & UI ────────────────────────────────────────────────────────────
lumen_add_module_library(lx.gfx modules/lx.gfx
    DEPENDS lx.foundation lx.runtime lx.trace)
lumen_compile_shaders(lx.gfx
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/composite.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/composite.frag"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/encode.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/encode.frag"
    # ADR-010 UI primitives (lx.gfx:ui_vk); their code is shaders/ui/*.glsl, shared with ui_gl.
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_quad.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_quad.frag"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_shadow.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_shadow.frag"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_mono.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_mono.frag"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_poly.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_poly.frag"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_underline.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui_underline.frag")
# The same UI GLSL as strings for lx.gfx:ui_gl, which compiles it at run time (GLES 3.1).
lumen_embed_glsl(lx.gfx ui
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/math.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/vertex.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/fragment.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/quad.vs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/quad.fs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/shadow.vs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/shadow.fs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/sprite.vs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/sprite.fs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/underline.vs.glsl"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/ui/underline.fs.glsl")
# lx.text:raster writes glyphs into the lx.gfx:atlas (ADR-010 §4.13: tile handles only).
lumen_add_module_library(lx.text modules/lx.text DEPENDS lx.foundation lx.runtime lx.trace lx.gfx)
lumen_link_platform_parts(lx.text text)
# Bundled fonts (assets/fonts, ADR-010 §5.4), registered with fontconfig by lx.text:faces: the
# source tree's copy, the install relative to the executable, the configured install.
include(GNUInstallDirs)
target_compile_definitions(lx.text PRIVATE
    LUMEN_FONT_DIR_BUILD="${CMAKE_SOURCE_DIR}/assets/fonts"
    LUMEN_FONT_DIR_INSTALL_RELATIVE="${CMAKE_INSTALL_DATADIR}/lumen/fonts"
    LUMEN_FONT_DIR_INSTALL="${CMAKE_INSTALL_FULL_DATADIR}/lumen/fonts")
lumen_add_module_library(lx.layout modules/lx.layout DEPENDS lx.foundation lx.runtime)
lumen_add_module_library(lx.a11y modules/lx.a11y DEPENDS lx.foundation)
lumen_add_module_library(lx.scene modules/lx.scene
    DEPENDS lx.gfx lx.layout lx.scheduler lx.runtime lx.sync)
lumen_add_module_library(lx.ui modules/lx.ui DEPENDS lx.scene lx.text lx.runtime lx.gfx lx.wayland.client lx.layout lx.a11y)
# Compiled themes (ADR-010 §4.8) and bundled Lucide icons (§5.5): found in the build tree, an
# install relative to the executable, or the configured install (LUMEN_THEME_DIR /
# LUMEN_ICON_DIR override both). Icons are rasterised by librsvg, dlopen'd on first use as
# lx.shell does: headers only, no link edge.
target_compile_definitions(lx.ui PRIVATE
    LUMEN_THEME_DIR_BUILD="${CMAKE_BINARY_DIR}/themes"
    LUMEN_THEME_DIR_INSTALL_RELATIVE="${CMAKE_INSTALL_DATADIR}/lumen/themes"
    LUMEN_THEME_DIR_INSTALL="${CMAKE_INSTALL_FULL_DATADIR}/lumen/themes"
    LUMEN_ICON_DIR_BUILD="${CMAKE_SOURCE_DIR}/assets/icons/lucide"
    LUMEN_ICON_DIR_INSTALL_RELATIVE="${CMAKE_INSTALL_DATADIR}/lumen/icons/lucide"
    LUMEN_ICON_DIR_INSTALL="${CMAKE_INSTALL_FULL_DATADIR}/lumen/icons/lucide")
if(LIBRSVG_FOUND)
    target_compile_definitions(lx.ui PRIVATE LUMEN_HAS_RSVG=1)
    target_include_directories(lx.ui PRIVATE ${LIBRSVG_INCLUDE_DIRS})
    target_link_libraries(lx.ui PUBLIC ${CMAKE_DL_LIBS})
endif()
# The theme compiler: Zed theme JSON → .lxtheme through lx.ui:theme.compile. It compiles the
# bundled One Dark and One Light into the build tree as part of `all`.
add_executable(lumen-theme tools/lumen-theme/main.cpp)
target_link_libraries(lumen-theme PRIVATE lx.ui)
lumen_apply_module_properties(lumen-theme)
lumen_link_platform(lumen-theme)
set(LUMEN_THEME_SOURCE "${CMAKE_SOURCE_DIR}/assets/themes/one/one.json")
set(LUMEN_THEME_OUTPUTS
    "${CMAKE_BINARY_DIR}/themes/one-dark.lxtheme"
    "${CMAKE_BINARY_DIR}/themes/one-light.lxtheme")
add_custom_command(
    OUTPUT ${LUMEN_THEME_OUTPUTS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/themes"
    COMMAND lumen-theme "${LUMEN_THEME_SOURCE}" "${CMAKE_BINARY_DIR}/themes/one-dark.lxtheme"
            --theme "One Dark" --quiet
    COMMAND lumen-theme "${LUMEN_THEME_SOURCE}" "${CMAKE_BINARY_DIR}/themes/one-light.lxtheme"
            --theme "One Light" --quiet
    DEPENDS lumen-theme "${LUMEN_THEME_SOURCE}"
    COMMENT "Compiling the bundled themes (One Dark, One Light)"
    VERBATIM)
add_custom_target(lumen-themes ALL DEPENDS ${LUMEN_THEME_OUTPUTS})
lumen_add_module_library(lx.ui.builder modules/lx.ui.builder DEPENDS lx.ui)

# ── Desktop ──────────────────────────────────────────────────────────────────
lumen_add_module_library(lx.shell.policy modules/lx.shell.policy
    DEPENDS lx.wayland.client lx.wayland.protocols)
lumen_add_module_library(lx.compositor modules/lx.compositor
    DEPENDS lx.wayland.server lx.gfx lx.text lx.scene lx.scheduler lx.input lx.drm lx.runtime lx.sync lx.trace lx.session lx.health lx.config)
# lumen-shell talks libwayland-client directly (lx.wayland.client is still a stub): the
# zlm_shell_v1, wlr-layer-shell and cursor-shape client glue comes from lumen-protocol-glue.
lumen_add_module_library(lx.shell modules/lx.shell
    DEPENDS lx.shell.policy lx.text lx.trace lx.config)
lumen_link_platform_parts(lx.shell wayland_client xkb text systemd)
if(TARGET lumen-protocol-glue AND WAYLAND_FOUND)
    target_link_libraries(lx.shell PRIVATE lumen-protocol-glue)
    target_compile_definitions(lx.shell PRIVATE LUMEN_SHELL_WAYLAND=1)
endif()
# Icon decoding for the panel (lx.shell:icon_theme) and background images
# (lx.shell:background), each optional.
if(LIBPNG_FOUND)
    target_compile_definitions(lx.shell PRIVATE LUMEN_HAS_PNG=1)
    target_include_directories(lx.shell PRIVATE ${LIBPNG_INCLUDE_DIRS})
    target_link_libraries(lx.shell PUBLIC ${LIBPNG_LINK_LIBRARIES})
endif()
if(LIBRSVG_FOUND)
    # Headers only: librsvg (and the GLib/cairo stack under it) is dlopen'd on the first SVG
    # (lx.shell:svg), not linked.
    target_compile_definitions(lx.shell PRIVATE LUMEN_HAS_RSVG=1)
    target_include_directories(lx.shell PRIVATE ${LIBRSVG_INCLUDE_DIRS})
    target_link_libraries(lx.shell PUBLIC ${CMAKE_DL_LIBS})
endif()
if(LIBTURBOJPEG_FOUND)
    target_compile_definitions(lx.shell PRIVATE LUMEN_HAS_JPEG=1)
    target_include_directories(lx.shell PRIVATE ${LIBTURBOJPEG_INCLUDE_DIRS})
    target_link_libraries(lx.shell PUBLIC ${LIBTURBOJPEG_LINK_LIBRARIES})
endif()
if(LIBWEBP_FOUND)
    target_compile_definitions(lx.shell PRIVATE LUMEN_HAS_WEBP=1)
    target_include_directories(lx.shell PRIVATE ${LIBWEBP_INCLUDE_DIRS})
    target_link_libraries(lx.shell PUBLIC ${LIBWEBP_LINK_LIBRARIES})
endif()

if(LUMEN_BUILD_XWAYLAND)
    lumen_add_module_library(lx.compositor.xwayland modules/lx.compositor.xwayland
        DEPENDS lx.wayland.server lx.foundation)
endif()
lumen_add_module_library(lx.app modules/lx.app
    DEPENDS lx.ui.builder lx.wayland.client lx.runtime)

# Platform APIs must link on MODULE libraries (not only executables).
lumen_link_platform_modules(
    lx.wayland.server
    lx.gfx
    lx.drm
    lx.input
    lx.session
    lx.compositor
)

# ── Executables ──────────────────────────────────────────────────────────────
add_executable(lumen-compositor executables/lumen-compositor/main.cpp)
target_link_libraries(lumen-compositor PRIVATE lx.compositor)
lumen_apply_module_properties(lumen-compositor)
lumen_check_executable_layers(lumen-compositor lx.compositor)
lumen_link_platform(lumen-compositor)

add_executable(lumen-shell executables/lumen-shell/main.cpp)
target_link_libraries(lumen-shell PRIVATE lx.shell)
lumen_apply_module_properties(lumen-shell)
lumen_check_executable_layers(lumen-shell lx.shell)
lumen_link_platform_parts(lumen-shell wayland_client xkb text systemd)

# lumen-ctl: the running session's control tool, a Varlink client of the compositor's
# control socket (design-session-and-wm S.8). Plain C++ on libsystemd (sd-varlink, sd-json,
# sd-bus for the units): no Lumen module, so it starts in a millisecond. sd-varlink is public
# since systemd 257 (LUMEN_HAS_VARLINK, cmake/LumenPlatform.cmake).
if(LUMEN_HAS_VARLINK)
    # `config check` / `config defaults` / `action --list` use lx.config (config.cpp); the
    # documented default file is compiled in.
    file(READ "${CMAKE_SOURCE_DIR}/deploy/lumen/config.toml" LUMEN_DEFAULT_CONFIG_TEXT)
    configure_file("${CMAKE_SOURCE_DIR}/executables/lumen-ctl/default_config.inc.in"
        "${CMAKE_BINARY_DIR}/generated/lumen-ctl/lumen_default_config.inc" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/deploy/lumen/config.toml")
    add_executable(lumen-ctl executables/lumen-ctl/main.cpp executables/lumen-ctl/config.cpp)
    target_include_directories(lumen-ctl PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS}
        "${CMAKE_BINARY_DIR}/generated/lumen-ctl")
    target_link_libraries(lumen-ctl PRIVATE ${LIBSYSTEMD_LIBRARIES} lx.config)
    lumen_apply_module_properties(lumen-ctl)
else()
    message(STATUS "no libsystemd >= 257 — lumen-ctl and the control socket unavailable")
endif()

# The session's files for runs from the build tree (run-desktop.sh --systemd): the units
# and lumen-session pointing at this build directory. Installed copies are configured at
# install time instead (cmake/LumenInstallSession.cmake).
set(LUMEN_BINDIR "${CMAKE_BINARY_DIR}")
foreach(_unit lumen-compositor.service lumen-shell.service)
    configure_file("${CMAKE_SOURCE_DIR}/deploy/systemd/${_unit}.in"
        "${CMAKE_BINARY_DIR}/share/systemd/user/${_unit}" @ONLY)
endforeach()
configure_file("${CMAKE_SOURCE_DIR}/deploy/systemd/lumen-shutdown.target"
    "${CMAKE_BINARY_DIR}/share/systemd/user/lumen-shutdown.target" COPYONLY)
configure_file("${CMAKE_SOURCE_DIR}/scripts/lumen-session.in"
    "${CMAKE_BINARY_DIR}/lumen-session" @ONLY
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                     WORLD_READ WORLD_EXECUTE)
configure_file("${CMAKE_SOURCE_DIR}/deploy/wayland-sessions/lumen.desktop.in"
    "${CMAKE_BINARY_DIR}/share/wayland-sessions/lumen.desktop" @ONLY)

# Diagnostic: exercises lx.input alone, so "typing does nothing" can be narrowed to
# permissions, devices, translation or routing instead of testing all four at once.
add_executable(lumen-input-probe executables/lumen-input-probe/main.cpp)
target_link_libraries(lumen-input-probe PRIVATE lx.input lx.session lx.trace)
lumen_apply_module_properties(lumen-input-probe)
lumen_link_platform(lumen-input-probe)

add_executable(lumen-hello executables/lumen-hello/main.cpp)
target_link_libraries(lumen-hello PRIVATE lx.app)
lumen_apply_module_properties(lumen-hello)

# Demo client: needs xdg-shell *client* glue, which the server-side codegen does not emit.
if(WAYLAND_FOUND AND LUMEN_HAS_WAYLAND_SCANNER)
    set(LUMEN_XDG_SHELL_XML "${CMAKE_SOURCE_DIR}/protocols/upstream/stable/xdg-shell/xdg-shell.xml")
    if(NOT EXISTS "${LUMEN_XDG_SHELL_XML}")
        pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        set(LUMEN_XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
    endif()

    if(EXISTS "${LUMEN_XDG_SHELL_XML}")
        set(LUMEN_CLOUDS_GEN "${CMAKE_BINARY_DIR}/generated/clients")
        file(MAKE_DIRECTORY "${LUMEN_CLOUDS_GEN}")
        add_custom_command(
            OUTPUT "${LUMEN_CLOUDS_GEN}/xdg-shell-client-protocol.h"
            COMMAND "${WAYLAND_SCANNER_EXE}" client-header "${LUMEN_XDG_SHELL_XML}"
                    "${LUMEN_CLOUDS_GEN}/xdg-shell-client-protocol.h"
            DEPENDS "${LUMEN_XDG_SHELL_XML}"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${LUMEN_CLOUDS_GEN}/xdg-shell-protocol.c"
            COMMAND "${WAYLAND_SCANNER_EXE}" private-code "${LUMEN_XDG_SHELL_XML}"
                    "${LUMEN_CLOUDS_GEN}/xdg-shell-protocol.c"
            DEPENDS "${LUMEN_XDG_SHELL_XML}"
            VERBATIM
        )

        add_executable(lumen-clouds
            executables/lumen-clouds/main.cpp
            "${LUMEN_CLOUDS_GEN}/xdg-shell-protocol.c"
            "${LUMEN_CLOUDS_GEN}/xdg-shell-client-protocol.h"
        )
        target_include_directories(lumen-clouds PRIVATE "${LUMEN_CLOUDS_GEN}")
        lumen_link_platform(lumen-clouds)
        target_link_libraries(lumen-clouds PRIVATE m)

        # Interactive move/resize demo. Hovering its border sets resize cursors through
        # wp_cursor_shape_v1 when that protocol's XML is available; the glue references
        # zwp_tablet_tool_v2, so tablet-v2 interface code is generated alongside it.
        add_executable(lumen-square
            executables/lumen-square/main.cpp
            "${LUMEN_CLOUDS_GEN}/xdg-shell-protocol.c"
            "${LUMEN_CLOUDS_GEN}/xdg-shell-client-protocol.h"
        )
        set(LUMEN_CURSOR_SHAPE_XML "${CMAKE_SOURCE_DIR}/protocols/upstream/staging/cursor-shape/cursor-shape-v1.xml")
        set(LUMEN_TABLET_XML "${CMAKE_SOURCE_DIR}/protocols/upstream/unstable/tablet/tablet-unstable-v2.xml")
        if(EXISTS "${LUMEN_CURSOR_SHAPE_XML}" AND EXISTS "${LUMEN_TABLET_XML}")
            add_custom_command(
                OUTPUT "${LUMEN_CLOUDS_GEN}/cursor-shape-v1-client-protocol.h"
                COMMAND "${WAYLAND_SCANNER_EXE}" client-header "${LUMEN_CURSOR_SHAPE_XML}"
                        "${LUMEN_CLOUDS_GEN}/cursor-shape-v1-client-protocol.h"
                DEPENDS "${LUMEN_CURSOR_SHAPE_XML}"
                VERBATIM
            )
            add_custom_command(
                OUTPUT "${LUMEN_CLOUDS_GEN}/cursor-shape-v1-protocol.c"
                COMMAND "${WAYLAND_SCANNER_EXE}" private-code "${LUMEN_CURSOR_SHAPE_XML}"
                        "${LUMEN_CLOUDS_GEN}/cursor-shape-v1-protocol.c"
                DEPENDS "${LUMEN_CURSOR_SHAPE_XML}"
                VERBATIM
            )
            add_custom_command(
                OUTPUT "${LUMEN_CLOUDS_GEN}/tablet-unstable-v2-protocol.c"
                COMMAND "${WAYLAND_SCANNER_EXE}" private-code "${LUMEN_TABLET_XML}"
                        "${LUMEN_CLOUDS_GEN}/tablet-unstable-v2-protocol.c"
                DEPENDS "${LUMEN_TABLET_XML}"
                VERBATIM
            )
            target_sources(lumen-square PRIVATE
                "${LUMEN_CLOUDS_GEN}/cursor-shape-v1-protocol.c"
                "${LUMEN_CLOUDS_GEN}/cursor-shape-v1-client-protocol.h"
                "${LUMEN_CLOUDS_GEN}/tablet-unstable-v2-protocol.c"
            )
            target_compile_definitions(lumen-square PRIVATE LUMEN_SQUARE_HAS_CURSOR_SHAPE)
        else()
            message(STATUS "cursor-shape-v1.xml not found — lumen-square builds without resize cursors")
        endif()
        target_include_directories(lumen-square PRIVATE "${LUMEN_CLOUDS_GEN}")
        lumen_link_platform(lumen-square)

        # Deterministic protocol client for scripts/app-smoke.sh (popups, layer shell,
        # selections). Its client glue is lumen-protocol-glue's — every manifest protocol —
        # rather than private copies, which would define xdg_wm_base_interface twice.
        if(LUMEN_BUILD_TESTS AND TARGET lumen-protocol-glue)
            add_executable(lumen-probe tests/clients/lumen-probe.cpp tests/clients/lumen-probe-output-mgmt.cpp
                tests/clients/lumen-probe-compat.cpp tests/clients/lumen-probe-window.cpp
                tests/clients/lumen-probe-pointer.cpp tests/clients/lumen-probe-capture.cpp
                tests/clients/lumen-probe-input.cpp tests/clients/lumen-probe-workspace.cpp
                tests/clients/lumen-probe-sni.cpp)
            target_link_libraries(lumen-probe PRIVATE lumen-protocol-glue)
            lumen_link_platform(lumen-probe)
            target_compile_definitions(lumen-probe PRIVATE LUMEN_PROBE_HAS_LAYER_SHELL)
        endif()
    else()
        message(STATUS "xdg-shell.xml not found — skipping lumen-clouds and lumen-square")
    endif()
else()
    message(STATUS "wayland-client/scanner unavailable — skipping lumen-clouds and lumen-square")
endif()

# ── Portal backend: xdg-desktop-portal-lumen (design-app-compat-capture Part E) ─
# Screenshot/PickColor for xdg-desktop-portal and the ScreenCast source chooser of
# xdg-desktop-portal-wlr. A Wayland client (the overlay draws with lx.shell's shm canvas)
# and a D-Bus service (sd-bus); without libpng it builds but cannot write files.
set(LUMEN_PORTAL_BUILT FALSE)
if(LUMEN_BUILD_PORTAL)
    if(LIBSYSTEMD_FOUND AND WAYLAND_FOUND AND TARGET lumen-protocol-glue)
        lumen_add_module_library(lx.portal modules/lx.portal
            DEPENDS lx.foundation lx.trace lx.text lx.shell)
        target_link_libraries(lx.portal PRIVATE lumen-protocol-glue)
        target_compile_definitions(lx.portal PRIVATE LUMEN_PORTAL_WAYLAND=1 LUMEN_PORTAL_DBUS=1)
        target_include_directories(lx.portal PRIVATE ${WAYLAND_INCLUDE_DIRS} ${LIBSYSTEMD_INCLUDE_DIRS})
        target_link_libraries(lx.portal PUBLIC ${WAYLAND_LIBRARIES} ${LIBSYSTEMD_LIBRARIES})
        if(LIBPNG_FOUND)
            target_compile_definitions(lx.portal PRIVATE LUMEN_PORTAL_PNG=1)
            target_include_directories(lx.portal PRIVATE ${LIBPNG_INCLUDE_DIRS})
            target_link_libraries(lx.portal PUBLIC ${LIBPNG_LINK_LIBRARIES})
        else()
            message(STATUS "libpng not found — xdg-desktop-portal-lumen cannot save screenshots")
        endif()
        add_executable(xdg-desktop-portal-lumen executables/xdg-desktop-portal-lumen/main.cpp)
        target_link_libraries(xdg-desktop-portal-lumen PRIVATE lx.portal)
        lumen_apply_module_properties(xdg-desktop-portal-lumen)
        lumen_check_executable_layers(xdg-desktop-portal-lumen lx.portal)
        set(LUMEN_PORTAL_BUILT TRUE)

        # Deploy files: installed ones are configured at install time against the prefix
        # (cmake/LumenInstallSession.cmake); the build tree gets its own copies under share/
        # (pointing at the build tree) for run-desktop.sh sessions.
        set(LUMEN_PORTAL_EXEC "$<TARGET_FILE:xdg-desktop-portal-lumen>")
        set(LUMEN_SHARE_DIR "${CMAKE_BINARY_DIR}/share")
        # The D-Bus service and its unit for a build-tree systemd session (run-desktop.sh
        # --systemd links them into $XDG_RUNTIME_DIR for that run only).
        foreach(_f org.freedesktop.impl.portal.desktop.lumen.service xdg-desktop-portal-lumen.service)
            set(_src "${CMAKE_SOURCE_DIR}/deploy/portal/${_f}.in")
            set(_dst "${LUMEN_SHARE_DIR}/dbus-1/services/${_f}")
            if(_f STREQUAL "xdg-desktop-portal-lumen.service")
                set(_src "${CMAKE_SOURCE_DIR}/deploy/systemd/${_f}.in")
                set(_dst "${LUMEN_SHARE_DIR}/systemd/user/${_f}")
            endif()
            file(READ "${_src}" _content)
            string(REPLACE "@LUMEN_PORTAL_EXEC@" "${LUMEN_PORTAL_EXEC}" _content "${_content}")
            file(GENERATE OUTPUT "${_dst}" CONTENT "${_content}")
        endforeach()
        file(MAKE_DIRECTORY "${LUMEN_SHARE_DIR}/xdg-desktop-portal/portals"
            "${LUMEN_SHARE_DIR}/xdg/xdg-desktop-portal-wlr")
        configure_file("${CMAKE_SOURCE_DIR}/deploy/portal/lumen.portal"
            "${LUMEN_SHARE_DIR}/xdg-desktop-portal/portals/lumen.portal" COPYONLY)
        configure_file("${CMAKE_SOURCE_DIR}/deploy/portal/lumen-portals.conf"
            "${LUMEN_SHARE_DIR}/xdg-desktop-portal/lumen-portals.conf" COPYONLY)
        # The chooser path is the built binary's, known only at generate time.
        file(READ "${CMAKE_SOURCE_DIR}/deploy/xdg/xdg-desktop-portal-wlr/Lumen.in" _xdpw)
        string(REPLACE "@LUMEN_PORTAL_EXEC@" "$<TARGET_FILE:xdg-desktop-portal-lumen>" _xdpw "${_xdpw}")
        file(GENERATE OUTPUT "${LUMEN_SHARE_DIR}/xdg/xdg-desktop-portal-wlr/Lumen" CONTENT "${_xdpw}")
    else()
        message(STATUS "xdg-desktop-portal-lumen skipped: needs libsystemd, wayland-client and the protocol glue")
    endif()
endif()

# Portal client for app-smoke group Z: the frontend's Screenshot/PickColor/ScreenCast, and a
# PipeWire consumer of the cast (libpipewire optional: without it the stream is not read).
if(LUMEN_BUILD_TESTS AND LIBSYSTEMD_FOUND)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
    endif()
    add_executable(lumen-portal-probe tests/clients/lumen-portal-probe.cpp)
    target_compile_features(lumen-portal-probe PRIVATE cxx_std_20)
    target_include_directories(lumen-portal-probe PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})
    target_link_libraries(lumen-portal-probe PRIVATE ${LIBSYSTEMD_LIBRARIES} m)
    if(PIPEWIRE_FOUND)
        target_compile_definitions(lumen-portal-probe PRIVATE LUMEN_PROBE_HAS_PIPEWIRE=1)
        target_include_directories(lumen-portal-probe PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
        target_compile_options(lumen-portal-probe PRIVATE ${PIPEWIRE_CFLAGS_OTHER})
        target_link_libraries(lumen-portal-probe PRIVATE ${PIPEWIRE_LIBRARIES})
    else()
        message(STATUS "libpipewire not found — lumen-portal-probe cannot read ScreenCast streams")
    endif()
endif()

# ── Install ──────────────────────────────────────────────────────────────────
# Every destination is relative to the prefix, so `cmake --install build --prefix DIR`
# installs a complete, self-consistent tree into DIR (the session files name DIR's paths).
include(GNUInstallDirs)
install(TARGETS lumen-compositor lumen-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET lumen-ctl)
    install(TARGETS lumen-ctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY protocols/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/protocols
    FILES_MATCHING PATTERN "*.xml" PATTERN "*.toml")
# The documented configuration (every key at its default): a template to copy to
# ~/.config/lumen/config.toml, not a layer itself (lumen-ctl config defaults prints it too).
install(FILES deploy/lumen/config.toml DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen)
# The bundled UI and mono fonts with their licenses (lx.text:faces finds them here).
install(DIRECTORY assets/fonts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/fonts)
# The compiled bundled themes and the Lucide icons with their licenses (lx.ui finds them here).
install(FILES ${LUMEN_THEME_OUTPUTS} DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/themes)
# They are compiled from Zed's MIT-licensed one.json: its notice goes with them.
install(FILES assets/themes/one/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/themes
    RENAME LICENSE-one)
install(DIRECTORY assets/icons/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/icons)
if(LUMEN_PORTAL_BUILT)
    install(TARGETS xdg-desktop-portal-lumen RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
    install(FILES deploy/portal/lumen.portal
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
    install(FILES deploy/portal/lumen-portals.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
endif()
# The session: systemd user units, share/wayland-sessions/lumen.desktop, bin/lumen-session
# and the portal service files, configured with the install prefix's paths. The unit
# directory: LUMEN_SYSTEMD_USER_UNIT_DIR if set; systemd's own (pkg-config) for /usr;
# else <prefix>/lib/systemd/user. Nothing is enabled: lumen-session starts the session.
set(LUMEN_SYSTEMD_USER_UNIT_DIR "" CACHE STRING
    "systemd user unit directory (empty: systemd's for /usr, else <prefix>/lib/systemd/user)")
set(_lumen_unit_dir_usr "")
if(PkgConfig_FOUND)
    pkg_get_variable(_lumen_unit_dir_usr systemd systemduserunitdir)
endif()
install(CODE "
    set(LUMEN_SOURCE_DIR \"${CMAKE_SOURCE_DIR}\")
    set(LUMEN_STAGE_DIR \"${CMAKE_BINARY_DIR}/install-stage\")
    set(LUMEN_UNIT_DIR_OVERRIDE \"${LUMEN_SYSTEMD_USER_UNIT_DIR}\")
    set(LUMEN_SYSTEMD_UNIT_DIR_USR \"${_lumen_unit_dir_usr}\")
    set(LUMEN_PORTAL_BUILT ${LUMEN_PORTAL_BUILT})
    set(LUMEN_BINDIR_REL \"${CMAKE_INSTALL_BINDIR}\")
    set(LUMEN_LIBEXECDIR_REL \"${CMAKE_INSTALL_LIBEXECDIR}\")
    set(LUMEN_DATADIR_REL \"${CMAKE_INSTALL_DATADIR}\")
    set(LUMEN_UNIT_SERVICE_TYPE \"${LUMEN_UNIT_SERVICE_TYPE}\")
    set(LUMEN_UNIT_EXEC_RELOAD \"${LUMEN_UNIT_EXEC_RELOAD}\")
")
install(SCRIPT cmake/LumenInstallSession.cmake)

enable_testing()

# wl-scanner-cpp golden: generate + compile smoke (always registered; needs built scanner)
add_test(
    NAME scanner_golden
    COMMAND
        bash "${CMAKE_SOURCE_DIR}/tests/test_scanner_golden.sh"
        "$<TARGET_FILE:wl-scanner-cpp>"
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/scanner-golden"
        "${CMAKE_CXX_COMPILER}"
)
set_tests_properties(scanner_golden PROPERTIES
    DEPENDS wl-scanner-cpp
)

if(LUMEN_BUILD_TESTS)
    add_executable(test_result tests/test_result.cpp)
    target_link_libraries(test_result PRIVATE lx.foundation)
    lumen_apply_module_properties(test_result)
    target_include_directories(test_result PRIVATE tests)
    add_test(NAME test_result COMMAND test_result)

    # The configuration file (design-session-and-wm Part 2): lx.config alone, and the
    # documented default file it must parse to the defaults.
    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config PRIVATE lx.config)
    lumen_apply_module_properties(test_config)
    target_compile_definitions(test_config PRIVATE LUMEN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_include_directories(test_config PRIVATE tests)
    add_test(NAME test_config COMMAND test_config)

    # Pointer device settings: the configuration → libinput setter calls (recording ops).
    add_executable(test_input_config tests/test_input_config.cpp)
    target_link_libraries(test_input_config PRIVATE lx.compositor lx.input lx.config)
    lumen_apply_module_properties(test_input_config)
    lumen_link_platform(test_input_config)
    target_include_directories(test_input_config PRIVATE tests)
    add_test(NAME test_input_config COMMAND test_input_config)

    # The session (design-session-and-wm Part 1): health levels, logind session discovery,
    # the control socket, lumen-session against a fake systemctl.
    add_executable(test_health tests/test_health.cpp)
    target_link_libraries(test_health PRIVATE lx.health)
    lumen_apply_module_properties(test_health)
    target_include_directories(test_health PRIVATE tests)
    add_test(NAME test_health COMMAND test_health)

    add_executable(test_session_discovery tests/test_session_discovery.cpp)
    target_link_libraries(test_session_discovery PRIVATE lx.session)
    lumen_apply_module_properties(test_session_discovery)
    lumen_link_platform(test_session_discovery)
    target_include_directories(test_session_discovery PRIVATE tests)
    add_test(NAME test_session_discovery COMMAND test_session_discovery)

    if(LUMEN_HAS_VARLINK)
        add_executable(test_control tests/test_control.cpp)
        target_link_libraries(test_control PRIVATE lx.compositor)
        lumen_apply_module_properties(test_control)
        lumen_link_platform(test_control)
        target_include_directories(test_control PRIVATE tests)
        add_test(NAME test_control COMMAND test_control)
        set_tests_properties(test_control PROPERTIES TIMEOUT 60)
    endif()

    add_test(NAME test_lumen_session
        COMMAND bash "${CMAKE_SOURCE_DIR}/tests/test_lumen_session.sh" "${CMAKE_SOURCE_DIR}"
                "${CMAKE_BINARY_DIR}/test_lumen_session")
    set_tests_properties(test_lumen_session PROPERTIES TIMEOUT 60)

    add_executable(test_logf tests/test_logf.cpp)
    target_link_libraries(test_logf PRIVATE lx.trace)
    lumen_apply_module_properties(test_logf)
    target_include_directories(test_logf PRIVATE tests)
    add_test(NAME test_logf COMMAND test_logf)

    add_executable(test_snapshot tests/test_snapshot.cpp)
    target_link_libraries(test_snapshot PRIVATE lx.scene lx.runtime)
    lumen_apply_module_properties(test_snapshot)
    target_include_directories(test_snapshot PRIVATE tests)
    add_test(NAME test_snapshot COMMAND test_snapshot)

    add_executable(test_primitives tests/test_primitives.cpp)
    target_link_libraries(test_primitives PRIVATE lx.ui lx.scene lx.gfx lx.runtime)
    lumen_apply_module_properties(test_primitives)
    lumen_link_platform(test_primitives)
    target_include_directories(test_primitives PRIVATE tests)
    add_test(NAME test_primitives COMMAND test_primitives)

    add_executable(test_atlas tests/test_atlas.cpp)
    target_link_libraries(test_atlas PRIVATE lx.gfx lx.runtime)
    lumen_apply_module_properties(test_atlas)
    lumen_link_platform(test_atlas)
    target_include_directories(test_atlas PRIVATE tests)
    add_test(NAME test_atlas COMMAND test_atlas)

    # The UI primitive golden images (ADR-010 §8.4): references in tests/golden/ui/*.png,
    # read and written through libpng. LUMEN_UI_GOLDEN_UPDATE=1 rewrites them.
    if(LIBPNG_FOUND)
        add_executable(test_ui_golden tests/test_ui_golden.cpp)
        target_link_libraries(test_ui_golden PRIVATE lx.ui lx.scene lx.gfx lx.runtime lx.a11y
            ${LIBPNG_LINK_LIBRARIES})
        lumen_apply_module_properties(test_ui_golden)
        lumen_link_platform(test_ui_golden)
        target_include_directories(test_ui_golden PRIVATE tests ${LIBPNG_INCLUDE_DIRS})
        target_compile_definitions(test_ui_golden PRIVATE
            LUMEN_UI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/ui"
            LUMEN_TEST_FONT_DIR="${CMAKE_SOURCE_DIR}/tests/fonts")
        add_test(NAME test_ui_golden COMMAND test_ui_golden)
        # The component gallery (ADR-010 Phase 3): docs/images/ui-gallery-one-{dark,light}.png
        # are its CPU renderings; --check redraws them (bit for bit) and on GL (G10).
        add_executable(lumen-ui-gallery tests/ui_gallery.cpp)
        target_link_libraries(lumen-ui-gallery PRIVATE lx.ui lx.scene lx.gfx lx.runtime lx.a11y lx.trace
            ${LIBPNG_LINK_LIBRARIES})
        lumen_apply_module_properties(lumen-ui-gallery)
        lumen_link_platform(lumen-ui-gallery)
        target_include_directories(lumen-ui-gallery PRIVATE tests ${LIBPNG_INCLUDE_DIRS})
        target_compile_definitions(lumen-ui-gallery PRIVATE
            LUMEN_GALLERY_DIR="${CMAKE_SOURCE_DIR}/docs/images"
            LUMEN_TEST_FONT_DIR="${CMAKE_SOURCE_DIR}/tests/fonts")
        add_test(NAME ui_gallery COMMAND lumen-ui-gallery --check)
        # The Vulkan cases again under VK_LAYER_KHRONOS_validation, which must log no error.
        find_file(LUMEN_VK_VALIDATION_LAYER VkLayer_khronos_validation.json
            PATHS /usr/share/vulkan/explicit_layer.d /usr/local/share/vulkan/explicit_layer.d
                  /etc/vulkan/explicit_layer.d
            NO_DEFAULT_PATH)
        if(LUMEN_VK_VALIDATION_LAYER)
            add_test(NAME test_ui_golden_vk_validation COMMAND test_ui_golden --filter=vk_)
        endif()
    else()
        message(STATUS "libpng not found — test_ui_golden (UI golden images) not built")
    endif()

    add_executable(test_scene_outputs tests/test_scene_outputs.cpp)
    target_link_libraries(test_scene_outputs PRIVATE lx.scene lx.runtime)
    lumen_apply_module_properties(test_scene_outputs)
    target_include_directories(test_scene_outputs PRIVATE tests)
    add_test(NAME test_scene_outputs COMMAND test_scene_outputs)

    add_executable(test_damage_region tests/test_damage_region.cpp)
    target_link_libraries(test_damage_region PRIVATE lx.scene lx.gfx lx.runtime)
    lumen_apply_module_properties(test_damage_region)
    target_include_directories(test_damage_region PRIVATE tests)
    add_test(NAME test_damage_region COMMAND test_damage_region)

    add_executable(test_wm_geometry tests/test_wm_geometry.cpp)
    target_link_libraries(test_wm_geometry PRIVATE lx.compositor)
    lumen_apply_module_properties(test_wm_geometry)
    lumen_link_platform(test_wm_geometry)
    target_include_directories(test_wm_geometry PRIVATE tests)
    add_test(NAME test_wm_geometry COMMAND test_wm_geometry)

    add_executable(test_shm_staging tests/test_shm_staging.cpp)
    target_link_libraries(test_shm_staging PRIVATE lx.compositor)
    lumen_apply_module_properties(test_shm_staging)
    lumen_link_platform(test_shm_staging)
    target_include_directories(test_shm_staging PRIVATE tests)
    add_test(NAME test_shm_staging COMMAND test_shm_staging)

    add_executable(test_positioner tests/test_positioner.cpp)
    target_link_libraries(test_positioner PRIVATE lx.compositor)
    lumen_apply_module_properties(test_positioner)
    lumen_link_platform(test_positioner)
    target_include_directories(test_positioner PRIVATE tests)
    add_test(NAME test_positioner COMMAND test_positioner)

    add_executable(test_data_transfer tests/test_data_transfer.cpp)
    target_link_libraries(test_data_transfer PRIVATE lx.compositor)
    lumen_apply_module_properties(test_data_transfer)
    lumen_link_platform(test_data_transfer)
    target_include_directories(test_data_transfer PRIVATE tests)
    add_test(NAME test_data_transfer COMMAND test_data_transfer)

    add_executable(test_session_pause tests/test_session_pause.cpp)
    target_link_libraries(test_session_pause PRIVATE lx.compositor)
    lumen_apply_module_properties(test_session_pause)
    lumen_link_platform(test_session_pause)
    target_include_directories(test_session_pause PRIVATE tests)
    add_test(NAME test_session_pause COMMAND test_session_pause)

    add_executable(test_viewport_geometry tests/test_viewport_geometry.cpp)
    target_link_libraries(test_viewport_geometry PRIVATE lx.compositor)
    lumen_apply_module_properties(test_viewport_geometry)
    lumen_link_platform(test_viewport_geometry)
    target_include_directories(test_viewport_geometry PRIVATE tests)
    add_test(NAME test_viewport_geometry COMMAND test_viewport_geometry)

    add_executable(test_presentation_feedback tests/test_presentation_feedback.cpp)
    target_link_libraries(test_presentation_feedback PRIVATE lx.compositor)
    lumen_apply_module_properties(test_presentation_feedback)
    lumen_link_platform(test_presentation_feedback)
    target_include_directories(test_presentation_feedback PRIVATE tests)
    add_test(NAME test_presentation_feedback COMMAND test_presentation_feedback)

    add_executable(test_display_power tests/test_display_power.cpp)
    target_link_libraries(test_display_power PRIVATE lx.compositor)
    lumen_apply_module_properties(test_display_power)
    lumen_link_platform(test_display_power)
    target_include_directories(test_display_power PRIVATE tests)
    add_test(NAME test_display_power COMMAND test_display_power)

    add_executable(test_session_lock tests/test_session_lock.cpp)
    target_link_libraries(test_session_lock PRIVATE lx.compositor)
    lumen_apply_module_properties(test_session_lock)
    lumen_link_platform(test_session_lock)
    target_include_directories(test_session_lock PRIVATE tests)
    add_test(NAME test_session_lock COMMAND test_session_lock)

    add_executable(test_text_input tests/test_text_input.cpp)
    target_link_libraries(test_text_input PRIVATE lx.compositor)
    lumen_apply_module_properties(test_text_input)
    lumen_link_platform(test_text_input)
    target_include_directories(test_text_input PRIVATE tests)
    add_test(NAME test_text_input COMMAND test_text_input)

    # Screen capture's Wayland-free parts: policy, damage ring, the CPU render side.
    add_executable(test_capture tests/test_capture.cpp)
    target_link_libraries(test_capture PRIVATE lx.compositor)
    lumen_apply_module_properties(test_capture)
    lumen_link_platform(test_capture)
    target_include_directories(test_capture PRIVATE tests)
    add_test(NAME test_capture COMMAND test_capture)

    # Screen capture on the GL backend against the render node (skips without hardware GL):
    # dma-buf targets, blits and crops, pixel-pack read-backs, the engine end to end.
    add_executable(test_gl_capture tests/test_gl_capture.cpp)
    target_link_libraries(test_gl_capture PRIVATE lx.compositor)
    lumen_apply_module_properties(test_gl_capture)
    lumen_link_platform(test_gl_capture)
    target_include_directories(test_gl_capture PRIVATE tests)
    add_test(NAME test_gl_capture COMMAND test_gl_capture)

    # Screen capture on the Vulkan backend (lavapipe without hardware Vulkan; skips without
    # any): the read-back into shm, a crop.
    add_executable(test_vk_capture tests/test_vk_capture.cpp)
    target_link_libraries(test_vk_capture PRIVATE lx.compositor)
    lumen_apply_module_properties(test_vk_capture)
    lumen_link_platform(test_vk_capture)
    target_include_directories(test_vk_capture PRIVATE tests)
    add_test(NAME test_vk_capture COMMAND test_vk_capture)
    set_tests_properties(test_vk_capture PROPERTIES TIMEOUT 60)

    # xdg-desktop-portal-lumen's pure parts: upright transforms (against lx.gfx's map),
    # layout composition, PNG round trip, file names, user-dirs, chooser lines.
    if(LUMEN_PORTAL_BUILT)
        add_executable(test_portal_compose tests/test_portal_compose.cpp)
        target_link_libraries(test_portal_compose PRIVATE lx.portal lx.gfx)
        lumen_apply_module_properties(test_portal_compose)
        lumen_link_platform(test_portal_compose)
        target_include_directories(test_portal_compose PRIVATE tests)
        add_test(NAME test_portal_compose COMMAND test_portal_compose)
    endif()

    add_executable(test_content_update tests/test_content_update.cpp)
    target_link_libraries(test_content_update PRIVATE lx.compositor)
    lumen_apply_module_properties(test_content_update)
    lumen_link_platform(test_content_update)
    target_include_directories(test_content_update PRIVATE tests)
    add_test(NAME test_content_update COMMAND test_content_update)

    # Idle tracking and inhibitors (compositor) with lumen-shell's idle policy (shell).
    add_executable(test_idle_notify tests/test_idle_notify.cpp)
    target_link_libraries(test_idle_notify PRIVATE lx.compositor lx.shell)
    lumen_apply_module_properties(test_idle_notify)
    lumen_link_platform(test_idle_notify)
    target_include_directories(test_idle_notify PRIVATE tests)
    add_test(NAME test_idle_notify COMMAND test_idle_notify)

    add_executable(test_keybindings tests/test_keybindings.cpp)
    target_link_libraries(test_keybindings PRIVATE lx.compositor lx.input lx.config)
    lumen_apply_module_properties(test_keybindings)
    lumen_link_platform(test_keybindings)
    target_include_directories(test_keybindings PRIVATE tests)
    add_test(NAME test_keybindings COMMAND test_keybindings)

    add_executable(test_workspaces tests/test_workspaces.cpp)
    target_link_libraries(test_workspaces PRIVATE lx.compositor)
    lumen_apply_module_properties(test_workspaces)
    lumen_link_platform(test_workspaces)
    target_include_directories(test_workspaces PRIVATE tests)
    add_test(NAME test_workspaces COMMAND test_workspaces)

    add_executable(test_toplevel_icon tests/test_toplevel_icon.cpp)
    target_link_libraries(test_toplevel_icon PRIVATE lx.compositor)
    lumen_apply_module_properties(test_toplevel_icon)
    lumen_link_platform(test_toplevel_icon)
    target_include_directories(test_toplevel_icon PRIVATE tests)
    add_test(NAME test_toplevel_icon COMMAND test_toplevel_icon)

    # pointer-constraints' policy: effective regions, confinement, activation (Part C).
    add_executable(test_pointer_constraints tests/test_pointer_constraints.cpp)
    target_link_libraries(test_pointer_constraints PRIVATE lx.compositor)
    lumen_apply_module_properties(test_pointer_constraints)
    lumen_link_platform(test_pointer_constraints)
    target_include_directories(test_pointer_constraints PRIVATE tests)
    add_test(NAME test_pointer_constraints COMMAND test_pointer_constraints)

    add_executable(test_input_serials tests/test_input_serials.cpp)
    target_link_libraries(test_input_serials PRIVATE lx.compositor)
    lumen_apply_module_properties(test_input_serials)
    lumen_link_platform(test_input_serials)
    target_include_directories(test_input_serials PRIVATE tests)
    add_test(NAME test_input_serials COMMAND test_input_serials)

    add_executable(test_dnd_grab tests/test_dnd_grab.cpp)
    target_link_libraries(test_dnd_grab PRIVATE lx.compositor)
    lumen_apply_module_properties(test_dnd_grab)
    lumen_link_platform(test_dnd_grab)
    target_include_directories(test_dnd_grab PRIVATE tests)
    add_test(NAME test_dnd_grab COMMAND test_dnd_grab)

    add_executable(test_output_membership tests/test_output_membership.cpp)
    target_link_libraries(test_output_membership PRIVATE lx.compositor)
    lumen_apply_module_properties(test_output_membership)
    lumen_link_platform(test_output_membership)
    target_include_directories(test_output_membership PRIVATE tests)
    add_test(NAME test_output_membership COMMAND test_output_membership)

    add_executable(test_output_layout tests/test_output_layout.cpp)
    target_link_libraries(test_output_layout PRIVATE lx.compositor)
    lumen_apply_module_properties(test_output_layout)
    lumen_link_platform(test_output_layout)
    target_include_directories(test_output_layout PRIVATE tests)
    add_test(NAME test_output_layout COMMAND test_output_layout)

    add_executable(test_liveness tests/test_liveness.cpp)
    target_link_libraries(test_liveness PRIVATE lx.compositor)
    lumen_apply_module_properties(test_liveness)
    lumen_link_platform(test_liveness)
    target_include_directories(test_liveness PRIVATE tests)
    add_test(NAME test_liveness COMMAND test_liveness)

    add_executable(test_activation_policy tests/test_activation_policy.cpp)
    target_link_libraries(test_activation_policy PRIVATE lx.compositor)
    lumen_apply_module_properties(test_activation_policy)
    lumen_link_platform(test_activation_policy)
    target_include_directories(test_activation_policy PRIVATE tests)
    add_test(NAME test_activation_policy COMMAND test_activation_policy)

    add_executable(test_decoration_geometry tests/test_decoration_geometry.cpp)
    target_link_libraries(test_decoration_geometry PRIVATE lx.compositor)
    lumen_apply_module_properties(test_decoration_geometry)
    lumen_link_platform(test_decoration_geometry)
    target_include_directories(test_decoration_geometry PRIVATE tests)
    add_test(NAME test_decoration_geometry COMMAND test_decoration_geometry)

    # ADR-010 §4.11: the compositor's backends draw the UI primitive stream (CPU, GL).
    add_executable(test_ui_composite tests/test_ui_composite.cpp)
    target_link_libraries(test_ui_composite PRIVATE lx.compositor)
    lumen_apply_module_properties(test_ui_composite)
    lumen_link_platform(test_ui_composite)
    target_include_directories(test_ui_composite PRIVATE tests)
    add_test(NAME test_ui_composite COMMAND test_ui_composite)

    # ADR-010 §4.11, Phase 2: title bars as atlas glyphs on the CPU, GL and Vulkan compositors
    # (references in tests/golden/compositor), and the Vulkan cases under validation.
    if(LIBPNG_FOUND)
        add_executable(test_deco_titles tests/test_deco_titles.cpp)
        target_link_libraries(test_deco_titles PRIVATE lx.compositor ${LIBPNG_LINK_LIBRARIES})
        lumen_apply_module_properties(test_deco_titles)
        lumen_link_platform(test_deco_titles)
        target_include_directories(test_deco_titles PRIVATE tests ${LIBPNG_INCLUDE_DIRS})
        target_compile_definitions(test_deco_titles PRIVATE
            LUMEN_DECO_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/compositor")
        add_test(NAME test_deco_titles COMMAND test_deco_titles)
        if(LUMEN_VK_VALIDATION_LAYER)
            add_test(NAME test_deco_titles_vk_validation COMMAND test_deco_titles --filter=vk_)
        endif()
    endif()

    add_executable(test_layer_arrange tests/test_layer_arrange.cpp)
    target_link_libraries(test_layer_arrange PRIVATE lx.compositor)
    lumen_apply_module_properties(test_layer_arrange)
    lumen_link_platform(test_layer_arrange)
    target_include_directories(test_layer_arrange PRIVATE tests)
    add_test(NAME test_layer_arrange COMMAND test_layer_arrange)

    add_executable(test_surface_tree tests/test_surface_tree.cpp)
    target_link_libraries(test_surface_tree PRIVATE lx.compositor)
    lumen_apply_module_properties(test_surface_tree)
    lumen_link_platform(test_surface_tree)
    target_include_directories(test_surface_tree PRIVATE tests)
    add_test(NAME test_surface_tree COMMAND test_surface_tree)

    add_executable(test_xcursor tests/test_xcursor.cpp)
    target_link_libraries(test_xcursor PRIVATE lx.compositor)
    lumen_apply_module_properties(test_xcursor)
    lumen_link_platform(test_xcursor)
    target_include_directories(test_xcursor PRIVATE tests)
    add_test(NAME test_xcursor COMMAND test_xcursor)

    add_executable(test_runtime_memory tests/test_runtime_memory.cpp)
    target_link_libraries(test_runtime_memory PRIVATE lx.runtime)
    lumen_apply_module_properties(test_runtime_memory)
    target_include_directories(test_runtime_memory PRIVATE tests)
    add_test(NAME test_runtime_memory COMMAND test_runtime_memory)

    add_executable(test_runtime_psi tests/test_runtime_psi.cpp)
    target_link_libraries(test_runtime_psi PRIVATE lx.runtime)
    lumen_apply_module_properties(test_runtime_psi)
    target_include_directories(test_runtime_psi PRIVATE tests)
    add_test(NAME test_runtime_psi COMMAND test_runtime_psi)

    add_executable(test_input_keymap tests/test_input_keymap.cpp)
    target_link_libraries(test_input_keymap PRIVATE lx.input)
    lumen_apply_module_properties(test_input_keymap)
    lumen_link_platform(test_input_keymap)
    target_include_directories(test_input_keymap PRIVATE tests)
    add_test(NAME test_input_keymap COMMAND test_input_keymap)

    add_executable(test_repaint_schedule tests/test_repaint_schedule.cpp)
    target_link_libraries(test_repaint_schedule PRIVATE lx.scheduler)
    lumen_apply_module_properties(test_repaint_schedule)
    lumen_link_platform(test_repaint_schedule)
    target_include_directories(test_repaint_schedule PRIVATE tests)
    add_test(NAME test_repaint_schedule COMMAND test_repaint_schedule)

    add_executable(test_frame_clocks tests/test_frame_clocks.cpp)
    target_link_libraries(test_frame_clocks PRIVATE lx.scheduler)
    lumen_apply_module_properties(test_frame_clocks)
    lumen_link_platform(test_frame_clocks)
    target_include_directories(test_frame_clocks PRIVATE tests)
    add_test(NAME test_frame_clocks COMMAND test_frame_clocks)

    add_executable(test_runtime_task tests/test_runtime_task.cpp)
    target_link_libraries(test_runtime_task PRIVATE lx.runtime)
    lumen_apply_module_properties(test_runtime_task)
    target_include_directories(test_runtime_task PRIVATE tests)
    add_test(NAME test_runtime_task COMMAND test_runtime_task)
    set_tests_properties(test_runtime_task PROPERTIES TIMEOUT 60)

    add_executable(test_import_cache tests/test_import_cache.cpp)
    target_link_libraries(test_import_cache PRIVATE lx.gfx)
    lumen_apply_module_properties(test_import_cache)
    target_include_directories(test_import_cache PRIVATE tests)
    add_test(NAME test_import_cache COMMAND test_import_cache)

    add_executable(test_fence_deadline tests/test_fence_deadline.cpp)
    target_link_libraries(test_fence_deadline PRIVATE lx.gfx)
    lumen_apply_module_properties(test_fence_deadline)
    target_include_directories(test_fence_deadline PRIVATE tests)
    add_test(NAME test_fence_deadline COMMAND test_fence_deadline)

    add_executable(test_device_select tests/test_device_select.cpp)
    target_link_libraries(test_device_select PRIVATE lx.gfx)
    lumen_apply_module_properties(test_device_select)
    target_include_directories(test_device_select PRIVATE tests)
    add_test(NAME test_device_select COMMAND test_device_select)

    add_executable(test_cpu_composite tests/test_cpu_composite.cpp)
    target_link_libraries(test_cpu_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(test_cpu_composite)
    target_include_directories(test_cpu_composite PRIVATE tests)
    add_test(NAME test_cpu_composite COMMAND test_cpu_composite)

    add_executable(test_gl_composite tests/test_gl_composite.cpp)
    target_link_libraries(test_gl_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(test_gl_composite)
    # GLES, to read the scanout framebuffer raw, independently of read_back.
    lumen_link_platform(test_gl_composite)
    target_include_directories(test_gl_composite PRIVATE tests)
    # Skips itself (passing) when there is no render node or GL is a software rasterizer.
    add_test(NAME test_gl_composite COMMAND test_gl_composite)

    add_executable(bench_frame tests/bench_frame_timing.cpp)
    target_link_libraries(bench_frame PRIVATE lx.gfx)
    lumen_apply_module_properties(bench_frame)
    target_include_directories(bench_frame PRIVATE tests)
    # Timing benchmark — always passes; prints avg ms for CI logs.
    add_test(NAME bench_frame COMMAND bench_frame)
    set_tests_properties(bench_frame PROPERTIES LABELS bench)

    add_executable(bench_composite tests/bench_composite.cpp)
    target_link_libraries(bench_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(bench_composite)
    target_include_directories(bench_composite PRIVATE tests)
    add_test(NAME bench_composite COMMAND bench_composite)
    set_tests_properties(bench_composite PROPERTIES LABELS bench)

    # ADR-010 §4.11: render-thread cost of a frame with and without decorations (CPU, GL).
    add_executable(bench_ui_composite tests/bench_ui_composite.cpp)
    target_link_libraries(bench_ui_composite PRIVATE lx.compositor)
    lumen_apply_module_properties(bench_ui_composite)
    lumen_link_platform(bench_ui_composite)
    target_include_directories(bench_ui_composite PRIVATE tests)
    add_test(NAME bench_ui_composite COMMAND bench_ui_composite --frames 60 --warmup 10)
    set_tests_properties(bench_ui_composite PROPERTIES LABELS bench)

    # ADR-010 §8: the UI engine's scenarios S1–S3 through lx.ui, a table and the JSON of
    # §8.5. scripts/ui-bench.sh drives it and keeps the baseline; here it only has to run.
    # Benches carry the `bench` label: the test presets exclude it, `ctest --preset bench` runs
    # them (a plain `-L bench` on top of another preset still meets its exclude).
    add_executable(lumen-ui-bench tests/bench_ui.cpp)
    target_link_libraries(lumen-ui-bench PRIVATE lx.ui lx.scene lx.gfx lx.runtime lx.trace)
    lumen_apply_module_properties(lumen-ui-bench)
    lumen_link_platform(lumen-ui-bench)
    target_include_directories(lumen-ui-bench PRIVATE tests)
    add_test(NAME bench_ui COMMAND lumen-ui-bench --scenario all --frames 120 --warmup 30)
    set_tests_properties(bench_ui PROPERTIES LABELS bench TIMEOUT 300)

    add_executable(test_headless_golden tests/test_headless_golden.cpp)
    target_link_libraries(test_headless_golden PRIVATE lx.gfx)
    lumen_apply_module_properties(test_headless_golden)
    target_include_directories(test_headless_golden PRIVATE tests)
    add_test(NAME test_headless_golden COMMAND test_headless_golden)

    add_executable(test_text_shaping tests/test_text_shaping.cpp)
    target_link_libraries(test_text_shaping PRIVATE lx.text)
    lumen_apply_module_properties(test_text_shaping)
    lumen_link_platform(test_text_shaping)
    target_include_directories(test_text_shaping PRIVATE tests)
    add_test(NAME test_text_shaping COMMAND test_text_shaping)

    add_executable(test_text_raster tests/test_text_raster.cpp)
    target_link_libraries(test_text_raster PRIVATE lx.text)
    lumen_apply_module_properties(test_text_raster)
    lumen_link_platform(test_text_raster)
    target_include_directories(test_text_raster PRIVATE tests)
    target_compile_definitions(test_text_raster PRIVATE
        LUMEN_TEST_FONT_DIR="${CMAKE_SOURCE_DIR}/tests/fonts")
    add_test(NAME test_text_raster COMMAND test_text_raster)

    add_executable(test_font_matches tests/test_font_matches.cpp)
    target_link_libraries(test_font_matches PRIVATE lx.text)
    lumen_apply_module_properties(test_font_matches)
    lumen_link_platform(test_font_matches)
    target_include_directories(test_font_matches PRIVATE tests)
    target_compile_definitions(test_font_matches PRIVATE
        LUMEN_ASSET_FONT_DIR="${CMAKE_SOURCE_DIR}/assets/fonts")
    add_test(NAME test_font_matches COMMAND test_font_matches)

    add_executable(test_text_lines tests/test_text_lines.cpp)
    target_link_libraries(test_text_lines PRIVATE lx.text)
    lumen_apply_module_properties(test_text_lines)
    lumen_link_platform(test_text_lines)
    target_include_directories(test_text_lines PRIVATE tests)
    add_test(NAME test_text_lines COMMAND test_text_lines)

    # lx.ui:text_node shaping large text on the worker affinity (ADR-010 §4.6).
    add_executable(test_text_node tests/test_text_node.cpp)
    target_link_libraries(test_text_node PRIVATE lx.ui lx.scene lx.gfx lx.runtime)
    lumen_apply_module_properties(test_text_node)
    lumen_link_platform(test_text_node)
    target_include_directories(test_text_node PRIVATE tests)
    add_test(NAME test_text_node COMMAND test_text_node)

    add_executable(test_protocol_bind tests/test_protocol_bind.cpp)
    target_link_libraries(test_protocol_bind PRIVATE lx.compositor)
    lumen_apply_module_properties(test_protocol_bind)
    lumen_link_platform(test_protocol_bind)
    target_include_directories(test_protocol_bind PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_protocol_bind COMMAND test_protocol_bind)
    set_tests_properties(test_protocol_bind PROPERTIES TIMEOUT 30)

    add_executable(test_task_model tests/test_task_model.cpp)
    target_link_libraries(test_task_model PRIVATE lx.shell)
    lumen_apply_module_properties(test_task_model)
    lumen_link_platform(test_task_model)
    target_include_directories(test_task_model PRIVATE tests)
    add_test(NAME test_task_model COMMAND test_task_model)

    # lumen-shell's launcher: desktop entries, the catalog, launching, the launcher layout.
    foreach(shell_test test_desktop_entry test_app_catalog test_app_launch test_launcher_layout
                       test_background test_switcher test_notifications test_tray test_icon_theme)
        add_executable(${shell_test} tests/${shell_test}.cpp)
        target_link_libraries(${shell_test} PRIVATE lx.shell)
        lumen_apply_module_properties(${shell_test})
        lumen_link_platform(${shell_test})
        target_include_directories(${shell_test} PRIVATE tests)
        add_test(NAME ${shell_test} COMMAND ${shell_test})
    endforeach()
    set_tests_properties(test_app_launch PROPERTIES TIMEOUT 30)

    # Compositor and lx.shell's bridge in one process: the shell side of zlm_policy_bridge_v1
    # against the real server side, with the privilege check configured to accept this binary.
    add_executable(test_shell_bridge tests/test_shell_bridge.cpp)
    target_link_libraries(test_shell_bridge PRIVATE lx.compositor lx.shell)
    lumen_apply_module_properties(test_shell_bridge)
    lumen_link_platform(test_shell_bridge)
    target_include_directories(test_shell_bridge PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_shell_bridge COMMAND test_shell_bridge)
    set_tests_properties(test_shell_bridge PROPERTIES TIMEOUT 30)

    add_executable(test_protocol_conformance tests/test_protocol_conformance.cpp)
    target_link_libraries(test_protocol_conformance PRIVATE lx.compositor)
    lumen_apply_module_properties(test_protocol_conformance)
    lumen_link_platform(test_protocol_conformance)
    target_include_directories(test_protocol_conformance PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_protocol_conformance COMMAND test_protocol_conformance)
    # design-app-compat-capture Part B: dialogs, icons, bell, xdg-foreign, shortcuts
    # inhibition, data control, security context — in-process, as above.
    add_executable(test_ext_workspace tests/test_ext_workspace.cpp)
    target_link_libraries(test_ext_workspace PRIVATE lx.compositor)
    lumen_apply_module_properties(test_ext_workspace)
    lumen_link_platform(test_ext_workspace)
    target_include_directories(test_ext_workspace PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_ext_workspace COMMAND test_ext_workspace)
    set_tests_properties(test_ext_workspace PROPERTIES TIMEOUT 60)

    add_executable(test_window_protocols tests/test_window_protocols.cpp)
    target_link_libraries(test_window_protocols PRIVATE lx.compositor)
    lumen_apply_module_properties(test_window_protocols)
    lumen_link_platform(test_window_protocols)
    target_include_directories(test_window_protocols PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_window_protocols COMMAND test_window_protocols)
    set_tests_properties(test_window_protocols PROPERTIES TIMEOUT 60)

    # Part C (design-app-compat-capture): pointer constraints, relative pointer, gestures —
    # in-process against a real compositor with injected relative motion and gestures.
    add_executable(test_pointer_protocols tests/test_pointer_protocols.cpp)
    target_link_libraries(test_pointer_protocols PRIVATE lx.compositor)
    lumen_apply_module_properties(test_pointer_protocols)
    lumen_link_platform(test_pointer_protocols)
    target_include_directories(test_pointer_protocols PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_pointer_protocols COMMAND test_pointer_protocols)
    set_tests_properties(test_pointer_protocols PROPERTIES TIMEOUT 60)

    # Part D (design-app-compat-capture): screen capture — the toplevel list, output and
    # window capture, every error, the lock, wlr-screencopy — in-process on a virtual output
    # composited on the CPU (frames go only into the capture clients' buffers).
    add_executable(test_capture_protocols tests/test_capture_protocols.cpp)
    target_link_libraries(test_capture_protocols PRIVATE lx.compositor)
    lumen_apply_module_properties(test_capture_protocols)
    lumen_link_platform(test_capture_protocols)
    target_include_directories(test_capture_protocols PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_capture_protocols COMMAND test_capture_protocols)
    set_tests_properties(test_capture_protocols PROPERTIES TIMEOUT 60)
    # ~25 s of round trips and settle waits on a 2-core VM, with jitter: 30 s was marginal.
    set_tests_properties(test_protocol_conformance PROPERTIES TIMEOUT 60)

    # Client contracts on the wire: wl_output and xdg-output values, wl_surface.enter/leave
    # (map, unmap, a late wl_output bind, subsurfaces, popups), against the headless output.
    add_executable(test_output_contract tests/test_output_contract.cpp)
    target_link_libraries(test_output_contract PRIVATE lx.compositor)
    lumen_apply_module_properties(test_output_contract)
    lumen_link_platform(test_output_contract)
    target_include_directories(test_output_contract PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_output_contract COMMAND test_output_contract)
    set_tests_properties(test_output_contract PROPERTIES TIMEOUT 30)

    add_executable(test_multi_output_contract tests/test_multi_output_contract.cpp)
    target_link_libraries(test_multi_output_contract PRIVATE lx.compositor)
    lumen_apply_module_properties(test_multi_output_contract)
    lumen_link_platform(test_multi_output_contract)
    target_include_directories(test_multi_output_contract PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_multi_output_contract COMMAND test_multi_output_contract)
    set_tests_properties(test_multi_output_contract PROPERTIES TIMEOUT 30)

    add_executable(test_hotplug_contract tests/test_hotplug_contract.cpp)
    target_link_libraries(test_hotplug_contract PRIVATE lx.compositor)
    lumen_apply_module_properties(test_hotplug_contract)
    lumen_link_platform(test_hotplug_contract)
    target_include_directories(test_hotplug_contract PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_hotplug_contract COMMAND test_hotplug_contract)
    set_tests_properties(test_hotplug_contract PROPERTIES TIMEOUT 30)

    add_executable(test_output_management_contract tests/test_output_management_contract.cpp)
    target_link_libraries(test_output_management_contract PRIVATE lx.compositor)
    lumen_apply_module_properties(test_output_management_contract)
    lumen_link_platform(test_output_management_contract)
    target_include_directories(test_output_management_contract PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_output_management_contract COMMAND test_output_management_contract)
    set_tests_properties(test_output_management_contract PROPERTIES TIMEOUT 60)

    add_executable(test_kms_device tests/test_kms_device.cpp)
    target_link_libraries(test_kms_device PRIVATE lx.drm)
    lumen_apply_module_properties(test_kms_device)
    lumen_link_platform(test_kms_device)
    target_include_directories(test_kms_device PRIVATE tests)
    add_test(NAME test_kms_device COMMAND test_kms_device)

    add_executable(test_crtc_alloc tests/test_crtc_alloc.cpp)
    target_link_libraries(test_crtc_alloc PRIVATE lx.drm)
    lumen_apply_module_properties(test_crtc_alloc)
    lumen_link_platform(test_crtc_alloc)
    target_include_directories(test_crtc_alloc PRIVATE tests)
    add_test(NAME test_crtc_alloc COMMAND test_crtc_alloc)

    # The VMware host helper for live hotplug tests (scripts/monitor-smoke.sh): sets the
    # virtual monitors' layout as open-vm-tools does. Built where the vmwgfx uapi header is,
    # never installed.
    include(CheckIncludeFile)
    if(LIBDRM_FOUND)
        set(CMAKE_REQUIRED_INCLUDES ${LIBDRM_INCLUDE_DIRS})
        check_include_file("libdrm/vmwgfx_drm.h" LUMEN_HAS_VMWGFX_DRM_H)
        unset(CMAKE_REQUIRED_INCLUDES)
    endif()
    if(LUMEN_HAS_VMWGFX_DRM_H)
        add_executable(lumen-vmw-layout tests/tools/lumen-vmw-layout.c)
        set_target_properties(lumen-vmw-layout PROPERTIES LINKER_LANGUAGE C)
        target_include_directories(lumen-vmw-layout PRIVATE ${LIBDRM_INCLUDE_DIRS})
        target_link_libraries(lumen-vmw-layout PRIVATE ${LIBDRM_LIBRARIES})
    endif()

    add_executable(test_cvt tests/test_cvt.cpp)
    target_link_libraries(test_cvt PRIVATE lx.drm)
    lumen_apply_module_properties(test_cvt)
    lumen_link_platform(test_cvt)
    add_test(NAME test_cvt COMMAND test_cvt)

    add_executable(test_uevent_parse tests/test_uevent_parse.cpp)
    target_link_libraries(test_uevent_parse PRIVATE lx.drm)
    lumen_apply_module_properties(test_uevent_parse)
    lumen_link_platform(test_uevent_parse)
    target_include_directories(test_uevent_parse PRIVATE tests)
    add_test(NAME test_uevent_parse COMMAND test_uevent_parse)

    add_executable(test_card_select tests/test_card_select.cpp)
    target_link_libraries(test_card_select PRIVATE lx.drm)
    lumen_apply_module_properties(test_card_select)
    target_include_directories(test_card_select PRIVATE tests)
    add_test(NAME test_card_select COMMAND test_card_select)

    foreach(t test_output_scan_diff test_output_policy test_window_migration
              test_output_management test_output_config_store)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE lx.compositor)
        lumen_apply_module_properties(${t})
        lumen_link_platform(${t})
        target_include_directories(${t} PRIVATE tests)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(test_dmabuf_composite tests/test_dmabuf_composite.cpp)
    target_link_libraries(test_dmabuf_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(test_dmabuf_composite)
    lumen_link_platform(test_dmabuf_composite)
    target_include_directories(test_dmabuf_composite PRIVATE tests)
    add_test(NAME test_dmabuf_composite COMMAND test_dmabuf_composite)
    set_tests_properties(test_dmabuf_composite PROPERTIES TIMEOUT 60)

    # ADR-010 §4.8, §5: the theme compiler over the bundled Zed themes, the .lxtheme format,
    # styles, styled boxes, the theme watcher and Lucide icons (SKIP without librsvg).
    add_executable(test_ui_theme tests/test_ui_theme.cpp)
    target_link_libraries(test_ui_theme PRIVATE lx.ui lx.scene lx.gfx lx.runtime)
    lumen_apply_module_properties(test_ui_theme)
    lumen_link_platform(test_ui_theme)
    target_include_directories(test_ui_theme PRIVATE tests)
    target_compile_definitions(test_ui_theme PRIVATE
        LUMEN_THEME_SOURCE_JSON="${LUMEN_THEME_SOURCE}"
        LUMEN_TEST_THEME_DIR="${CMAKE_BINARY_DIR}/themes")
    add_dependencies(test_ui_theme lumen-themes)
    add_test(NAME test_ui_theme COMMAND test_ui_theme)

    add_executable(test_ui_reconcile tests/test_ui_reconcile.cpp)
    target_link_libraries(test_ui_reconcile PRIVATE lx.ui)
    lumen_apply_module_properties(test_ui_reconcile)
    lumen_link_platform(test_ui_reconcile)
    target_include_directories(test_ui_reconcile PRIVATE tests)
    add_test(NAME test_ui_reconcile COMMAND test_ui_reconcile)

    # ADR-010 §7 Phase 3: focus over the retained tree, reducers wired into the root, pointer
    # state and motion, and §5.6's basic components against Zed's sizes and tokens.
    add_executable(test_ui_components tests/test_ui_components.cpp)
    target_link_libraries(test_ui_components PRIVATE lx.ui lx.scene lx.gfx lx.text lx.runtime lx.a11y)
    lumen_apply_module_properties(test_ui_components)
    lumen_link_platform(test_ui_components)
    target_include_directories(test_ui_components PRIVATE tests)
    target_compile_definitions(test_ui_components PRIVATE
        LUMEN_TEST_FONT_DIR="${CMAKE_SOURCE_DIR}/tests/fonts")
    add_test(NAME test_ui_components COMMAND test_ui_components)

    # lx.layout:flex against Chrome's layout of Taffy's fixtures, and lx.ui's flex container
    # with the per-node measure cache (ADR-010 §4.7).
    add_executable(test_layout_flex tests/test_layout_flex.cpp)
    target_link_libraries(test_layout_flex PRIVATE lx.ui lx.layout lx.scene lx.text lx.runtime)
    lumen_apply_module_properties(test_layout_flex)
    lumen_link_platform(test_layout_flex)
    target_include_directories(test_layout_flex PRIVATE tests)
    add_test(NAME test_layout_flex COMMAND test_layout_flex)

    add_executable(test_kms_vkms tests/test_kms_vkms.cpp)
    target_link_libraries(test_kms_vkms PRIVATE lx.drm)
    lumen_apply_module_properties(test_kms_vkms)
    lumen_link_platform(test_kms_vkms)
    target_include_directories(test_kms_vkms PRIVATE tests)
    add_test(NAME test_kms_vkms COMMAND test_kms_vkms)

    add_executable(test_kms_vulkan_scanout tests/test_kms_vulkan_scanout.cpp)
    target_link_libraries(test_kms_vulkan_scanout PRIVATE lx.drm lx.gfx)
    lumen_apply_module_properties(test_kms_vulkan_scanout)
    lumen_link_platform(test_kms_vulkan_scanout)
    target_include_directories(test_kms_vulkan_scanout PRIVATE tests)
    add_test(NAME test_kms_vulkan_scanout COMMAND test_kms_vulkan_scanout)

    # A sanitizer report must fail the test, not just print to stderr.
    set(LUMEN_TSAN_OPTS "halt_on_error=1")
    set(LUMEN_ASAN_OPTS "halt_on_error=1:abort_on_error=1")
    set(LUMEN_UBSAN_OPTS "halt_on_error=1:print_stacktrace=1")

    # Both suppression files apply everywhere. They are scoped to third-party shared
    # objects by name, so a test that never loads a graphics driver simply matches nothing
    # — and any test that later grows a compositor gets the right behavior without being
    # remembered here. Only the blunt instrument below is opt-in.
    set(LUMEN_SANITIZER_ENV
        "TSAN_OPTIONS=${LUMEN_TSAN_OPTS}:suppressions=${CMAKE_SOURCE_DIR}/tests/sanitizers/tsan.supp"
        "ASAN_OPTIONS=${LUMEN_ASAN_OPTS}"
        "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/sanitizers/lsan.supp"
        "UBSAN_OPTIONS=${LUMEN_UBSAN_OPTS}")

    # Tests that bring up a Vulkan device load the system ICD, whose teardown is not
    # sanitizer-clean. Both concessions below are scoped to these two tests alone, so the
    # rest of the suite keeps full leak and race detection. Analysis in TEST.md.
    set(LUMEN_SANITIZER_ENV_VULKAN_ICD
        # Races among the driver's own threads, suppressed by object name — see
        # tests/sanitizers/tsan.supp for why it is scoped that way and not by symbol.
        "TSAN_OPTIONS=${LUMEN_TSAN_OPTS}:suppressions=${CMAKE_SOURCE_DIR}/tests/sanitizers/tsan.supp"
        "ASAN_OPTIONS=${LUMEN_ASAN_OPTS}"
        # The ICD is dlclose'd before LSan's exit check, so its frames resolve to
        # "<unknown module>" and no `leak:` pattern can match them. Turning off the exit
        # leak check is the only scoping available here; every other ASan check
        # (use-after-free, overflow, out-of-bounds) stays active for these tests.
        "LSAN_OPTIONS=detect_leaks=0"
        "UBSAN_OPTIONS=${LUMEN_UBSAN_OPTS}")

    if(TARGET test_control)
        set_tests_properties(test_control PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV}")
    endif()
    set_tests_properties(test_health test_session_discovery
        PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV}")
    if(TARGET test_ui_golden)
        # It brings up a Vulkan device (ui_vk): the ICD's threads and dlclose need the same
        # sanitizer settings as the other Vulkan tests.
        set_tests_properties(test_ui_golden PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV_VULKAN_ICD}")
        if(TEST test_ui_golden_vk_validation)
            set_tests_properties(test_ui_golden_vk_validation PROPERTIES ENVIRONMENT
                "${LUMEN_SANITIZER_ENV_VULKAN_ICD};LUMEN_VULKAN_VALIDATION=1;LUMEN_VK_VALIDATION_LAYER=${LUMEN_VK_VALIDATION_LAYER}")
        endif()
    endif()
    set_tests_properties(test_result test_snapshot test_primitives test_atlas test_scene_outputs test_damage_region test_wm_geometry
        test_surface_tree test_positioner test_layer_arrange test_xcursor
        test_data_transfer test_input_serials test_session_pause test_keybindings test_workspaces
        test_viewport_geometry test_presentation_feedback
        test_dnd_grab test_output_membership test_liveness
        test_activation_policy
        test_import_cache bench_frame
        test_runtime_memory test_runtime_psi test_runtime_task test_cpu_composite test_repaint_schedule test_frame_clocks test_input_keymap
        test_headless_golden test_text_shaping test_protocol_bind test_protocol_conformance
        test_output_contract test_multi_output_contract test_hotplug_contract
        test_output_management_contract
        test_task_model test_shell_bridge
        test_desktop_entry test_app_catalog test_app_launch test_launcher_layout test_background
        test_switcher test_notifications test_tray
        test_kms_device test_kms_vkms test_crtc_alloc test_output_layout
        test_uevent_parse test_output_scan_diff test_output_policy test_window_migration
        test_cvt test_output_management test_output_config_store
        test_ui_reconcile test_ui_theme test_layout_flex test_gl_composite bench_composite bench_ui
        PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV}")
    if(TEST test_ui_components)
        set_tests_properties(test_ui_components PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV}")
    endif()
    if(TEST ui_gallery)
        set_tests_properties(ui_gallery PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV}")
    endif()

    set_tests_properties(test_dmabuf_composite test_kms_vulkan_scanout
        PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV_VULKAN_ICD}")
    if(TARGET test_deco_titles)
        set_tests_properties(test_deco_titles PROPERTIES ENVIRONMENT "${LUMEN_SANITIZER_ENV_VULKAN_ICD}")
        if(TEST test_deco_titles_vk_validation)
            set_tests_properties(test_deco_titles_vk_validation PROPERTIES ENVIRONMENT
                "${LUMEN_SANITIZER_ENV_VULKAN_ICD};LUMEN_VULKAN_VALIDATION=1;LUMEN_VK_VALIDATION_LAYER=${LUMEN_VK_VALIDATION_LAYER}")
        endif()
    endif()
endif()

# ── Fuzzers ──────────────────────────────────────────────────────────────────
#
# Opt-in and clang-only. The compositor's socket is its one untrusted input, so the
# request-dispatch path is the boundary worth fuzzing.
if(LUMEN_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "LUMEN_BUILD_FUZZERS requires clang (libFuzzer)")
    endif()

    add_executable(fuzz_wayland_dispatch tests/fuzz_wayland_dispatch.cpp)
    # Seeds that reach past the header into real dispatch — see tests/fuzz/corpus/README.
    file(COPY "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus" DESTINATION "${CMAKE_BINARY_DIR}")
    target_link_libraries(fuzz_wayland_dispatch PRIVATE lx.compositor)
    lumen_apply_module_properties(fuzz_wayland_dispatch)
    lumen_link_platform(fuzz_wayland_dispatch)
    target_include_directories(fuzz_wayland_dispatch PRIVATE tests "${LUMEN_GENERATED_DIR}")
    # address+fuzzer, not fuzzer alone: a memory-safety bug in protocol handling is the
    # thing worth finding here, and without ASan most of them are silent.
    target_compile_options(fuzz_wayland_dispatch PRIVATE -fsanitize=fuzzer,address -g)
    target_link_options(fuzz_wayland_dispatch PRIVATE -fsanitize=fuzzer,address)

    # A short run in CI is a smoke test, not a campaign — it catches a harness that stopped
    # building or immediately crashes, while real fuzzing runs long and out of band.
    add_test(NAME fuzz_wayland_dispatch_smoke
        COMMAND fuzz_wayland_dispatch "${CMAKE_BINARY_DIR}/corpus"
                -max_total_time=10 -rss_limit_mb=2048)
    set_tests_properties(fuzz_wayland_dispatch_smoke PROPERTIES TIMEOUT 120
        ENVIRONMENT "${LUMEN_SANITIZER_ENV_VULKAN_ICD}")
endif()

# No test reads the user's configuration file (~/.config/lumen/config.toml): every test
# starts from the defaults unless it points LUMEN_CONFIG at a file of its own.
get_property(_lumen_tests DIRECTORY PROPERTY TESTS)
if(_lumen_tests)
    set_property(TEST ${_lumen_tests} APPEND PROPERTY ENVIRONMENT "LUMEN_CONFIG=0")
endif()

message(STATUS "Lumen ${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD} — ${CMAKE_CXX_COMPILER_ID}")
