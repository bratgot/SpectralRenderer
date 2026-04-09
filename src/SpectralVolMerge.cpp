// SpectralVolMerge — merges multiple VDBRead + light inputs
// Created by Marten Blumen

#include "SpectralVolMerge.h"
#include "SpectralVDBRead.h"
#include "SpectralEnvLight.h"
#include "SpectralStudioLight.h"
#include <DDImage/ViewerContext.h>
#include <DDImage/gl.h>
#include <cmath>
#include "usg/geom/PointsPrim.h"

using namespace DD::Image;
using namespace fdk;
using namespace usg;

const char* const SpectralVolMerge::CLASS = "SpectralVolMerge";
static Op* buildVolMerge(Node* node) { return new SpectralVolMerge(node); }
const GeomOp::Description SpectralVolMerge::description(CLASS, buildVolMerge);

// Static shared preview data — survives Op recreation
static std::vector<SpectralVolMerge::PreviewPoint> s_previewPoints;
static int s_previewFrame = -999;

// Static pointer for Engine→VolMerge access (set in constructor)
static SpectralVolMerge* s_lastVolMerge = nullptr;

class SpectralVolMerge::Engine : public SourceEngine
{
public:
    Engine(GeomOpNode* parent) : SourceEngine(parent) {}
    void createPrims(GeomSceneContext& context, const Path& path) override
    {
        if (!context.doGeometryProcessing()) return;
        LayerRef layer = editLayer();
        if (!layer) return;

        SpectralVolMerge* volMerge = s_lastVolMerge;
        if (!volMerge) {
            fprintf(stderr, "VolMerge::createPrims: s_lastVolMerge=NULL\n");
            return;
        }

        // Read frame knob — creates Engine dependency for per-frame invalidation
        TimeValue firstTime = fdk::defaultTimeValue();
        for (const TimeValue& t : context.processTimes()) { firstTime = t; break; }
        int frame = knob("vm_cur_frame").get<int>(firstTime);

        auto& cached = s_previewPoints;

        // First-time fallback: if static cache empty, build directly
        if (cached.empty() && volMerge) {
            for (int i = 0; i < volMerge->inputs(); ++i) {
                Op* op = volMerge->input(i);
                if (op) op->validate(true);
            }
            auto entries = volMerge->GetVolumes(int(firstTime), 32);
            for (size_t vi = 0; vi < entries.size(); ++vi) {
                auto& vol = entries[vi].volume;
                if (!vol || !vol->IsValid()) continue;
                pxr::GfVec3f bMin = vol->bboxMin, bSz = vol->bboxMax - vol->bboxMin;
                int total = vol->resX * vol->resY * vol->resZ;
                int maxPts = std::max(200, 5000 / std::max(1, (int)entries.size()));
                int step = std::max(1, (int)std::cbrt(double(total) / maxPts));
                float hue = float(vi) * 0.25f;
                float rxr=vol->rotate[0]*3.14159265f/180.f, ryr=vol->rotate[1]*3.14159265f/180.f, rzr=vol->rotate[2]*3.14159265f/180.f;
                float cx=std::cos(rxr),sx=std::sin(rxr),cy=std::cos(ryr),sy=std::sin(ryr),cz=std::cos(rzr),sz=std::sin(rzr);
                float rm[9]={cy*cz+sy*sx*sz,-cy*sz+sy*sx*cz,sy*cx, cx*sz,cx*cz,-sx, -sy*cz+cy*sx*sz,sy*sz+cy*sx*cz,cy*cx};
                auto xf=[&](pxr::GfVec3f p)->pxr::GfVec3f{
                    if(!vol->hasTransform)return p;
                    pxr::GfVec3f s(p[0]*vol->scale[0],p[1]*vol->scale[1],p[2]*vol->scale[2]);
                    return vol->translate+pxr::GfVec3f(rm[0]*s[0]+rm[1]*s[1]+rm[2]*s[2],rm[3]*s[0]+rm[4]*s[1]+rm[5]*s[2],rm[6]*s[0]+rm[7]*s[1]+rm[8]*s[2]);};
                // Bbox edges
                pxr::GfVec3f corners[8];
                for(int c=0;c<8;++c) corners[c]=xf(pxr::GfVec3f((c&1)?vol->bboxMax[0]:vol->bboxMin[0],(c&2)?vol->bboxMax[1]:vol->bboxMin[1],(c&4)?vol->bboxMax[2]:vol->bboxMin[2]));
                static const int edges[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
                for(int e=0;e<12;++e)for(int s=0;s<=8;++s){float t=float(s)/8.f;auto p=corners[edges[e][0]]*(1-t)+corners[edges[e][1]]*t;
                    cached.push_back({p[0],p[1],p[2],0.3f+hue,0.7f,0.4f});}
                for(int iz=0;iz<vol->resZ;iz+=step)for(int iy=0;iy<vol->resY;iy+=step)for(int ix=0;ix<vol->resX;ix+=step){
                    float u=float(ix)/vol->resX,uv=float(iy)/vol->resY,w=float(iz)/vol->resZ;
                    float d=vol->SampleDensity(u,uv,w); if(d<0.01f)continue; float t=std::min(d*3.f,1.f);
                    auto wp=xf(pxr::GfVec3f(bMin[0]+u*bSz[0],bMin[1]+uv*bSz[1],bMin[2]+w*bSz[2]));
                    cached.push_back({wp[0],wp[1],wp[2],t*(0.8f+hue*0.2f),t*0.85f,t*(0.6f-hue*0.1f)});}
            }
        }

        PointsPrim prim = PointsPrim::defineInLayer(layer, path);
        _transformSubEngine.apply(context, prim);

        for (const TimeValue& time : context.processTimes()) {
            std::vector<Vec3f> allPos;
            std::vector<Vec3f> allCol;
            std::vector<float> allWid;

            for (auto& pt : cached) {
                allPos.push_back(Vec3f(pt.x, pt.y, pt.z));
                allCol.push_back(Vec3f(pt.r, pt.g, pt.b));
                allWid.push_back(3.f);
            }

            if (allPos.empty()) {
                allPos.push_back(Vec3f(0,0,0));
                allCol.push_back(Vec3f(0,0,0));
                allWid.push_back(0.001f);
            }

            Vec3fArray pts(allPos.begin(), allPos.end());
            FloatArray wids(allWid.begin(), allWid.end());
            Vec3fArray cols(allCol.begin(), allCol.end());
            prim.setPoints(pts, time);
            prim.setWidths(wids, time);
            prim.setDisplayColor(cols, time);
            prim.setBoundsAttr(pts, time);
        }
        prim.setCustomData("spectralIsVolMerge", Value(true));
    }
};

SpectralVolMerge::SpectralVolMerge(Node* node)
    : SourceGeomOp(node, BuildEngine<Engine>())
{
    s_lastVolMerge = this;
}

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
        "Also accepts SpectralEnvLight and SpectralStudioLight inputs."
        "</font>"
    );
    Divider(f, "");
    Int_knob(f, &_volCount, "vol_count", "volumes found");
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION);
    Int_knob(f, &_curFrame, "vm_cur_frame", "");
    SetFlags(f, Knob::INVISIBLE);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralVolMerge - SpectralRenderer for Nuke 17"
                 "</font>");
}

void SpectralVolMerge::append(DD::Image::Hash& hash)
{
    hash.append(outputContext().frame());
    for (int i = 0; i < inputs(); ++i) {
        Op* op = input(i);
        if (op) hash.append(op->hash());
    }
}

void SpectralVolMerge::_validate(bool forReal)
{
    int frame = int(outputContext().frame());

    // Guard against re-entrant _validate (caused by knob changes or invalidate)
    if (_validating) {
        SourceGeomOp::_validate(forReal);
        return;
    }
    _validating = true;

    // Build into temp — don't clear _previewPoints until new data ready
    // (createPrims may run at any time and reads _previewPoints)
    std::vector<PreviewPoint> newPoints;
    auto entries = GetVolumes(frame, 32);
    for (size_t vi = 0; vi < entries.size(); ++vi) {
        auto& vol = entries[vi].volume;
        if (!vol || !vol->IsValid()) continue;

        pxr::GfVec3f bMin = vol->bboxMin, bSz = vol->bboxMax - vol->bboxMin;
        int total = vol->resX * vol->resY * vol->resZ;
        int maxPts = std::max(200, 5000 / std::max(1, (int)entries.size()));
        int step = std::max(1, (int)std::cbrt(double(total) / maxPts));
        float hue = float(vi) * 0.25f;

        // ZXY rotation matrix (Nuke GeoTransform default)
        float rxr = vol->rotate[0]*3.14159265f/180.f;
        float ryr = vol->rotate[1]*3.14159265f/180.f;
        float rzr = vol->rotate[2]*3.14159265f/180.f;
        float cx=std::cos(rxr), sx=std::sin(rxr);
        float cy=std::cos(ryr), sy=std::sin(ryr);
        float cz=std::cos(rzr), sz=std::sin(rzr);
        float rm[9];
        rm[0]=cy*cz+sy*sx*sz;  rm[1]=-cy*sz+sy*sx*cz; rm[2]=sy*cx;
        rm[3]=cx*sz;            rm[4]=cx*cz;            rm[5]=-sx;
        rm[6]=-sy*cz+cy*sx*sz; rm[7]=sy*sz+cy*sx*cz;   rm[8]=cy*cx;

        auto xformPt = [&](pxr::GfVec3f lp) -> pxr::GfVec3f {
            if (!vol->hasTransform) return lp;
            pxr::GfVec3f sc(lp[0]*vol->scale[0], lp[1]*vol->scale[1], lp[2]*vol->scale[2]);
            return vol->translate + pxr::GfVec3f(
                rm[0]*sc[0]+rm[1]*sc[1]+rm[2]*sc[2],
                rm[3]*sc[0]+rm[4]*sc[1]+rm[5]*sc[2],
                rm[6]*sc[0]+rm[7]*sc[1]+rm[8]*sc[2]);
        };

        // Bbox edges
        pxr::GfVec3f corners[8];
        for (int c = 0; c < 8; ++c)
            corners[c] = xformPt(pxr::GfVec3f(
                (c&1)?vol->bboxMax[0]:vol->bboxMin[0],
                (c&2)?vol->bboxMax[1]:vol->bboxMin[1],
                (c&4)?vol->bboxMax[2]:vol->bboxMin[2]));
        static const int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};
        for (int e = 0; e < 12; ++e)
            for (int s = 0; s <= 8; ++s) {
                float t = float(s)/8.f;
                pxr::GfVec3f p = corners[edges[e][0]]*(1-t) + corners[edges[e][1]]*t;
                newPoints.push_back({p[0],p[1],p[2], 0.3f+hue,0.7f,0.4f});
            }

        // Density points
        for (int iz = 0; iz < vol->resZ; iz += step)
            for (int iy = 0; iy < vol->resY; iy += step)
                for (int ix = 0; ix < vol->resX; ix += step) {
                    float u=float(ix)/vol->resX, uv=float(iy)/vol->resY, w=float(iz)/vol->resZ;
                    float d = vol->SampleDensity(u,uv,w);
                    if (d < 0.01f) continue;
                    float t = std::min(d*3.f, 1.f);
                    pxr::GfVec3f wp = xformPt(pxr::GfVec3f(
                        bMin[0]+u*bSz[0], bMin[1]+uv*bSz[1], bMin[2]+w*bSz[2]));
                    newPoints.push_back({wp[0],wp[1],wp[2],
                        t*(0.8f+hue*0.2f), t*0.85f, t*(0.6f-hue*0.1f)});
                }
    }
    // Only swap if we got data (protect against re-entrant empty results)
    if (!newPoints.empty()) {
        s_previewPoints.swap(newPoints);
    }
    _curFrame = frame;

    fprintf(stderr, "VolMerge::_validate frame=%d entries=%d pts=%d\n",
            frame, (int)entries.size(), (int)s_previewPoints.size());

    if (Knob* k = knob("vm_cur_frame")) {
        k->set_value(frame);
        k->changed();
    }

    SourceGeomOp::_validate(forReal);
    _validating = false;
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

            if (strcmp(cur->Class(), "SpectralVDBRead") == 0) {
                vdbRead = static_cast<SpectralVDBRead*>(cur);
                break;
            }
            // Chained VolMerge: recursively collect its volumes
            if (strcmp(cur->Class(), "SpectralVolMerge") == 0) {
                SpectralVolMerge* childMerge = static_cast<SpectralVolMerge*>(cur);
                auto childVols = childMerge->GetVolumes(frame, maxRes);
                for (auto& cv : childVols) result.push_back(cv);
                if (!_envLight) _envLight = childMerge->GetEnvLight();
                if (!_studioLight) _studioLight = childMerge->GetStudioLight();
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
                const char* transNames[] = {"translate","xform_translate","trans","position",nullptr};
                for (const char** tn = transNames; *tn; ++tn) {
                    if (Knob* k = cur->knob(*tn)) {
                        tx=k->get_value(0); ty=k->get_value(1); tz=k->get_value(2);
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
                if (Knob* us = cur->knob("uniform_scale")) {
                    double u = us->get_value(0);
                    scx *= u; scy *= u; scz *= u;
                }
            }

            if (cur->inputs() > 0 && cur->input(0)) {
                cur = cur->input(0);
                cur->validate(true);
            } else break;
        }

        if (vdbRead && !vdbRead->node_disabled()) {
            vdbRead->validate(true);
            int nodeRes = vdbRead->GetMaxRes();
            int effectiveRes = std::min(maxRes, nodeRes);
            auto vol = vdbRead->GetVolumeAtFrame(frame, effectiveRes);
            if (vol && vol->IsValid()) {
                VolMergeEntry e;
                e.volume = vol;
                e.vdbRead = vdbRead;
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
            }
        }
    }

    _volCount = (int)result.size();
    if (Knob* k = knob("vol_count")) k->set_value(_volCount);
    return result;
}

void SpectralVolMerge::build_handles(DD::Image::ViewerContext* ctx)
{
    if (ctx->transform_mode() == VIEWER_2D) return;
    if (node_disabled()) return;
    for (int i = 0; i < inputs(); ++i) {
        Op* op = input(i);
        if (op && !op->node_disabled()) op->build_handles(ctx);
    }
}

void SpectralVolMerge::draw_handle(DD::Image::ViewerContext* ctx)
{
    // GL fallback — only used if Hydra doesn't render createPrims
}
