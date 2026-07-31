# Platform dependencies (Linux compositor target)

find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND QUIET wayland-server wayland-client)
    pkg_check_modules(WAYLAND_SCANNER QUIET wayland-scanner)
    pkg_check_modules(LIBDRM QUIET libdrm)
    pkg_check_modules(LIBINPUT QUIET libinput)
    # EGL + GLES + GBM back the hardware GL present path. All three are required
    # together: GBM allocates the scanout buffer, EGL binds it, GLES draws into it.
    pkg_check_modules(LIBEGL QUIET egl glesv2 gbm)
    pkg_check_modules(HARFBUZZ QUIET harfbuzz freetype2 fontconfig)
    pkg_check_modules(LIBSYSTEMD QUIET libsystemd)
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

    if(LIBINPUT_FOUND)
        target_include_directories(${target} PRIVATE ${LIBINPUT_INCLUDE_DIRS})
        target_link_libraries(${target} PRIVATE ${LIBINPUT_LIBRARIES})
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
    endif()
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
