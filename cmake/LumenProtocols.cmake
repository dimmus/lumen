# Protocol code generation via wl-scanner-cpp + system wayland-scanner + manifest.toml
# Requires target: wl-scanner-cpp (must be defined before include)

set(LUMEN_PROTOCOL_MANIFEST "${CMAKE_SOURCE_DIR}/protocols/manifest.toml")
set(LUMEN_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/protocols")
set(LUMEN_GENERATED_PROTOCOL_SOURCES "")
set(LUMEN_PROTOCOL_GLUE_SOURCES "")
set(LUMEN_PROTOCOL_GLUE_HEADERS "")

find_program(LUMEN_PYTHON3 python3 python REQUIRED)

set(_LUMEN_PROTOCOL_PRIORITIES "P0")
if(LUMEN_PROTOCOLS_P1)
    list(APPEND _LUMEN_PROTOCOL_PRIORITIES "P1")
endif()
if(LUMEN_PROTOCOLS_P2)
    list(APPEND _LUMEN_PROTOCOL_PRIORITIES "P2")
endif()
list(JOIN _LUMEN_PROTOCOL_PRIORITIES "," _LUMEN_PRIORITY_CSV)

# Generate typed C++ modules (wl-scanner-cpp). Glue C sources are listed by the
# generated CMake (protocol_manifest.py) so list append happens at file scope.
function(lumen_generate_protocol xml_path output_module output_var)
    get_filename_component(xml_name "${xml_path}" NAME_WE)
    set(out_cppm "${LUMEN_GENERATED_DIR}/${output_module}.cppm")
    set(out_dispatch "${LUMEN_GENERATED_DIR}/${output_module}.dispatch.cppm")
    set(out_cpp "${LUMEN_GENERATED_DIR}/${output_module}.gen.cpp")

    add_custom_command(
        OUTPUT "${out_cppm}" "${out_dispatch}" "${out_cpp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMEN_GENERATED_DIR}"
        COMMAND "$<TARGET_FILE:wl-scanner-cpp>"
            --input "${xml_path}"
            --module "${output_module}"
            --output-dir "${LUMEN_GENERATED_DIR}"
        DEPENDS wl-scanner-cpp "${xml_path}"
        COMMENT "Generating Wayland protocol ${output_module} from ${xml_name}"
        VERBATIM
    )

    set(${output_var} "${out_cppm};${out_dispatch};${out_cpp}" PARENT_SCOPE)
endfunction()

# Emit wayland-scanner server-header + private-code.
# Core wayland.xml is skipped entirely — headers/symbols ship with libwayland-server.
function(lumen_generate_protocol_glue xml_path)
    if(NOT LUMEN_HAS_WAYLAND_SCANNER)
        return()
    endif()
    get_filename_component(xml_name "${xml_path}" NAME_WE)
    if(xml_name STREQUAL "wayland")
        return()
    endif()

    set(out_server_h "${LUMEN_GENERATED_DIR}/${xml_name}-server-protocol.h")
    set(out_client_h "${LUMEN_GENERATED_DIR}/${xml_name}-client-protocol.h")
    set(out_protocol_c "${LUMEN_GENERATED_DIR}/${xml_name}-protocol.c")

    add_custom_command(
        OUTPUT "${out_server_h}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMEN_GENERATED_DIR}"
        COMMAND "${WAYLAND_SCANNER_EXE}" server-header "${xml_path}" "${out_server_h}"
        DEPENDS "${xml_path}"
        COMMENT "wayland-scanner server-header ${xml_name}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${out_client_h}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMEN_GENERATED_DIR}"
        COMMAND "${WAYLAND_SCANNER_EXE}" client-header "${xml_path}" "${out_client_h}"
        DEPENDS "${xml_path}"
        COMMENT "wayland-scanner client-header ${xml_name}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${out_protocol_c}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LUMEN_GENERATED_DIR}"
        COMMAND "${WAYLAND_SCANNER_EXE}" private-code "${xml_path}" "${out_protocol_c}"
        DEPENDS "${xml_path}"
        COMMENT "wayland-scanner private-code ${xml_name}"
        VERBATIM
    )
endfunction()

set(LUMEN_GENERATED_PROTOCOLS_CMAKE "${CMAKE_BINARY_DIR}/generated/GeneratedProtocols.cmake")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
execute_process(
    COMMAND "${LUMEN_PYTHON3}" "${CMAKE_SOURCE_DIR}/scripts/protocol_manifest.py"
        --root "${CMAKE_SOURCE_DIR}"
        --manifest "${LUMEN_PROTOCOL_MANIFEST}"
        --cmake-out "${LUMEN_GENERATED_PROTOCOLS_CMAKE}"
        --priorities "${_LUMEN_PRIORITY_CSV}"
    RESULT_VARIABLE _LUMEN_MANIFEST_RESULT
)
if(NOT _LUMEN_MANIFEST_RESULT EQUAL 0)
    message(FATAL_ERROR "protocol_manifest.py failed (${_LUMEN_MANIFEST_RESULT})")
endif()

include("${LUMEN_GENERATED_PROTOCOLS_CMAKE}")
# LUMEN_GENERATED_PROTOCOL_SOURCES / LUMEN_PROTOCOL_GLUE_* set by included file.