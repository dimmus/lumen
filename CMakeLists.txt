cmake_minimum_required(VERSION 3.28)
project(lumen VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(LumenOptions)
include(LumenCXXModules)
include(LumenPlatform)

# ── Codegen tool (must exist before protocol generation) ────────────────────
add_executable(wl-scanner-cpp
    tools/wl-scanner-cpp/main.cpp
)
target_include_directories(wl-scanner-cpp PRIVATE tools/wl-scanner-cpp)
target_compile_features(wl-scanner-cpp PRIVATE cxx_std_20)

add_executable(lumen-theme
    tools/lumen-theme/main.cpp
)
target_compile_features(lumen-theme PRIVATE cxx_std_20)

include(LumenProtocols)

# ── Foundation ──────────────────────────────────────────────────────────────
lumen_add_module_library(lx.foundation modules/lx.foundation)
lumen_add_module_library(lx.sync modules/lx.sync DEPENDS lx.foundation)
lumen_add_module_library(lx.trace modules/lx.trace DEPENDS lx.foundation)
lumen_add_module_library(lx.runtime modules/lx.runtime DEPENDS lx.foundation lx.trace lx.sync)

# ── Platform ─────────────────────────────────────────────────────────────────
lumen_add_module_library(lx.input modules/lx.input DEPENDS lx.runtime)
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
    DEPENDS lx.foundation lx.runtime lx.drm)
lumen_compile_shaders(lx.gfx
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/composite.vert"
    "${CMAKE_SOURCE_DIR}/modules/lx.gfx/shaders/composite.frag")
lumen_add_module_library(lx.text modules/lx.text DEPENDS lx.gfx)
lumen_link_platform_modules(lx.text)
lumen_add_module_library(lx.layout modules/lx.layout DEPENDS lx.foundation lx.runtime)
lumen_add_module_library(lx.a11y modules/lx.a11y DEPENDS lx.foundation)
lumen_add_module_library(lx.scene modules/lx.scene
    DEPENDS lx.gfx lx.layout lx.scheduler lx.runtime lx.sync)
lumen_add_module_library(lx.ui modules/lx.ui DEPENDS lx.scene lx.text lx.runtime lx.gfx lx.wayland.client lx.layout lx.a11y)
lumen_add_module_library(lx.ui.builder modules/lx.ui.builder DEPENDS lx.ui)

# ── Desktop ──────────────────────────────────────────────────────────────────
lumen_add_module_library(lx.shell.policy modules/lx.shell.policy
    DEPENDS lx.wayland.client lx.wayland.protocols)
lumen_add_module_library(lx.compositor modules/lx.compositor
    DEPENDS lx.wayland.server lx.gfx lx.scene lx.scheduler lx.input lx.drm lx.runtime lx.sync lx.trace lx.session)
lumen_add_module_library(lx.shell modules/lx.shell
    DEPENDS lx.shell.policy lx.ui lx.wayland.client)

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
lumen_link_platform(lumen-shell)

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
    else()
        message(STATUS "xdg-shell.xml not found — skipping lumen-clouds")
    endif()
else()
    message(STATUS "wayland-client/scanner unavailable — skipping lumen-clouds")
endif()

# ── Optional Flatpak portal adapter ─────────────────────────────────────────
if(LUMEN_BUILD_FLATPAK_PORTAL)
    lumen_add_module_library(lx.portal modules/lx.portal DEPENDS lx.foundation lx.shell.policy)
endif()

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS lumen-compositor lumen-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY protocols/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/protocols
    FILES_MATCHING PATTERN "*.xml" PATTERN "*.toml")
install(DIRECTORY deploy/systemd/ DESTINATION ${CMAKE_INSTALL_DATADIR}/lumen/systemd)

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

    add_executable(test_snapshot tests/test_snapshot.cpp)
    target_link_libraries(test_snapshot PRIVATE lx.scene lx.runtime)
    lumen_apply_module_properties(test_snapshot)
    target_include_directories(test_snapshot PRIVATE tests)
    add_test(NAME test_snapshot COMMAND test_snapshot)

    add_executable(test_import_cache tests/test_import_cache.cpp)
    target_link_libraries(test_import_cache PRIVATE lx.gfx)
    lumen_apply_module_properties(test_import_cache)
    target_include_directories(test_import_cache PRIVATE tests)
    add_test(NAME test_import_cache COMMAND test_import_cache)

    add_executable(test_cpu_composite tests/test_cpu_composite.cpp)
    target_link_libraries(test_cpu_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(test_cpu_composite)
    target_include_directories(test_cpu_composite PRIVATE tests)
    add_test(NAME test_cpu_composite COMMAND test_cpu_composite)

    add_executable(test_gl_composite tests/test_gl_composite.cpp)
    target_link_libraries(test_gl_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(test_gl_composite)
    target_include_directories(test_gl_composite PRIVATE tests)
    # Skips itself (passing) when there is no render node or GL is a software rasterizer.
    add_test(NAME test_gl_composite COMMAND test_gl_composite)

    add_executable(bench_frame tests/bench_frame_timing.cpp)
    target_link_libraries(bench_frame PRIVATE lx.gfx)
    lumen_apply_module_properties(bench_frame)
    target_include_directories(bench_frame PRIVATE tests)
    # Timing benchmark — always passes; prints avg ms for CI logs.
    add_test(NAME bench_frame COMMAND bench_frame)

    add_executable(bench_composite tests/bench_composite.cpp)
    target_link_libraries(bench_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(bench_composite)
    target_include_directories(bench_composite PRIVATE tests)
    add_test(NAME bench_composite COMMAND bench_composite)

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

    add_executable(test_protocol_bind tests/test_protocol_bind.cpp)
    target_link_libraries(test_protocol_bind PRIVATE lx.compositor)
    lumen_apply_module_properties(test_protocol_bind)
    lumen_link_platform(test_protocol_bind)
    target_include_directories(test_protocol_bind PRIVATE tests "${LUMEN_GENERATED_DIR}")
    add_test(NAME test_protocol_bind COMMAND test_protocol_bind)
    set_tests_properties(test_protocol_bind PROPERTIES TIMEOUT 30)

    add_executable(test_kms_device tests/test_kms_device.cpp)
    target_link_libraries(test_kms_device PRIVATE lx.drm)
    lumen_apply_module_properties(test_kms_device)
    lumen_link_platform(test_kms_device)
    target_include_directories(test_kms_device PRIVATE tests)
    add_test(NAME test_kms_device COMMAND test_kms_device)

    add_executable(test_dmabuf_composite tests/test_dmabuf_composite.cpp)
    target_link_libraries(test_dmabuf_composite PRIVATE lx.gfx)
    lumen_apply_module_properties(test_dmabuf_composite)
    lumen_link_platform(test_dmabuf_composite)
    target_include_directories(test_dmabuf_composite PRIVATE tests)
    add_test(NAME test_dmabuf_composite COMMAND test_dmabuf_composite)
    set_tests_properties(test_dmabuf_composite PROPERTIES TIMEOUT 60)

    add_executable(test_ui_reconcile tests/test_ui_reconcile.cpp)
    target_link_libraries(test_ui_reconcile PRIVATE lx.ui)
    lumen_apply_module_properties(test_ui_reconcile)
    lumen_link_platform(test_ui_reconcile)
    target_include_directories(test_ui_reconcile PRIVATE tests)
    add_test(NAME test_ui_reconcile COMMAND test_ui_reconcile)

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
    set_tests_properties(test_result test_snapshot test_import_cache bench_frame
        test_headless_golden test_text_shaping test_protocol_bind test_kms_device test_kms_vkms
        test_kms_vulkan_scanout test_dmabuf_composite test_ui_reconcile
        PROPERTIES ENVIRONMENT
            "TSAN_OPTIONS=halt_on_error=1;ASAN_OPTIONS=halt_on_error=1:abort_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()

message(STATUS "Lumen ${PROJECT_VERSION} — C++${CMAKE_CXX_STANDARD} — ${CMAKE_CXX_COMPILER_ID}")
