# Platform dependencies (Linux compositor target)

find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND QUIET wayland-server wayland-client)
    # A Wayland client alone (lumen-shell): libwayland-client without the server.
    pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
    pkg_check_modules(WAYLAND_SCANNER QUIET wayland-scanner)
    pkg_check_modules(LIBDRM QUIET libdrm)
    # libdisplay-info parses EDID into the make/model/serial a monitor is identified by
    # (wlroots, Mutter, Weston use it); lx.drm has a base-block fallback without it.
    pkg_check_modules(LIBDISPLAYINFO QUIET libdisplay-info)
    # libinput needs udev to enumerate a seat; xkbcommon turns keycodes into keysyms and
    # tracks modifier state. All three go together — a keyboard without xkb can report that
    # a key moved and nothing about what it means.
    pkg_check_modules(LIBINPUT QUIET libinput libudev)
    pkg_check_modules(XKBCOMMON QUIET xkbcommon)
    # Display hotplug (lx.drm:uevent): a udev netlink monitor for drm change events. The
    # same libudev libinput needs, checked on its own so hotplug does not depend on input.
    pkg_check_modules(LIBUDEV QUIET libudev)
    # EGL + GLES + GBM back the hardware GL present path. All three are required
    # together: GBM allocates the scanout buffer, EGL binds it, GLES draws into it.
    pkg_check_modules(LIBEGL QUIET egl glesv2 gbm)
    pkg_check_modules(HARFBUZZ QUIET harfbuzz freetype2 fontconfig)
    pkg_check_modules(LIBSYSTEMD QUIET libsystemd)
    # lumen-shell's icon theme lookup (task-bar icons): PNG through libpng, SVG through
    # librsvg (which brings cairo). Both optional: without them the panel falls back to the
    # window's own pixel buffers (xdg-toplevel-icon-v1) and then to a letter tile.
    pkg_check_modules(LIBPNG QUIET libpng)
    pkg_check_modules(LIBRSVG QUIET librsvg-2.0)
    # Desktop background images (lx.shell:background), each optional: JPEG through
    # libjpeg-turbo (DCT-scaled decode of large photos), WebP through libwebp.
    pkg_check_modules(LIBTURBOJPEG QUIET libturbojpeg>=3.0)
    pkg_check_modules(LIBWEBP QUIET libwebp)
endif()

if(WAYLAND_SCANNER_FOUND)
    pkg_get_variable(WAYLAND_SCANNER_EXE wayland-scanner wayland_scanner)
endif()
if(NOT WAYLAND_SCANNER_EXE)
    find_program(WAYLAND_SCANNER_EXE NAMES wayland-scanner)
endif()
if(WAYLAND_SCANNER_EXE)
    set(LUMEN_HAS_WAYLAND_SCANNER TRUE)
    message(STATUS "wayland-scanner: ${WAYLAND_SCANNER_EXE}")
else()
    set(LUMEN_HAS_WAYLAND_SCANNER FALSE)
    message(STATUS "wayland-scanner not found — protocol C glue unavailable")
endif()

if(LIBSYSTEMD_FOUND)
    set(LUMEN_HAS_LOGIND TRUE)
    message(STATUS "libsystemd ${LIBSYSTEMD_VERSION} — logind/sd-bus available")
else()
    set(LUMEN_HAS_LOGIND FALSE)
endif()
# The compositor's control socket and lumen-ctl use sd-varlink and sd-json, public API
# since systemd 257. Older libsystemd (Ubuntu 24.04: 255, Debian 12: 252) still gives
# logind, sd-bus and sd-notify; the session then runs without the control socket.
if(LIBSYSTEMD_FOUND AND LIBSYSTEMD_VERSION VERSION_GREATER_EQUAL 257)
    set(LUMEN_HAS_VARLINK TRUE)
else()
    set(LUMEN_HAS_VARLINK FALSE)
    if(LIBSYSTEMD_FOUND)
        message(STATUS "libsystemd ${LIBSYSTEMD_VERSION} < 257 — no sd-varlink: "
                       "control socket and lumen-ctl unavailable")
    endif()
endif()
# The compositor unit's reload protocol: Type=notify-reload (systemd 253+) sends SIGHUP and
# waits for READY=1 itself; before that, Type=notify with an ExecReload= that sends it.
# Both are configured into deploy/systemd/lumen-compositor.service.in.
if(LIBSYSTEMD_FOUND AND LIBSYSTEMD_VERSION VERSION_LESS 253)
    set(LUMEN_UNIT_SERVICE_TYPE "notify")
    set(LUMEN_UNIT_EXEC_RELOAD "ExecReload=/bin/kill -HUP $MAINPID")
else()
    set(LUMEN_UNIT_SERVICE_TYPE "notify-reload")
    set(LUMEN_UNIT_EXEC_RELOAD "# (no ExecReload=: notify-reload sends SIGHUP itself)")
endif()

find_program(LUMEN_GLSLC NAMES glslc)
if(LUMEN_GLSLC)
    set(LUMEN_HAS_GLSLC TRUE)
    message(STATUS "glslc: ${LUMEN_GLSLC}")
else()
    set(LUMEN_HAS_GLSLC FALSE)
    message(STATUS "glslc not found — Vulkan composite pass unavailable")
endif()

# Compile GLSL sources to SPIR-V as C initializer lists so shaders are embedded in the
# binary; a compositor must not depend on shader files at runtime.
function(lumen_compile_shaders target)
    if(NOT LUMEN_HAS_GLSLC)
        return()
    endif()
    set(out_dir "${CMAKE_BINARY_DIR}/generated/shaders")
    file(MAKE_DIRECTORY "${out_dir}")
    set(outputs "")
    foreach(src IN LISTS ARGN)
        get_filename_component(name "${src}" NAME)
        set(out "${out_dir}/${name}.spv.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${LUMEN_GLSLC} --target-env=vulkan1.1 -O -mfmt=c -o "${out}" "${src}"
            DEPENDS "${src}"
            COMMENT "glslc ${name}"
            VERBATIM
        )
        list(APPEND outputs "${out}")
    endforeach()
    add_custom_target(${target}-shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}-shaders)
    target_compile_definitions(${target} PRIVATE LUMEN_HAS_SHADERS=1)
endfunction()

# Link platform libs and define LUMEN_HAS_* on a target (executables + modules).
function(lumen_link_platform target)
    if(Vulkan_FOUND)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_VULKAN=1)
    else()
        message(STATUS "${target}: Vulkan not found — building stubs")
    endif()

    if(WAYLAND_FOUND)
        target_include_directories(${target} PRIVATE ${WAYLAND_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${WAYLAND_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_WAYLAND=1)
    endif()

    if(LIBDRM_FOUND)
        target_include_directories(${target} PRIVATE ${LIBDRM_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBDRM_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_DRM=1)
    endif()

    if(LIBDISPLAYINFO_FOUND)
        target_include_directories(${target} PRIVATE ${LIBDISPLAYINFO_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBDISPLAYINFO_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_DISPLAY_INFO=1)
    endif()

    if(LIBUDEV_FOUND)
        target_include_directories(${target} PRIVATE ${LIBUDEV_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBUDEV_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_UDEV=1)
    endif()

    if(LIBINPUT_FOUND AND XKBCOMMON_FOUND)
        target_include_directories(${target} PRIVATE ${LIBINPUT_INCLUDE_DIRS}
                                                     ${XKBCOMMON_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBINPUT_LIBRARIES} ${XKBCOMMON_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_INPUT=1)
        # Three-finger drag (libinput 1.28+), a touchpad setting of the configuration file.
        if(LIBINPUT_libinput_VERSION VERSION_GREATER_EQUAL 1.28)
            target_compile_definitions(${target} PRIVATE LUMEN_HAS_LIBINPUT_3FG_DRAG=1)
        endif()
    elseif(LIBINPUT_FOUND)
        message(STATUS "${target}: xkbcommon missing — input stubbed")
    endif()

    if(LIBEGL_FOUND)
        target_include_directories(${target} PRIVATE ${LIBEGL_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBEGL_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_EGL=1)
    else()
        message(STATUS "${target}: EGL/GLES/GBM not found — GL present path stubbed")
    endif()

    if(HARFBUZZ_FOUND)
        target_include_directories(${target} PRIVATE ${HARFBUZZ_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${HARFBUZZ_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_TEXT=1)
    endif()

    if(LIBSYSTEMD_FOUND)
        target_include_directories(${target} PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBSYSTEMD_LIBRARIES})
        target_compile_definitions(${target} PRIVATE LUMEN_HAS_LOGIND=1)
        if(LUMEN_HAS_VARLINK)
            target_compile_definitions(${target} PRIVATE LUMEN_HAS_VARLINK=1)
        endif()
    endif()
endfunction()

# Only the named platform parts, for a target that is not the compositor (lumen-shell and the
# modules it is built from): `lumen_link_platform` brings Vulkan, DRM, libinput, udev and
# EGL/GBM as well, which a client loads and relocates at every start for nothing.
#   wayland_client  libwayland-client          LUMEN_HAS_WAYLAND
#   xkb             xkbcommon                  LUMEN_HAS_XKBCOMMON
#   text            HarfBuzz, FreeType, fontconfig  LUMEN_HAS_TEXT
#   systemd         libsystemd (sd-bus)        LUMEN_HAS_LOGIND
function(lumen_link_platform_parts target)
    foreach(part IN LISTS ARGN)
        if(part STREQUAL "wayland_client")
            if(WAYLAND_CLIENT_FOUND)
                target_include_directories(${target} PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
                target_link_libraries(${target} PRIVATE ${WAYLAND_CLIENT_LIBRARIES})
                target_compile_definitions(${target} PRIVATE LUMEN_HAS_WAYLAND=1)
            endif()
        elseif(part STREQUAL "xkb")
            if(XKBCOMMON_FOUND)
                target_include_directories(${target} PRIVATE ${XKBCOMMON_INCLUDE_DIRS})
                target_link_libraries(${target} PRIVATE ${XKBCOMMON_LIBRARIES})
                target_compile_definitions(${target} PRIVATE LUMEN_HAS_XKBCOMMON=1)
            endif()
        elseif(part STREQUAL "text")
            if(HARFBUZZ_FOUND)
                target_include_directories(${target} PRIVATE ${HARFBUZZ_INCLUDE_DIRS})
                target_link_libraries(${target} PRIVATE ${HARFBUZZ_LIBRARIES})
                target_compile_definitions(${target} PRIVATE LUMEN_HAS_TEXT=1)
            endif()
        elseif(part STREQUAL "systemd")
            if(LIBSYSTEMD_FOUND)
                target_include_directories(${target} PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})
                target_link_libraries(${target} PRIVATE ${LIBSYSTEMD_LIBRARIES})
                target_compile_definitions(${target} PRIVATE LUMEN_HAS_LOGIND=1)
            endif()
        else()
            message(FATAL_ERROR "lumen_link_platform_parts: unknown part '${part}'")
        endif()
    endforeach()
endfunction()

# Link platform deps into module libraries that call platform APIs directly.
# Prefer this over linking platform only to executables.
function(lumen_link_platform_modules)
    foreach(target IN LISTS ARGN)
        if(TARGET ${target})
            lumen_link_platform(${target})
        else()
            message(WARNING "lumen_link_platform_modules: unknown target '${target}'")
        endif()
    endforeach()
endfunction()
