// SpectralVDBRead — SourceGeomOp VDB reader for Nuke 17
// Created by Marten Blumen

#include "SpectralVDBRead.h"
#include "usg/geom/PointsPrim.h"
#include "ndk/geo/utils/MeshUtils.h"

#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace DD::Image;
using namespace fdk;
using namespace usg;

const char* const SpectralVDBRead::CLASS = "SpectralVDBRead";

const char* const SpectralVDBRead::kGridMenu[] = {
    "(none)", "density", "density_1", "smoke", "soot",
    "temperature", "temp", "heat",
    "flame", "fire", "fuel", "burn",
    "vel", "velocity", "v", "motion",
    "Cd", "color", "colour", "albedo",
    nullptr
};

const GeomOp::Description SpectralVDBRead::description(CLASS, SpectralVDBRead::Build);

// ---------------------------------------------------------------------------
std::string SpectralVDBRead::Engine::_resolveFrame(const std::string& pattern, int frame)
{
    std::string p = pattern;
    size_t h = p.find('#');
    if (h != std::string::npos) {
        size_t he = h;
        while (he < p.size() && p[he] == '#') ++he;
        char buf[64];
        std::snprintf(buf, 64, "%0*d", int(he - h), frame);
        p.replace(h, he - h, buf);
    }
    return p;
}

// ---------------------------------------------------------------------------
// Engine — createPrims
// ---------------------------------------------------------------------------
void SpectralVDBRead::Engine::createPrims(GeomSceneContext& context,
                                           const Path& path)
{
    if (!context.doGeometryProcessing()) return;
    LayerRef defineLayer = editLayer();
    if (!defineLayer) return;

    TimeValue firstTime = fdk::defaultTimeValue();
    for (const TimeValue& t : context.processTimes()) { firstTime = t; break; }

    // --- Read knobs (constant across time) ---
    std::string filePath    = knob("file").get<std::string>(firstTime);
    std::string densityName = knob("density_override").get<std::string>(firstTime);
    bool autoSeq     = knob("auto_sequence").get<bool>(firstTime);
    int  firstFrame  = knob("first_frame").get<int>(firstTime);
    int  lastFrame   = knob("last_frame").get<int>(firstTime);
    int  frameOffset = knob("frame_offset").get<int>(firstTime);
    int  previewRes  = knob("preview_res").get<int>(firstTime);
    int  maxPoints   = knob("max_points").get<int>(firstTime);
    float pointSize  = knob("point_size").get<float>(firstTime);
    float threshold  = knob("density_threshold").get<float>(firstTime);
    bool  lit        = knob("lit").get<bool>(firstTime);

    previewRes = std::max(4, std::min(previewRes, 256));
    maxPoints  = std::max(10, maxPoints);
    pointSize  = std::max(0.01f, pointSize);
    float densityCap = 50.0f; // cull outlier points with extreme density

    // Define single PointsPrim (density points + bbox edges all in one)
    PointsPrim prim = PointsPrim::defineInLayer(defineLayer, path);
    _transformSubEngine.apply(context, prim);

    // ---------------------------------------------------------------
    // Process each time sample
    // ---------------------------------------------------------------
    for (const TimeValue& time : context.processTimes()) {

        // --- Resolve file path at THIS time ---
        std::string resolvedPath = filePath;
        if (autoSeq && filePath.find('#') != std::string::npos) {
            int frame = int(time) + frameOffset;
            frame = std::max(firstFrame, std::min(frame, lastFrame));
            resolvedPath = _resolveFrame(filePath, frame);
        }

        // --- Load VDB at this frame ---
        std::vector<Vec3f> samplePos;
        std::vector<Vec3f> sampleCol;
        std::vector<Vec3f> sampleNrm;
        pxr::GfVec3f bboxMin(-0.5f), bboxMax(0.5f);
        bool hasVDB = false;

#ifdef SPECTRAL_HAS_VDB
        if (!resolvedPath.empty()) {
            auto vol = pxr::SpectralVDBLoader::Load(
                resolvedPath.c_str(),
                densityName.empty() ? nullptr : densityName.c_str(),
                nullptr, previewRes);

            if (vol && vol->IsValid()) {
                hasVDB = true;
                bboxMin = vol->bboxMin;
                bboxMax = vol->bboxMax;
                pxr::GfVec3f sz = bboxMax - bboxMin;

                // Gradient step sizes (in normalised coords)
                float epsX = 1.0f / float(std::max(1, vol->resX));
                float epsY = 1.0f / float(std::max(1, vol->resY));
                float epsZ = 1.0f / float(std::max(1, vol->resZ));

                // Light directions
                float keyX=0.4f, keyY=0.8f, keyZ=0.3f;
                float keyLen = std::sqrt(keyX*keyX + keyY*keyY + keyZ*keyZ);
                keyX/=keyLen; keyY/=keyLen; keyZ/=keyLen;

                bool hasTemp  = !vol->temperature.empty();
                bool hasFlame = !vol->flame.empty();

                int count = 0, total = vol->resX * vol->resY * vol->resZ;
                int stride = 1;
                if (total > maxPoints * 2)
                    stride = std::max(1, (int)std::cbrt((double)total / maxPoints));

                for (int iz = 0; iz < vol->resZ; iz += stride) {
                    for (int iy = 0; iy < vol->resY; iy += stride) {
                        for (int ix = 0; ix < vol->resX; ix += stride) {
                            if (count >= maxPoints) goto done;
                            float u = (float(ix) + 0.5f) / float(vol->resX);
                            float v = (float(iy) + 0.5f) / float(vol->resY);
                            float w = (float(iz) + 0.5f) / float(vol->resZ);
                            float d = vol->SampleDensity(u, v, w);

                            if (d > threshold && d < densityCap) {
                                samplePos.push_back(Vec3f(
                                    bboxMin[0]+u*sz[0],
                                    bboxMin[1]+v*sz[1],
                                    bboxMin[2]+w*sz[2]));

                                // --- Gradient normal from density ---
                                float gx = vol->SampleDensity(std::min(u+epsX,1.f),v,w)
                                         - vol->SampleDensity(std::max(u-epsX,0.f),v,w);
                                float gy = vol->SampleDensity(u,std::min(v+epsY,1.f),w)
                                         - vol->SampleDensity(u,std::max(v-epsY,0.f),w);
                                float gz = vol->SampleDensity(u,v,std::min(w+epsZ,1.f))
                                         - vol->SampleDensity(u,v,std::max(w-epsZ,0.f));
                                float gl = std::sqrt(gx*gx + gy*gy + gz*gz);
                                float nx, ny, nz;
                                if (gl > 1e-6f) {
                                    nx = -gx/gl; ny = -gy/gl; nz = -gz/gl;
                                } else {
                                    nx = 0; ny = 1; nz = 0;
                                }

                                // --- Key light (warm, top-right-front) ---
                                float NdotL = nx*keyX + ny*keyY + nz*keyZ;
                                float wrapD = (NdotL + 0.3f) / 1.3f;
                                wrapD = std::max(0.0f, wrapD);

                                // --- Fill light (cool, opposite) ---
                                float fillD = std::max(0.0f, -(NdotL));
                                fillD *= 0.15f;

                                // --- Density depth ---
                                float densVis = std::min(d * 3.0f, 1.0f);
                                densVis = std::sqrt(densVis); // gamma for softer falloff

                                // --- Base shade ---
                                float ambient = 0.12f;
                                float shade = ambient + 0.65f * wrapD + fillD;
                                shade *= densVis;
                                shade = std::min(shade, 1.0f);

                                // --- Color by field type ---
                                float cr, cg, cb;

                                if (hasFlame) {
                                    float fl = vol->SampleFlame(u, v, w);
                                    if (fl > 0.01f) {
                                        // Fire ramp: black → dark red → orange → yellow → white
                                        float t = std::min(fl * 3.0f, 1.0f);
                                        cr = std::min(1.0f, t * 3.0f);
                                        cg = std::min(1.0f, std::max(0.0f, (t - 0.3f) * 2.5f));
                                        cb = std::min(1.0f, std::max(0.0f, (t - 0.7f) * 3.3f));
                                        cr *= shade * 1.5f;
                                        cg *= shade * 1.2f;
                                        cb *= shade;
                                    } else {
                                        // Smoke (grey with slight blue)
                                        cr = shade * 0.85f;
                                        cg = shade * 0.88f;
                                        cb = shade * 0.95f;
                                    }
                                }
                                else if (hasTemp) {
                                    float temp = vol->SampleTemperature(u, v, w);
                                    float t = (temp - 400.0f) / 5000.0f;
                                    t = std::max(0.0f, std::min(1.0f, t));
                                    if (t > 0.01f) {
                                        // Blackbody ramp
                                        cr = std::min(1.0f, t * 3.0f) * shade * 1.4f;
                                        cg = std::min(1.0f, std::max(0.0f, (t-0.25f) * 2.8f)) * shade * 1.1f;
                                        cb = std::min(1.0f, std::max(0.0f, (t-0.55f) * 3.0f)) * shade;
                                    } else {
                                        cr = shade * 0.85f;
                                        cg = shade * 0.88f;
                                        cb = shade * 0.95f;
                                    }
                                }
                                else {
                                    // Density only — warm key + cool fill
                                    cr = shade * (0.92f + 0.08f * wrapD);
                                    cg = shade * (0.90f + 0.05f * wrapD);
                                    cb = shade * (0.85f + 0.15f * fillD / std::max(0.01f, shade));
                                }

                                cr = std::max(0.0f, std::min(1.0f, cr));
                                cg = std::max(0.0f, std::min(1.0f, cg));
                                cb = std::max(0.0f, std::min(1.0f, cb));

                                sampleCol.push_back(Vec3f(cr, cg, cb));
                                sampleNrm.push_back(Vec3f(nx, ny, nz));
                                count++;
                            }
                        }
                    }
                }
                done:;

                fprintf(stderr, "SpectralVDBRead: frame=%.0f %zu pts, res=%d%s%s\n",
                        double(time), samplePos.size(), previewRes,
                        hasTemp ? " +temp" : "", hasFlame ? " +flame" : "");
            }
        }
#endif

        // --- Build combined point arrays ---
        size_t nDensity = samplePos.size();
        size_t nTotal   = nDensity;
        if (nTotal == 0) nTotal = 1;

        Vec3fArray pts, cols, nrms;
        FloatArray wids;
        pts.resize(nTotal); cols.resize(nTotal); nrms.resize(nTotal); wids.resize(nTotal);

        // Density points
        for (size_t i = 0; i < nDensity; ++i) {
            pts[i]  = samplePos[i];
            nrms[i] = sampleNrm[i];
            wids[i] = pointSize;
            cols[i] = lit ? Vec3f(0.85f, 0.85f, 0.85f) : sampleCol[i];
        }

        // Placeholder if totally empty
        if (nDensity == 0) {
            pts[0]  = Vec3f(0, 0, 0);
            cols[0] = Vec3f(0, 0, 0);
            nrms[0] = Vec3f(0, 1, 0);
            wids[0] = 0.001f;
        }

        // --- Set on prim at this time ---
        prim.setPoints(pts, time);
        prim.setBoundsAttr(pts, time);
        prim.setWidths(wids, time);
        prim.setDisplayColor(cols, time);

        if (lit) {
            prim.setNormals(nrms, time);
        } else {
            usg::Attribute normAttr = prim.getNormalsAttr();
            if (normAttr) normAttr.block();
        }

    } // end processTimes loop

    prim.setCustomData("spectralIsVolume", Value(true));
    assignMaterial(context, {path});
}

// ---------------------------------------------------------------------------
// Op
// ---------------------------------------------------------------------------
SpectralVDBRead::SpectralVDBRead(Node* node)
    : SourceGeomOp(node, BuildEngine<Engine>())
{
    fprintf(stderr, "SpectralVDBRead: build %s %s\n", __DATE__, __TIME__);
}

const char* SpectralVDBRead::node_help() const
{
    return "SpectralVDBRead — OpenVDB Volume Reader\n\n"
           "Reads OpenVDB (.vdb) files for volume rendering.\n"
           "Displays density point cloud in the 3D viewer.\n\n"
           "SpectralVDBRead -> GeoScene -> SpectralRender\n\n"
           "Created by Marten Blumen";
}

const char* SpectralVDBRead::_GetDensityName() const
{
    if (_densityOverride && strlen(_densityOverride) > 0) return _densityOverride;
    if (_densityGridIdx > 0) return kGridMenu[_densityGridIdx];
    return nullptr;
}

const char* SpectralVDBRead::_GetTempName() const
{
    if (_tempOverride && strlen(_tempOverride) > 0) return _tempOverride;
    if (_tempGridIdx > 0) return kGridMenu[_tempGridIdx];
    return nullptr;
}

std::shared_ptr<pxr::SpectralVolume> SpectralVDBRead::GetVolume()                        { _LoadVDB(); return _volume; }
std::shared_ptr<pxr::SpectralVolume> SpectralVDBRead::GetVolumeAtFrame(int f, int res)  { _requestFrame=f; _LoadVDBAtFrame(f, res); return _volume; }
bool SpectralVDBRead::HasVolume()                                              { _LoadVDB(); return _volume && _volume->IsValid(); }

int SpectralVDBRead::GetMaxRes() const
{
    switch (_voxelRes) {
        case 0: return 64;    // 1/8
        case 1: return 128;   // 1/4
        case 2: return 256;   // 1/2
        case 3: return 512;   // Full
        case 4: return 1024;  // Native
        default: return 128;
    }
}

// ---------------------------------------------------------------------------
void SpectralVDBRead::_UpdateVDBInfo()
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) {
        if (Knob* k = knob("vdb_info")) k->set_text("");
        return;
    }
    int frame = _clampedFrame();
    if (frame < 0) frame = _firstFrame;
    std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
    if (resolved.empty()) return;

    auto meta = pxr::SpectralVDBLoader::LoadMetadataOnly(resolved.c_str(), _GetDensityName());
    if (meta && meta->HasBbox()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Native: %d x %d x %d", meta->resX, meta->resY, meta->resZ);
        if (Knob* k = knob("vdb_info")) k->set_text(buf);
    }
#endif
}

// ---------------------------------------------------------------------------
// knobs
// ---------------------------------------------------------------------------
void SpectralVDBRead::knobs(Knob_Callback f)
{
    SourceGeomOp::knobs(f);

    Tab_knob(f, 0, "VDB");
    Text_knob(f, "<b><font size='3'>SpectralVDBRead</font></b>");
    Newline(f);
    Text_knob(f, "<font color='#888' size='-1'>OpenVDB volume reader for SpectralRender</font>");

    Divider(f, "File");
    File_knob(f, &_filePath, "file", "file");
    Tooltip(f, "OpenVDB volume file (.vdb).\nSupports #### frame padding.");
    KnobModifiesAttribValues(f);

    Bool_knob(f, &_autoSequence, "auto_sequence", "auto sequence");
    ClearFlags(f, Knob::STARTLINE);
    Button(f, "reload", "Reload");
    ClearFlags(f, Knob::STARTLINE);

    Divider(f, "Frame Range");
    Int_knob(f, &_firstFrame, "first_frame", "first"); SetRange(f, 0, 10000);
    static const char* const m1[] = {"hold","black","bounce","loop",nullptr};
    Enumeration_knob(f, &_beforeMode, m1, "before_mode", "");
    ClearFlags(f, Knob::STARTLINE);

    Int_knob(f, &_lastFrame, "last_frame", "last"); SetRange(f, 0, 10000);
    static const char* const m2[] = {"hold","black","bounce","loop",nullptr};
    Enumeration_knob(f, &_afterMode, m2, "after_mode", "");
    ClearFlags(f, Knob::STARTLINE);

    Int_knob(f, &_frameOffset, "frame_offset", "offset"); SetRange(f, -100, 100);
    Button(f, "detect_range", "Detect Range");
    ClearFlags(f, Knob::STARTLINE);

    Obsolete_knob(f, "orig_file", nullptr);
    String_knob(f, &_origFile, "orig_file", ""); SetFlags(f, Knob::INVISIBLE);

    Divider(f, "Grids");
    Button(f, "discover_grids", "Discover Grids");

    Enumeration_knob(f, &_densityGridIdx, kGridMenu, "density_grid", "density");
    String_knob(f, &_densityOverride, "density_override", "");
    ClearFlags(f, Knob::STARTLINE);

    Enumeration_knob(f, &_tempGridIdx, kGridMenu, "temp_grid", "temperature");
    String_knob(f, &_tempOverride, "temp_override", "");
    ClearFlags(f, Knob::STARTLINE);

    Enumeration_knob(f, &_flameGridIdx, kGridMenu, "flame_grid", "flame");
    String_knob(f, &_flameOverride, "flame_override", "");
    ClearFlags(f, Knob::STARTLINE);

    Enumeration_knob(f, &_velGridIdx, kGridMenu, "vel_grid", "velocity");
    String_knob(f, &_velOverride, "vel_override", "");
    ClearFlags(f, Knob::STARTLINE);

    Enumeration_knob(f, &_colorGridIdx, kGridMenu, "color_grid", "color");
    String_knob(f, &_colorOverride, "color_override", "");
    ClearFlags(f, Knob::STARTLINE);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>Created by Marten Blumen</font>");

    // ===================== VOXEL RESOLUTION =====================
    Divider(f, "Voxel Resolution");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Controls how many voxels the VDB is resampled to for rendering.<br>"
        "Lower = faster load + less memory. Higher = sharper detail.<br>"
        "SpectralRender's master resolution caps this per-node setting."
        "</font>"
    );
    Newline(f);
    static const char* const volResOpts[] = {
        "1/8 (fastest)", "1/4 (preview)", "1/2 (production)", "Full (high)", "Native", nullptr
    };
    Enumeration_knob(f, &_voxelRes, volResOpts, "voxel_res", "resolution");
    Tooltip(f, "Fraction of native VDB resolution to render at.\n"
               "1/8 = ~0.2%% of native voxels (very fast, rough look)\n"
               "1/4 = ~1.6%% of native voxels (good for layout)\n"
               "1/2 = ~12.5%% of native voxels (production)\n"
               "Full = ~100%% at 512 cap (high quality)\n"
               "Native = actual VDB dims, capped at 1024");
    Button(f, "estimate_mem", "Estimate Memory");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Calculate the memory required for the current\n"
               "resolution setting with the loaded VDB file.");
    Newline(f);
    String_knob(f, &_memInfo, "mem_info", " ");
    SetFlags(f, Knob::DISABLED | Knob::NO_UNDO);

    // ===================== DISPLAY TAB =====================
    Tab_knob(f, 0, "Display");
    SourceGeomOp::addDisplayOptionsKnobs(f);

    Divider(f, "Preview");

    Int_knob(f, &_previewRes, "preview_res", "resolution");
    SetRange(f, 8, 128);
    KnobModifiesAttribValues(f);
    Tooltip(f, "Grid resolution for preview.\n8-32 for scrubbing, 64+ for detail.");

    String_knob(f, &_vdbInfo, "vdb_info", "");
    ClearFlags(f, Knob::STARTLINE);
    SetFlags(f, Knob::DISABLED | Knob::NO_ANIMATION);

    Int_knob(f, &_maxPoints, "max_points", "max points");
    SetRange(f, 100, 50000);
    SetFlags(f, Knob::SLIDER);
    KnobModifiesAttribValues(f);

    Float_knob(f, &_pointSize, "point_size", "point size");
    SetRange(f, 0.1, 10.0);
    KnobModifiesAttribValues(f);

    Float_knob(f, &_densityThreshold, "density_threshold", "threshold");
    SetRange(f, 0.001, 1.0);
    KnobModifiesAttribValues(f);

    Bool_knob(f, &_lit, "lit", "lit");
    KnobModifiesAttribValues(f);
    Tooltip(f, "Use viewport lights for shading.\n"
               "Off: baked gradient lighting (always visible).\n"
               "On: neutral color + normals, lit by scene lights.");

    Bool_knob(f, &_showBbox, "show_bbox", "bounding box");
    ClearFlags(f, Knob::STARTLINE);
    KnobModifiesAttribValues(f);

    Color_knob(f, _bboxColor, "bbox_color", "bbox color");
    KnobModifiesAttribValues(f);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralVDBRead \xc2\xb7 VDBReadFix \xc2\xb7 Created by Marten Blumen"
                 "</font>");
}

// ---------------------------------------------------------------------------
// knob_changed
// ---------------------------------------------------------------------------
int SpectralVDBRead::knob_changed(Knob* k)
{
    if (k->is("discover_grids")) { _DiscoverGrids(); return 1; }
    if (k->is("detect_range"))   { _DetectFrameRange(); return 1; }
    if (k->is("reload")) {
        _loadedPath.clear(); _loadedMaxRes = 0; _volume.reset();
        _LoadVDB(); _UpdateVDBInfo();
        return 1;
    }
    if (k->is("file")) {
        _loadedPath.clear(); _loadedMaxRes = 0; _UpdateVDBInfo();
        return 1;
    }
    if (k->is("auto_sequence")) {
        if (_autoSequence) {
            std::string p(_filePath ? _filePath : "");
            if (Knob* ok = knob("orig_file")) ok->set_text(p.c_str());
            if (!p.empty()) {
                size_t dot = p.rfind(".vdb");
                if (dot == std::string::npos) dot = p.rfind(".VDB");
                if (dot != std::string::npos) {
                    size_t end = dot, start = end;
                    while (start > 0 && p[start-1] >= '0' && p[start-1] <= '9') --start;
                    if (start < end) {
                        int ndig = int(end - start); if (ndig < 4) ndig = 4;
                        p = p.substr(0, start) + std::string(ndig, '#') + p.substr(end);
                        if (Knob* fk = knob("file")) fk->set_text(p.c_str());
                    }
                }
            }
            _DetectFrameRange();
        } else {
            const char* orig = _origFile ? _origFile : "";
            if (orig[0]) {
                if (Knob* fk = knob("file")) fk->set_text(orig);
                if (Knob* ok = knob("orig_file")) ok->set_text("");
            }
        }
        _loadedPath.clear(); _loadedMaxRes = 0; _UpdateVDBInfo();
        return 1;
    }
    if (k->is("frame_offset") || k->is("density_grid") ||
        k->is("temp_grid") || k->is("density_override") || k->is("temp_override")) {
        _loadedPath.clear(); _loadedMaxRes = 0;
        return 1;
    }
    if (k->is("show_bbox") || k->is("preview_res") || k->is("max_points") ||
        k->is("point_size") || k->is("density_threshold") || k->is("bbox_color") ||
        k->is("lit")) {
        return 1;
    }
    if (k->is("voxel_res")) {
        _loadedPath.clear(); _loadedMaxRes = 0;
        _UpdateVDBInfo();
        return 1;
    }
    if (k->is("estimate_mem")) {
#ifdef SPECTRAL_HAS_VDB
        if (_filePath && strlen(_filePath) > 0) {
            int frame = _clampedFrame();
            if (frame < 0) frame = _firstFrame;
            std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
            if (!resolved.empty()) {
                auto grids = pxr::SpectralVDBLoader::DiscoverGrids(resolved.c_str());
                int nativeMax = 0, gridCount = 0;
                int nativeX = 0, nativeY = 0, nativeZ = 0;
                for (const auto& g : grids) {
                    if (g.voxelCount > 0) {
                        int d = int(std::cbrt(double(g.voxelCount)));
                        if (d > nativeMax) nativeMax = d;
                        gridCount++;
                    }
                }
                auto meta = pxr::SpectralVDBLoader::LoadMetadataOnly(resolved.c_str(), _GetDensityName());
                if (meta && meta->HasBbox()) {
                    pxr::GfVec3f bs = meta->bboxMax - meta->bboxMin;
                    float maxS = std::max({bs[0], bs[1], bs[2]});
                    if (maxS > 0) {
                        nativeX = int(nativeMax * bs[0] / maxS);
                        nativeY = int(nativeMax * bs[1] / maxS);
                        nativeZ = int(nativeMax * bs[2] / maxS);
                    }
                }
                if (nativeMax == 0) nativeMax = nativeX = nativeY = nativeZ = 128;

                int renderMax;
                if (_voxelRes == 4) renderMax = std::min(1024, nativeMax);
                else if (_voxelRes == 3) renderMax = std::min(512, nativeMax);
                else {
                    static const float fracs[] = { 0.125f, 0.25f, 0.5f };
                    renderMax = std::max(32, int(nativeMax * fracs[_voxelRes]));
                }
                float ratio = float(renderMax) / std::max(1, nativeMax);
                int rX = std::max(1, int(nativeX * ratio));
                int rY = std::max(1, int(nativeY * ratio));
                int rZ = std::max(1, int(nativeZ * ratio));
                size_t voxels = size_t(rX) * rY * rZ;
                int numGrids = std::max(1, std::min(gridCount, 4));
                float memMB = voxels * sizeof(float) * numGrids / (1024.f * 1024.f);

                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "Native: %dx%dx%d | Render: %dx%dx%d | ~%.0f MB (%d grid%s)",
                    nativeX, nativeY, nativeZ, rX, rY, rZ, memMB,
                    numGrids, numGrids > 1 ? "s" : "");
                if (Knob* mk = knob("mem_info")) mk->set_text(buf);
            }
        } else {
            if (Knob* mk = knob("mem_info")) mk->set_text("No VDB file loaded");
        }
#endif
        return 1;
    }
    return SourceGeomOp::knob_changed(k);
}

// ---------------------------------------------------------------------------
int SpectralVDBRead::_clampedFrame() const
{
    int f = int(uiContext().frame()) + _frameOffset;
    if (f >= _firstFrame && f <= _lastFrame) return f;
    int mode = (f < _firstFrame) ? _beforeMode : _afterMode;
    int range = _lastFrame - _firstFrame;
    switch (mode) {
        case 0: return (f < _firstFrame) ? _firstFrame : _lastFrame;
        case 1: return -1;
        case 2: { if (range<=0) return _firstFrame;
                  int off=std::abs(f-_firstFrame)%(range*2);
                  return (off<=range)?_firstFrame+off:_lastFrame-(off-range); }
        case 3: { int r=range+1; if(r<=0) return _firstFrame;
                  return _firstFrame+((f-_firstFrame)%r+r)%r; }
        default: return f;
    }
}

std::string SpectralVDBRead::_resolveFramePath(int frame) const
{
    std::string p(_filePath ? _filePath : "");
    if (p.empty()) return p;
    size_t h = p.find('#');
    if (h != std::string::npos) {
        size_t he = h;
        while (he < p.size() && p[he] == '#') ++he;
        char buf[64];
        std::snprintf(buf, 64, "%0*d", int(he - h), frame);
        p.replace(h, he - h, buf);
    }
    return p;
}

// ---------------------------------------------------------------------------
void SpectralVDBRead::_DiscoverGrids()
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) { error("No VDB file."); return; }
    int frame = _clampedFrame(); if (frame < 0) { error("Frame outside range."); return; }
    std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
    auto grids = pxr::SpectralVDBLoader::DiscoverGrids(resolved.c_str());
    if (grids.empty()) { error("No grids in %s", resolved.c_str()); return; }

    std::string bestD, bestT, bestF, bestV, bestC;
    for (const auto& g : grids) {
        if (bestD.empty() && g.category == "density") bestD = g.name;
        if (bestT.empty() && g.category == "temperature") bestT = g.name;
        if (bestF.empty() && g.category == "flame") bestF = g.name;
        if (bestV.empty() && g.category == "velocity") bestV = g.name;
        if (bestC.empty() && g.category == "color") bestC = g.name;
    }
    if (bestD.empty()) for (const auto& g : grids) if (g.type == "float") { bestD = g.name; break; }

    if (!bestD.empty() && knob("density_override")) knob("density_override")->set_text(bestD.c_str());
    if (!bestT.empty() && knob("temp_override"))    knob("temp_override")->set_text(bestT.c_str());
    if (!bestF.empty() && knob("flame_override"))    knob("flame_override")->set_text(bestF.c_str());
    if (!bestV.empty() && knob("vel_override"))      knob("vel_override")->set_text(bestV.c_str());
    if (!bestC.empty() && knob("color_override"))    knob("color_override")->set_text(bestC.c_str());

    _loadedPath.clear(); _loadedMaxRes = 0;
    _UpdateVDBInfo();
#else
    error("OpenVDB not compiled.");
#endif
}

// ---------------------------------------------------------------------------
void SpectralVDBRead::_DetectFrameRange()
{
    std::string origPath = (_origFile && strlen(_origFile) > 0) ? _origFile : (_filePath ? _filePath : "");
    if (origPath.empty()) return;
    for (char& c : origPath) if (c == '\\') c = '/';

    size_t dot = origPath.rfind(".vdb");
    if (dot == std::string::npos) dot = origPath.rfind(".VDB");
    if (dot == std::string::npos) return;

    size_t numEnd = dot, numStart = numEnd;
    while (numStart > 0 && origPath[numStart-1] >= '0' && origPath[numStart-1] <= '9') --numStart;
    if (numStart >= numEnd) {
        origPath = _filePath ? _filePath : "";
        for (char& c : origPath) if (c == '\\') c = '/';
        size_t h = origPath.find('#');
        if (h == std::string::npos) return;
        numStart = h; numEnd = h;
        while (numEnd < origPath.size() && origPath[numEnd] == '#') ++numEnd;
        dot = origPath.rfind(".vdb");
        if (dot == std::string::npos) dot = origPath.rfind(".VDB");
        if (dot == std::string::npos) return;
    }

    std::string dir, filePrefix, fileSuffix;
    size_t lastSlash = origPath.rfind('/');
    if (lastSlash != std::string::npos) {
        dir = origPath.substr(0, lastSlash + 1);
        filePrefix = origPath.substr(lastSlash + 1, numStart - (lastSlash + 1));
    } else { filePrefix = origPath.substr(0, numStart); }
    fileSuffix = origPath.substr(numEnd);

    int minFrame = 999999, maxFrame = -999999, count = 0;
#ifdef _WIN32
    std::string searchDir = dir;
    for (char& c : searchDir) if (c == '/') c = '\\';
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((searchDir + filePrefix + "*" + fileSuffix).c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string name(fd.cFileName);
            if (name.size() > filePrefix.size() + fileSuffix.size()) {
                std::string numStr = name.substr(filePrefix.size(),
                    name.size() - filePrefix.size() - fileSuffix.size());
                bool ok = !numStr.empty();
                for (char c : numStr) if (c < '0' || c > '9') { ok = false; break; }
                if (ok) {
                    int fr = std::atoi(numStr.c_str());
                    if (fr < minFrame) minFrame = fr;
                    if (fr > maxFrame) maxFrame = fr;
                    count++;
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#endif
    if (count > 0) {
        _firstFrame = minFrame; _lastFrame = maxFrame;
        if (knob("first_frame")) knob("first_frame")->set_value(minFrame);
        if (knob("last_frame"))  knob("last_frame")->set_value(maxFrame);
    }
}

// ---------------------------------------------------------------------------
void SpectralVDBRead::_LoadVDBAtFrame(int frame, int maxRes)
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) { _volume.reset(); return; }
    if (frame < _firstFrame || frame > _lastFrame) {
        int mode = (frame < _firstFrame) ? _beforeMode : _afterMode;
        int range = _lastFrame - _firstFrame;
        switch (mode) {
            case 0: frame = (frame<_firstFrame)?_firstFrame:_lastFrame; break;
            case 1: _volume.reset(); return;
            case 2: { if(range>0){int o=std::abs(frame-_firstFrame)%(range*2);
                      frame=(o<=range)?_firstFrame+o:_lastFrame-(o-range);}
                      else frame=_firstFrame; break; }
            case 3: { int r=range+1; if(r>0) frame=_firstFrame+((frame-_firstFrame)%r+r)%r;
                      else frame=_firstFrame; break; }
        }
    }
    std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
    if (resolved.empty()) return;
    int curRes = (_volume && _volume->IsValid()) ? std::max({_volume->resX, _volume->resY, _volume->resZ}) : 0;
    if (_volume && _volume->IsValid() && _loadedPath == resolved && _loadedMaxRes == maxRes) return;
    _volume = pxr::SpectralVDBLoader::Load(resolved.c_str(), _GetDensityName(), _GetTempName(), maxRes);
    if (_volume) {
        _loadedPath = resolved;
        _loadedMaxRes = maxRes;
        fprintf(stderr, "SpectralVDBRead: loaded %dx%dx%d (maxRes=%d)\n",
                _volume->resX, _volume->resY, _volume->resZ, maxRes);
    }
#endif
}

void SpectralVDBRead::_LoadVDB()
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) { _volume.reset(); return; }
    int frame = _clampedFrame(); if (frame < 0) { _volume.reset(); return; }
    std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
    if (resolved.empty()) return;
    if (_volume && _volume->IsValid() && _loadedPath == resolved) return;
    _volume = pxr::SpectralVDBLoader::Load(resolved.c_str(), _GetDensityName(), _GetTempName());
    if (_volume) _loadedPath = resolved;
#endif
}
