option(LUMEN_BUILD_PORTAL "Build xdg-desktop-portal-lumen (needs libsystemd, wayland-client; libpng for files)" ON)
option(LUMEN_VULKAN_VALIDATION "Enable Vulkan validation layers" OFF)
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

# The presets name two tools that are optional to the build and fatal to it when they are
# named but absent: `lli`/`lld` (CMAKE_LINKER_TYPE) and `ccache` (the compiler launcher). A
# cache variable naming a program CMake cannot find is only an error deep inside the first
# link, so the quick start ("cmake --preset release") failed on any machine without both,
# with a message about a missing linker rather than about the missing package. Each is
# dropped — with a note — when it is not installed; the build then uses the default linker
# and no compiler cache, which is slower and works.
function(lumen_drop_missing_tools)
    if(CMAKE_LINKER_TYPE)
        string(TOLOWER "${CMAKE_LINKER_TYPE}" _lumen_linker)
        find_program(LUMEN_LLD_PROGRAM NAMES "ld.${_lumen_linker}" "${_lumen_linker}" "ld.${_lumen_linker}-${CMAKE_CXX_COMPILER_VERSION_MAJOR}")
        if(NOT LUMEN_LLD_PROGRAM AND _lumen_linker STREQUAL "lld")
            # `lld` is what CMAKE_LINKER_TYPE=LLD really wants; accept either name.
            find_program(LUMEN_LLD_PROGRAM NAMES lld ld.lld)
        endif()
        if(NOT LUMEN_LLD_PROGRAM)
            message(STATUS
                "${CMAKE_LINKER_TYPE} is not installed: linking with the default linker "
                "(install lld for faster links)")
            set(CMAKE_LINKER_TYPE "" CACHE STRING "Linker type (lld, gold, ...)" FORCE)
        endif()
    endif()
    if(CMAKE_CXX_COMPILER_LAUNCHER)
        set(_lumen_still_here "")
        foreach(_lumen_tool IN LISTS CMAKE_CXX_COMPILER_LAUNCHER)
            find_program(LUMEN_LAUNCHER_PROGRAM_${_lumen_tool} NAMES "${_lumen_tool}")
            if(LUMEN_LAUNCHER_PROGRAM_${_lumen_tool})
                list(APPEND _lumen_still_here "${_lumen_tool}")
            else()
                message(STATUS
                    "${_lumen_tool} is not installed: building without a compiler cache "
                    "(install ccache for faster rebuilds)")
            endif()
        endforeach()
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_lumen_still_here}" CACHE STRING
            "Compiler launcher (ccache, sccache, ...)" FORCE)
    endif()
endfunction()
lumen_drop_missing_tools()

# Treat warnings as errors in CI / when explicitly requested.
option(LUMEN_WERROR "Treat compiler warnings as errors" OFF)
if(LUMEN_WERROR)
    # -Wunused-private-field fires on every not_implemented stub that already
    # holds the handle its real implementation will need. Re-enable once the
    # platform layers are implemented.
    add_compile_options(-Werror -Wall -Wextra -Wno-unused-private-field)
endif()
