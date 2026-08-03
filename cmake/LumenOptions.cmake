option(LUMEN_BUILD_FLATPAK_PORTAL "Build optional Flatpak portal adapter" OFF)
option(LUMEN_ENABLE_TILING_PLUGIN "Build tiling WM plugin (dlopen)" ON)
option(LUMEN_VULKAN_VALIDATION "Enable Vulkan validation layers" OFF)
option(LUMEN_PROTOCOLS_P1 "Generate and enable P1 Wayland protocols" ON)
option(LUMEN_PROTOCOLS_P2 "Generate and enable P2 Wayland protocols" OFF)
option(LUMEN_BUILD_TESTS "Build unit / stress tests" ON)
option(LUMEN_BUILD_XWAYLAND "Build optional rootless XWayland bridge module" OFF)
option(LUMEN_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)

# Coverage instrumentation has to reach the code under test, not just the harness. Without
# this every module is an opaque blob to libFuzzer and it degenerates into random input
# generation — the giveaway is a coverage count that never moves off single digits.
if(LUMEN_BUILD_FUZZERS)
    # C sources (the wayland-scanner glue) build with the system C compiler, which may not
    # be clang and would reject fuzzer-no-link — so coverage is CXX-only and ASan is both.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=fuzzer-no-link,address>
        $<$<COMPILE_LANGUAGE:C>:-fsanitize=address>
        -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()
set(LUMEN_SANITIZER "" CACHE STRING "Sanitizer: empty | asan | tsan")

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "Lumen targets Clang; current compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

if(LUMEN_VULKAN_VALIDATION)
    add_compile_definitions(LUMEN_VULKAN_VALIDATION=1)
endif()

if(LUMEN_ENABLE_TILING_PLUGIN)
    add_compile_definitions(LUMEN_ENABLE_TILING_PLUGIN=1)
endif()

if(LUMEN_BUILD_FLATPAK_PORTAL)
    add_compile_definitions(LUMEN_BUILD_FLATPAK_PORTAL=1)
endif()

if(LUMEN_BUILD_XWAYLAND)
    add_compile_definitions(LUMEN_BUILD_XWAYLAND=1)
endif()

if(LUMEN_SANITIZER STREQUAL "asan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(LUMEN_SANITIZER STREQUAL "tsan")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT LUMEN_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown LUMEN_SANITIZER='${LUMEN_SANITIZER}' (use asan or tsan)")
endif()

# Treat warnings as errors in CI / when explicitly requested.
option(LUMEN_WERROR "Treat compiler warnings as errors" OFF)
if(LUMEN_WERROR)
    # -Wunused-private-field fires on every not_implemented stub that already
    # holds the handle its real implementation will need. Re-enable once the
    # platform layers are implemented.
    add_compile_options(-Werror -Wall -Wextra -Wno-unused-private-field)
endif()
