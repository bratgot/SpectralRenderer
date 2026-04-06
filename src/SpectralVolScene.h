#pragma once
#include "HdSpectralApi.h"
#include "SpectralVolume.h"
#include <DDImage/SourceGeomOp.h>
#include <DDImage/Knobs.h>
#include <pxr/base/gf/vec3f.h>
#include <vector>
#include <memory>

using namespace DD::Image;

class SpectralVDBRead;
class SpectralEnvLight;
class SpectralStudioLight;

struct VolSceneEntry {
    std::shared_ptr<pxr::SpectralVolume> volume;
    pxr::GfVec3f translate = pxr::GfVec3f(0);
    pxr::GfVec3f rotate    = pxr::GfVec3f(0);
    pxr::GfVec3f scale     = pxr::GfVec3f(1);
    bool hasXform = false;
};

class HDSPECTRAL_API SpectralVolScene : public SourceGeomOp
{
public:
    SpectralVolScene(Node* node);
    const char* Class() const override { return CLASS; }
    const char* node_help() const override;
    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 32; }
    bool test_input(int input, Op* op) const override;
    const char* input_label(int input, char* buf) const override;
    void knobs(Knob_Callback f) override;

    std::vector<VolSceneEntry> GetVolumes(int frame, int maxRes = 512);

    // Light accessors for SpectralRender
    SpectralEnvLight*    GetEnvLight()    const { return _envLight; }
    SpectralStudioLight* GetStudioLight() const { return _studioLight; }

    static const char* const CLASS;
    static const GeomOp::Description description;

private:
    class Engine;
    int _volCount = 0;
    SpectralEnvLight*    _envLight = nullptr;
    SpectralStudioLight* _studioLight = nullptr;
};
