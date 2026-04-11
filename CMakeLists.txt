cmake_minimum_required(VERSION 3.22)
project(3DV12 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" _ver_h)
string(REGEX MATCH "#define VERSION +([0-9]+)" _ "${_ver_h}")
set(VERSION_INT "${CMAKE_MATCH_1}")
message(STATUS "3DV12 version integer: ${VERSION_INT}")

# ---------------------------------------------------------------------------
# Generate d3d12.def from real d3d12.dll
#
# Uses scripts/gen_def.py (pure-Python PE parser, no dumpbin needed).
# On Windows builds the system d3d12.dll is at %SystemRoot%\System32\d3d12.dll.
# If Python3 or the DLL is unavailable (cross-compilation), we fall back to
# the hand-maintained src/d3d12.def.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter QUIET)

set(_real_d3d12    "$ENV{SystemRoot}/System32/d3d12.dll")
set(_gen_def       "${CMAKE_BINARY_DIR}/d3d12_generated.def")
set(_gen_stubs_cpp "${CMAKE_BINARY_DIR}/d3d12_unknown_stubs.cpp")
set(_gen_def_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_def.py")
set(_USE_GEN_DEF FALSE)

if(Python3_FOUND AND EXISTS "${_real_d3d12}" AND EXISTS "${_gen_def_script}")
    message(STATUS "3DV12: generating d3d12.def from real DLL via gen_def.py...")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_gen_def_script}"
                "${_real_d3d12}" "${_gen_def}" "${_gen_stubs_cpp}"
        RESULT_VARIABLE _gen_rc
        OUTPUT_VARIABLE _gen_out
        ERROR_VARIABLE  _gen_err
    )
    if(_gen_rc EQUAL 0 AND EXISTS "${_gen_def}")
        message(STATUS "3DV12: ${_gen_out}")
        set(_USE_GEN_DEF TRUE)
    else()
        message(WARNING "3DV12: gen_def.py failed (rc=${_gen_rc}): ${_gen_err}\n"
                        "Falling back to static src/d3d12.def")
    endif()
else()
    if(NOT Python3_FOUND)
        message(WARNING "3DV12: Python3 not found – using static src/d3d12.def")
    elseif(NOT EXISTS "${_real_d3d12}")
        message(WARNING "3DV12: ${_real_d3d12} not found – using static src/d3d12.def")
    endif()
endif()

# Choose which .def to use
if(_USE_GEN_DEF)
    set(_DEF_FILE "${_gen_def}")
else()
    set(_DEF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/d3d12.def")
    # Create an empty stubs file so the build always compiles
    file(WRITE "${_gen_stubs_cpp}"
        "// No stubs generated (static fallback .def in use).\n")
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SOURCES
    src/dllmain.cpp
    src/log.cpp
    src/config.cpp
    src/dxbc_parser.cpp
    src/shader_patcher.cpp
    src/stereo_engine.cpp
    src/wrapped_device.cpp
    src/wrapped_command_list.cpp
    "${_gen_stubs_cpp}"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)
list(APPEND SOURCES "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

# ---------------------------------------------------------------------------
# DLL target
# ---------------------------------------------------------------------------
add_library(3DV12 SHARED ${SOURCES} "${_DEF_FILE}")

target_include_directories(3DV12 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"          # for generated stubs header if needed
    "$<$<BOOL:${NVAPI_SDK_DIR}>:${NVAPI_SDK_DIR}>"
)

target_compile_definitions(3DV12 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    "$<$<BOOL:${NVAPI_SDK_DIR}>:HAVE_NVAPI_SDK>"
)

target_compile_options(3DV12 PRIVATE
    /W4
    /WX-
    /permissive-
    /Zc:__cplusplus
    "$<$<CONFIG:Release>:/O2>"
    "$<$<CONFIG:Debug>:/Od;/Zi>"
)

target_link_libraries(3DV12 PRIVATE
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
    "$<$<BOOL:${NVAPI_SDK_DIR}>:${NVAPI_SDK_DIR}/amd64/nvapi64.lib>"
)

set_target_properties(3DV12 PROPERTIES
    OUTPUT_NAME  "d3d12"
    SUFFIX       ".dll"
    PDB_NAME     "3DV12"
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS 3DV12
    RUNTIME DESTINATION "${CMAKE_SOURCE_DIR}/Release"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/3DV12.ini.default"
    DESTINATION "${CMAKE_SOURCE_DIR}/Release"
    OPTIONAL
)

source_group("Source" FILES ${SOURCES})
