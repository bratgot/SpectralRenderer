cmake_minimum_required(VERSION 3.22)
project(HdSpectral VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Toolchain — Nuke 17.0v1 Windows requires MSVC 14.3x (VS2022 / v143)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# NUKE_ROOT — only thing you ever need to change
# ---------------------------------------------------------------------------
set(NUKE_ROOT "C:/Program Files/Nuke17.0v1" CACHE PATH
    "Root directory of the Nuke 17.0v1 installation")

if(NOT EXISTS "${NUKE_ROOT}/DDImage.lib")
    message(FATAL_ERROR
        "NUKE_ROOT does not look like a Nuke install (DDImage.lib missing):\n"
        "  ${NUKE_ROOT}\n"
        "Pass the correct path with -D NUKE_ROOT=<path>")
endif()

message(STATUS "NUKE_ROOT = ${NUKE_ROOT}")

# ---------------------------------------------------------------------------
# Confirmed directory layout for Nuke 17.0v1
#
#   Headers
#     NDK / DDImage / fdk / usg:    ${NUKE_ROOT}/include/
#       subdirs: DDImage  ndk  fdk  usg  Blink  GL  ...
#     PXR / Hydra / USD:            ${NUKE_ROOT}/FnUSD/include/
#       subdirs: pxr/base  pxr/usd  pxr/imaging  ...
#
#   Libraries (all in ${NUKE_ROOT}/)
#     DDImage.lib          — Nuke image/op API
#     FdkBase.2.1.lib      — Foundry base math (note versioned name)
#     Ndk.lib              — NDK C++ plugin API
#     FnUsdAbstraction.lib — Hydra + all PXR symbols re-exported here
#     FnUsdEngine.lib      — USD stage/engine API
#     tbb.lib              — Intel TBB (for std::execution::par_unseq)
# ---------------------------------------------------------------------------
set(NUKE_NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NUKE_PXR_INCLUDE_DIR "${NUKE_ROOT}/FnUSD/include")
set(NUKE_LIB_DIR         "${NUKE_ROOT}")

# ---------------------------------------------------------------------------
# Find libraries — using exact names confirmed by dir listing
# ---------------------------------------------------------------------------
find_library(LIB_DDIMAGE    DDImage            PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_FDKBASE    "FdkBase.2.1"      PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_NDK        Ndk                PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDABS     FnUsdAbstraction   PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDENGINE  FnUsdEngine        PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_TBB        tbb                PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDSHIM    "FnUsdShim.0.8"    PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH)

message(STATUS "DDImage.lib      = ${LIB_DDIMAGE}")
message(STATUS "FdkBase.2.1.lib  = ${LIB_FDKBASE}")
message(STATUS "Ndk.lib          = ${LIB_NDK}")
message(STATUS "FnUsdAbstraction = ${LIB_USDABS}")
message(STATUS "FnUsdEngine      = ${LIB_USDENGINE}")
message(STATUS "tbb.lib          = ${LIB_TBB}")

# ---------------------------------------------------------------------------
# The plugin shared library
# ---------------------------------------------------------------------------
add_library(HdSpectral SHARED
    src/HdSpectralRendererPlugin.cpp
    src/HdSpectralRenderDelegate.cpp
    src/HdSpectralRenderPass.cpp
    src/HdSpectralMesh.cpp
    src/HdSpectralRenderBuffer.cpp
)

target_include_directories(HdSpectral
    PRIVATE
        # PXR/Hydra headers FIRST — must win over any system pxr if installed
        "${NUKE_PXR_INCLUDE_DIR}"
        # NDK, DDImage, fdk, usg
        "${NUKE_NDK_INCLUDE_DIR}"
        # Our source headers
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(HdSpectral
    PRIVATE
        NOMINMAX
        _USE_MATH_DEFINES
        WIN32_LEAN_AND_MEAN
        PXR_BUILDING_FOR_EXTERNAL=1
        HDSPECTRAL_EXPORTS
)

target_compile_options(HdSpectral
    PRIVATE
        /W3
        /wd4251   # dll-interface for STL members in PXR headers
        /wd4275   # dll-interface base class in PXR headers
        /wd4267   # size_t->int narrowing in PXR headers
        /wd4244   # double->float narrowing in PXR math
        /MP
        /Zc:__cplusplus
        /utf-8
)

target_link_libraries(HdSpectral
    PRIVATE
        "${LIB_DDIMAGE}"
        "${LIB_FDKBASE}"
        "${LIB_NDK}"
        "${LIB_USDABS}"
        "${LIB_USDENGINE}"
        "${LIB_TBB}"
)

if(LIB_USDSHIM)
    target_link_libraries(HdSpectral PRIVATE "${LIB_USDSHIM}")
endif()

# ---------------------------------------------------------------------------
# Output layout
#   build/HdSpectral/HdSpectral.dll
#   build/HdSpectral/resources/plugInfo.json
#
#   PXR_PLUGINPATH_NAME  →  build/          (parent of HdSpectral/)
#   PATH                 →  build/HdSpectral/
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json"
    @ONLY
)

set_target_properties(HdSpectral PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    LIBRARY_OUTPUT_DIRECTORY                "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    PDB_OUTPUT_DIRECTORY                    "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    PDB_OUTPUT_DIRECTORY_RELEASE            "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
)

install(TARGETS HdSpectral RUNTIME DESTINATION HdSpectral LIBRARY DESTINATION HdSpectral)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json"
    DESTINATION HdSpectral/resources)
