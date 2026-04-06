// SpectralVolMerge — merges multiple VDBRead + light inputs
// Created by Marten Blumen

#include "SpectralVolMerge.h"
#include "SpectralVDBRead.h"
#include "SpectralEnvLight.h"
#include "SpectralStudioLight.h"
#include <DDImage/ViewerContext.h>
#include "usg/geom/PointsPrim.h"

using namespace DD::Image;
using namespace fdk;
using namespace usg;

const char* const SpectralVolMerge::CLASS = "SpectralVolMerge";
static Op* buildVolMerge(Node* node) { return new SpectralVolMerge(node); }
const GeomOp::Description SpectralVolMerge::description(CLASS, buildVolMerge);

class SpectralVolMerge::Engine : public SourceEngine
{
public:
    Engine(GeomOpNode* parent) : SourceEngine(parent) {}
    void createPrims(GeomSceneContext& context, const Path& path) override
    {
        if (!context.doGeometryProcessing()) return;
        LayerRef layer = editLayer();
        if (!layer) return;
        PointsPrim prim = PointsPrim::defineInLayer(layer, path);
        _transformSubEngine.apply(context, prim);
        for (const TimeValue& time : context.processTimes()) {
            Vec3fArray pts(1, Vec3f(0, 0, 0));
            FloatArray wids(1, 0.001f);
            prim.setPoints(pts, time);
            prim.setWidths(wids, time);
            prim.setBoundsAttr(pts, time);
        }
        prim.setCustomData("spectralIsVolMerge", Value(true));
    }
};

SpectralVolMerge::SpectralVolMerge(Node* node)
    : SourceGeomOp(node, BuildEngine<Engine>()) {}

const char* SpectralVolMerge::node_help() const
{
    return "SpectralVolMerge -- Volume Scene Merge\n\n"
           "Merges multiple VDBRead and light nodes for multi-volume rendering.\n"
           "Each input accepts VDBRead, EnvLight, or StudioLight,\n"
           "optionally through GeoTransform.\n\n"
           "CONNECTION\n"
           "  VDBRead1 -> [GeoTransform] -> VolMerge input 0\n"
           "  VDBRead2 -> [GeoTransform] -> VolMerge input 1\n"
           "  EnvLight -> VolMerge input 2\n"
           "  VolMerge -> GeoScene -> SpectralRender\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

bool SpectralVolMerge::test_input(int input, Op* op) const
{
    return dynamic_cast<GeomOp*>(op) != nullptr;
}

const char* SpectralVolMerge::input_label(int input, char* buf) const
{
    sprintf(buf, "vol%d", input);
    return buf;
}

void SpectralVolMerge::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>Volume Scene</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Merges multiple VDBRead nodes for multi-volume rendering.<br>"
        "Also accepts SpectralEnvLight and SpectralStudioLight inputs.<br>"
        "<font color='#68a'>Multi-volume compositing currently uses CPU.</font><br>"
        "Single volume renders on GPU as normal."
        "</font>"
    );
    Divider(f, "");
    Int_knob(f, &_volCount, "vol_count", "volumes found");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralVolMerge - SpectralRenderer for Nuke 17"
                 "</font>");
}

std::vector<VolMergeEntry> SpectralVolMerge::GetVolumes(int frame, int maxRes)
{
    std::vector<VolMergeEntry> result;
    _envLight = nullptr;
    _studioLight = nullptr;

    for (int inp = 0; inp < inputs(); ++inp) {
        Op* op = input(inp);
        if (!op || op->node_disabled()) continue;
        op->validate(true);

        // Walk up to 4 levels to find VDBRead or light nodes
        double tx=0,ty=0,tz=0, rx=0,ry=0,rz=0, scx=1,scy=1,scz=1;
        bool foundXform = false;
        SpectralVDBRead* vdbRead = nullptr;

        Op* cur = op;
        for (int depth = 0; depth < 4 && cur; ++depth) {
            if (cur->node_disabled()) break;
            fprintf(stderr, "VolMerge: inp%d depth%d class=%s\n", inp, depth, cur->Class());

            if (strcmp(cur->Class(), "SpectralVDBRead") == 0) {
                vdbRead = static_cast<SpectralVDBRead*>(cur);
                break;
            }
            if (!_envLight && strcmp(cur->Class(), "SpectralEnvLight") == 0) {
                _envLight = static_cast<SpectralEnvLight*>(cur);
                break;
            }
            if (!_studioLight && strcmp(cur->Class(), "SpectralStudioLight") == 0) {
                _studioLight = static_cast<SpectralStudioLight*>(cur);
                break;
            }

            if (strcmp(cur->Class(), "GeoTransform") == 0 ||
                strcmp(cur->Class(), "TransformGeo") == 0) {
                foundXform = true;
                // Debug: dump knob names to find translate/rotate/scale
                fprintf(stderr, "VolMerge: %s knobs:", cur->Class());
                for (int ki = 0; ; ++ki) {
                    Knob* kn = cur->knob(ki);
                    if (!kn) break;
                    if (!kn->name().empty())
                        fprintf(stderr, " %s", kn->name().c_str());
                }
                fprintf(stderr, "\n");
                // Try known Nuke translate knob names
                const char* transNames[] = {"translate","xform_translate","trans","position",nullptr};
                for (const char** tn = transNames; *tn; ++tn) {
                    if (Knob* k = cur->knob(*tn)) {
                        tx=k->get_value(0); ty=k->get_value(1); tz=k->get_value(2);
                        fprintf(stderr, "VolMerge: translate knob '%s' = (%.1f,%.1f,%.1f)\n", *tn, tx,ty,tz);
                        break;
                    }
                }
                const char* rotNames[] = {"rotate","xform_rotate","rot",nullptr};
                for (const char** rn = rotNames; *rn; ++rn) {
                    if (Knob* k = cur->knob(*rn)) {
                        rx=k->get_value(0); ry=k->get_value(1); rz=k->get_value(2);
                        break;
                    }
                }
                const char* scaleNames[] = {"scaling","xform_scale","scale",nullptr};
                for (const char** sn = scaleNames; *sn; ++sn) {
                    if (Knob* k = cur->knob(*sn)) {
                        scx=k->get_value(0); scy=k->get_value(1); scz=k->get_value(2);
                        break;
                    }
                }
                // uniform_scale multiplies all axes
                if (Knob* us = cur->knob("uniform_scale")) {
                    double u = us->get_value(0);
                    scx *= u; scy *= u; scz *= u;
                }
                fprintf(stderr, "VolMerge: xform T=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) S=(%.2f,%.2f,%.2f)\n",
                        tx,ty,tz, rx,ry,rz, scx,scy,scz);
            }

            if (cur->inputs() > 0 && cur->input(0)) {
                cur = cur->input(0);
                cur->validate(true);
            } else break;
        }

        if (vdbRead && !vdbRead->node_disabled()) {
            auto vol = vdbRead->GetVolumeAtFrame(frame, maxRes);
            if (vol && vol->IsValid()) {
                VolMergeEntry e;
                e.volume = vol;
                if (foundXform) {
                    e.hasXform = true;
                    e.translate = pxr::GfVec3f(float(tx),float(ty),float(tz));
                    e.rotate = pxr::GfVec3f(float(rx),float(ry),float(rz));
                    e.scale = pxr::GfVec3f(float(scx),float(scy),float(scz));
                    vol->translate = e.translate;
                    vol->rotate = e.rotate;
                    vol->scale = e.scale;
                    vol->BuildTransform();
                }
                result.push_back(e);
                fprintf(stderr, "VolMerge: inp%d vol %dx%dx%d bbox(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)%s\n",
                        inp, vol->resX, vol->resY, vol->resZ,
                        vol->GetBboxMin()[0],vol->GetBboxMin()[1],vol->GetBboxMin()[2],
                        vol->GetBboxMax()[0],vol->GetBboxMax()[1],vol->GetBboxMax()[2],
                        foundXform?" [xform]":"");
            }
        }
    }

    _volCount = (int)result.size();
    if (Knob* k = knob("vol_count")) k->set_value(_volCount);
    return result;
}
