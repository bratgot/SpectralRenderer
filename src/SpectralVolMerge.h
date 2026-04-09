#pragma once
#include "HdSpectralApi.h"
#include "SpectralVolume.h"
#include <DDImage/SourceGeomOp.h>
#include <DDImage/Knobs.h>
#include <pxr/base/gf/vec3f.h>
#include <vector>
#include <array>
#include <memory>

using namespace DD::Image;

class SpectralVDBRead;
class SpectralEnvLight;
class SpectralStudioLight;

struct VolMergeEntry {
    std::shared_ptr<pxr::SpectralVolume> volume;
    pxr::GfVec3f translate = pxr::GfVec3f(0);
    pxr::GfVec3f rotate    = pxr::GfVec3f(0);
    pxr::GfVec3f scale     = pxr::GfVec3f(1);
    bool hasXform = false;
    SpectralVDBRead* vdbRead = nullptr;  // source node for material lookup
};

class HDSPECTRAL_API SpectralVolMerge : public SourceGeomOp
{
public:
    SpectralVolMerge(Node* node);
    const char* Class() const override { return CLASS; }
    const char* node_help() const override;
    int minimum_inputs() const override { return 1; }
    int maximum_inputs() const override { return 32; }
    bool test_input(int input, Op* op) const override;
    const char* input_label(int input, char* buf) const override;
    void knobs(Knob_Callback f) override;
    void build_handles(DD::Image::ViewerContext* ctx) override;
    void draw_handle(DD::Image::ViewerContext* ctx) override;
    void append(DD::Image::Hash& hash) override;
    void _validate(bool forReal) override;

    std::vector<VolMergeEntry> GetVolumes(int frame, int maxRes = 512);

    // Light accessors for SpectralRender
    SpectralEnvLight*    GetEnvLight()    const { return _envLight; }
    SpectralStudioLight* GetStudioLight() const { return _studioLight; }

    static const char* const CLASS;
    static const GeomOp::Description description;

    struct PreviewPoint { float x, y, z, r, g, b; };

    struct VolumeDisplayInfo {
        std::string filePath;
        std::string densityField;
        std::string tempField;
        std::string flameField;
        float tx, ty, tz;   // translate
    };

private:
    class Engine;
    int _volCount = 0;
    int _curFrame = -999;
    int _displayMode = 1;  // 0=points, 1=volume
    bool _validating = false;
    SpectralEnvLight*    _envLight = nullptr;
    SpectralStudioLight* _studioLight = nullptr;
};
