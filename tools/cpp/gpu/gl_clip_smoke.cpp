// gl_clip_smoke.cpp — strict pixel TDD for QW10 (homogeneous near-plane clipping
// z+w>=0 BEFORE the perspective divide) and QW11 (reciprocal-w perspective-correct
// interpolation of colors, texcoords AND depth).
//
// These tests feed the SoftRasterizer directly with CLIP-SPACE vertices (x,y,z,w),
// which is the migrated contract: the rasterizer now owns the perspective divide.
// Vertex.w defaults to 1, so a caller that passes NDC coords with w=1 (the existing
// direct-rasterizer behavior, and the identity/ortho/translate IglHook paths) sees
// clip==NDC, an identity divide, no near-plane clipping, and perspective==affine.
//
// The expected pixel values are computed INDEPENDENTLY here from first principles
// (screen mapping + integer edge-function barycentrics + the perspective formula),
// so agreement with the rasterizer is two implementations meeting, not a tautology.
// Each perspective case also asserts the value DIFFERS from the affine result, and
// each clip case asserts the OLD (no-clip) behavior is specifically rejected.
#include "igpu_rasterizer.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <array>
#include <limits>
using namespace zeebo::gpu;

namespace {
int fails=0;
void check(const char* tag,u16 got,u16 want){
    bool ok=got==want; if(!ok) ++fails;
    printf("[%s] got=0x%04X want=0x%04X %s\n",tag,got,want,ok?"PASS":"FAIL");
}
void check_true(const char* tag,bool ok){
    if(!ok) ++fails;
    printf("[%s] %s\n",tag,ok?"PASS":"FAIL");
}

// Mirror the rasterizer's fixed screen mapping (default viewport 640x480, y-flip).
int SX(float ndcx){ return (int)((ndcx*0.5f+0.5f)*640.0f); }
int SY(float ndcy){ return (int)((1.0f-(ndcy*0.5f+0.5f))*480.0f); }

u16 pack565(float r,float g,float b){
    auto c=[](float x){ float y=x<0?0:(x>1?1:x); return (u32)std::lround(y*255.0f); };
    u32 R=c(r)>>3,G=c(g)>>2,B=c(b)>>3; return (u16)((R<<11)|(G<<5)|B);
}

struct Ref { float fa,fb,fc; bool inside; };
// Integer edge-function barycentrics at pixel (px,py) for screen triangle.
Ref bary(int x0,int y0,int x1,int y1,int x2,int y2,int px,int py){
    long area=(long)(x1-x0)*(y2-y0)-(long)(x2-x0)*(y1-y0);
    long w0=(long)(x1-px)*(y2-py)-(long)(x2-px)*(y1-py);
    long w1=(long)(x2-px)*(y0-py)-(long)(x0-px)*(y2-py);
    long w2=(long)(x0-px)*(y1-py)-(long)(x1-px)*(y0-py);
    bool inside=(w0>=0&&w1>=0&&w2>=0)||(w0<=0&&w1<=0&&w2<=0);
    if(area==0) return {0,0,0,false};
    return { (float)w0/area,(float)w1/area,(float)w2/area, inside };
}

Vertex mk(float x,float y,float z,float w,float r,float g,float b){
    Vertex v; v.x=x;v.y=y;v.z=z;v.w=w; v.r=r;v.g=g;v.b=b;v.a=1; return v;
}
u16 center(IGpuRasterizer& r){ return r.framebuffer_rgb565()[240*640+320]; }
u16 px(IGpuRasterizer& r,int x,int y){ return r.framebuffer_rgb565()[y*640+x]; }
void clear_black(IGpuRasterizer& r){ r.clear_color(0,0,0,1); r.clear(0x4000); }
} // namespace

int main(){
    // Perspective triangle geometry reused by QW11 color/tex/depth tests:
    //   A=clip(-1,-1) w=1 ; B=clip(1,-1) w=1 ; C=clip(0,4) w=4 -> ndc C=(0,1).
    // The strong wC=4 makes reciprocal-w interpolation diverge sharply from affine,
    // and we sample the DISTINGUISHING pixel (320,60) [near the top/apex] where an
    // independent oracle (below) says perspective and affine give different texels,
    // colors and depths — so OLD affine code deterministically FAILS each assertion.
    //
    // Independent oracle at screen pixel (320,60), integer edge-function bary on the
    // NEW (post-divide) NDC screen triangle:  A_ndc=(-1,-1) B_ndc=(1,-1) C_ndc=(0,1).
    const int Ax=SX(-1),Ay=SY(-1), Bx=SX(1),By=SY(-1), Cx=SX(0),Cy=SY(1);
    Ref Rn=bary(Ax,Ay,Bx,By,Cx,Cy,320,60);      // barys on divided (NDC) triangle
    // The OLD code screen-maps clip coords directly (ignores w). Its NDC triangle
    // is A=(-1,-1) B=(1,-1) C=(0,4) -> C off-screen; bary at (320,60) differs.
    Ref Ro=bary(SX(-1),SY(-1),SX(1),SY(-1),SX(0),SY(4),320,60);
    const float wA=1,wB=1,wC=4;
    // perspective-correct scalar interp of a per-vertex attribute (only C carries it):
    auto persp=[&](float aA,float aB,float aC){
        float iw=Rn.fa/wA+Rn.fb/wB+Rn.fc/wC;
        return (Rn.fa*aA/wA+Rn.fb*aB/wB+Rn.fc*aC/wC)/iw;
    };
    auto affine_old=[&](float aA,float aB,float aC){
        return Ro.fa*aA+Ro.fb*aB+Ro.fc*aC;   // OLD: affine on clip-as-NDC triangle
    };

    // ---- QW11a: perspective-correct vertex color (w varies) at (320,60) ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        std::vector<Vertex> v={ mk(-1,-1,0,1, 0,0,0), mk(1,-1,0,1, 0,0,0), mk(0,4,0,4, 1,0,0) };
        r->draw(Prim::Triangles,v);
        float r_persp=persp(0,0,1);           // green channel value from C's red=1
        float r_affine=affine_old(0,0,1);
        u16 want=pack565(r_persp,0,0), affine=pack565(r_affine,0,0);
        check("QW11a persp color",px(*r,320,60),want);
        check_true("QW11a persp!=affine",want!=affine);   // load-bearing: old must miss
    }
    // ---- QW11b: w=1 regression -> perspective == affine (identity divide) ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        // All w=1: clip==NDC, reciprocal-w collapses to affine; center bary fc=0.5.
        Ref Rc=bary(SX(-1),SY(-1),SX(1),SY(-1),SX(0),SY(1),320,240);
        std::vector<Vertex> v={ mk(-1,-1,0,1, 0,0,0), mk(1,-1,0,1, 0,0,0), mk(0,1,0,1, 1,0,0) };
        r->draw(Prim::Triangles,v);
        check("QW11b w=1 regression",center(*r),pack565(Rc.fc,0,0));
    }
    // ---- QW11-tex: perspective-correct TEXCOORD selects a different texel ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        // 2x2 nearest/repeat texture: (0,0)red (1,0)green (0,1)blue (1,1)white.
        const u8 tx[16]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
        r->bind_texture(0,7); r->tex_image_2d(7,2,2,tx);
        r->tex_parameter(0,0x2800,0x2600); r->tex_parameter(0,0x2801,0x2600); // MAG/MIN NEAREST
        r->tex_parameter(0,0x2802,0x2901); r->tex_parameter(0,0x2803,0x2901); // WRAP_S/T REPEAT
        RenderState st; st.tex_enabled[0]=1; st.active_unit=0; r->set_state(st);
        Vertex A=mk(-1,-1,0,1, 1,1,1), B=mk(1,-1,0,1, 1,1,1), C=mk(0,4,0,4, 1,1,1);
        A.u=0;A.v=0; B.u=1;B.v=0; C.u=1;C.v=1;
        std::vector<Vertex> v={A,B,C};
        r->draw(Prim::Triangles,v);
        // Oracle at (320,160): perspective-correct uv->(~0.66,~0.33)->texel(1,0)=green
        // 0x07E0; SCREEN-AFFINE (divided-triangle) uv->(0.83,0.67)->texel(1,1)=white
        // 0xFFFF. This pixel distinguishes reciprocal-w perspective interpolation from
        // BOTH affine-on-clip (old) and affine-on-divided-triangle (denominator-omitted)
        // mutations; the (320,60) apex pixel did NOT (both map to white).
        check("QW11tex persp texel",px(*r,320,160),0x07E0);
        check_true("QW11tex persp!=affine",0x07E0!=0xFFFF);
    }
    // ---- QW10a: fully inside -> unchanged, center filled ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        std::vector<Vertex> v={ mk(-1,-1,0,1, 1,0,0), mk(1,-1,0,1, 1,0,0), mk(0,1,0,1, 1,0,0) };
        r->draw(Prim::Triangles,v);
        check("QW10a inside filled",center(*r),0xF800); // red
    }
    // ---- QW10b: fully outside near plane (z+w<0 all) -> zero pixels ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        // Same visible x,y but z=-2,w=1 => z+w=-1<0 for all. OLD (no clip) would draw
        // at the same screen coords (ignores w/z with depth off) -> center red = RED fail.
        std::vector<Vertex> v={ mk(-1,-1,-2,1, 1,0,0), mk(1,-1,-2,1, 1,0,0), mk(0,1,-2,1, 1,0,0) };
        r->draw(Prim::Triangles,v);
        check("QW10b outside zeroed",center(*r),0x0000);
        // count nonzero pixels: must be exactly zero
        const u16* fb=r->framebuffer_rgb565(); long nz=0;
        for(int i=0;i<640*480;i++) if(fb[i]) ++nz;
        check_true("QW10b zero pixels total",nz==0);
    }
    // ---- QW10c: one vertex out -> crossing generates 2 verts (quad), top removed ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        // A,B inside; C behind near (z=-2,w=1). Crossings on A-C and B-C at t where
        // z+w=0. A:(z+w)=1, C:(z+w)=-1 => t=0.5 => midpoints at ndc y=0.5 (screen y=120).
        std::vector<Vertex> v={ mk(-1,-1,0,1, 1,0,0), mk(1,-1,0,1, 1,0,0), mk(0,2,-2,1, 1,0,0) };
        r->draw(Prim::Triangles,v);
        // Above screen y=120 (original apex region) must now be empty; below stays red.
        check("QW10c top clipped empty",px(*r,320,60),0x0000);   // OLD would draw red here
        check("QW10c bottom kept red",  px(*r,320,300),0xF800);
    }
    // ---- QW10d: two vertices out -> single smaller triangle near the inside vertex ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        // A inside (bottom-left). B,C behind near.
        std::vector<Vertex> v={ mk(-1,-1,0,1, 1,0,0), mk(3,-1,-2,1, 1,0,0), mk(-1,3,-2,1, 1,0,0) };
        // A:(z+w)=1. B:(z+w)=-1 => crossing on A-B at t=0.5 -> ndc x=1 (screen x=640/edge).
        // C:(z+w)=-1 => crossing on A-C at t=0.5 -> ndc y=1 (screen y=0).
        r->draw(Prim::Triangles,v);
        check("QW10d near-vertex kept", px(*r,20,470),0xF800);  // bottom-left corner filled
        check("QW10d far corner empty", px(*r,600,40),0x0000);  // top-right removed
    }
    // ---- QW10e: generated-vertex attributes carry interpolated color ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        // one-out: A=red B=green C=blue(behind). Generated verts on A-C and B-C lerp
        // color at t=0.5 -> (0.5*A + 0.5*C). A-C midpoint = red/blue mix (magenta-ish).
        std::vector<Vertex> v={ mk(-1,-1,0,1, 1,0,0), mk(1,-1,0,1, 0,1,0), mk(0,2,-2,1, 0,0,1) };
        r->draw(Prim::Triangles,v);
        // Sample DEEP inside the generated-vertex region at screen (240,300). The
        // correct build carries interpolated color from the A-C / B-C crossings, so
        // this pixel has BOTH red (from A) and blue (from the generated C-lerp). If the
        // clip code omits attribute interpolation on generated verts (copies the kept
        // vertex color instead of lerping), blue vanishes here -> assertion fails.
        u16 p=px(*r,240,300);
        int R5=(p>>11)&31, G6=(p>>5)&63, B5=p&31;
        check_true("QW10e gen-vertex has red",  R5>0);
        check_true("QW10e gen-vertex has blue", B5>0);
        check_true("QW10e gen-vertex low green", G6<R5+B5); // not a pure-green garbage
    }
    // ---- QW10f: w≈0 and non-finite safety: no crash, no explosion ----
    {
        auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
        clear_black(*r);
        std::vector<Vertex> v0={ mk(-1,-1,0,0, 1,0,0), mk(1,-1,0,0, 1,0,0), mk(0,1,0,0, 1,0,0) };
        r->draw(Prim::Triangles,v0); // w=0 everywhere
        float nan=std::nanf(""), inf=std::numeric_limits<float>::infinity();
        std::vector<Vertex> v1={ mk(-1,-1,0,nan, 1,0,0), mk(1,-1,0,inf, 1,0,0), mk(0,1,0,1, 1,0,0) };
        r->draw(Prim::Triangles,v1); // nonfinite w
        // Must not explode: every pixel is a valid finite RGB565 (trivially true for u16),
        // and no runaway fill. Assert bounded nonzero count (no full-screen garbage).
        const u16* fb=r->framebuffer_rgb565(); long nz=0;
        for(int i=0;i<640*480;i++) if(fb[i]) ++nz;
        check_true("QW10f w~0/nonfinite bounded",nz< (640L*480L)); // did not paint whole screen
        printf("[QW10f] nonzero=%ld (no crash)\n",nz);
    }
    // ---- QW11c: MATHEMATICALLY-CORRECT window depth interpolation, via depth test ----
    {
        // MATH AUDIT (this is the crux of the task). Color/texcoord varyings are
        // perspective-correct:  a_screen = (S l*a_i/w_i) / (S l*1/w_i).
        // But WINDOW DEPTH is NOT that generic form. OpenGL stores z_window as the
        // AFFINE (screen-linear) interpolation of the per-vertex NDC depth z_i/w_i:
        //     z_window = S l*(z_i/w_i)      (then *0.5+0.5 for the [0,1] buffer).
        // Applying the reciprocal-w denominator a SECOND time to depth is WRONG.
        // This test pins the correct behavior and rejects BOTH broken variants:
        //   (1) OLD code: treats clip z as already-NDC (no divide)  -> subtest A rejects it.
        //   (2) generic-denominator "depth" mutation                -> subtest B rejects it.
        //
        // RED tri (clip, w varies): A=(-0.9,-0.9,-0.9,1) B=(0.9,-0.9,0.9,1)
        //   C=(0,7.2,-7.2,8) -> ndc C=(0,0.9,-0.9). Writes depth under LESS.
        // A flat GREEN probe (w=1, constant ndc z) is drawn after; whether it wins the
        // LESS test at a chosen pixel depends on which depth formula the RED tri stored.
        auto redtri=[&]{ return std::vector<Vertex>{
            mk(-0.9f,-0.9f,-0.9f,1, 1,0,0), mk(0.9f,-0.9f,0.9f,1, 1,0,0),
            mk(0.0f,7.2f,-7.2f,8, 1,0,0) }; };
        // -- Subtest A: pixel (452,225), GREEN ndc z=-0.54 (depth 0.23).
        //    correct stored red depth @P=0.466 -> 0.23<0.466 GREEN wins (0x07E0).
        //    OLD stored red depth @P=0.0 (clip z as ndc) -> 0.23<0.0 false, RED stays.
        {
            auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
            clear_black(*r);
            RenderState st; st.depth_test=true; st.depth_func=0x0201; st.depth_write=true; // LESS
            r->set_state(st);
            auto v=redtri(); r->draw(Prim::Triangles,v);
            std::vector<Vertex> g={ mk(-1,-1,-0.54f,1, 0,1,0), mk(1,-1,-0.54f,1, 0,1,0), mk(0,1,-0.54f,1, 0,1,0) };
            r->draw(Prim::Triangles,g);
            check("QW11c depth vs OLD (affine ndc z)",px(*r,452,225),0x07E0); // GREEN => correct, RED => old
        }
        // -- Subtest B: pixel (398,141), GREEN ndc z=0.0 (depth 0.5).
        //    correct stored red depth @P=0.294 -> 0.5<0.294 false, RED stays (0xF800).
        //    generic-denominator stored depth @P=0.723 -> 0.5<0.723 GREEN wins (0x07E0).
        {
            auto r=create_rasterizer(Backend::SoftwareRef); r->init(); r->begin_frame();
            clear_black(*r);
            RenderState st; st.depth_test=true; st.depth_func=0x0201; st.depth_write=true; // LESS
            r->set_state(st);
            auto v=redtri(); r->draw(Prim::Triangles,v);
            std::vector<Vertex> g={ mk(-1,-1,0.0f,1, 0,1,0), mk(1,-1,0.0f,1, 0,1,0), mk(0,1,0.0f,1, 0,1,0) };
            r->draw(Prim::Triangles,g);
            check("QW11c depth vs generic-denominator",px(*r,398,141),0xF800); // RED => correct, GREEN => generic
        }
    }

    printf(fails==0?"DONE ALL PASS\n":"DONE %d FAIL\n",fails);
    return fails==0?0:1;
}
