// SpectralVDBRead — SourceGeomOp VDB reader for Nuke 17
// Created by Marten Blumen

#include "SpectralVDBRead.h"
#include "usg/geom/MeshPrim.h"
#include "ndk/geo/utils/MeshUtils.h"

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
// Engine
// ---------------------------------------------------------------------------
void SpectralVDBRead::Engine::createPrims(GeomSceneContext& context,
                                           const Path& path)
{
    if (!context.doGeometryProcessing()) return;
    LayerRef defineLayer = editLayer();
    if (!defineLayer) return;

    ndk::MeshSample sample;
    MeshPrim mesh = MeshPrim::defineInLayer(defineLayer, path);
    _transformSubEngine.apply(context, mesh);

    bool buildTopology = context.doGeometryInitialization();
    for (const TimeValue& time : context.processTimes()) {
        sample.points = {
            Vec3f(-0.5f,-0.5f,-0.5f), Vec3f(0.5f,-0.5f,-0.5f),
            Vec3f(0.5f,0.5f,-0.5f),   Vec3f(-0.5f,0.5f,-0.5f),
            Vec3f(-0.5f,-0.5f,0.5f),  Vec3f(0.5f,-0.5f,0.5f),
            Vec3f(0.5f,0.5f,0.5f),    Vec3f(-0.5f,0.5f,0.5f),
        };
        if (buildTopology) {
            sample.faceVertexCounts.clear();
            sample.faceVertexIndices.clear();
            sample.all_quads = true; sample.all_tris = false;
            sample.addQuad(0,3,2,1); sample.addQuad(4,5,6,7);
            sample.addQuad(0,1,5,4); sample.addQuad(2,3,7,6);
            sample.addQuad(0,4,7,3); sample.addQuad(1,2,6,5);
            sample.facevert_normals.resize(1, Vec3f(0,1,0));
            sample.setMeshPrimFaceTopology(mesh);
            buildTopology = false;
        }
        sample.setMeshPrimProperties(mesh, false, true);
        mesh.setPoints(sample.points, time);
        mesh.setBoundsAttr(sample.points, time);
    }
    assignMaterial(context, {path});
}

// ---------------------------------------------------------------------------
// Op
// ---------------------------------------------------------------------------
SpectralVDBRead::SpectralVDBRead(Node* node)
    : SourceGeomOp(node, BuildEngine<Engine>())
{
    fprintf(stderr, "SpectralVDBRead: SourceGeomOp build %s %s\n", __DATE__, __TIME__);
}

const char* SpectralVDBRead::node_help() const
{
    return "SpectralVDBRead — OpenVDB Volume Reader\n\n"
           "Reads OpenVDB (.vdb) files for volume rendering.\n\n"
           "SpectralVDBRead -> GeoScene -> SpectralRender\n\n"
           "SpectralRender auto-detects this node in the scene.\n\n"
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

std::shared_ptr<pxr::SpectralVolume> SpectralVDBRead::GetVolume()
{
    _LoadVDB();
    return _volume;
}

std::shared_ptr<pxr::SpectralVolume> SpectralVDBRead::GetVolumeAtFrame(int frame)
{
    _requestFrame = frame;
    _LoadVDBAtFrame(frame);
    return _volume;
}

bool SpectralVDBRead::HasVolume()
{
    _LoadVDB();
    return _volume && _volume->IsValid();
}

// ---------------------------------------------------------------------------
// knobs
// ---------------------------------------------------------------------------
void SpectralVDBRead::knobs(Knob_Callback f)
{
    SourceGeomOp::knobs(f);

    Tab_knob(f, 0, "VDB");

    // Title
    Text_knob(f, "<b><font size='3'>SpectralVDBRead</font></b>");
    Newline(f);
    Text_knob(f, "<font color='#888' size='-1'>OpenVDB volume reader for SpectralRender</font>");

    // --- File ---
    Divider(f, "File");
    File_knob(f, &_filePath, "file", "file");
    Tooltip(f, "OpenVDB volume file (.vdb).\n"
               "Supports #### frame padding for sequences.");
    KnobModifiesAttribValues(f);

    Bool_knob(f, &_autoSequence, "auto_sequence", "auto sequence");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Treat as frame sequence.\n"
               "Replaces frame numbers with ####.\n"
               "Auto-detects first/last frame on disk.");
    Button(f, "reload", "Reload");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Force reload from disk.");

    // --- Frame Range ---
    Divider(f, "Frame Range");
    Int_knob(f, &_firstFrame, "first_frame", "first"); SetRange(f, 0, 10000);
    Tooltip(f, "First frame of VDB sequence.");
    static const char* const m1[] = {"hold","black","bounce","loop",nullptr};
    Enumeration_knob(f, &_beforeMode, m1, "before_mode", "");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Behaviour before first frame.");

    Int_knob(f, &_lastFrame, "last_frame", "last"); SetRange(f, 0, 10000);
    Tooltip(f, "Last frame of VDB sequence.");
    static const char* const m2[] = {"hold","black","bounce","loop",nullptr};
    Enumeration_knob(f, &_afterMode, m2, "after_mode", "");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Behaviour after last frame.");

    Int_knob(f, &_frameOffset, "frame_offset", "offset"); SetRange(f, -100, 100);
    Tooltip(f, "Offset the frame number.\nUseful for retiming simulations.");
    Button(f, "detect_range", "Detect Range");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Scan directory for matching VDB files\n"
               "and set first/last frame automatically.");

    Obsolete_knob(f, "orig_file", nullptr);
    String_knob(f, &_origFile, "orig_file", ""); SetFlags(f, Knob::INVISIBLE);

    // --- Grids ---
    Divider(f, "Grids");
    Button(f, "discover_grids", "Discover Grids");
    Tooltip(f, "Scan VDB file and auto-detect grid names.\n"
               "Populates override fields below.");

    Enumeration_knob(f, &_densityGridIdx, kGridMenu, "density_grid", "density");
    String_knob(f, &_densityOverride, "density_override", "");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Override: type a custom grid name.\nTakes priority over pulldown.");

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

    // --- Footer ---
    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>Created by Marten Blumen · github.com/bratgot/SpectralRenderer</font>");

    // --- Display tab ---
    Tab_knob(f, 0, "Display");
    SourceGeomOp::addDisplayOptionsKnobs(f);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralVDBRead \xc2\xb7 SpectralRenderer for Nuke 17 \xc2\xb7 Created by Marten Blumen"
                 "</font>");
}

// ---------------------------------------------------------------------------
// knob_changed
// ---------------------------------------------------------------------------
int SpectralVDBRead::knob_changed(Knob* k)
{
    if (k->is("discover_grids")) { _DiscoverGrids(); return 1; }
    if (k->is("detect_range")) { _DetectFrameRange(); return 1; }
    if (k->is("reload")) {
        _loadedPath.clear(); _volume.reset();
        

        _LoadVDB(); // explicit user action — load now
        return 1;
    }
    if (k->is("file")) {
        _loadedPath.clear(); 

        // Don't load here — render or viewport will trigger it
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
            // Detect first/last frame from files on disk

        } else {
            const char* orig = _origFile ? _origFile : "";
            if (orig[0]) {
                if (Knob* fk = knob("file")) fk->set_text(orig);
                if (Knob* ok = knob("orig_file")) ok->set_text("");
            }
        }
        _loadedPath.clear();
        // Don't load here — next render or viewport will pick it up
        return 1;
    }
    if (k->is("frame_offset") || k->is("density_grid") ||
        k->is("temp_grid") || k->is("density_override") || k->is("temp_override")) {
        _loadedPath.clear(); 
        return 1;
    }
    return SourceGeomOp::knob_changed(k);
}

// ---------------------------------------------------------------------------
// Frame
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
        case 2: {
            if (range <= 0) return _firstFrame;
            int off = std::abs(f - _firstFrame) % (range * 2);
            return (off <= range) ? _firstFrame + off : _lastFrame - (off - range);
        }
        case 3: {
            int r = range + 1; if (r <= 0) return _firstFrame;
            return _firstFrame + ((f - _firstFrame) % r + r) % r;
        }
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
// Discover — single-line Python via TCL python command
// ---------------------------------------------------------------------------
void SpectralVDBRead::_DiscoverGrids()
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) { error("No VDB file."); return; }
    int frame = _clampedFrame();
    if (frame < 0) { error("Frame outside range."); return; }
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
    if (!bestF.empty() && knob("flame_override"))   knob("flame_override")->set_text(bestF.c_str());
    if (!bestV.empty() && knob("vel_override"))      knob("vel_override")->set_text(bestV.c_str());
    if (!bestC.empty() && knob("color_override"))   knob("color_override")->set_text(bestC.c_str());

    // Build message for popup
    std::string msg = "Grids found:\\n\\n";
    for (const auto& g : grids) {
        msg += g.name + "  (" + g.type + ")";
        if (g.category != "other" && g.category != "float") msg += "  [" + g.category + "]";
        msg += "\\n";
    }
    msg += "\\n";
    if (!bestD.empty()) msg += "Density: " + bestD + "\\n";
    if (!bestT.empty()) msg += "Temperature: " + bestT + "\\n";
    if (!bestF.empty()) msg += "Flame: " + bestF + "\\n";
    if (!bestV.empty()) msg += "Velocity: " + bestV + "\\n";
    if (!bestC.empty()) msg += "Color: " + bestC + "\\n";
    msg += "\\nOverride fields updated.";

    // Use TCL python command for single-line execution
    std::string cmd = "python {nuke.message('" + msg + "')}";
    script_command(cmd.c_str());

    _loadedPath.clear(); 
#else
    error("OpenVDB not compiled.");
#endif
}

// ---------------------------------------------------------------------------
// _DetectFrameRange — scan directory for matching VDB sequence files
// ---------------------------------------------------------------------------
void SpectralVDBRead::_DetectFrameRange()
{
    // Use original file path (before #### substitution) for scanning
    std::string origPath = (_origFile && strlen(_origFile) > 0) ? _origFile : (_filePath ? _filePath : "");
    if (origPath.empty()) return;

    // Normalize slashes
    for (char& c : origPath) if (c == '\\') c = '/';

    // Find where the frame number is in the original filename
    // e.g. "C:/vdb/explosion.0045.vdb" -> prefix="explosion.", suffix=".vdb"
    size_t dot = origPath.rfind(".vdb");
    if (dot == std::string::npos) dot = origPath.rfind(".VDB");
    if (dot == std::string::npos) return;

    size_t numEnd = dot;
    size_t numStart = numEnd;
    while (numStart > 0 && origPath[numStart-1] >= '0' && origPath[numStart-1] <= '9') --numStart;
    if (numStart >= numEnd) {
        // No frame number found — try scanning with #### from _filePath
        origPath = _filePath ? _filePath : "";
        for (char& c : origPath) if (c == '\\') c = '/';
        size_t h = origPath.find('#');
        if (h == std::string::npos) return;
        numStart = h;
        numEnd = h;
        while (numEnd < origPath.size() && origPath[numEnd] == '#') ++numEnd;
        dot = origPath.rfind(".vdb");
        if (dot == std::string::npos) dot = origPath.rfind(".VDB");
        if (dot == std::string::npos) return;
    }

    int ndig = int(numEnd - numStart);
    if (ndig < 1) ndig = 4;

    // Extract directory and filename parts
    std::string dir, filePrefix, fileSuffix;
    size_t lastSlash = origPath.rfind('/');
    if (lastSlash != std::string::npos) {
        dir = origPath.substr(0, lastSlash + 1);
        filePrefix = origPath.substr(lastSlash + 1, numStart - (lastSlash + 1));
    } else {
        dir = "";
        filePrefix = origPath.substr(0, numStart);
    }
    fileSuffix = origPath.substr(numEnd);

    fprintf(stderr, "SpectralVDBRead: scanning '%s' for '%s*%s'\n", dir.c_str(), filePrefix.c_str(), fileSuffix.c_str());

    int minFrame = 999999, maxFrame = -999999;
    int count = 0;

#ifdef _WIN32
    // Convert to backslash for Windows API
    std::string searchDir = dir;
    for (char& c : searchDir) if (c == '/') c = '\\';
    std::string pattern = searchDir + filePrefix + "*" + fileSuffix;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string name(fd.cFileName);
            // Extract frame number between prefix and suffix
            if (name.size() > filePrefix.size() + fileSuffix.size()) {
                std::string numStr = name.substr(filePrefix.size(),
                    name.size() - filePrefix.size() - fileSuffix.size());
                bool allDigits = !numStr.empty();
                for (char c : numStr) if (c < '0' || c > '9') { allDigits = false; break; }
                if (allDigits) {
                    int frame = std::atoi(numStr.c_str());
                    if (frame < minFrame) minFrame = frame;
                    if (frame > maxFrame) maxFrame = frame;
                    count++;
                }
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    } else {
        fprintf(stderr, "SpectralVDBRead: FindFirstFile failed for '%s'\n", pattern.c_str());
    }
#endif

    if (count > 0) {
        _firstFrame = minFrame;
        _lastFrame = maxFrame;
        if (knob("first_frame")) knob("first_frame")->set_value(minFrame);
        if (knob("last_frame"))  knob("last_frame")->set_value(maxFrame);
        fprintf(stderr, "SpectralVDBRead: frame range %d-%d (%d files)\n", minFrame, maxFrame, count);
        char msg[256];
        std::snprintf(msg, sizeof(msg), "Frame range: %d - %d\\n%d VDB files found", minFrame, maxFrame, count);
        script_command((std::string("python {nuke.message('") + msg + "')}").c_str());
    } else {
        error("No sequence files found matching pattern.");
        fprintf(stderr, "SpectralVDBRead: no sequence files found\n");
    }
}

// ---------------------------------------------------------------------------
// _LoadVDBAtFrame — load VDB at a specific frame
// ---------------------------------------------------------------------------
void SpectralVDBRead::_LoadVDBAtFrame(int frame)
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) { _volume.reset(); return; }

    // Apply frame clamping
    if (frame < _firstFrame || frame > _lastFrame) {
        int mode = (frame < _firstFrame) ? _beforeMode : _afterMode;
        int range = _lastFrame - _firstFrame;
        switch (mode) {
            case 0: frame = (frame < _firstFrame) ? _firstFrame : _lastFrame; break;
            case 1: _volume.reset(); return;
            case 2: {
                if (range > 0) {
                    int off = std::abs(frame - _firstFrame) % (range * 2);
                    frame = (off <= range) ? _firstFrame + off : _lastFrame - (off - range);
                } else frame = _firstFrame;
                break;
            }
            case 3: {
                int r = range + 1;
                if (r > 0) frame = _firstFrame + ((frame - _firstFrame) % r + r) % r;
                else frame = _firstFrame;
                break;
            }
        }
    }

    std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
    if (resolved.empty()) return;
    if (_volume && _volume->IsValid() && _loadedPath == resolved) return;

    fprintf(stderr, "SpectralVDBRead: loading frame %d '%s'\n", frame, resolved.c_str());
    _volume = pxr::SpectralVDBLoader::Load(resolved.c_str(), _GetDensityName(), _GetTempName());
    if (_volume) {
        _loadedPath = resolved;
        
        fprintf(stderr, "SpectralVDBRead: %dx%dx%d\n", _volume->resX, _volume->resY, _volume->resZ);
    }
#endif
}

// ---------------------------------------------------------------------------
// _LoadVDB — load at current UI frame
// ---------------------------------------------------------------------------
void SpectralVDBRead::_LoadVDB()
{
#ifdef SPECTRAL_HAS_VDB
    if (!_filePath || strlen(_filePath) == 0) { _volume.reset(); return; }
    int frame = _clampedFrame();
    if (frame < 0) { _volume.reset(); return; }
    std::string resolved = _autoSequence ? _resolveFramePath(frame) : std::string(_filePath);
    if (resolved.empty()) return;
    if (_volume && _volume->IsValid() && _loadedPath == resolved) return;

    fprintf(stderr, "SpectralVDBRead: loading '%s'\n", resolved.c_str());
    _volume = pxr::SpectralVDBLoader::Load(resolved.c_str(), _GetDensityName(), _GetTempName());
    if (_volume) {
        _loadedPath = resolved;
        
        fprintf(stderr, "SpectralVDBRead: %dx%dx%d\n", _volume->resX, _volume->resY, _volume->resZ);
    }
#endif
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------
// Viewport preview lives on SpectralRender (Iop build_handles works there).
// SourceGeomOp::build_handles does not propagate GL draws in Nuke 17.
