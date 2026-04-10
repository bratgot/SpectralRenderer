cmake_minimum_required(VERSION 3.22)
project(HdSpectral VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# CUDA + OptiX
# ---------------------------------------------------------------------------
set(CUDA_TOOLKIT_ROOT "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6" CACHE PATH "CUDA toolkit")
set(OPTIX_ROOT "C:/ProgramData/NVIDIA Corporation/OptiX SDK 9.0.0" CACHE PATH "OptiX SDK")

set(HAS_OPTIX FALSE)
if(EXISTS "${OPTIX_ROOT}/include/optix.h" AND EXISTS "${CUDA_TOOLKIT_ROOT}/bin/nvcc.exe")
    message(STATUS "OptiX found: ${OPTIX_ROOT}")
    set(HAS_OPTIX TRUE)
    find_library(LIB_CUDA cuda PATHS "${CUDA_TOOLKIT_ROOT}/lib/x64" NO_DEFAULT_PATH)
    find_library(LIB_CUDART cudart PATHS "${CUDA_TOOLKIT_ROOT}/lib/x64" NO_DEFAULT_PATH)

    set(PTX_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/SpectralGPUKernel.cu")
    set(PTX_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SpectralGPUKernel.ptx")
    set(PTX_HEADER "${CMAKE_CURRENT_BINARY_DIR}/SpectralGPUKernel_ptx.h")

    add_custom_command(OUTPUT "${PTX_OUTPUT}"
        COMMAND "${CUDA_TOOLKIT_ROOT}/bin/nvcc.exe"
            -ptx -O3 --use_fast_math --ftz=true --prec-div=false --prec-sqrt=false
            -I "${OPTIX_ROOT}/include"
            -I "${CUDA_TOOLKIT_ROOT}/include"
            -I "${CMAKE_CURRENT_SOURCE_DIR}/src"
            -o "${PTX_OUTPUT}" "${PTX_SOURCE}"
        DEPENDS "${PTX_SOURCE}" "${CMAKE_CURRENT_SOURCE_DIR}/src/SpectralGPUParams.h"
        COMMENT "Compiling OptiX PTX kernel" VERBATIM)

    add_custom_command(OUTPUT "${PTX_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
            -DPTX_FILE=${PTX_OUTPUT} -DHEADER_FILE=${PTX_HEADER}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedPTX.cmake"
        DEPENDS "${PTX_OUTPUT}"
        COMMENT "Embedding PTX as C header" VERBATIM)

    add_custom_target(SpectralPTX DEPENDS "${PTX_HEADER}")
    set(DENSIFY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/SpectralNanoDensify.cu")
    set(DENSIFY_OBJ "${CMAKE_CURRENT_BINARY_DIR}/SpectralNanoDensify.obj")
    add_custom_command(OUTPUT "${DENSIFY_OBJ}"
        COMMAND "${CUDA_TOOLKIT_ROOT}/bin/nvcc.exe"
            -c -O3 --use_fast_math --std c++17
            -I "${CUDA_TOOLKIT_ROOT}/include"
            -I "${VCPKG_INSTALLED}/include"
            --machine 64 -Xcompiler "/MD /O2"
            -o "${DENSIFY_OBJ}" "${DENSIFY_SOURCE}"
        DEPENDS "${DENSIFY_SOURCE}"
        COMMENT "Compiling NanoVDB densify kernel" VERBATIM)
    add_custom_target(SpectralDensify DEPENDS "${DENSIFY_OBJ}")
else()
    message(STATUS "OptiX or CUDA not found — GPU rendering disabled")
endif()

# ---------------------------------------------------------------------------
# Nuke 17
# ---------------------------------------------------------------------------
set(NUKE_ROOT "C:/Program Files/Nuke17.0v1" CACHE PATH "Nuke install root")
if(NOT EXISTS "${NUKE_ROOT}/DDImage.lib")
    message(FATAL_ERROR "DDImage.lib not found in NUKE_ROOT: ${NUKE_ROOT}")
endif()

set(NUKE_NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NUKE_PY_INCLUDE_DIR  "${NUKE_ROOT}/include/include")
set(NUKE_PXR_INCLUDE_DIR "${NUKE_ROOT}/FnUSD/include")
set(NUKE_LIB_DIR         "${NUKE_ROOT}")
set(NUKE_USD_LIB_DIR     "${NUKE_ROOT}/FnUSD/lib")

find_library(LIB_DDIMAGE   DDImage          PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_FDKBASE   "FdkBase.2.1"   PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_NDK       Ndk              PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDABS    FnUsdAbstraction PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_USDENGINE FnUsdEngine      PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_library(LIB_TBB       tbb              PATHS "${NUKE_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)

set(USD_LIB_NAMES
    usd_tf usd_gf usd_vt usd_arch usd_js usd_trace usd_work usd_plug
    usd_sdf usd_pcp usd_ar usd_kind
    usd_usd usd_usdGeom usd_usdShade usd_usdLux usd_usdSkel usd_usdRender usd_usdVol
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
    endif()
endforeach()

set(SPECTRAL_INCLUDES
    "${NUKE_PXR_INCLUDE_DIR}" "${NUKE_NDK_INCLUDE_DIR}"
    "${NUKE_PY_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set(SPECTRAL_DEFINES NOMINMAX _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN HDSPECTRAL_EXPORTS)
set(SPECTRAL_OPTIONS /W3 /wd4251 /wd4275 /wd4267 /wd4244 /wd4305 /wd4996 /MP /Zc:__cplusplus /utf-8)

# ---------------------------------------------------------------------------
# Embree 4
# ---------------------------------------------------------------------------
set(EMBREE_ROOT "C:/dev/embree-4" CACHE PATH "Embree 4 install root")
find_library(LIB_EMBREE embree4 PATHS "${EMBREE_ROOT}/lib" NO_DEFAULT_PATH)
if(LIB_EMBREE)
    set(HAS_EMBREE TRUE)
    set(EMBREE_INCLUDE_DIR "${EMBREE_ROOT}/include")
else()
    set(HAS_EMBREE FALSE)
endif()

# ---------------------------------------------------------------------------
# OpenSubdiv
# ---------------------------------------------------------------------------
set(OSD_ROOT "C:/dev/opensubdiv-3.6.0" CACHE PATH "OpenSubdiv source root")
set(OSD_LIB_DIR "${OSD_ROOT}/build/lib/Release" CACHE PATH "OpenSubdiv lib dir")
find_library(LIB_OSD_CPU osdCPU PATHS "${OSD_LIB_DIR}" NO_DEFAULT_PATH)
if(LIB_OSD_CPU)
    set(HAS_OSD TRUE)
    set(OSD_INCLUDE_DIR "${OSD_ROOT}")
else()
    set(HAS_OSD FALSE)
endif()

# ---------------------------------------------------------------------------
# OpenVDB — volume rendering (Phase 10)
# Tries find_package first, then falls back to manual vcpkg path detection.
# Install: vcpkg install openvdb:x64-windows
# ---------------------------------------------------------------------------
set(HAS_VDB FALSE)
set(VCPKG_INSTALLED "C:/vcpkg/installed/x64-windows" CACHE PATH "vcpkg installed prefix")

find_package(OpenVDB QUIET)
if(OpenVDB_FOUND)
    set(HAS_VDB TRUE)
    message(STATUS "OpenVDB found via find_package: ${OpenVDB_VERSION}")
else()
    # Manual fallback: look in vcpkg installed prefix
    find_library(LIB_OPENVDB openvdb
        PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
    if(LIB_OPENVDB AND EXISTS "${VCPKG_INSTALLED}/include/openvdb/openvdb.h")
        set(HAS_VDB TRUE)
        set(OPENVDB_INCLUDE_DIR "${VCPKG_INSTALLED}/include")
        set(OPENVDB_LIB "${LIB_OPENVDB}")
        message(STATUS "OpenVDB found (manual): ${LIB_OPENVDB}")
        # OpenVDB deps from vcpkg
        find_library(LIB_BLOSC blosc PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        find_library(LIB_ZLIB zlib PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        find_library(LIB_ZSTD zstd PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        find_library(LIB_LZ4 lz4 PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        # Imath contains imath_half_to_float_table needed by OpenVDB
        set(LIB_IMATH "${VCPKG_INSTALLED}/lib/Imath-3_2.lib")
        if(NOT EXISTS "${LIB_IMATH}")
            message(WARNING "Imath-3_2.lib not found at ${LIB_IMATH}")
            set(LIB_IMATH "")
        endif()
        # OpenEXR for EXR texture loading (HDRI env maps)
        find_library(LIB_OPENEXR_CORE OpenEXRCore-3_4 PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        find_library(LIB_OPENEXR      OpenEXR-3_4     PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        find_library(LIB_ILMTHREAD    IlmThread-3_4   PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        if(NOT LIB_OPENEXR)
            find_library(LIB_OPENEXR_CORE OpenEXRCore-3_2 PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
            find_library(LIB_OPENEXR      OpenEXR-3_2     PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
            find_library(LIB_ILMTHREAD    IlmThread-3_2   PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        endif()
        if(NOT LIB_OPENEXR)
            find_library(LIB_OPENEXR OpenEXR PATHS "${VCPKG_INSTALLED}/lib" NO_DEFAULT_PATH)
        endif()
        if(LIB_OPENEXR)
            message(STATUS "OpenEXR found: ${LIB_OPENEXR}")
        else()
            message(WARNING "OpenEXR not found — EXR texture loading disabled")
        endif()
    else()
        message(STATUS "OpenVDB NOT found — volume rendering disabled")
        message(STATUS "  Install: vcpkg install openvdb:x64-windows")
        message(STATUS "  Or set VCPKG_INSTALLED to your vcpkg prefix")
    endif()
endif()

# ---------------------------------------------------------------------------
# SpectralCore — static lib shared by both DLLs
# ---------------------------------------------------------------------------
set(SPECTRAL_CORE_SOURCES src/SpectralIntegrator.cpp src/SpectralTexture.cpp)
if(HAS_EMBREE)
    list(APPEND SPECTRAL_CORE_SOURCES src/SpectralBVH.cpp)
endif()
if(HAS_OSD)
    list(APPEND SPECTRAL_CORE_SOURCES src/SpectralSubdiv.cpp)
endif()
if(HAS_OPTIX)
    list(APPEND SPECTRAL_CORE_SOURCES src/SpectralGPU.cpp)
endif()
if(HAS_VDB)
    list(APPEND SPECTRAL_CORE_SOURCES src/SpectralVDBLoader.cpp)
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
if(HAS_OSD)
    target_include_directories(SpectralCore PUBLIC "${OSD_INCLUDE_DIR}")
    target_link_libraries(SpectralCore PUBLIC "${LIB_OSD_CPU}")
    target_compile_definitions(SpectralCore PUBLIC SPECTRAL_HAS_OSD=1)
endif()
if(HAS_OPTIX)
    target_include_directories(SpectralCore PUBLIC
        "${OPTIX_ROOT}/include" "${CUDA_TOOLKIT_ROOT}/include"
        "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(SpectralCore PUBLIC "${LIB_CUDA}" "${LIB_CUDART}")
    target_compile_definitions(SpectralCore PUBLIC SPECTRAL_HAS_OPTIX=1)
    add_dependencies(SpectralCore SpectralPTX)
    add_dependencies(SpectralCore SpectralDensify)
    target_link_libraries(SpectralCore PUBLIC "${DENSIFY_OBJ}")
endif()
if(HAS_VDB)
    if(TARGET OpenVDB::openvdb)
        target_link_libraries(SpectralCore PUBLIC OpenVDB::openvdb)
    else()
        target_include_directories(SpectralCore PUBLIC "${OPENVDB_INCLUDE_DIR}")
        target_link_libraries(SpectralCore PUBLIC "${OPENVDB_LIB}")
        if(LIB_BLOSC)
            target_link_libraries(SpectralCore PUBLIC "${LIB_BLOSC}")
        endif()
        if(LIB_ZLIB)
            target_link_libraries(SpectralCore PUBLIC "${LIB_ZLIB}")
        endif()
        if(LIB_ZSTD)
            target_link_libraries(SpectralCore PUBLIC "${LIB_ZSTD}")
        endif()
        if(LIB_LZ4)
            target_link_libraries(SpectralCore PUBLIC "${LIB_LZ4}")
        endif()
        if(LIB_IMATH)
            target_link_libraries(SpectralCore PUBLIC "${LIB_IMATH}")
            message(STATUS "  Imath linked: ${LIB_IMATH}")
        endif()
        if(LIB_OPENEXR)
            # OpenEXR is optional — disable if it causes DLL conflicts
            # Set SPECTRAL_SKIP_OPENEXR=1 to force disable
            if(NOT SPECTRAL_SKIP_OPENEXR)
                target_link_libraries(SpectralCore PUBLIC "${LIB_OPENEXR}")
                target_include_directories(SpectralCore PUBLIC
                    "${VCPKG_INSTALLED}/include/OpenEXR"
                    "${VCPKG_INSTALLED}/include/Imath")
                if(LIB_OPENEXR_CORE)
                    target_link_libraries(SpectralCore PUBLIC "${LIB_OPENEXR_CORE}")
                endif()
                if(LIB_ILMTHREAD)
                    target_link_libraries(SpectralCore PUBLIC "${LIB_ILMTHREAD}")
                endif()
                message(STATUS "  OpenEXR linked: ${LIB_OPENEXR}")
                target_compile_definitions(SpectralCore PUBLIC SPECTRAL_HAS_OPENEXR=1)
            else()
                message(STATUS "  OpenEXR SKIPPED (SPECTRAL_SKIP_OPENEXR=1)")
            endif()
        endif()
    endif()
    target_compile_definitions(SpectralCore PUBLIC SPECTRAL_HAS_VDB=1)
    # vcpkg Imath is a DLL — need dllimport decoration for symbols
    target_compile_definitions(SpectralCore PUBLIC IMATH_DLL=1 OPENVDB_DLL=1 OPENEXR_DLL=1)
    # OpenVDB 12 uses C++20/23 expansion statements — only VDB files need this
    set_source_files_properties(src/SpectralVDBLoader.cpp
        PROPERTIES COMPILE_FLAGS "/std:c++latest")
endif()

# ---------------------------------------------------------------------------
# HdSpectral.dll — Hydra render delegate (DISABLED — use GL viewer)
# Uncomment to re-enable the Hydra preview viewer.
# ---------------------------------------------------------------------------
# add_library(HdSpectral SHARED
#     src/HdSpectralRendererPlugin.cpp src/HdSpectralRenderDelegate.cpp
#     src/HdSpectralRenderPass.cpp src/HdSpectralMesh.cpp src/HdSpectralRenderBuffer.cpp
# )
# target_include_directories(HdSpectral PRIVATE ${SPECTRAL_INCLUDES})
# target_compile_definitions(HdSpectral PRIVATE ${SPECTRAL_DEFINES})
# target_compile_options(HdSpectral PRIVATE ${SPECTRAL_OPTIONS})
# target_link_libraries(HdSpectral PRIVATE
#     SpectralCore "${LIB_DDIMAGE}" "${LIB_FDKBASE}" "${LIB_NDK}"
#     "${LIB_USDABS}" "${LIB_USDENGINE}" "C:/dev/SpectralRenderer/python311.lib" ${USD_LIBS}
# )
#
# configure_file("${CMAKE_CURRENT_SOURCE_DIR}/plugInfo.json.in"
#     "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral/resources/plugInfo.json" @ONLY)
# file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/plugInfo.json"
#     "{ \"Includes\": [ \"HdSpectral/resources/\" ] }\n")
#
# set_target_properties(HdSpectral PROPERTIES
#     RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
#     RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
#     RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
#     LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
#     PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/HdSpectral"
# )

# ---------------------------------------------------------------------------
# SpectralRender.dll — NDK RenderIop (Nuke plugin)
# ---------------------------------------------------------------------------
add_library(SpectralRender SHARED
    src/SpectralRenderIop.cpp src/SpectralSurfaceOp.cpp src/SpectralVDBRead.cpp src/SpectralVolumeMaterial.cpp
    src/SpectralEnvLight.cpp src/SpectralStudioLight.cpp src/SpectralVolMerge.cpp
)
target_include_directories(SpectralRender PRIVATE ${SPECTRAL_INCLUDES})
target_compile_definitions(SpectralRender PRIVATE ${SPECTRAL_DEFINES})
target_compile_options(SpectralRender PRIVATE ${SPECTRAL_OPTIONS})
target_link_libraries(SpectralRender PRIVATE
    SpectralCore "${LIB_DDIMAGE}" "${LIB_FDKBASE}" "${LIB_TBB}" "${LIB_NDK}"
    "${LIB_USDABS}" "${LIB_USDENGINE}" "C:/dev/SpectralRenderer/python311.lib" ${USD_LIBS}
    opengl32 "${NUKE_ROOT}/glew32.lib"
)

set_target_properties(SpectralRender PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/SpectralRender"
)

# ---------------------------------------------------------------------------
# Status
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  SpectralRenderer configuration:")
message(STATUS "    Nuke:        ${NUKE_ROOT}")
message(STATUS "    Embree:      ${HAS_EMBREE}")
message(STATUS "    OpenSubdiv:  ${HAS_OSD}")
message(STATUS "    OptiX/CUDA:  ${HAS_OPTIX}")
message(STATUS "    OpenVDB:     ${HAS_VDB}")
message(STATUS "")

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS SpectralRender RUNTIME DESTINATION SpectralRender)
