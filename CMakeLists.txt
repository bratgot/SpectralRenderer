cmake_minimum_required(VERSION 3.22)
project(HdSpectral VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NUKE_ROOT "C:/Program Files/Nuke17.0v1" CACHE PATH "Nuke 17.0v1 install root")

if(NOT EXISTS "${NUKE_ROOT}/DDImage.lib")
    message(FATAL_ERROR "DDImage.lib not found in NUKE_ROOT: ${NUKE_ROOT}")
endif()

message(STATUS "NUKE_ROOT = ${NUKE_ROOT}")

set(NUKE_NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NUKE_PY_INCLUDE_DIR  "${NUKE_ROOT}/include/include")
set(NUKE_PXR_INCLUDE_DIR "${NUKE_ROOT}/FnUSD/include")
set(NUKE_LIB_DIR         "${NUKE_ROOT}")
set(NUKE_USD_LIB_DIR     "${NUKE_ROOT}/FnUSD/lib")

# ---------------------------------------------------------------------------
# Nuke / NDK libs
# ---------------------------------------------------------------------------
find_library(LIB_DDIMAGE   DDImage          PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_FDKBASE   "FdkBase.2.1"   PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_NDK       Ndk              PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDABS    FnUsdAbstraction PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDENGINE FnUsdEngine      PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_TBB       tbb              PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)

# ---------------------------------------------------------------------------
# PXR libs (usd_ prefixed, in FnUSD/lib)
# ---------------------------------------------------------------------------
set(USD_LIB_NAMES
    usd_tf usd_gf usd_vt usd_arch usd_js usd_trace usd_work usd_plug
    usd_sdf usd_pcp usd_ar usd_kind
    usd_usd usd_usdGeom usd_usdShade usd_usdLux usd_usdSkel usd_usdRender
    usd_hf usd_hd usd_hdx usd_hgi usd_hgiGL usd_hgiInterop
    usd_hdsi usd_hdSt usd_hdar
    usd_usdImaging usd_usdImagingGL usd_usdHydra
    usd_cameraUtil usd_pxOsd usd_geomUtil usd_sdr
    usd_python usd_garch usd_glf
)
foreach(_lib ${USD_LIB_NAMES})
    find_library(_found_${_lib} ${_lib} PATHS "${NUKE_USD_LIB_DIR}" NO_DEFAULT_PATH)
    if(_found_${_lib})
        list(APPEND USD_LIBS ${_found_${_lib}})
    else()
        message(WARNING "NOT FOUND: ${_lib}.lib")
    endif()
endforeach()

# Common include dirs used by both targets
set(SPECTRAL_INCLUDES
    "${NUKE_PXR_INCLUDE_DIR}"
    "${NUKE_NDK_INCLUDE_DIR}"
    "${NUKE_PY_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set(SPECTRAL_DEFINES
    NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN HDSPECTRAL_EXPORTS
)

set(SPECTRAL_OPTIONS
    /W3 /wd4251 /wd4275 /wd4267 /wd4244 /wd4305 /MP /Zc:__cplusplus /utf-8
)

# ---------------------------------------------------------------------------
# Embree 4 — BVH acceleration (Phase 2)
# ---------------------------------------------------------------------------
set(EMBREE_ROOT "C:/dev/embree-4" CACHE PATH "Embree 4 install root")

find_library(LIB_EMBREE embree4 PATHS "${EMBREE_ROOT}/lib" NO_DEFAULT_PATH)
if(LIB_EMBREE)
    message(STATUS "Embree 4 found: ${LIB_EMBREE}")
    set(EMBREE_INCLUDE_DIR "${EMBREE_ROOT}/include")
    set(HAS_EMBREE TRUE)
else()
    message(WARNING "Embree 4 NOT found — falling back to brute-force intersection")
    set(HAS_EMBREE FALSE)
endif()

# ---------------------------------------------------------------------------
# SpectralCore — static lib shared by both DLLs
#   Contains: SpectralIntegrator (ray tracer), SpectralScene (geometry),
#             SpectralBVH (Embree wrapper, Phase 2)
# ---------------------------------------------------------------------------
set(SPECTRAL_CORE_SOURCES src/SpectralIntegrator.cpp)
if(HAS_EMBREE)
    list(APPEND SPECTRAL_CORE_SOURCES src/SpectralBVH.cpp)
endif()

add_library(SpectralCore STATIC ${SPECTRAL_CORE_SOURCES})
target_include_directories(SpectralCore PUBLIC ${SPECTRAL_INCLUDES})
target_compile_definitions(SpectralCore PRIVATE ${SPECTRAL_DEFINES})
target_compile_options(SpectralCore PRIVATE ${SPECTRAL_OPTIONS})
target_link_libraries(SpectralCore PUBLIC "${LIB_TBB}")

if(HAS_EMBREE)
    target_include_directories(SpectralCore PUBLIC "${EMBREE_INCLUDE_DIR}")
    target_link_libraries(SpectralCore PUBLIC "${LIB_EMBREE}")
    target_compile_definitions(SpectralCore PUBLIC SPECTRAL_HAS_EMBREE=1)
endif()

# ---------------------------------------------------------------------------
# HdSpectral.dll — Hydra render delegate (PXR plugin)
#   Loaded by PXR_PLUGINPATH_NAME at Nuke startup.
#   No DDImage/Iop code — pure Hydra/USD.
# ---------------------------------------------------------------------------
add_library(HdSpectral SHARED
    src/HdSpectralRendererPlugin.cpp
    src/HdSpectralRenderDelegate.cpp
    src/HdSpectralRenderPass.cpp
    src/HdSpectralMesh.cpp
    src/HdSpectralRenderBuffer.cpp
)
target_include_directories(HdSpectral PRIVATE ${SPECTRAL_INCLUDES})
target_compile_definitions(HdSpectral PRIVATE ${SPECTRAL_DEFINES})
target_compile_options(HdSpectral PRIVATE ${SPECTRAL_OPTIONS})
target_link_libraries(HdSpectral PRIVATE
    SpectralCore
    "${LIB_DDIMAGE}"
    "${LIB_FDKBASE}"
    "${LIB_NDK}"
    "${LIB_USDABS}"
    "${LIB_USDENGINE}"
    "C:/dev/SpectralRenderer/python311.lib"
    ${USD_LIBS}
)

# plugInfo.json — only for HdSpectral (Hydra delegate)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json"
    @ONLY
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    "{ \"Includes\": [ \"HdSpectral/resources/\" ] }\n")

set_target_properties(HdSpectral PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
    PDB_OUTPUT_DIRECTORY             "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
)

# ---------------------------------------------------------------------------
# SpectralRender.dll — NDK RenderIop (Nuke plugin)
#   Loaded by NUKE_PATH — completely separate from HdSpectral.dll.
#   No PXR/Hydra code — pure DDImage/NDK.
# ---------------------------------------------------------------------------
add_library(SpectralRender SHARED
    src/SpectralRenderIop.cpp
)
target_include_directories(SpectralRender PRIVATE ${SPECTRAL_INCLUDES})
target_compile_definitions(SpectralRender PRIVATE ${SPECTRAL_DEFINES})
target_compile_options(SpectralRender PRIVATE ${SPECTRAL_OPTIONS})
target_link_libraries(SpectralRender PRIVATE
    SpectralCore
    "${LIB_DDIMAGE}"
    "${LIB_TBB}"
    "${LIB_USDABS}"
    "C:/dev/SpectralRenderer/python311.lib"
    ${USD_LIBS}
)

set_target_properties(SpectralRender PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    PDB_OUTPUT_DIRECTORY             "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS HdSpectral RUNTIME DESTINATION HdSpectral)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json"
    DESTINATION HdSpectral/resources)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
    DESTINATION .)
install(TARGETS SpectralRender RUNTIME DESTINATION SpectralRender)
