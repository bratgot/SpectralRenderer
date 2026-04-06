// SpectralStudioLight — three-point studio lighting for SpectralRender
// 47 presets: photography, cinematography, equipment, famous styles
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
           "Professional three-point rig with 47 presets.\n\n"
           "CONNECTION\n"
           "  Connect directly to GeoScene as a separate input.\n\n"
           "Created by Marten Blumen\n"
           "github.com/bratgot/SpectralRenderer";
}

// ---------------------------------------------------------------------------
// Preset data — 47 presets
// ---------------------------------------------------------------------------
struct SP {
    double kI,kE,kA; float kR,kG,kB;
    double fR; float fCR,fCG,fCB;
    double rI; float rCR,rCG,rCB;
    double ss;
};

// Category ranges into kP[]: [catStart[c] .. catStart[c+1]-1]
//   0=Custom, 1-11=PhotoStudio, 12-18=PhotoField, 19-30=Equipment, 31-37=CinemaStyles, 38-46=FamousDPs
static const int kCatStart[] = { 0, 1, 12, 19, 31, 38, 47 };

//                              kI   kE   kA    kR    kG    kB    fR    fCR   fCG   fCB   rI    rCR   rCG   rCB   ss
static const SP kP[] = {
    {},  // 0: Custom

    // ── Photography Studio (1-11) ────────────────────────────────────
    {5,   45,  45,  1.f,  0.95f,0.88f, 0.35, 0.85f,0.9f, 1.f,   2,   1.f,  1.f,  1.f,   0.3},   // 1: Portrait (Rembrandt)
    {6,   55,  10,  1.f,  0.98f,0.95f, 0.6,  0.95f,0.95f,1.f,   1.5, 1.f,  1.f,  1.f,   0.5},   // 2: Beauty Dish
    {8,   40,  30,  1.f,  0.97f,0.92f, 0.15, 0.8f, 0.85f,1.f,   4,   1.f,  0.95f,0.9f,  0.2},   // 3: Vogue
    {10,  60,  45,  1.f,  1.f,  1.f,   0.2,  1.f,  1.f,  1.f,   5,   1.f,  1.f,  1.f,   0.05},  // 4: Product
    {4,   40,  30,  1.f,  0.98f,0.95f, 0.5,  0.9f, 0.93f,1.f,   1.5, 1.f,  1.f,  1.f,   0.6},   // 5: Corporate
    {5,   50,  5,   1.f,  0.98f,0.96f, 0.7,  1.f,  0.98f,0.95f, 1,   1.f,  1.f,  1.f,   0.7},   // 6: Instagram
    {3,   30,  60,  1.f,  0.92f,0.82f, 0.5,  1.f,  0.95f,0.9f,  1,   1.f,  0.95f,0.85f, 0.8},   // 7: Boudoir
    // Famous studio photographers
    {7,   35,  55,  1.f,  0.95f,0.87f, 0.2,  0.8f, 0.85f,1.f,   3.5, 1.f,  0.97f,0.9f,  0.35},  // 8: Annie Leibovitz
    {4,   40,  45,  0.95f,0.95f,0.95f, 0.4,  0.9f, 0.9f, 0.9f,  1.5, 1.f,  1.f,  1.f,   0.55},  // 9: Peter Lindbergh
    {10,  50,  70,  1.f,  0.97f,0.92f, 0.05, 0.7f, 0.75f,0.85f, 4,   1.f,  1.f,  1.f,   0.05},  // 10: Helmut Newton
    {5,   30,  40,  1.f,  0.93f,0.8f,  0.35, 0.85f,0.88f,1.f,   2,   1.f,  0.95f,0.85f, 0.6},   // 11: Gregory Crewdson

    // ── Photography Field (12-18) ────────────────────────────────────
    {12,  25,  15,  1.f,  0.97f,0.9f,  0.05, 0.7f, 0.8f, 1.f,   0.5, 1.f,  1.f,  1.f,   0.05},  // 12: Nick Nichols
    {15,  10,  0,   1.f,  0.95f,0.85f, 0.02, 0.6f, 0.7f, 0.9f,  0.3, 1.f,  1.f,  1.f,   0.0},   // 13: Remote Flash
    {3,   45,  90,  1.f,  0.98f,0.93f, 0.6,  0.9f, 0.95f,1.f,   0.5, 1.f,  1.f,  1.f,   0.7},   // 14: Documentary
    {6,   70,  0,   1.f,  1.f,  1.f,   0.9,  1.f,  1.f,  1.f,   0.2, 1.f,  1.f,  1.f,   0.9},   // 15: Macro Ring
    // Famous field/documentary photographers
    {4,   35,  80,  1.f,  0.97f,0.9f,  0.5,  0.9f, 0.93f,1.f,   0.5, 1.f,  1.f,  1.f,   0.65},  // 16: Steve McCurry
    {6,   40,  60,  0.95f,0.93f,0.9f,  0.1,  0.7f, 0.7f, 0.7f,  1,   1.f,  1.f,  1.f,   0.3},   // 17: Salgado
    {8,   45,  10,  1.f,  0.98f,0.95f, 0.15, 0.85f,0.88f,1.f,   2.5, 1.f,  1.f,  1.f,   0.2},   // 18: Platon

    // ── Equipment (19-30) ────────────────────────────────────────────
    {5,   40,  45,  1.f,  0.98f,0.95f, 0.3,  0.9f, 0.93f,1.f,   1.5, 1.f,  1.f,  1.f,   0.7},   // 19: Large Softbox
    {6,   45,  70,  1.f,  0.97f,0.93f, 0.15, 0.85f,0.9f, 1.f,   3,   1.f,  1.f,  1.f,   0.4},   // 20: Strip Softbox
    {4,   40,  45,  1.f,  0.98f,0.95f, 0.45, 0.92f,0.95f,1.f,   1,   1.f,  1.f,  1.f,   0.65},  // 21: Umbrella Shoot-Through
    {7,   40,  45,  1.f,  0.97f,0.93f, 0.3,  0.9f, 0.93f,1.f,   2,   1.f,  1.f,  1.f,   0.5},   // 22: Umbrella Bounce
    {6,   55,  15,  1.f,  0.98f,0.95f, 0.5,  0.93f,0.95f,1.f,   1.5, 1.f,  1.f,  1.f,   0.45},  // 23: Beauty Dish
    {5,   60,  0,   1.f,  1.f,  1.f,   0.85, 1.f,  1.f,  1.f,   0.3, 1.f,  1.f,  1.f,   0.95},  // 24: Ring Light
    {3,   50,  90,  1.f,  0.99f,0.97f, 0.6,  0.95f,0.97f,1.f,   0.5, 1.f,  1.f,  1.f,   0.85},  // 25: Scrim / Silk
    {10,  45,  45,  1.f,  0.97f,0.92f, 0.1,  0.8f, 0.85f,1.f,   3,   1.f,  1.f,  1.f,   0.02},  // 26: Fresnel Spot
    {4,   25,  60,  1.f,  0.83f,0.57f, 0.3,  1.f,  0.87f,0.65f, 1,   1.f,  0.85f,0.6f,  0.5},   // 27: Tungsten 2800K
    {4,   40,  30,  0.95f,0.97f,1.f,   0.5,  0.93f,0.95f,1.f,   1,   0.95f,0.97f,1.f,   0.75},  // 28: Kino Flo
    {6,   45,  45,  1.f,  0.97f,0.92f, 0.0,  0.3f, 0.3f, 0.35f, 3,   1.f,  1.f,  1.f,   0.2},   // 29: Negative Fill
    {6,   65,  0,   1.f,  0.98f,0.95f, 0.6,  1.f,  0.98f,0.95f, 1,   1.f,  1.f,  1.f,   0.5},   // 30: Butterfly

    // ── Cinema Styles (31-37) ────────────────────────────────────────
    {8,   70,  80,  1.f,  0.95f,0.88f, 0.02, 0.6f, 0.65f,0.8f,  4,   1.f,  0.95f,0.9f,  0.0},   // 31: Film Noir
    {6,   50,  45,  1.f,  0.97f,0.92f, 0.35, 0.9f, 0.92f,1.f,   3,   1.f,  1.f,  1.f,   0.3},   // 32: Golden Age
    {10,  60,  90,  1.f,  0.92f,0.78f, 0.01, 0.4f, 0.35f,0.3f,  0.3, 1.f,  0.9f, 0.7f,  0.0},   // 33: Chiaroscuro
    // New cinema styles
    {5,   45,  45,  0.9f, 0.93f,1.f,   0.2,  0.8f, 0.85f,1.f,   2,   0.9f, 0.95f,1.f,   0.15},  // 34: Kubrick
    {4,   65,  40,  0.85f,0.88f,0.9f,  0.1,  0.6f, 0.65f,0.7f,  2.5, 0.9f, 0.93f,1.f,   0.1},   // 35: Fincher
    {5,   45,  30,  1.f,  0.95f,0.8f,  0.6,  1.f,  0.95f,0.85f, 1,   1.f,  0.97f,0.9f,  0.5},   // 36: Wes Anderson
    {4,   35,  70,  0.9f, 0.93f,1.f,   0.25, 0.75f,0.8f, 0.95f, 2,   0.85f,0.9f, 1.f,   0.4},   // 37: Ridley Scott

    // ── Famous DPs (38-46) ───────────────────────────────────────────
    {7,   35,  60,  1.f,  0.97f,0.9f,  0.3,  0.85f,0.9f, 1.f,   3,   1.f,  0.98f,0.95f, 0.25},  // 38: Kaminski
    {12,  20,  45,  1.f,  0.9f, 0.7f,  0.1,  0.5f, 0.75f,1.f,   5,   1.f,  0.85f,0.6f,  0.05},  // 39: Bay
    {5,   30,  110, 0.9f, 1.f,  0.8f,  0.3,  0.7f, 0.5f, 1.f,   2,   0.8f, 1.f,  0.9f,  0.4},   // 40: Doyle
    {4,   40,  75,  1.f,  0.98f,0.94f, 0.4,  0.88f,0.92f,1.f,   1.5, 1.f,  1.f,  1.f,   0.5},   // 41: Deakins
    {4,   80,  30,  1.f,  0.93f,0.82f, 0.05, 0.5f, 0.45f,0.4f,  0.5, 1.f,  0.95f,0.85f, 0.15},  // 42: Willis
    {6,   35,  50,  1.f,  0.88f,0.7f,  0.25, 0.7f, 0.8f, 1.f,   2.5, 1.f,  0.85f,0.6f,  0.35},  // 43: Storaro
    {3,   25,  100, 1.f,  0.97f,0.88f, 0.6,  0.9f, 0.95f,1.f,   0.5, 1.f,  0.98f,0.9f,  0.8},   // 44: Lubezki
    {2.5, 30,  70,  1.f,  0.93f,0.82f, 0.25, 0.7f, 0.75f,0.85f, 0.8, 1.f,  0.95f,0.88f, 0.6},   // 45: Bradford Young
    {4,   40,  60,  0.93f,0.96f,1.f,   0.35, 0.85f,0.9f, 1.f,   1.5, 0.9f, 0.95f,1.f,   0.45},  // 46: van Hoytema
};
static const int kPC = sizeof(kP)/sizeof(kP[0]);

// ---------------------------------------------------------------------------
// 3D Viewport — visible when viewing this node directly in a 3D viewer.
// Not visible through GeoScene (Hydra renders USD prims, not GL handles).
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

    glColor4f(0.3f,0.3f,0.35f,0.2f); glLineWidth(1.f);
    glBegin(GL_LINE_LOOP);
    for(int i=0;i<48;++i){float a=float(i)/48.f*2.f*float(kPi);glVertex3f(R*std::cos(a),0,R*std::sin(a));}
    glEnd();

    if (showLights) {
        glColor4f(keyColor[0],keyColor[1],keyColor[2],0.8f*b);
        glPointSize(14.f); glBegin(GL_POINTS);glVertex3f(kx,ky,kz);glEnd();
        glColor4f(keyColor[0],keyColor[1],keyColor[2],0.25f*b);
        glPointSize(24.f); glBegin(GL_POINTS);glVertex3f(kx,ky,kz);glEnd();
        glColor4f(keyColor[0],keyColor[1],keyColor[2],0.3f*b); glLineWidth(2.f);
        glBegin(GL_LINES);glVertex3f(kx,ky,kz);glVertex3f(0,0,0);glEnd();

        glColor4f(fillColor[0],fillColor[1],fillColor[2],0.6f*b);
        glPointSize(10.f); glBegin(GL_POINTS);glVertex3f(fx,fy,fz);glEnd();
        glColor4f(fillColor[0],fillColor[1],fillColor[2],0.2f*b); glLineWidth(1.f);
        glBegin(GL_LINES);glVertex3f(fx,fy,fz);glVertex3f(0,0,0);glEnd();

        glColor4f(rimColor[0],rimColor[1],rimColor[2],0.7f*b);
        glPointSize(10.f); glBegin(GL_POINTS);glVertex3f(rx,ry,rz);glEnd();
        glColor4f(rimColor[0],rimColor[1],rimColor[2],0.15f*b); glLineWidth(1.f);
        glBegin(GL_LINES);glVertex3f(rx,ry,rz);glVertex3f(0,0,0);glEnd();
    }

    if (showCones) {
        float ca=(15.f+float(shadowSoftness)*20.f)*float(kPi)/180.f;
        auto cone=[&](float ox,float oy,float oz,float cr,float cg,float cb,float al){
            float dx=-ox,dy=-oy,dz=-oz;
            float len=std::sqrt(dx*dx+dy*dy+dz*dz); if(len<1e-4f)return;
            dx/=len;dy/=len;dz/=len;
            float cl=len*0.35f,br=cl*std::tan(ca);
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
// Knobs
// ---------------------------------------------------------------------------
void SpectralStudioLight::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>Studio Light</b>");
    Newline(f);
    Text_knob(f,
        "<font color='#777' size='-1'>"
        "Professional three-point rig with 47 presets.<br>"
        "Select a category, then choose a preset."
        "</font>"
    );

    Divider(f, "Preset");
    static const char* const catNames[] = {
        "Photography Studio", "Photography Field", "Equipment",
        "Cinema Styles", "Famous Cinematographers", nullptr
    };
    Enumeration_knob(f, &presetCategory, catNames, "preset_category", "category");
    Tooltip(f, "Photography Studio -- Portrait, Fashion, Product, Famous photographers\n"
               "Photography Field -- Wildlife, Documentary, Photojournalism\n"
               "Equipment -- Softbox, Umbrella, Fresnel, Kino, Scrim\n"
               "Cinema Styles -- Noir, Kubrick, Fincher, Wes Anderson\n"
               "Famous DPs -- Deakins, Storaro, Willis, Lubezki, Doyle");

    // ── Photography Studio ──
    static const char* const ps[] = {
        "Select...",
        "Portrait Classic (Rembrandt)", "Beauty Dish Fashion",
        "Vogue Editorial", "Product Shot", "Corporate Headshot",
        "Instagram Beauty", "Boudoir",
        "Annie Leibovitz", "Peter Lindbergh",
        "Helmut Newton", "Gregory Crewdson",
        nullptr
    };
    Enumeration_knob(f, &presetPhotoStudio, ps, "preset_photo_studio", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Portrait Classic -- Rembrandt triangle. Key at 45deg, cool fill.\n"
               "Beauty Dish -- Fashion workhorse since the 1980s.\n"
               "Vogue Editorial -- Hard glamour. Irving Penn / Richard Avedon.\n"
               "Product Shot -- Clean hard key, maximum detail.\n"
               "Corporate Headshot -- Soft, neutral, professional.\n"
               "Instagram Beauty -- Front ring-like, flat, glowy.\n"
               "Boudoir -- Soft, warm, intimate, low contrast.\n\n"
               "Annie Leibovitz -- Dramatic celebrity portraits for Vanity Fair\n"
               "  and Rolling Stone. Theatrical setups, rich colour, strong mood.\n"
               "  John Lennon/Yoko Ono, Demi Moore, Queen Elizabeth II.\n"
               "Peter Lindbergh -- Raw natural beauty, stripped-back fashion.\n"
               "  Pioneered the supermodel era. Minimal retouching, honest light.\n"
               "  First Vogue cover with Naomi, Cindy, Linda, Christy, Tatjana.\n"
               "Helmut Newton -- Provocative fashion, hard punchy light.\n"
               "  Sharp shadows, architectural compositions, power and glamour.\n"
               "Gregory Crewdson -- Cinematic staged suburban scenes.\n"
               "  Movie-scale production for single photographs. Eerie Americana.");

    // ── Photography Field ──
    static const char* const pf[] = {
        "Select...",
        "Nat Geo Wildlife (Nichols)", "Remote Flash Wildlife",
        "Documentary Natural", "Macro Ring Flash",
        "Steve McCurry", "Sebastiao Salgado", "Platon",
        nullptr
    };
    Enumeration_knob(f, &presetPhotoField, pf, "preset_photo_field", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Nick Nichols -- Camera-trap flash in dense jungle. Hard light,\n"
               "  near-zero fill. Pioneered remote flash for National Geographic.\n"
               "  War elephants in Chad, tigers by camera trap in India.\n"
               "Remote Flash -- Bare flash ambush, no fill. Night wildlife.\n"
               "Documentary Natural -- Window-light simulation.\n"
               "Macro Ring Flash -- Even, shadowless, clinical detail.\n\n"
               "Steve McCurry -- 'Afghan Girl' (1984), the most recognised\n"
               "  National Geographic cover. Natural light, intimate proximity.\n"
               "  Available-light master in conflict zones and remote cultures.\n"
               "Sebastiao Salgado -- Epic black & white documentary.\n"
               "  Workers, Migrations, Genesis. High contrast, monumental scale.\n"
               "  Turned social documentary into fine art.\n"
               "Platon -- Powerful close-up portraits of world leaders.\n"
               "  Putin, Obama, Gaddafi. Hard front light, confrontational.\n"
               "  Time and New Yorker covers. Ring flash + hard key.");

    // ── Equipment ──
    static const char* const eq[] = {
        "Select...",
        "Large Softbox (4x6ft)", "Strip Softbox",
        "Umbrella Shoot-Through", "Umbrella Bounce Silver",
        "Beauty Dish", "Ring Light",
        "Scrim / Silk Diffusion", "Fresnel Spot",
        "Tungsten Practical (2800K)", "Kino Flo Bank",
        "Negative Fill (Flag)", "Butterfly / Clamshell",
        nullptr
    };
    Enumeration_knob(f, &presetEquipment, eq, "preset_equipment", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Large Softbox -- 4x6ft panel, broad wrap-around.\n"
               "Strip Softbox -- Narrow, sculpts edges and cheekbones.\n"
               "Umbrella Shoot-Through -- Broad, soft, forgiving.\n"
               "Umbrella Bounce Silver -- Harder, more specular punch.\n"
               "Beauty Dish -- Focused soft, fashion standard.\n"
               "Ring Light -- Flat, catch-light ring. YouTube/beauty.\n"
               "Scrim / Silk -- Diffused natural light through fabric.\n"
               "Fresnel Spot -- Hard, focusable theatre/film light.\n"
               "Tungsten Practical (2800K) -- Warm motivated. Table lamps.\n"
               "Kino Flo Bank -- Cool daylight fluorescent. Film set standard.\n"
               "Negative Fill -- Flag absorbs light, deepens shadow side.\n"
               "Butterfly / Clamshell -- Key above + fill below. Beauty standard.");

    // ── Cinema Styles ──
    static const char* const cs[] = {
        "Select...",
        "Film Noir", "Golden Age Hollywood", "Chiaroscuro (Caravaggio)",
        "Kubrick", "Fincher", "Wes Anderson", "Ridley Scott",
        nullptr
    };
    Enumeration_knob(f, &presetCinemaStyle, cs, "preset_cinema_style", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Film Noir -- 1940s Hollywood. Extreme angles, deep shadows.\n"
               "  Double Indemnity, The Third Man, Touch of Evil.\n"
               "Golden Age Hollywood -- Studio glamour. Butterfly shadow,\n"
               "  strong backlight, star glow.\n"
               "Chiaroscuro -- Caravaggio-inspired. Single dramatic source.\n\n"
               "Kubrick -- Cold, symmetrical, clinical precision.\n"
               "  One-point perspective, practicals, wide-angle distortion.\n"
               "  2001, Clockwork Orange, The Shining, Barry Lyndon.\n"
               "Fincher -- Dark, desaturated, top-lit faces.\n"
               "  Green-tinted shadows, oppressive atmosphere.\n"
               "  Se7en, Fight Club, Zodiac, Gone Girl, Mindhunter.\n"
               "Wes Anderson -- Flat, symmetrical, warm pastel palette.\n"
               "  Even soft light, whimsical, storybook feel.\n"
               "  Grand Budapest Hotel, Moonrise Kingdom, French Dispatch.\n"
               "Ridley Scott -- Atmospheric smoke/haze, strong backlight.\n"
               "  Shafts of light through atmosphere. Blade Runner, Alien,\n"
               "  Gladiator, Black Hawk Down, Kingdom of Heaven.");

    // ── Famous DPs ──
    static const char* const dp[] = {
        "Select...",
        "Spielberg / Kaminski", "Michael Bay", "Christopher Doyle",
        "Roger Deakins", "Gordon Willis (Godfather)",
        "Vittorio Storaro", "Emmanuel Lubezki",
        "Bradford Young", "Hoyte van Hoytema",
        nullptr
    };
    Enumeration_knob(f, &presetFamousDP, dp, "preset_famous_dp", "preset");
    SetFlags(f, Knob::HIDDEN);
    Tooltip(f, "Kaminski -- Overexposed windows, warm practicals.\n"
               "  Schindler's List, Saving Private Ryan. 2x Oscar.\n"
               "Michael Bay -- Amber key vs teal fill. 'Bayhem'.\n"
               "  Transformers, Bad Boys, The Rock.\n"
               "Christopher Doyle -- Neon-soaked, moody, Wong Kar-wai's DP.\n"
               "  In the Mood for Love, Chungking Express, Hero.\n"
               "Roger Deakins -- Every source motivated. Elegant simplicity.\n"
               "  Blade Runner 2049, 1917, Skyfall. 2x Oscar.\n"
               "Gordon Willis -- 'Prince of Darkness'. Eyes in shadow.\n"
               "  The Godfather, All the President's Men, Manhattan.\n"
               "Vittorio Storaro -- Colour as emotion. Painterly.\n"
               "  Apocalypse Now, Last Emperor. 3x Oscar.\n"
               "Emmanuel Lubezki -- Natural light, golden hour, long takes.\n"
               "  The Revenant, Gravity, Birdman. 3x consecutive Oscar.\n"
               "Bradford Young -- Intimate underexposure, warm skin.\n"
               "  Arrival, Selma, Solo.\n"
               "Hoyte van Hoytema -- IMAX, cool atmospheric.\n"
               "  Interstellar, Dunkirk, Oppenheimer.");

    Double_knob(f, &mix, "studio_mix", "mix"); SetRange(f, 0, 1);
    Tooltip(f, "Overall studio light contribution.");

    Divider(f, "Key Light");
    Double_knob(f, &keyIntensity, "key_intensity", "intensity"); SetRange(f, 0, 20);
    Double_knob(f, &keyElevation, "key_elevation", "elevation"); SetRange(f, 0, 90);
    Double_knob(f, &keyAzimuth, "key_azimuth", "azimuth");
    ClearFlags(f, Knob::STARTLINE); SetRange(f, 0, 360);
    Color_knob(f, keyColor, "key_color", "colour");

    Divider(f, "Fill Light");
    Double_knob(f, &fillRatio, "fill_ratio", "fill ratio"); SetRange(f, 0, 1);
    Color_knob(f, fillColor, "fill_color", "colour");

    Divider(f, "Rim Light");
    Double_knob(f, &rimIntensity, "rim_intensity", "intensity"); SetRange(f, 0, 10);
    Color_knob(f, rimColor, "rim_color", "colour");

    Divider(f, "Shadow");
    Double_knob(f, &shadowSoftness, "shadow_softness", "softness"); SetRange(f, 0, 1);

    Divider(f, "3D Display");
    Text_knob(f,
        "<font color='#666' size='-1'>"
        "Select this node and open a 3D viewer to see the rig.<br>"
        "Not visible when viewing through GeoScene."
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
// knob_changed
// ---------------------------------------------------------------------------
int SpectralStudioLight::knob_changed(Knob* k)
{
    if (k->is("preset_category")) {
        if (Knob* kn = knob("preset_photo_studio")) kn->visible(presetCategory == 0);
        if (Knob* kn = knob("preset_photo_field"))  kn->visible(presetCategory == 1);
        if (Knob* kn = knob("preset_equipment"))    kn->visible(presetCategory == 2);
        if (Knob* kn = knob("preset_cinema_style")) kn->visible(presetCategory == 3);
        if (Knob* kn = knob("preset_famous_dp"))    kn->visible(presetCategory == 4);
        return 1;
    }

    int gi = -1;
    if (k->is("preset_photo_studio") && presetPhotoStudio > 0)
        gi = kCatStart[1] + presetPhotoStudio - 1;
    else if (k->is("preset_photo_field") && presetPhotoField > 0)
        gi = kCatStart[2] + presetPhotoField - 1;
    else if (k->is("preset_equipment") && presetEquipment > 0)
        gi = kCatStart[3] + presetEquipment - 1;
    else if (k->is("preset_cinema_style") && presetCinemaStyle > 0)
        gi = kCatStart[4] + presetCinemaStyle - 1;
    else if (k->is("preset_famous_dp") && presetFamousDP > 0)
        gi = kCatStart[5] + presetFamousDP - 1;

    if (gi > 0 && gi < kPC) {
        const auto& s = kP[gi];
        keyIntensity=s.kI; keyElevation=s.kE; keyAzimuth=s.kA;
        keyColor[0]=s.kR; keyColor[1]=s.kG; keyColor[2]=s.kB;
        fillRatio=s.fR;
        fillColor[0]=s.fCR; fillColor[1]=s.fCG; fillColor[2]=s.fCB;
        rimIntensity=s.rI;
        rimColor[0]=s.rCR; rimColor[1]=s.rCG; rimColor[2]=s.rCB;
        shadowSoftness=s.ss;
        if (Knob* kn = knob("key_intensity"))   kn->set_value(s.kI);
        if (Knob* kn = knob("key_elevation"))   kn->set_value(s.kE);
        if (Knob* kn = knob("key_azimuth"))     kn->set_value(s.kA);
        if (Knob* kn = knob("key_color"))       { kn->set_value(s.kR,0); kn->set_value(s.kG,1); kn->set_value(s.kB,2); }
        if (Knob* kn = knob("fill_ratio"))      kn->set_value(s.fR);
        if (Knob* kn = knob("fill_color"))      { kn->set_value(s.fCR,0); kn->set_value(s.fCG,1); kn->set_value(s.fCB,2); }
        if (Knob* kn = knob("rim_intensity"))   kn->set_value(s.rI);
        if (Knob* kn = knob("rim_color"))       { kn->set_value(s.rCR,0); kn->set_value(s.rCG,1); kn->set_value(s.rCB,2); }
        if (Knob* kn = knob("shadow_softness")) kn->set_value(s.ss);
        return 1;
    }

    return SourceGeomOp::knob_changed(k);
}
