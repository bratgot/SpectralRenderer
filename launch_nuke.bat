@echo off
set NUKE_INSTALL=C:\Program Files\Nuke17.0v1
set HDSPECTRAL_BUILD=C:\dev\SpectralRenderer\HdSpectral_Phase1\HdSpectral\build

set PXR_PLUGINPATH_NAME=%HDSPECTRAL_BUILD%
set NUKE_PATH=%HDSPECTRAL_BUILD%\SpectralRender
set PATH=%HDSPECTRAL_BUILD%\HdSpectral;C:\dev\SpectralRenderer;%NUKE_INSTALL%;%PATH%
set TF_DEBUG=HD_RENDERER_PLUGIN
set PATH=C:\dev\embree-4\bin;%PATH%

"%NUKE_INSTALL%\Nuke17.0.exe" %*