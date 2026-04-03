#pragma once
// HdSpectralApi.h — DLL export/import macros
// Created by Marten Blumen

#ifdef _WIN32
#  ifdef HDSPECTRAL_EXPORTS
#    define HDSPECTRAL_API __declspec(dllexport)
#  else
#    define HDSPECTRAL_API __declspec(dllimport)
#  endif
#else
#  define HDSPECTRAL_API __attribute__((visibility("default")))
#endif
