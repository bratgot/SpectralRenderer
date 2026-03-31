#pragma once

// ---------------------------------------------------------------------------
// HDSPECTRAL_API  — DLL export/import macro
//
// When building the DLL:   define HDSPECTRAL_EXPORTS  (set in CMakeLists)
// When consuming the DLL:  include this header — symbols are __declspec(dllimport)
// On non-Windows:          expands to nothing (GCC/Clang visibility handled
//                          separately via -fvisibility=hidden if needed)
// ---------------------------------------------------------------------------

#if defined(_WIN32) || defined(_WIN64)
  #ifdef HDSPECTRAL_EXPORTS
    #define HDSPECTRAL_API __declspec(dllexport)
  #else
    #define HDSPECTRAL_API __declspec(dllimport)
  #endif
#else
  #define HDSPECTRAL_API __attribute__((visibility("default")))
#endif
