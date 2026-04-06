// SpectralEnvLight — environment/sky lighting for SpectralRender
// Created by Marten Blumen

#include "SpectralEnvLight.h"
#include <DDImage/Knobs.h>
#include <DDImage/ViewerContext.h>
#include <DDImage/gl.h>
#include "usg/geom/PointsPrim.h"
#include <cmath>

using namespace DD::Image;
using namespace fdk;
using namespace usg;

const char* const SpectralEnvLight::CLASS = "SpectralEnvLight";
static Op* buildEnv(Node* node) { return new SpectralEnvLight(node); }
const GeomOp::Description SpectralEnvLight::description(CLASS, buildEnv);

SpectralEnvLight::SpectralEnvLight(Node* node)
    : SourceGeomOp(node, BuildEngine<Engine>()) {}

void SpectralEnvLight::Engine::createPrims(GeomSceneContext& context, const Path& path)
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
    prim.setCustomData("spectralIsEnvLight", Value(true));
}

const char* SpectralEnvLight::node_help() const
{
    return "SpectralEnvLight -- Environment & Sky Lighting\n\n"
           "Sun/sky model with 18 presets including planetary atmospheres.\n"
           "Solar position from latitude, longitude, and time of day.\n"
           "HDRI environment map loading with rotation and intensity.\n\n"
           "CONNECTION\n"
           "  Connect directly to GeoScene as a separate input.\n"
           "  Do NOT use GeoBind -- it breaks animated updates.\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

static const double kPi = 3.14159265358979;

void SpectralEnvLight::ComputeSunPosition()
{
    double B = (360.0 / 365.0) * (dayOfYear - 81) * kPi / 180.0;
    double dec = 23.45 * std::sin(B);
    double eot = 9.87*std::sin(2*B) - 7.53*std::cos(B) - 1.5*std::sin(B);
    double solarTime = timeOfDay + eot/60.0 + longitude/15.0;
    double hourAngle = (solarTime - 12.0) * 15.0;
    double latR = latitude * kPi / 180.0;
    double decR = dec * kPi / 180.0;
    double haR = hourAngle * kPi / 180.0;
    double sinElev = std::sin(latR)*std::sin(decR) + std::cos(latR)*std::cos(decR)*std::cos(haR);
    sunElevation = std::asin(std::max(-1.0, std::min(1.0, sinElev))) * 180.0 / kPi;
    double cosAz = (std::sin(decR) - std::sin(latR)*sinElev) / (std::cos(latR)*std::max(std::cos(std::asin(sinElev)), 0.001));
    sunAzimuth = std::acos(std::max(-1.0, std::min(1.0, cosAz))) * 180.0 / kPi;
    if (hourAngle > 0) sunAzimuth = 360.0 - sunAzimuth;
}

// ---------------------------------------------------------------------------
// 3D Viewport Display
// ---------------------------------------------------------------------------
void SpectralEnvLight::build_handles(ViewerContext* ctx)
{
    if (ctx->transform_mode() == VIEWER_2D) return;
    add_draw_handle(ctx);
}

void SpectralEnvLight::draw_handle(ViewerContext* ctx)
{
    float R = float(domeRadius);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);

    // --- Ground plane circle ---
    glColor4f(0.4f, 0.4f, 0.45f, 0.3f);
    glLineWidth(1.f);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < 64; ++i) {
        float a = float(i) / 64.f * 2.f * float(kPi);
        glVertex3f(R * std::cos(a), 0.f, R * std::sin(a));
    }
    glEnd();

    // --- Dome hemisphere wireframe ---
    if (showDome) {
        glColor4f(0.35f, 0.55f, 0.8f, 0.25f);
        glLineWidth(1.f);
        // Latitude lines
        for (int lat = 15; lat <= 75; lat += 15) {
            float elR = float(lat) * float(kPi) / 180.f;
            float rr = R * std::cos(elR);
            float y = R * std::sin(elR);
            glBegin(GL_LINE_LOOP);
            for (int i = 0; i < 48; ++i) {
                float a = float(i) / 48.f * 2.f * float(kPi);
                glVertex3f(rr * std::cos(a), y, rr * std::sin(a));
            }
            glEnd();
        }
        // Longitude meridians
        for (int lon = 0; lon < 360; lon += 30) {
            float azR = float(lon) * float(kPi) / 180.f;
            glBegin(GL_LINE_STRIP);
            for (int i = 0; i <= 24; ++i) {
                float elR = float(i) / 24.f * float(kPi) * 0.5f;
                float rr = R * std::cos(elR);
                float y = R * std::sin(elR);
                glVertex3f(rr * std::sin(azR), y, rr * std::cos(azR));
            }
            glEnd();
        }
        // Zenith point
        glColor4f(0.5f, 0.7f, 1.f, 0.4f);
        glPointSize(4.f);
        glBegin(GL_POINTS);
        glVertex3f(0.f, R, 0.f);
        glEnd();
    }

    // --- Compass rose ---
    if (showCompass) {
        float cr = R * 1.08f;
        struct CD { float x, z; float r, g, b; };
        CD dirs[] = {
            { 0,  1, 1.f, 0.3f, 0.3f},  // N (red)
            { 0, -1, 0.6f,0.6f, 0.6f},   // S
            { 1,  0, 0.6f,0.6f, 0.6f},   // E
            {-1,  0, 0.6f,0.6f, 0.6f},   // W
        };
        glLineWidth(2.f);
        for (auto& d : dirs) {
            glColor4f(d.r, d.g, d.b, 0.7f);
            glBegin(GL_LINES);
            glVertex3f(d.x*R*0.92f, 0.f, d.z*R*0.92f);
            glVertex3f(d.x*cr, 0.f, d.z*cr);
            glEnd();
        }
        // Intercardinal ticks
        glColor4f(0.4f, 0.4f, 0.4f, 0.4f);
        glLineWidth(1.f);
        float s45 = float(std::sin(kPi/4));
        float iO = R*0.95f, iI = R*1.05f;
        float off[][2] = {{s45,s45},{s45,-s45},{-s45,-s45},{-s45,s45}};
        for (auto& o : off) {
            glBegin(GL_LINES);
            glVertex3f(iO*o[0], 0.f, iO*o[1]);
            glVertex3f(iI*o[0], 0.f, iI*o[1]);
            glEnd();
        }
    }

    // --- Sun direction arrow ---
    if (showSunArrow && skyPreset > 0) {
        float elR = float(sunElevation) * float(kPi) / 180.f;
        float azR = float(sunAzimuth) * float(kPi) / 180.f;
        float sx = R * std::cos(elR) * std::sin(azR);
        float sy = R * std::sin(elR);
        float sz = R * std::cos(elR) * std::cos(azR);

        // Shaft
        float bright = std::min(1.f, float(sunIntensity) / 5.f);
        glColor4f(1.f, 0.85f, 0.3f, 0.5f + bright * 0.4f);
        glLineWidth(3.f);
        glBegin(GL_LINES);
        glVertex3f(0.f, 0.f, 0.f);
        glVertex3f(sx, sy, sz);
        glEnd();

        // Sun disc
        glColor4f(1.f, 0.9f, 0.4f, 0.9f);
        glPointSize(12.f);
        glBegin(GL_POINTS); glVertex3f(sx, sy, sz); glEnd();
        glColor4f(1.f, 0.8f, 0.2f, 0.3f);
        glPointSize(22.f);
        glBegin(GL_POINTS); glVertex3f(sx, sy, sz); glEnd();

        // Arrowhead fins
        float len = std::sqrt(sx*sx + sy*sy + sz*sz);
        if (len > 1e-4f) {
            float dx=sx/len, dy=sy/len, dz=sz/len;
            float px, py, pz;
            if (std::abs(dy) < 0.9f) { px=-dz; py=0; pz=dx; }
            else { px=1; py=0; pz=0; }
            float pl = std::sqrt(px*px+py*py+pz*pz);
            if (pl>1e-6f) { px/=pl; py/=pl; pz/=pl; }
            float hs = R * 0.06f;
            float bx=sx-dx*hs*2, by=sy-dy*hs*2, bz=sz-dz*hs*2;
            glColor4f(1.f, 0.85f, 0.3f, 0.7f);
            glLineWidth(2.f);
            glBegin(GL_LINES);
            glVertex3f(sx,sy,sz); glVertex3f(bx+px*hs,by+py*hs,bz+pz*hs);
            glVertex3f(sx,sy,sz); glVertex3f(bx-px*hs,by-py*hs,bz-pz*hs);
            glEnd();
        }

        // Horizon projection
        if (sunElevation > 1.0) {
            glColor4f(1.f, 0.85f, 0.3f, 0.15f);
            glLineWidth(1.f);
            glEnable(GL_LINE_STIPPLE); glLineStipple(2, 0xAAAA);
            glBegin(GL_LINES);
            glVertex3f(sx, sy, sz);
            glVertex3f(R*std::sin(azR), 0.f, R*std::cos(azR));
            glEnd();
            glDisable(GL_LINE_STIPPLE);
        }
    }

    // --- Center crosshair ---
    glColor4f(0.8f, 0.8f, 0.8f, 0.5f);
    float cm = R * 0.03f;
    glLineWidth(1.f);
    glBegin(GL_LINES);
    glVertex3f(-cm,0,0); glVertex3f(cm,0,0);
    glVertex3f(0,-cm,0); glVertex3f(0,cm,0);
    glVertex3f(0,0,-cm); glVertex3f(0,0,cm);
    glEnd();

    glPopAttrib();
}

// ---------------------------------------------------------------------------
// Knobs
// ---------------------------------------------------------------------------
void SpectralEnvLight::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>Environment Light</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Sky model, solar position, and HDRI environment maps.<br>"
        "Connect to GeoScene alongside SpectralVDBRead."
        "</font>"
    );

    Divider(f, "Sky");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Physically-inspired sky model based on Preetham analytical atmosphere.<br>"
        "18 presets include Earth atmospheres and planetary skies.<br>"
        "Sun intensity is squared for exponential response (matches real-world<br>"
        "perception where small slider moves = large brightness changes)."
        "</font>"
    );
    Newline(f);
    static const char* const skyNames[] = {
        "Off", "Custom", "Clear Day", "Golden Hour", "Red Sky Dawn",
        "Sunrise", "Overcast", "Blue Hour", "Moonlit", "Starlight",
        "Alpine Light", "Desert Noon", "Arctic Twilight",
        "Mars", "Titan", "Krypton", "Tatooine", "Pandora",
        nullptr
    };
    Enumeration_knob(f, &skyPreset, skyNames, "sky_preset", "sky");
    Tooltip(f, "EARTH ATMOSPHERES\n"
               "  Clear Day -- Blue sky, sharp shadows.\n"
               "  Golden Hour -- Low sun, warm amber, long shadows.\n"
               "  Red Sky Dawn -- Deep red pre-sunrise, max Rayleigh scattering.\n"
               "  Sunrise -- Pink-orange horizon, cool blue zenith.\n"
               "  Overcast -- Even, shadowless, cool. Soft natural light.\n"
               "  Blue Hour -- Deep blue, no direct sun. ~20min after sunset.\n"
               "  Moonlit -- Silver-blue, very dim.\n"
               "  Starlight -- Near-total darkness.\n"
               "  Alpine -- High altitude, intense UV, vivid blue.\n"
               "  Desert Noon -- Harsh overhead, almost no atmosphere.\n"
               "  Arctic Twilight -- Low sun, long golden wash.\n\n"
               "PLANETARY\n"
               "  Mars -- Butterscotch sky, blue sunset (Curiosity rover data).\n"
               "  Titan -- Orange methane haze, Saturn's largest moon.\n"
               "  Krypton -- Deep crimson sky, red sun (DC Comics).\n"
               "  Tatooine -- Twin amber suns, dusty (Star Wars).\n"
               "  Pandora -- Blue-violet bioluminescence (Avatar).\n\n"
               "Off = HDRI only. Custom = manual sun position.");

    Double_knob(f, &sunIntensity, "sun_intensity", "sun");
    SetRange(f, 0, 20);
    Tooltip(f, "Sun brightness (squared for exponential response).");
    Double_knob(f, &skyIntensity, "sky_intensity", "sky fill");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 20);
    Tooltip(f, "Sky dome fill brightness.");

    Double_knob(f, &sunElevation, "sun_elevation", "elevation");
    SetRange(f, -10, 90);
    Tooltip(f, "Sun angle above horizon in degrees.");
    Double_knob(f, &sunAzimuth, "sun_azimuth", "azimuth");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 360);
    Tooltip(f, "Sun compass direction in degrees.");

    BeginClosedGroup(f, "grp_location", "Location and time of day");
    {
        static const char* const locNames[] = {
            "Custom", "London", "New York", "Los Angeles", "Tokyo",
            "Sydney", "Paris", "Dubai", "Reykjavik", "Cape Town",
            "Mumbai", "North Pole", "Quito", nullptr
        };
        Enumeration_knob(f, &locationPreset, locNames, "location_preset", "location");
        Double_knob(f, &latitude, "latitude", "lat"); SetRange(f, -90, 90);
        Double_knob(f, &longitude, "longitude", "lon");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, -180, 180);
        Double_knob(f, &timeOfDay, "time_of_day", "time"); SetRange(f, 0, 24);
        Int_knob(f, &dayOfYear, "day_of_year", "day");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 1, 365);
        Button(f, "compute_sun", "Compute Sun Position");
    }
    EndGroup(f);

    Divider(f, "HDRI Environment Map");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Image-based lighting from .hdr or .exr environment maps.<br>"
        "Composes additively with the sky model above.<br>"
        "Use HDRI Haven, Poly Haven, or sIBL Archive for free HDRIs."
        "</font>"
    );
    Newline(f);
    File_knob(f, &hdriFile, "hdri_file", "HDRI file");
    Tooltip(f, "Load an HDRI environment map (.hdr, .exr).\n"
               "Creates a dome light for image-based lighting.\n"
               "Typical workflow: load HDRI, adjust intensity and rotation\n"
               "to match your plate's lighting direction.");
    Double_knob(f, &hdriIntensity, "hdri_intensity", "intensity");
    SetRange(f, 0, 20); SetFlags(f, Knob::LOG_SLIDER);
    Double_knob(f, &hdriRotate, "hdri_rotate", "rotate");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 360);

    BeginClosedGroup(f, "grp_env", "Environment settings");
    {
        Double_knob(f, &envIntensity, "env_intensity", "env intensity");
        SetRange(f, 0, 10); SetFlags(f, Knob::LOG_SLIDER);
        Double_knob(f, &envDiffuse, "env_diffuse", "env diffuse");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 2);
        static const char* const envModes[] = {
            "Average colour (fast)", "SH + Virtual Lights", nullptr
        };
        Enumeration_knob(f, &envMode, envModes, "env_mode", "mode");
        Int_knob(f, &envVirtualLights, "env_virtual_lights", "virtual lights");
        ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 4);
        Bool_knob(f, &useReSTIR, "use_restir", "ReSTIR sampling");
    }
    EndGroup(f);

    // --- Display ---
    Divider(f, "3D Display");
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "View this node directly in a 3D viewer to see the dome,<br>"
        "sun arrow, and compass. Not visible through GeoScene."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &showDome, "show_dome", "dome");
    Tooltip(f, "Hemisphere wireframe in the 3D viewer.");
    Bool_knob(f, &showSunArrow, "show_sun_arrow", "sun arrow");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "Arrow pointing toward the sun position.");
    Bool_knob(f, &showCompass, "show_compass", "compass");
    ClearFlags(f, Knob::STARTLINE);
    Tooltip(f, "N/S/E/W compass markers at dome base.");
    Double_knob(f, &domeRadius, "dome_radius", "radius");
    SetRange(f, 10, 500);
    Tooltip(f, "Dome display radius in world units.");

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralEnvLight - SpectralRenderer for Nuke 17"
                 "</font>");
}

int SpectralEnvLight::knob_changed(Knob* k)
{
    if (k->is("compute_sun")) {
        ComputeSunPosition();
        if (Knob* kn = knob("sun_elevation")) kn->set_value(sunElevation);
        if (Knob* kn = knob("sun_azimuth")) kn->set_value(sunAzimuth);
        return 1;
    }
    if (k->is("location_preset") && locationPreset > 0) {
        static const double locs[][2] = {
            {0,0}, {51.5,-0.12}, {40.71,-74.01}, {34.05,-118.24},
            {35.68,139.69}, {-33.87,151.21}, {48.86,2.35},
            {25.20,55.27}, {64.13,-21.90}, {-33.92,18.42},
            {19.08,72.88}, {90,0}, {-0.18,-78.47}
        };
        if (locationPreset < 13) {
            latitude = locs[locationPreset][0];
            longitude = locs[locationPreset][1];
            if (Knob* kn = knob("latitude")) kn->set_value(latitude);
            if (Knob* kn = knob("longitude")) kn->set_value(longitude);
            ComputeSunPosition();
            if (Knob* kn = knob("sun_elevation")) kn->set_value(sunElevation);
            if (Knob* kn = knob("sun_azimuth")) kn->set_value(sunAzimuth);
        }
        return 1;
    }
    if (k->is("sky_preset") && skyPreset > 1) {
        struct SP { double elev,az,sunI,skyI; };
        static const SP presets[] = {
            {},{}, {55,180,5,1}, {5,250,4,0.6}, {-2,90,3,0.4},
            {2,95,3.5,0.5}, {40,180,0.5,2}, {-5,270,0.1,0.3},
            {-15,180,0.01,0.08}, {-20,180,0,0.01},
            {50,180,6,1.5}, {75,180,8,0.8}, {3,180,0.3,0.2},
            {30,180,3,0.5}, {15,180,1,0.3}, {35,180,2,0.4},
            {25,200,4,0.6}, {40,180,2,0.7},
        };
        int n = sizeof(presets)/sizeof(presets[0]);
        if (skyPreset < n) {
            const auto& s = presets[skyPreset];
            sunElevation=s.elev; sunAzimuth=s.az; sunIntensity=s.sunI; skyIntensity=s.skyI;
            if (Knob* kn = knob("sun_elevation")) kn->set_value(sunElevation);
            if (Knob* kn = knob("sun_azimuth")) kn->set_value(sunAzimuth);
            if (Knob* kn = knob("sun_intensity")) kn->set_value(sunIntensity);
            if (Knob* kn = knob("sky_intensity")) kn->set_value(skyIntensity);
        }
        return 1;
    }
    return SourceGeomOp::knob_changed(k);
}
