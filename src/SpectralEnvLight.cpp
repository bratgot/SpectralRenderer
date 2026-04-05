// SpectralEnvLight — environment/sky lighting for SpectralRender
// Created by Marten Blumen

#include "SpectralEnvLight.h"
#include <DDImage/Knobs.h>
#include <cmath>

using namespace DD::Image;

const char* const SpectralEnvLight::CLASS = "SpectralEnvLight";
static Op* build(Node* node) { return new SpectralEnvLight(node); }
const Op::Description SpectralEnvLight::description(CLASS, build);

SpectralEnvLight::SpectralEnvLight(Node* node) : ShaderOp(node) {}

const char* SpectralEnvLight::node_help() const
{
    return "SpectralEnvLight \xe2\x80\x94 Environment & Sky Lighting\n\n"
           "Sun/sky model with 18 presets including planetary atmospheres.\n"
           "Solar position from latitude, longitude, and time of day.\n"
           "HDRI environment map loading with rotation and intensity.\n\n"
           "CONNECTION\n"
           "  SpectralEnvLight -> GeoScene -> SpectralRender (scn)\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

void SpectralEnvLight::ComputeSunPosition()
{
    double B = (360.0 / 365.0) * (dayOfYear - 81) * 3.14159265 / 180.0;
    double dec = 23.45 * std::sin(B);
    double eot = 9.87*std::sin(2*B) - 7.53*std::cos(B) - 1.5*std::sin(B);
    double solarTime = timeOfDay + eot/60.0 + longitude/15.0;
    double hourAngle = (solarTime - 12.0) * 15.0;
    double latR = latitude * 3.14159265 / 180.0;
    double decR = dec * 3.14159265 / 180.0;
    double haR = hourAngle * 3.14159265 / 180.0;
    double sinElev = std::sin(latR)*std::sin(decR) + std::cos(latR)*std::cos(decR)*std::cos(haR);
    sunElevation = std::asin(std::max(-1.0, std::min(1.0, sinElev))) * 180.0 / 3.14159265;
    double cosAz = (std::sin(decR) - std::sin(latR)*sinElev) / (std::cos(latR)*std::max(std::cos(std::asin(sinElev)), 0.001));
    sunAzimuth = std::acos(std::max(-1.0, std::min(1.0, cosAz))) * 180.0 / 3.14159265;
    if (hourAngle > 0) sunAzimuth = 360.0 - sunAzimuth;
}

void SpectralEnvLight::knobs(Knob_Callback f)
{
    // ─── Header ─────────────────────────────────────────────────────
    Text_knob(f, "<b>Environment Light</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Sky model, solar position, and HDRI environment maps.<br>"
        "Connect to GeoScene alongside SpectralVDBRead."
        "</font>"
    );

    // ─── Sky Preset ─────────────────────────────────────────────────
    Divider(f, "Sky");
    static const char* const skyNames[] = {
        "Off", "Custom", "Clear Day", "Golden Hour", "Red Sky Dawn",
        "Sunrise", "Overcast", "Blue Hour", "Moonlit", "Starlight",
        "Alpine Light", "Desert Noon", "Arctic Twilight",
        "Mars", "Titan", "Krypton", "Tatooine", "Pandora",
        nullptr
    };
    Enumeration_knob(f, &skyPreset, skyNames, "sky_preset", "sky");
    Tooltip(f, "Physically-inspired sky presets.\n\n"
               "Earth atmospheres: Clear Day, Golden Hour, Sunrise, etc.\n"
               "Planetary: Mars (butterscotch), Titan (orange methane),\n"
               "Krypton (red crimson), Tatooine (twin amber), Pandora (blue-violet).\n\n"
               "Off = no built-in sky (use HDRI only).\n"
               "Custom = manual sun elevation and azimuth.");

    Double_knob(f, &sunIntensity, "sun_intensity", "sun");
    SetRange(f, 0, 20);
    Tooltip(f, "Sun brightness (squared for exponential response).\n"
               "0 = no direct sun. 3 = soft. 5 = typical. 10+ = harsh.");
    Double_knob(f, &skyIntensity, "sky_intensity", "sky fill");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0, 20);
    Tooltip(f, "Sky dome brightness (squared for exponential response).\n"
               "Fills shadows with sky colour. 0 = no fill. 1 = natural.");

    Double_knob(f, &sunElevation, "sun_elevation", "elevation");
    SetRange(f, -10, 90);
    Tooltip(f, "Sun angle above horizon in degrees.\n"
               "0 = horizon. 45 = afternoon. 90 = noon overhead.\n"
               "Negative = below horizon (dawn/twilight colours).");
    Double_knob(f, &sunAzimuth, "sun_azimuth", "azimuth");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0, 360);
    Tooltip(f, "Sun compass direction in degrees.\n"
               "0 = north. 90 = east. 180 = south. 270 = west.");

    // ─── Location ───────────────────────────────────────────────────
    BeginClosedGroup(f, "grp_location", "Location and time of day");
    {
        Text_knob(f,
            "<font color='#777' size='-1'>"
            "Compute sun position from real-world coordinates and time.<br>"
            "Uses Spencer 1971 solar position algorithm."
            "</font>"
        );
        Newline(f);
        static const char* const locNames[] = {
            "Custom", "London", "New York", "Los Angeles", "Tokyo",
            "Sydney", "Paris", "Dubai", "Reykjavik", "Cape Town",
            "Mumbai", "North Pole", "Quito", nullptr
        };
        Enumeration_knob(f, &locationPreset, locNames, "location_preset", "location");
        Tooltip(f, "Named locations with pre-set latitude/longitude.\n"
                   "Select Custom to enter your own coordinates.");
        Double_knob(f, &latitude, "latitude", "lat");
        SetRange(f, -90, 90);
        Tooltip(f, "Latitude in degrees. Positive = north. Negative = south.\n"
                   "London = 51.5. NYC = 40.7. Sydney = -33.9.");
        Double_knob(f, &longitude, "longitude", "lon");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, -180, 180);
        Tooltip(f, "Longitude in degrees. Positive = east. Negative = west.\n"
                   "London = -0.12. NYC = -74. Tokyo = 139.7.");
        Double_knob(f, &timeOfDay, "time_of_day", "time");
        SetRange(f, 0, 24);
        Tooltip(f, "Local solar time (24h clock).\n"
                   "6 = sunrise. 12 = noon. 18 = sunset.");
        Int_knob(f, &dayOfYear, "day_of_year", "day");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 1, 365);
        Tooltip(f, "Day of year (1-365). Affects sun path.\n"
                   "80 = spring equinox. 172 = summer solstice.\n"
                   "266 = autumn equinox. 355 = winter solstice.");
        Button(f, "compute_sun", "Compute Sun Position");
        Tooltip(f, "Calculate sun elevation and azimuth from\n"
                   "the latitude, longitude, time, and day above.");
    }
    EndGroup(f);

    // ─── HDRI ───────────────────────────────────────────────────────
    Divider(f, "HDRI Environment Map");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Load an .hdr or .exr file for image-based lighting.<br>"
        "Composes additively with the sky model above."
        "</font>"
    );
    Newline(f);
    File_knob(f, &hdriFile, "hdri_file", "HDRI file");
    Tooltip(f, "Load an HDRI environment map (.hdr, .exr).\n"
               "Creates a dome light for image-based lighting.");
    Double_knob(f, &hdriIntensity, "hdri_intensity", "intensity");
    SetRange(f, 0, 20); SetFlags(f, Knob::LOG_SLIDER);
    Tooltip(f, "HDRI brightness multiplier.\n"
               "1 = as-authored. 3-5 = typical for volumes.");
    Double_knob(f, &hdriRotate, "hdri_rotate", "rotate");
    ClearFlags(f, Knob::STARTLINE);
    SetRange(f, 0, 360);
    Tooltip(f, "Rotate HDRI horizontally in degrees.\n"
               "Repositions the sun/highlights.");

    // ─── Environment Controls ───────────────────────────────────────
    BeginClosedGroup(f, "grp_env", "Environment settings");
    {
        Double_knob(f, &envIntensity, "env_intensity", "env intensity");
        SetRange(f, 0, 10); SetFlags(f, Knob::LOG_SLIDER);
        Tooltip(f, "Overall environment brightness multiplier.\n"
                   "Affects both sky and HDRI. 1 = natural.");
        Double_knob(f, &envDiffuse, "env_diffuse", "env diffuse");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 0, 2);
        Tooltip(f, "Environment contribution to diffuse illumination.\n"
                   "0 = off. 0.5 = natural. 1 = full. >1 = boosted.");
        Divider(f, "");
        static const char* const envModes[] = {
            "Average colour (fast)", "SH + Virtual Lights", nullptr
        };
        Enumeration_knob(f, &envMode, envModes, "env_mode", "mode");
        Tooltip(f, "Average colour: mean HDRI colour for isotropic ambient.\n"
                   "SH + Virtual Lights: directional sampling via\n"
                   "spherical harmonics + brightest peaks as lights.");
        Int_knob(f, &envVirtualLights, "env_virtual_lights", "virtual lights");
        ClearFlags(f, Knob::STARTLINE);
        SetRange(f, 0, 4);
        Tooltip(f, "HDRI peaks extracted as virtual directional lights.\n"
                   "0 = ambient only. 2 = recommended. 4 = max.");
        Bool_knob(f, &useReSTIR, "use_restir", "ReSTIR sampling");
        Tooltip(f, "Importance sampling for env lighting.\n"
                   "Currently stubbed -- full implementation planned.");
    }
    EndGroup(f);

    // ─── Footer ─────────────────────────────────────────────────────
    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralEnvLight \xc2\xb7 SpectralRenderer for Nuke 17"
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
    // Sky presets set elevation/intensity
    if (k->is("sky_preset") && skyPreset > 1) {
        struct SP { double elev; double az; double sunI; double skyI; };
        static const SP presets[] = {
            {}, {},             // 0=off, 1=custom
            {55,180,5,1},      // Clear Day
            {5,250,4,0.6},     // Golden Hour
            {-2,90,3,0.4},     // Red Sky Dawn
            {2,95,3.5,0.5},    // Sunrise
            {40,180,0.5,2},    // Overcast
            {-5,270,0.1,0.3},  // Blue Hour
            {-15,180,0.01,0.08}, // Moonlit
            {-20,180,0,0.01},  // Starlight
            {50,180,6,1.5},    // Alpine
            {75,180,8,0.8},    // Desert Noon
            {3,180,0.3,0.2},   // Arctic Twilight
            {30,180,3,0.5},    // Mars
            {15,180,1,0.3},    // Titan
            {35,180,2,0.4},    // Krypton
            {25,200,4,0.6},    // Tatooine
            {40,180,2,0.7},    // Pandora
        };
        int n = sizeof(presets)/sizeof(presets[0]);
        if (skyPreset < n) {
            const auto& s = presets[skyPreset];
            sunElevation = s.elev; sunAzimuth = s.az;
            sunIntensity = s.sunI; skyIntensity = s.skyI;
            if (Knob* kn = knob("sun_elevation")) kn->set_value(sunElevation);
            if (Knob* kn = knob("sun_azimuth")) kn->set_value(sunAzimuth);
            if (Knob* kn = knob("sun_intensity")) kn->set_value(sunIntensity);
            if (Knob* kn = knob("sky_intensity")) kn->set_value(skyIntensity);
        }
        return 1;
    }
    return ShaderOp::knob_changed(k);
}
