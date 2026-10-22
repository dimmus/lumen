# `import std;` for every Lumen module and executable.
#
# The standard library module is built as an ordinary module library from the source the
# toolchain ships (libstdc++ `bits/std.cc`, libc++ `std.cppm`), located through the
# library's modules manifest. CMake's own CXX_MODULE_STD is still gated behind an
# experimental switch whose key changes with every CMake release; building the module
# ourselves works on any CMake that builds named modules (3.28+).
#
# The module is compiled once with the project-wide flags (build type, sanitizers), so its
# BMI matches every importer.

function(_lumen_find_std_module_manifest out_var)
    foreach(_lib libstdc++ libc++)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" ${CMAKE_CXX_FLAGS} -print-file-name=${_lib}.modules.json
            OUTPUT_VARIABLE _path
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(IS_ABSOLUTE "${_path}" AND EXISTS "${_path}")
            set(${out_var} "${_path}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

_lumen_find_std_module_manifest(_lumen_std_manifest)
if(NOT _lumen_std_manifest)
    message(FATAL_ERROR
        "Lumen uses `import std;` but ${CMAKE_CXX_COMPILER} reports no standard library "
        "modules manifest (libstdc++.modules.json / libc++.modules.json). "
        "Use libstdc++ 15+ or libc++ 19+.")
endif()

file(READ "${_lumen_std_manifest}" _lumen_std_json)
get_filename_component(_lumen_std_manifest_dir "${_lumen_std_manifest}" DIRECTORY)
string(JSON _lumen_std_count LENGTH "${_lumen_std_json}" modules)
math(EXPR _lumen_std_last "${_lumen_std_count} - 1")
set(LUMEN_STD_MODULE_SOURCE "")
foreach(_i RANGE ${_lumen_std_last})
    string(JSON _name GET "${_lumen_std_json}" modules ${_i} logical-name)
    if(_name STREQUAL "std")
        string(JSON _src GET "${_lumen_std_json}" modules ${_i} source-path)
        get_filename_component(LUMEN_STD_MODULE_SOURCE "${_src}" ABSOLUTE
                               BASE_DIR "${_lumen_std_manifest_dir}")
    endif()
endforeach()
if(NOT EXISTS "${LUMEN_STD_MODULE_SOURCE}")
    message(FATAL_ERROR "std module source not found (manifest: ${_lumen_std_manifest})")
endif()
message(STATUS "import std: ${LUMEN_STD_MODULE_SOURCE}")

add_library(lumen-std STATIC)
get_filename_component(_lumen_std_dir "${LUMEN_STD_MODULE_SOURCE}" DIRECTORY)
target_sources(lumen-std PUBLIC
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    BASE_DIRS "${_lumen_std_dir}"
    FILES "${LUMEN_STD_MODULE_SOURCE}")
set_target_properties(lumen-std PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # The std module is system code: no project warnings, never -Werror.
    COMPILE_WARNING_AS_ERROR OFF)
# Source-file options, not target ones: consumers import a BMI that CMake rebuilds from this
# source with their own flags (a synthetic target), and only source properties reach that
# compile. The warning state is kept in the BMI, so this silences libstdc++ 16's own use of
# is_trivial_v (deprecated in C++26) inside <inplace_vector>, which every user of
# inplace_vector would otherwise get — without turning the warning off for Lumen's code.
set_source_files_properties("${LUMEN_STD_MODULE_SOURCE}" PROPERTIES
    COMPILE_OPTIONS "-Wno-reserved-module-identifier;-Wno-deprecated-declarations")
target_compile_options(lumen-std PRIVATE -w)
