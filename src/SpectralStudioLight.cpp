// SpectralStudioLight — three-point studio lighting for SpectralRender
// 36 presets: photography, cinematography, equipment, famous styles
// Created by Marten Blumen

#include "SpectralStudioLight.h"
#include <DDImage/Knobs.h>
#include <DDImage/ViewerContext.h>
#include <DDImage/gl.h>
#include "usg/geom/PointsPrim.h"
#include <cmath>

using namespace DD::Image;
using namespace fdk;
using namespace usg;

const char* const SpectralStudioLight::CLASS = "SpectralStudioLight";
static Op* buildStudio(Node* node) { return new SpectralStudioLight(node); }
const GeomOp::Description SpectralStudioLight::description(CLASS, buildStudio);

SpectralStudioLight::SpectralStudioLight(Node* node)
    : SourceGeomOp(node, BuildEngine<Engine>()) {}

void SpectralStudioLight::Engine::createPrims(GeomSceneContext& context, const Path& path)
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
    prim.setCustomData("spectralIsStudioLight", Value(true));
}

const char* SpectralStudioLight::node_help() const
{
    return "SpectralStudioLight -- Studio Lighting\n\n"
           "Professional three-point rig with 36 presets covering\n"
           "photography, cinematography, equipment, and famous DP styles.\n\n"
           "CONNECTION\n"
           "  Connect directly to GeoScene as a separate input.\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

// ---------------------------------------------------------------------------
// Preset data
// ---------------------------------------------------------------------------
struct SP {
    double kI,kE,kA; float kR,kG,kB;
    double fR; float fCR,fCG,fCB;
    double rI; float rCR,rCG,rCB;
    double ss;
};

// Category ranges: presets[catStart[c]] .. presets[catStart[c+1]-1]
static const int kCatStart[] = { 0, 1, 8, 12, 24, 27, 36 };
static const int kNumCats = 5;

//                             kI   kE   kA    kR    kG    kB    fR    fCR   fCG   fCB   rI    rCR   rCG   rCB   ss
static const SP kP[] = {
    {},  // 0: Custom

    // ── 1: Photography Studio (1-7) ──
    {5,   45,  45,  1.f,  0.95f,0.88f, 0.35, 0.85f,0.9f, 1.f,   2,   1.f,  1.f,  1.f,   0.3},   // Portrait (Rembrandt)
    {6,   55,  10,  1.f,  0.98f,0.95f, 0.6,  0.95f,0.95f,1.f,   1.5, 1.f,  1.f,  1.f,   0.5},   // Beauty Dish
    {8,   40,  30,  1.f,  0.97f,0.92f, 0.15, 0.8f, 0.85f,1.f,   4,   1.f,  0.95f,0.9f,  0.2},   // Vogue
    {10,  60,  45,  1.f,  1.f,  1.f,   0.2,  1.f,  1.f,  1.f,   5,   1.f,  1.f,  1.f,   0.05},  // Product
    {4,   40,  30,  1.f,  0.98f,0.95f, 0.5,  0.9f, 0.93f,1.f,   1.5, 1.f,  1.f,  1.f,   0.6},   // Corporate
    {5,   50,  5,   1.f,  0.98f,0.96f, 0.7,  1.f,  0.98f,0.95f, 1,   1.f,  1.f,  1.f,   0.7},   // Instagram
    {3,   30,  60,  1.f,  0.92f,0.82f, 0.5,  1.f,  0.95f,0.9f,  1,   1.f,  0.95f,0.85f, 0.8},   // Boudoir

    // ── 2: Photography Field (8-11) ──
    {12,  25,  15,  1.f,  0.97f,0.9f,  0.05, 0.7f, 0.8f, 1.f,   0.5, 1.f,  1.f,  1.f,   0.05},  // Nat Geo (Nichols)
    {15,  10,  0,   1.f,  0.95f,0.85f, 0.02, 0.6f, 0.7f, 0.9f,  0.3, 1.f,  1.f,  1.f,   0.0},   // Remote Flash
    {3,   45,  90,  1.f,  0.98f,0.93f, 0.6,  0.9f, 0.95f,1.f,   0.5, 1.f,  1.f,  1.f,   0.7},   // Documentary
    {6,   70,  0,   1.f,  1.f,  1.f,   0.9,  1.f,  1.f,  1.f,   0.2, 1.f,  1.f,  1.f,   0.9},   // Macro Ring

    // ── 3: Equipment (12-23) ──
    {5,   40,  45,  1.f,  0.98f,0.95f, 0.3,  0.9f, 0.93f,1.f,   1.5, 1.f,  1.f,  1.f,   0.7},   // Large Softbox
    {6,   45,  70,  1.f,  0.97f,0.93f, 0.15, 0.85f,0.9f, 1.f,   3,   1.f,  1.f,  1.f,   0.4},   // Strip Softbox
    {4,   40,  45,  1.f,  0.98f,0.95f, 0.45, 0.92f,0.95f,1.f,   1,   1.f,  1.f,  1.f,   0.65},  // Umbrella Shoot-Through
    {7,   40,  45,  1.f,  0.97f,0.93f, 0.3,  0.9f, 0.93f,1.f,   2,   1.f,  1.f,  1.f,   0.5},   // Umbrella Bounce
    {6,   55,  15,  1.f,  0.98f,0.95f, 0.5,  0.93f,0.95f,1.f,   1.5, 1.f,  1.f,  1.f,   0.45},  // Beauty Dish
    {5,   60,  0,   1.f,  1.f,  1.f,   0.85, 1.f,  1.f,  1.f,   0.3, 1.f,  1.f,  1.f,   0.95},  // Ring Light
    {3,   50,  90,  1.f,  0.99f,0.97f, 0.6,  0.95f,0.97f,1.f,   0.5, 1.f,  1.f,  1.f,   0.85},  // Scrim / Silk
    {10,  45,  45,  1.f,  0.97f,0.92f, 0.1,  0.8f, 0.85f,1.f,   3,   1.f,  1.f,  1.f,   0.02},  // Fresnel Spot
    {4,   25,  60,  1.f,  0.83f,0.57f, 0.3,  1.f,  0.87f,0.65f, 1,   1.f,  0.85f,0.6f,  0.5},   // Tungsten 2800K
    {4,   40,  30,  0.95f,0.97f,1.f,   0.5,  0.93f,0.95f,1.f,   1,   0.95f,0.97f,1.f,   0.75},  // Kino Flo
    {6,   45,  45,  1.f,  0.97f,0.92f, 0.0,  0.3f, 0.3f, 0.35f, 3,   1.f,  1.f,  1.f,   0.2},   // Negative Fill
    {6,   65,  0,   1.f,  0.98f,0.95f, 0.6,  1.f,  0.98f,0.95f, 1,   1.f,  1.f,  1.f,   0.5},   // Butterfly

    // ── 4: Cinema Styles (24-26) ──
    {8,   70,  80,  1.f,  0.95f,0.88f, 0.02, 0.6f, 0.65f,0.8f,  4,   1.f,  0.95f,0.9f,  0.0},   // Film Noir
    {6,   50,  45,  1.f,  0.97f,0.92f, 0.35, 0.9f, 0.92f,1.f,   3,   1.f,  1.f,  1.f,   0.3},   // Golden Age
    {10,  60,  90,  1.f,  0.92f,0.78f, 0.01, 0.4f, 0.35f,0.3f,  0.3, 1.f,  0.9f, 0.7f,  0.0},   // Chiaroscuro

    // ── 5: Famous DPs (27-35) ──
    {7,   35,  60,  1.f,  0.97f,0.9f,  0.3,  0.85f,0.9f, 1.f,   3,   1.f,  0.98f,0.95f, 0.25},  // Kaminski
    {12,  20,  45,  1.f,  0.9f, 0.7f,  0.1,  0.5f, 0.75f,1.f,   5,   1.f,  0.85f,0.6f,  0.05},  // Bay
    {5,   30,  110, 0.9f, 1.f,  0.8f,  0.3,  0.7f, 0.5f, 1.f,   2,   0.8f, 1.f,  0.9f,  0.4},   // Doyle
    {4,   40,  75,  1.f,  0.98f,0.94f, 0.4,  0.88f,0.92f,1.f,   1.5, 1.f,  1.f,  1.f,   0.5},   // Deakins
    {4,   80,  30,  1.f,  0.93f,0.82f, 0.05, 0.5f, 0.45f,0.4f,  0.5, 1.f,  0.95f,0.85f, 0.15},  // Willis
    {6,   35,  50,  1.f,  0.88f,0.7f,  0.25, 0.7f, 0.8f, 1.f,   2.5, 1.f,  0.85f,0.6f,  0.35},  // Storaro
    {3,   25,  100, 1.f,  0.97f,0.88f, 0.6,  0.9f, 0.95f,1.f,   0.5, 1.f,  0.98f,0.9f,  0.8},   // Lubezki
    {2.5, 30,  70,  1.f,  0.93f,0.82f, 0.25, 0.7f, 0.75f,0.85f, 0.8, 1.f,  0.95f,0.88f, 0.6},   // Bradford Young
    {4,   40,  60,  0.93f,0.96f,1.f,   0.35, 0.85f,0.9f, 1.f,   1.5, 0.9f, 0.95f,1.f,   0.45},  // van Hoytema
};
static const int kPC = sizeof(kP)/sizeof(kP[0]);

// ---------------------------------------------------------------------------
// 3D Viewport
// ---------------------------------------------------------------------------
static const double kPi = 3.14159265358979;

void SpectralStudioLight::build_handles(ViewerContext* ctx)
{
    if (ctx->transform_mode() == VIEWER_2D) return;
    add_draw_handle(ctx);
}

void SpectralStudioLight::draw_handle(ViewerContext* ctx)
{
    float R = float(rigRadius);
    float elR=float(keyElevation)*float(kPi)/180.f;
    float azR=float(keyAzimuth)*float(kPi)/180.f;
    float kx=R*std::cos(elR)*std::sin(azR), ky=R*std::sin(elR), kz=R*std::cos(elR)*std::cos(azR);
    float fAz=azR+float(kPi), fEl=elR*0.3f;
    float fx=R*0.8f*std::cos(fEl)*std::sin(fAz), fy=R*0.8f*std::sin(fEl), fz=R*0.8f*std::cos(fEl)*std::cos(fAz);
    float rAz=azR+float(kPi), rEl=elR*0.8f;
    float rx=R*std::cos(rEl)*std::sin(rAz), ry=R*std::sin(rEl), rz=R*std::cos(rEl)*std::cos(rAz);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING); glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    float b=float(mix);

    // Ground circle
    glColor4f(0.3f,0.3f,0.35f,0.2f); glLineWidth(1.f);
    glBegin(GL_LINE_LOOP);
    for(int i=0;i<48;++i){float a=float(i)/48.f*2.f*float(kPi);glVertex3f(R*std::cos(a),0,R*std::sin(a));}
    glEnd();

    if (showLights) {
        // Key
        glColor4f(keyColor[0],keyColor[1],keyColor[2],0.8f*b);
        glPointSize(14.f); glBegin(GL_POINTS);glVertex3f(kx,ky,kz);glEnd();
        glColor4f(keyColor[0],keyColor[1],keyColor[2],0.25f*b);
        glPointSize(24.f); glBegin(GL_POINTS);glVertex3f(kx,ky,kz);glEnd();
        glColor4f(keyColor[0],keyColor[1],keyColor[2],0.3f*b); glLineWidth(2.f);
        glBegin(GL_LINES);glVertex3f(kx,ky,kz);glVertex3f(0,0,0);glEnd();
        // Fill
        glColor4f(fillColor[0],fillColor[1],fillColor[2],0.6f*b);
        glPointSize(10.f); glBegin(GL_POINTS);glVertex3f(fx,fy,fz);glEnd();
        glColor4f(fillColor[0],fillColor[1],fillColor[2],0.2f*b); glLineWidth(1.f);
        glBegin(GL_LINES);glVertex3f(fx,fy,fz);glVertex3f(0,0,0);glEnd();
        // Rim
        glColor4f(rimColor[0],rimColor[1],rimColor[2],0.7f*b);
        glPointSize(10.f); glBegin(GL_POINTS);glVertex3f(rx,ry,rz);glEnd();
        glColor4f(rimColor[0],rimColor[1],rimColor[2],0.15f*b); glLineWidth(1.f);
        glBegin(GL_LINES);glVertex3f(rx,ry,rz);glVertex3f(0,0,0);glEnd();
    }

    if (showCones) {
        float ca=15.f+float(shadowSoftness)*20.f;
        auto cone=[&](float ox,float oy,float oz,float cr,float cg,float cb,float al){
            float dx=-ox,dy=-oy,dz=-oz;
            float len=std::sqrt(dx*dx+dy*dy+dz*dz); if(len<1e-4f)return;
            dx/=len;dy/=len;dz/=len;
            float cl=len*0.35f,br=cl*std::tan(ca*float(kPi)/180.f);
            float bx=ox+dx*cl,by=oy+dy*cl,bz=oz+dz*cl;
            float px,py,pz;
            if(std::abs(dy)<0.9f){px=-dz;py=0;pz=dx;}else{px=1;py=0;pz=0;}
            float pl=std::sqrt(px*px+py*py+pz*pz);if(pl>1e-6f){px/=pl;py/=pl;pz/=pl;}
            float qx=dy*pz-dz*py,qy=dz*px-dx*pz,qz=dx*py-dy*px;
            glColor4f(cr,cg,cb,al); glLineWidth(1.f);
            for(int i=0;i<8;++i){
                float a=float(i)/8.f*2.f*float(kPi);
                glBegin(GL_LINES);glVertex3f(ox,oy,oz);
                glVertex3f(bx+br*(px*std::cos(a)+qx*std::sin(a)),
                           by+br*(py*std::cos(a)+qy*std::sin(a)),
                           bz+br*(pz*std::cos(a)+qz*std::sin(a)));
                glEnd();
            }
        };
        cone(kx,ky,kz,keyColor[0],keyColor[1],keyColor[2],0.12f*b);
        cone(fx,fy,fz,fillColor[0],fillColor[1],fillColor[2],0.06f*b);
        cone(rx,ry,rz,rimColor[0],rimColor[1],rimColor[2],0.06f*b);
    }

    // Subject crosshair
    float cm=R*0.03f;
    glColor4f(0.7f,0.7f,0.7f,0.4f); glLineWidth(1.f);
    glBegin(GL_LINES);
    glVertex3f(-cm,0,0);glVertex3f(cm,0,0);
    glVertex3f(0,-cm,0);glVertex3f(0,cm,0);
    glVertex3f(0,0,-cm);glVertex3f(0,0,cm);
    glEnd();

    glPopAttrib();
}

// ---------------------------------------------------------------------------
// Knobs — category + preset two-level system
// ---------------------------------------------------------------------------
void SpectralStudioLight::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>Studio Light</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Professional three-point rig with 36 presets.<br>"
        "Select a category then choose a preset within it.<br>"
        "3D display visible when viewing this node in a 3D viewer."
        "</font>"
    );

    // ─── Category + Preset ──────────────────────────────────────────
    Divider(f, "Preset");
    static const char* const catNames[] = {
        "Photography Studio", "Photography Field", "Equipment",
        "Cinema Styles", "Famous Cinematographers", nullptr
    };
    Enumeration_knob(f, &presetCategory, catNames, "preset_category", "category");
    Tooltip(f, "Select a category to browse presets.\n\n"
               "Photography Studio: Portrait, Fashion, Product, Corporate\n"
               "Photography Field: Nat Geo, Wildlife, Documentary\n"
               "Equipment: Softbox, Umbrella, Fresnel, Kino, Scrim\n"
               "Cinema Styles: Film Noir, Golden Age, Chiaroscuro\n"
               "Famous DPs: Deakins, Storaro, Willis, Lubezki, Doyle");

    // ── Photography Studio presets ──
    static const char* const photoStudio[] = {
        "Select...",
        "Portrait Classic (Rembrandt)",
        "Beauty Dish Fashion",
        "Vogue Editorial",
        "Product Shot",
        "Corporate Headshot",
        "Instagram Beauty",
        "Boudoir",
        nullptr
    };
    // ── Photography Field presets ──
    static const char* const photoField[] = {
        "Select...",
        "Nat Geo Wildlife (Nichols)",
        "Remote Flash Wildlife",
        "Documentary Natural",
        "Macro Ring Flash",
        nullptr
    };
    // ── Equipment presets ──
    static const char* const equipment[] = {
        "Select...",
        "Large Softbox (4x6ft)",
        "Strip Softbox",
        "Umbrella Shoot-Through",
        "Umbrella Bounce Silver",
        "Beauty Dish",
        "Ring Light",
        "Scrim / Silk Diffusion",
        "Fresnel Spot",
        "Tungsten Practical (2800K)",
        "Kino Flo Bank",
        "Negative Fill (Flag)",
        "Butterfly / Clamshell",
        nullptr
    };
    // ── Cinema Styles presets ──
    static const char* const cinemaStyles[] = {
        "Select...",
        "Film Noir",
        "Golden Age Hollywood",
        "Chiaroscuro (Caravaggio)",
        nullptr
    };
    // ── Famous DPs presets ──
    static const char* const famousDPs[] = {
        "Select...",
        "Spielberg / Kaminski",
        "Michael Bay",
        "Christopher Doyle",
        "Roger Deakins",
        "Gordon Willis (Godfather)",
        "Vittorio Storaro",
        "Emmanuel Lubezki",
        "Bradford Young",
        "Hoyte van Hoytema",
        nullptr
    };

    Enumeration_knob(f, &presetPhotoStudio, photoStudio, "preset_photo_studio", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Portrait Classic -- Rembrandt triangle lighting. Named for the Dutch\n"
               "  master's signature nose shadow. Key at 45deg, cool fill.\n"
               "Beauty Dish Fashion -- The fashion photographer's workhorse since the 1980s.\n"
               "  Focused soft light from above, high fill ratio.\n"
               "Vogue Editorial -- Hard glamour inspired by Irving Penn and Richard Avedon.\n"
               "  Strong rim separation, low fill, dramatic contrast.\n"
               "Product Shot -- Clean, hard key. Maximum detail and edge definition.\n"
               "Corporate Headshot -- Soft, neutral, professional. Safe and flattering.\n"
               "Instagram Beauty -- Front-heavy ring-like setup, flat, glowy skin.\n"
               "Boudoir -- Very soft, warm, intimate, low contrast window-light feel.");
    Enumeration_knob(f, &presetPhotoField, photoField, "preset_photo_field", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Nat Geo Wildlife (Nick Nichols) -- Bold remote flash in dense jungle.\n"
               "  Hard light, near-zero fill. Pioneered camera-trap flash photography\n"
               "  for National Geographic. Known for photographing war elephants in\n"
               "  Chad and tigers by camera trap in India.\n"
               "Remote Flash Wildlife -- Bare flash ambush, no fill. Night wildlife.\n"
               "Documentary Natural -- Window-light simulation, soft and real.\n"
               "Macro Ring Flash -- Even, shadowless, clinical detail for close-ups.");
    Enumeration_knob(f, &presetEquipment, equipment, "preset_equipment", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Large Softbox (4x6ft) -- Broad wrap-around panel. Workhorse of portrait studios.\n"
               "Strip Softbox -- Narrow, sculpts edges and cheekbones.\n"
               "Umbrella Shoot-Through -- Broad, soft, forgiving. Great for groups.\n"
               "Umbrella Bounce Silver -- Harder than shoot-through, more specular punch.\n"
               "Beauty Dish -- Focused soft light, the fashion and beauty standard.\n"
               "Ring Light -- Flat, catch-light ring in eyes. YouTube/beauty/social media.\n"
               "Scrim / Silk -- Diffused natural light through fabric. Outdoor portraits.\n"
               "Fresnel Spot -- Hard, focusable theatre/film light. Sharp shadows.\n"
               "Tungsten Practical (2800K) -- Warm motivated light. Table lamps, candles.\n"
               "Kino Flo Bank -- Cool daylight fluorescent. Standard on film sets since the 1990s.\n"
               "Negative Fill (Flag) -- Blocker absorbs light, deepens shadow side.\n"
               "Butterfly / Clamshell -- Key above + fill below = beauty standard. Minimal nose shadow.");
    Enumeration_knob(f, &presetCinemaStyle, cinemaStyles, "preset_cinema_style", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Film Noir -- Born in 1940s Hollywood. Extreme high-angle key, deep shadows,\n"
               "  venetian blind patterns. Double Indemnity, The Third Man, Touch of Evil.\n"
               "Golden Age Hollywood -- Studio-era glamour. Butterfly shadow under nose,\n"
               "  strong backlight, star glow. The look that made legends.\n"
               "Chiaroscuro (Caravaggio) -- Single dramatic source, the rest falls to darkness.\n"
               "  Extreme contrast. Named for the Italian master's revolutionary paintings.");
    Enumeration_knob(f, &presetFamousDP, famousDPs, "preset_famous_dp", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Spielberg / Kaminski -- Janusz Kaminski's signature: overexposed windows,\n"
               "  warm practicals, naturalistic motivation. Schindler's List, Saving Private\n"
               "  Ryan, Lincoln. 2x Oscar winner.\n"
               "Michael Bay -- 'Bayhem'. Amber key vs teal fill, aggressive backlight,\n"
               "  lens flares everywhere. Transformers, Bad Boys, The Rock.\n"
               "Christopher Doyle -- Wong Kar-wai's DP. Neon-soaked, moody, unconventional\n"
               "  angles. In the Mood for Love, Chungking Express, Hero.\n"
               "Roger Deakins -- Master of motivated light. Every source has a reason.\n"
               "  Blade Runner 2049, 1917, No Country for Old Men, Skyfall. 2x Oscar.\n"
               "Gordon Willis -- 'The Prince of Darkness'. Deep underexposure, top-lit faces,\n"
               "  eyes hidden in shadow. The Godfather, All the President's Men, Manhattan.\n"
               "Vittorio Storaro -- Painterly. Colour as emotion: warm ambers vs cool blues\n"
               "  tell the story through light. Apocalypse Now, Last Tango, Last Emperor.\n"
               "Emmanuel Lubezki -- 'Chivo'. Natural light, golden hour obsession, long takes.\n"
               "  The Revenant, Gravity, Tree of Life, Birdman. 3x consecutive Oscar.\n"
               "Bradford Young -- Intimate underexposure, warm skin tones. Lets faces emerge\n"
               "  from darkness. Arrival, Selma, A Most Violent Year, Solo.\n"
               "Hoyte van Hoytema -- IMAX-native, cool atmospheric, desaturated palette.\n"
               "  Interstellar, Dunkirk, Oppenheimer, Tenet. Nolan's go-to DP.");

    Double_knob(f, &mix, "studio_mix", "mix");
    SetRange(f, 0, 1);
    Tooltip(f, "Overall studio light contribution.\n"
               "0 = off. 1 = full strength. Blend with environment lighting.");

    // ─── Key Light ──────────────────────────────────────────────────
    Divider(f, "Key Light");
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Main light. Defines the primary direction and mood."
        "</font>"
    );
    Newline(f);
    Double_knob(f, &keyIntensity, "key_intensity", "intensity"); SetRange(f, 0, 20);
    Tooltip(f, "Key light brightness.\n"
               "3 = soft/natural. 5 = standard. 8 = punchy. 12+ = hard flash.");
    Double_knob(f, &keyElevation, "key_elevation", "elevation"); SetRange(f, 0, 90);
    Tooltip(f, "Key light angle above horizon.\n"
               "25 = low drama. 35 = portrait. 60 = product. 80 = top-down/noir.");
    Double_knob(f, &keyAzimuth, "key_azimuth", "azimuth");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 360);
    Tooltip(f, "Key light direction.\n"
               "0 = front. 45 = classic 3/4. 90 = split. 180 = silhouette.");
    Color_knob(f, keyColor, "key_color", "colour");
    Tooltip(f, "Key light tint. Warm = tungsten. Neutral = daylight. Cool = moonlight.");

    // ─── Fill Light ─────────────────────────────────────────────────
    Divider(f, "Fill Light");
    Double_knob(f, &fillRatio, "fill_ratio", "fill ratio"); SetRange(f, 0, 1);
    Tooltip(f, "Fill brightness relative to key.\n"
               "0 = max contrast (noir). 0.3 = dramatic. 0.5 = natural. 1 = flat.");
    Color_knob(f, fillColor, "fill_color", "colour");
    Tooltip(f, "Fill tint. Cool blue lifts shadows naturally.");

    // ─── Rim Light ──────────────────────────────────────────────────
    Divider(f, "Rim Light");
    Double_knob(f, &rimIntensity, "rim_intensity", "intensity"); SetRange(f, 0, 10);
    Tooltip(f, "Rim brightness. 0 = off. 2 = subtle. 5 = dramatic halo.");
    Color_knob(f, rimColor, "rim_color", "colour");
    Tooltip(f, "Rim tint. White = clean. Warm = golden hour backlight.");

    // ─── Shadow ─────────────────────────────────────────────────────
    Divider(f, "Shadow");
    Double_knob(f, &shadowSoftness, "shadow_softness", "softness"); SetRange(f, 0, 1);
    Tooltip(f, "0 = point source. 0.3 = small softbox. 0.7 = large panel. 1 = huge silk.");

    // ─── Display ────────────────────────────────────────────────────
    Divider(f, "3D Display");
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "View this node in a 3D viewer to see the rig."
        "</font>"
    );
    Newline(f);
    Bool_knob(f, &showLights, "show_lights", "lights");
    Bool_knob(f, &showCones, "show_cones", "cones"); ClearFlags(f, Knob::STARTLINE);
    Double_knob(f, &rigRadius, "rig_radius", "radius"); SetRange(f, 10, 300);

    Divider(f, "");
    Text_knob(f, "<font size='1' color='#555'>"
                 "SpectralStudioLight - SpectralRenderer for Nuke 17"
                 "</font>");
}

// ---------------------------------------------------------------------------
// knob_changed — category switching + preset application
// ---------------------------------------------------------------------------
int SpectralStudioLight::knob_changed(Knob* k)
{
    // Category changed — show/hide the right preset enum
    if (k->is("preset_category")) {
        if (Knob* kn = knob("preset_photo_studio")) kn->visible(presetCategory == 0);
        if (Knob* kn = knob("preset_photo_field"))  kn->visible(presetCategory == 1);
        if (Knob* kn = knob("preset_equipment"))    kn->visible(presetCategory == 2);
        if (Knob* kn = knob("preset_cinema_style")) kn->visible(presetCategory == 3);
        if (Knob* kn = knob("preset_famous_dp"))    kn->visible(presetCategory == 4);
        return 1;
    }

    // Apply preset from any category
    int globalIdx = -1;
    if (k->is("preset_photo_studio") && presetPhotoStudio > 0)
        globalIdx = kCatStart[1] + presetPhotoStudio - 1;
    else if (k->is("preset_photo_field") && presetPhotoField > 0)
        globalIdx = kCatStart[2] + presetPhotoField - 1;
    else if (k->is("preset_equipment") && presetEquipment > 0)
        globalIdx = kCatStart[3] + presetEquipment - 1;
    else if (k->is("preset_cinema_style") && presetCinemaStyle > 0)
        globalIdx = kCatStart[4] + presetCinemaStyle - 1;
    else if (k->is("preset_famous_dp") && presetFamousDP > 0)
        globalIdx = kCatStart[5] + presetFamousDP - 1;

    if (globalIdx > 0 && globalIdx < kPC) {
        const auto& s = kP[globalIdx];
        keyIntensity=s.kI; keyElevation=s.kE; keyAzimuth=s.kA;
        keyColor[0]=s.kR; keyColor[1]=s.kG; keyColor[2]=s.kB;
        fillRatio=s.fR;
        fillColor[0]=s.fCR; fillColor[1]=s.fCG; fillColor[2]=s.fCB;
        rimIntensity=s.rI;
        rimColor[0]=s.rCR; rimColor[1]=s.rCG; rimColor[2]=s.rCB;
        shadowSoftness=s.ss;

        if (Knob* kn = knob("key_intensity"))   kn->set_value(keyIntensity);
        if (Knob* kn = knob("key_elevation"))   kn->set_value(keyElevation);
        if (Knob* kn = knob("key_azimuth"))     kn->set_value(keyAzimuth);
        if (Knob* kn = knob("key_color"))       { kn->set_value(s.kR,0); kn->set_value(s.kG,1); kn->set_value(s.kB,2); }
        if (Knob* kn = knob("fill_ratio"))      kn->set_value(fillRatio);
        if (Knob* kn = knob("fill_color"))      { kn->set_value(s.fCR,0); kn->set_value(s.fCG,1); kn->set_value(s.fCB,2); }
        if (Knob* kn = knob("rim_intensity"))   kn->set_value(rimIntensity);
        if (Knob* kn = knob("rim_color"))       { kn->set_value(s.rCR,0); kn->set_value(s.rCG,1); kn->set_value(s.rCB,2); }
        if (Knob* kn = knob("shadow_softness")) kn->set_value(shadowSoftness);
        return 1;
    }

    return SourceGeomOp::knob_changed(k);
}
