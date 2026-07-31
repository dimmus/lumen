# C++26 module helpers for Lumen

# Layer graph: dependencies may only flow downward (equal or lower layer).
# See docs/architecture.md §3.
set(LUMEN_LAYER_lx.foundation 0)
set(LUMEN_LAYER_lx.sync 1)
set(LUMEN_LAYER_lx.trace 1)
set(LUMEN_LAYER_lx.runtime 1)
set(LUMEN_LAYER_lx.input 2)
set(LUMEN_LAYER_lx.drm 2)
set(LUMEN_LAYER_lx.session 2)
set(LUMEN_LAYER_lx.scheduler 2)
set(LUMEN_LAYER_lx.wayland.protocols 2)
set(LUMEN_LAYER_lx.wayland.server 2)
set(LUMEN_LAYER_lx.wayland.client 2)
set(LUMEN_LAYER_lx.gfx 3)
set(LUMEN_LAYER_lx.text 3)
set(LUMEN_LAYER_lx.layout 3)
set(LUMEN_LAYER_lx.scene 3)
set(LUMEN_LAYER_lx.compositor 4)
set(LUMEN_LAYER_lx.compositor.xwayland 4)
set(LUMEN_LAYER_lx.shell.policy 5)
set(LUMEN_LAYER_lx.shell 5)
set(LUMEN_LAYER_lx.a11y 5)
# Widget toolkit is shared by shell (desktop) and apps — same layer as shell,
# not above it, so lx.shell → lx.ui is not an upward edge.
set(LUMEN_LAYER_lx.ui 5)
set(LUMEN_LAYER_lx.ui.builder 6)
set(LUMEN_LAYER_lx.app 6)
set(LUMEN_LAYER_lx.portal 5)

function(lumen_add_module_library target source_dir)
    cmake_parse_arguments(ARG "INTERFACE" "" "DEPENDS;GENERATED" ${ARGN})

    if(DEFINED LUMEN_LAYER_${target})
        foreach(dep ${ARG_DEPENDS})
            if(DEFINED LUMEN_LAYER_${dep})
                if(LUMEN_LAYER_${dep} GREATER LUMEN_LAYER_${target})
                    message(FATAL_ERROR
                        "Layer violation: ${target} (L${LUMEN_LAYER_${target}}) "
                        "depends on ${dep} (L${LUMEN_LAYER_${dep}})")
                endif()
            endif()
        endforeach()
    endif()

    file(GLOB module_interfaces CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}/*.cppm"
    )
    file(GLOB module_impls CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}/*.cpp"
    )

    if(NOT module_interfaces AND NOT module_impls AND NOT ARG_GENERATED)
        message(FATAL_ERROR "lumen_add_module_library(${target}): no sources in ${source_dir}")
    endif()

    add_library(${target})

    # CMAKE_BINARY_DIR lives under the source tree (./build), so it cannot share a
    # FILE_SET BASE_DIRS list with CMAKE_SOURCE_DIR — CMake rejects nested bases.
    # Only .cppm interface/partition units belong in CXX_MODULES; .cpp impl units
    # are ordinary scanned sources.
    if(module_interfaces)
        target_sources(${target} PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${CMAKE_SOURCE_DIR}
            FILES ${module_interfaces}
        )
    endif()
    if(module_impls)
        target_sources(${target} PRIVATE ${module_impls})
    endif()

    if(ARG_GENERATED)
        set(_lumen_gen_modules "")
        set(_lumen_gen_sources "")
        foreach(_src ${ARG_GENERATED})
            get_filename_component(_ext "${_src}" LAST_EXT)
            if(_ext STREQUAL ".cppm")
                list(APPEND _lumen_gen_modules "${_src}")
            else()
                list(APPEND _lumen_gen_sources "${_src}")
            endif()
        endforeach()
        if(_lumen_gen_modules)
            target_sources(${target} PUBLIC
                FILE_SET cxx_modules_generated
                TYPE CXX_MODULES
                BASE_DIRS ${CMAKE_BINARY_DIR}
                FILES ${_lumen_gen_modules}
            )
        endif()
        if(_lumen_gen_sources)
            target_sources(${target} PRIVATE ${_lumen_gen_sources})
        endif()
    endif()

    if(ARG_DEPENDS)
        target_link_libraries(${target} PUBLIC ${ARG_DEPENDS})
    endif()

    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/modules>"
        "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>"
    )

    lumen_apply_module_properties(${target})
endfunction()

function(lumen_check_executable_layers target)
    get_target_property(_links ${target} LINK_LIBRARIES)
    if(NOT _links)
        return()
    endif()
    foreach(_dep ${_links})
        if(NOT TARGET ${_dep})
            continue()
        endif()
        get_target_property(_dep_type ${_dep} TYPE)
        if(NOT _dep_type STREQUAL "STATIC_LIBRARY")
            continue()
        endif()
        if(DEFINED LUMEN_LAYER_${_dep})
            foreach(_root ${ARGN})
                if(DEFINED LUMEN_LAYER_${_root})
                    if(LUMEN_LAYER_${_dep} GREATER LUMEN_LAYER_${_root})
                        message(FATAL_ERROR
                            "Executable layer violation: ${target} links ${ _root } (L${LUMEN_LAYER_${_root}}) "
                            "through ${ _dep } (L${LUMEN_LAYER_${_dep}})")
                    endif()
                endif()
            endforeach()
        endif()
    endforeach()
endfunction()

function(lumen_apply_module_properties target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # CMake manages module output paths per object; only add the prebuilt
        # search path so dependents can find PCM artifacts.
        target_compile_options(${target} PRIVATE
            -fprebuilt-module-path=${CMAKE_BINARY_DIR}/pcm
        )
    endif()
endfunction()
