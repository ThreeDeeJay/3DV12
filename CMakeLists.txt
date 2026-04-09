cmake_minimum_required(VERSION 3.22)
project(3DV12 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force static runtime so the DLL has no MSVC CRT dependency
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Version (single source of truth: src/version.h)
# We extract the integer at configure time for the RC file.
# ---------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" _ver_h)
string(REGEX MATCH "#define VERSION +([0-9]+)" _ "${_ver_h}")
set(VERSION_INT "${CMAKE_MATCH_1}")
message(STATUS "3DV12 version integer: ${VERSION_INT}")

# ---------------------------------------------------------------------------
# Source files
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
)

# Resource file (version info for the PE binary)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)
list(APPEND SOURCES "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

# ---------------------------------------------------------------------------
# DLL target
# ---------------------------------------------------------------------------
add_library(3DV12 SHARED ${SOURCES} src/d3d12.def)

target_include_directories(3DV12 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # If the NVAPI SDK is available, set NVAPI_SDK_DIR on the command line:
    # -DNVAPI_SDK_DIR=C:/nvapi
    "$<$<BOOL:${NVAPI_SDK_DIR}>:${NVAPI_SDK_DIR}>"
)

target_compile_definitions(3DV12 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    "$<$<BOOL:${NVAPI_SDK_DIR}>:HAVE_NVAPI_SDK>"
)

target_compile_options(3DV12 PRIVATE
    /W4        # High warning level
    /WX-       # Warnings not errors (relaxed for a research project)
    /permissive-
    /Zc:__cplusplus
    "$<$<CONFIG:Release>:/O2>"
    "$<$<CONFIG:Debug>:/Od;/Zi>"
)

target_link_libraries(3DV12 PRIVATE
    d3d12.lib
    dxgi.lib
    d3dcompiler.lib
    "$<$<BOOL:${NVAPI_SDK_DIR}>:${NVAPI_SDK_DIR}/amd64/nvapi64.lib>"
)

# ---------------------------------------------------------------------------
# Output name: d3d12.dll (drop-in proxy)
# ---------------------------------------------------------------------------
set_target_properties(3DV12 PROPERTIES
    OUTPUT_NAME  "d3d12"
    SUFFIX       ".dll"
    PDB_NAME     "3DV12"
)

# ---------------------------------------------------------------------------
# Install to Release/ directory
# ---------------------------------------------------------------------------
install(TARGETS 3DV12
    RUNTIME DESTINATION "${CMAKE_SOURCE_DIR}/Release"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/3DV12.ini.default"
    DESTINATION "${CMAKE_SOURCE_DIR}/Release"
    OPTIONAL
)

# ---------------------------------------------------------------------------
# IDE grouping
# ---------------------------------------------------------------------------
source_group("Source" FILES ${SOURCES})
