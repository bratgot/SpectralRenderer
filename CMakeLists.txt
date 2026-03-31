cmake_minimum_required(VERSION 3.22)
project(HdSpectral VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NUKE_ROOT "C:/Program Files/Nuke17.0v1" CACHE PATH
    "Root directory of the Nuke 17.0v1 installation")

if(NOT EXISTS "${NUKE_ROOT}/DDImage.lib")
    message(FATAL_ERROR "DDImage.lib not found in NUKE_ROOT: ${NUKE_ROOT}")
endif()

message(STATUS "NUKE_ROOT = ${NUKE_ROOT}")

# ---------------------------------------------------------------------------
# Confirmed paths for Nuke 17.0v1
#
#   NDK / DDImage / fdk / usg headers:  ${NUKE_ROOT}/include/
#   PXR / Hydra headers:                ${NUKE_ROOT}/FnUSD/include/
#   Python headers (pyconfig.h):        ${NUKE_ROOT}/include/include/
#
#   Nuke libs:  ${NUKE_ROOT}/*.lib
#   PXR libs:   ${NUKE_ROOT}/FnUSD/lib/usd_*.lib
# ---------------------------------------------------------------------------
set(NUKE_NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NUKE_PY_INCLUDE_DIR  "${NUKE_ROOT}/include/include")
set(NUKE_PXR_INCLUDE_DIR "${NUKE_ROOT}/FnUSD/include")
set(NUKE_LIB_DIR         "${NUKE_ROOT}")
set(NUKE_USD_LIB_DIR     "${NUKE_ROOT}/FnUSD/lib")

# ---------------------------------------------------------------------------
# Nuke / NDK libs  (in NUKE_ROOT)
# ---------------------------------------------------------------------------
find_library(LIB_DDIMAGE   DDImage          PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_FDKBASE   "FdkBase.2.1"   PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_NDK       Ndk              PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDABS    FnUsdAbstraction PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDENGINE FnUsdEngine      PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_TBB       tbb              PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)

message(STATUS "DDImage   = ${LIB_DDIMAGE}")
message(STATUS "FdkBase   = ${LIB_FDKBASE}")
message(STATUS "Ndk       = ${LIB_NDK}")
message(STATUS "UsdAbs    = ${LIB_USDABS}")
message(STATUS "UsdEngine = ${LIB_USDENGINE}")
message(STATUS "tbb       = ${LIB_TBB}")

# ---------------------------------------------------------------------------
# PXR libs  (in FnUSD/lib, all prefixed usd_)
#
#   Naming: usd_hd.lib, usd_tf.lib, usd_sdf.lib, etc.
# ---------------------------------------------------------------------------
set(USD_LIB_NAMES
    # Foundation
    usd_tf usd_gf usd_vt usd_arch usd_js usd_trace usd_work usd_plug
    # SDF / PCP / AR
    usd_sdf usd_pcp usd_ar usd_kind
    # USD core
    usd_usd usd_usdGeom usd_usdShade usd_usdLux usd_usdSkel usd_usdRender
    # Hydra core
    usd_hf usd_hd usd_hdx usd_hgi usd_hgiGL usd_hgiInterop
    usd_hdsi usd_hdSt usd_hdar
    # USD imaging / Hydra delegates
    usd_usdImaging usd_usdImagingGL usd_usdHydra
    # Utilities
    usd_cameraUtil usd_pxOsd usd_geomUtil usd_sdr
    # Python / boost.python (provides __imp__Py_* symbols via usd_python.lib)
    usd_python
    # Garch (GPU arch abstraction — needed by hdSt)
    usd_garch usd_glf
)

foreach(_lib ${USD_LIB_NAMES})
    find_library(_found_${_lib} ${_lib}
        PATHS "${NUKE_USD_LIB_DIR}" NO_DEFAULT_PATH)
    if(_found_${_lib})
        list(APPEND USD_LIBS ${_found_${_lib}})
        message(STATUS "  Found: ${_lib}.lib")
    else()
        message(WARNING "  NOT FOUND: ${_lib}.lib — linker may fail")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The plugin
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
        "${NUKE_PXR_INCLUDE_DIR}"      # pxr/imaging/hd/... — MUST be first
        "${NUKE_NDK_INCLUDE_DIR}"      # DDImage, ndk, fdk, usg, Blink, GL
        "${NUKE_PY_INCLUDE_DIR}"       # pyconfig.h (nested include/include/)
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(HdSpectral
    PRIVATE
        NOMINMAX
        _USE_MATH_DEFINES
        WIN32_LEAN_AND_MEAN
        HDSPECTRAL_EXPORTS
)

target_compile_options(HdSpectral
    PRIVATE
        /W3
        /wd4251 /wd4275 /wd4267 /wd4244 /wd4305
        /MP
        /Zc:__cplusplus
        /utf-8
)

target_link_libraries(HdSpectral
    PRIVATE
        # Nuke
        "${LIB_DDIMAGE}"
        "${LIB_FDKBASE}"
        "${LIB_NDK}"
        "${LIB_USDABS}"
        "${LIB_USDENGINE}"
        "${LIB_TBB}"
        # PXR (all usd_*.lib from FnUSD/lib)
        ${USD_LIBS}
        "C:/dev/SpectralRenderer/python311.lib"
)

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json"
    @ONLY
)

# Root plugInfo.json — tells PXR where to find the plugin within this directory tree
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    "{ \"Includes\": [ \"HdSpectral/resources/\" ] }\n")

set_target_properties(HdSpectral PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    PDB_OUTPUT_DIRECTORY             "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    PDB_OUTPUT_DIRECTORY_RELEASE     "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
)

install(TARGETS HdSpectral RUNTIME DESTINATION HdSpectral)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json"
    DESTINATION HdSpectral/resources)
