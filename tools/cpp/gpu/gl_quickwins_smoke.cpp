// gl_quickwins_smoke.cpp — strict pixel TDD for the low-risk GLES quick wins.
//   QW4: alpha-test (glAlphaFuncx) + back/front-face culling (glCullFace).
//   QW5: glTexParameterx nearest/linear + repeat/clamp, per texture.
//   QW7: remaining GLES1.x depth funcs and blend factors.
// Each behavior is proved by an EXACT RGB565 pixel value read from the 640x480
// software framebuffer. No Unicorn: a fake GuestMachine serves register args and
// a host "guest memory" vector. Malformed args are validated to be safe (no UB).
#include "igl_hook.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace zeebo::gpu;

namespace {
// GL enums used by the tests (public Khronos values).
constexpr u32 NEVER=0x0200, LESS=0x0201, EQUAL=0x0202, LEQUAL=0x0203,
              GREATER=0x0204, NOTEQUAL=0x0205, GEQUAL=0x0206, ALWAYS=0x0207;
constexpr u32 ZERO=0x0000, ONE=0x0001, DST_COLOR=0x0306, ONE_MINUS_SRC_ALPHA=0x0303,
              SRC_ALPHA=0x0302;
constexpr u32 MAG_FILTER=0x2800, MIN_FILTER=0x2801, WRAP_S=0x2802, WRAP_T=0x2803;
constexpr u32 NEAREST=0x2600, LINEAR=0x2601, REPEAT=0x2901, CLAMP_TO_EDGE=0x812F;
constexpr u32 BACK=0x0405, FRONT=0x0404;
constexpr u32 COLOR_BIT=0x4000, DEPTH_BIT=0x0100;

struct Harness {
    std::unique_ptr<IGpuRasterizer> rast = create_rasterizer(Backend::SoftwareRef);
    IglHook hook{*rast};
    std::vector<u32> mem = std::vector<u32>(0x400/4, 0);
    std::vector<u32> regs; u32 ret=0;
    GuestMachine gm;
    static constexpr u32 BASE=0x1000;
    Harness(){
        rast->init(); rast->begin_frame();
        gm.arg=[&](int n){ return n<(int)regs.size()?regs[n]:0u; };
        gm.set_ret=[&](u32 r){ ret=r; };
        gm.read=[&](u32 va,void* dst,u32 size)->bool{
            if(va<BASE) return false;
            u32 off=va-BASE;
            if(off+size>mem.size()*4) return false;
            std::memcpy(dst,(uint8_t*)mem.data()+off,size); return true; };
    }
    static u32 FX(float f){ return u32(int32_t(f*65536.0f)); }
    void call(int slot){ hook.dispatch_igl(slot,gm); }
    u16 center(){ return rast->framebuffer_rgb565()[240*640+320]; }
    u16 px(int x,int y){ return rast->framebuffer_rgb565()[y*640+x]; }
    // Full-screen triangle covering the center, XY GLfixed at BASE.
    void put_tri(){
        float v[3][2]={{-2.f,-2.f},{4.f,-2.f},{-2.f,4.f}};
        size_t w=0; for(auto&p:v){ mem[w++]=FX(p[0]); mem[w++]=FX(p[1]); }
        regs={2,glenum::FIXED,0,BASE}; call(igl_slot::glVertexPointer);
        regs={glenum::VERTEX_ARRAY}; call(igl_slot::glEnableClientState);
    }
    void clear_black(){
        regs={0,0,0,FX(1)}; call(igl_slot::glClearColorx);
        regs={COLOR_BIT|DEPTH_BIT}; call(igl_slot::glClear);
    }
};

int fails=0;
void check(const char* tag,u16 got,u16 want){
    bool ok=got==want; if(!ok) ++fails;
    printf("[%s] got=0x%04X want=0x%04X %s\n",tag,got,want,ok?"PASS":"FAIL");
}
}

int main(){
    // ---------- QW4a: alpha-test discards fragments ----------
    {
        Harness h; h.clear_black();
        h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(0.25f)}; h.call(igl_slot::glColor4x); // white, alpha 0.25
        h.regs={glenum::ALPHA_TEST}; h.call(igl_slot::glEnable);
        // glAlphaFuncx(GL_GREATER, 0.5): 0.25 fails -> pixel stays black.
        h.regs={GREATER,h.FX(0.5f)}; h.call(igl_slot::glAlphaFuncx);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW4 alpha discard",h.center(),0x0000);
    }
    // ---------- QW4b: alpha-test passes ----------
    {
        Harness h; h.clear_black();
        h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(0.9f)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::ALPHA_TEST}; h.call(igl_slot::glEnable);
        h.regs={GEQUAL,h.FX(0.5f)}; h.call(igl_slot::glAlphaFuncx); // 0.9>=0.5 passes
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW4 alpha pass",h.center(),0xF800);
    }
    // ---------- QW4c: cull BACK removes a CW (back) triangle ----------
    {
        Harness h; h.clear_black();
        // CW winding in NDC: v0,v1,v2 with negative signed area.
        float v[3][2]={{-2.f,-2.f},{-2.f,4.f},{4.f,-2.f}}; // reversed => back face
        size_t w=0; for(auto&p:v){ h.mem[w++]=Harness::FX(p[0]); h.mem[w++]=Harness::FX(p[1]); }
        h.regs={2,glenum::FIXED,0,Harness::BASE}; h.call(igl_slot::glVertexPointer);
        h.regs={glenum::VERTEX_ARRAY}; h.call(igl_slot::glEnableClientState);
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::CULL_FACE}; h.call(igl_slot::glEnable);
        h.regs={BACK}; h.call(igl_slot::glCullFace);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW4 cull back",h.center(),0x0000);
    }
    // ---------- QW4d: cull BACK keeps a CCW (front) triangle ----------
    {
        Harness h; h.clear_black();
        h.put_tri(); // CCW front face
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::CULL_FACE}; h.call(igl_slot::glEnable);
        h.regs={BACK}; h.call(igl_slot::glCullFace);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW4 cull front-kept",h.center(),0xF800);
    }
    // ---------- QW4e: default cull face is GL_BACK (no glCullFace call) ----------
    // GLES1 spec: GL_CULL_FACE_MODE defaults to GL_BACK. Enabling culling without
    // ever calling glCullFace must discard a CW (back) triangle. Proves the
    // RenderState.cull_face default is 0x0405, not 0.
    {
        Harness h; h.clear_black();
        float v[3][2]={{-2.f,-2.f},{-2.f,4.f},{4.f,-2.f}}; // CW => back face
        size_t w=0; for(auto&p:v){ h.mem[w++]=Harness::FX(p[0]); h.mem[w++]=Harness::FX(p[1]); }
        h.regs={2,glenum::FIXED,0,Harness::BASE}; h.call(igl_slot::glVertexPointer);
        h.regs={glenum::VERTEX_ARRAY}; h.call(igl_slot::glEnableClientState);
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::CULL_FACE}; h.call(igl_slot::glEnable); // NO glCullFace
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW4 default cull back",h.center(),0x0000);
    }
    // ---------- QW5a: NEAREST + REPEAT samples exact texel ----------
    {
        Harness h; h.clear_black();
        // 2x2 texture: red, green / blue, white (RGBA8) at BASE+0x100.
        const u32 TEX=Harness::BASE+0x100;
        const u8 tx[16]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
        std::memcpy((u8*)h.mem.data()+(TEX-Harness::BASE),tx,16);
        h.regs={0x0de1,7}; h.call(igl_slot::glBindTexture);
        h.regs={0x0de1,0,glenum::RGBA,2,2,0,glenum::RGBA,glenum::UBYTE,TEX};
        h.call(igl_slot::glTexImage2D);
        h.regs={0x0de1,MAG_FILTER,NEAREST}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,MIN_FILTER,NEAREST}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,WRAP_S,REPEAT}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,WRAP_T,REPEAT}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1}; h.call(igl_slot::glEnable);
        // UV=0.4 -> sample point sx=0.3 (between texels): NEAREST snaps to texel(0,0)
        // = pure red; default BILINEAR would blend red/green/blue (fails RED).
        const u32 UV=Harness::BASE+0x180;
        for(int i=0;i<3;i++){ h.mem[(UV-Harness::BASE)/4+i*2]=Harness::FX(0.4f);
                              h.mem[(UV-Harness::BASE)/4+i*2+1]=Harness::FX(0.4f); }
        h.regs={2,glenum::FIXED,0,UV}; h.call(igl_slot::glTexCoordPointer);
        h.regs={glenum::TEXCOORD_ARRAY}; h.call(igl_slot::glEnableClientState);
        h.put_tri();
        h.regs={h.FX(1),h.FX(1),h.FX(1),h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW5 nearest texel",h.center(),0xF800); // red
    }
    // ---------- QW5b: CLAMP vs REPEAT at UV=1.75 (out of range) ----------
    {
        Harness h; h.clear_black();
        const u32 TEX=Harness::BASE+0x100;
        const u8 tx[16]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
        std::memcpy((u8*)h.mem.data()+(TEX-Harness::BASE),tx,16);
        h.regs={0x0de1,9}; h.call(igl_slot::glBindTexture);
        h.regs={0x0de1,0,glenum::RGBA,2,2,0,glenum::RGBA,glenum::UBYTE,TEX};
        h.call(igl_slot::glTexImage2D);
        h.regs={0x0de1,MAG_FILTER,NEAREST}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,MIN_FILTER,NEAREST}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,WRAP_S,CLAMP_TO_EDGE}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,WRAP_T,CLAMP_TO_EDGE}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1}; h.call(igl_slot::glEnable);
        const u32 UV=Harness::BASE+0x180;
        // UV=1.1 -> CLAMP+NEAREST pins to last texel (1,1)=white; default
        // REPEAT+BILINEAR wraps & blends (fails RED).
        for(int i=0;i<3;i++){ h.mem[(UV-Harness::BASE)/4+i*2]=Harness::FX(1.1f);
                              h.mem[(UV-Harness::BASE)/4+i*2+1]=Harness::FX(1.1f); }
        h.regs={2,glenum::FIXED,0,UV}; h.call(igl_slot::glTexCoordPointer);
        h.regs={glenum::TEXCOORD_ARRAY}; h.call(igl_slot::glEnableClientState);
        h.put_tri();
        h.regs={h.FX(1),h.FX(1),h.FX(1),h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW5 clamp edge",h.center(),0xFFFF); // white (last texel)
    }
    // ---------- QW7a: depth GREATER passes over cleared far depth ----------
    {
        Harness h; h.clear_black();
        h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::DEPTH_TEST}; h.call(igl_slot::glEnable);
        h.regs={GREATER}; h.call(igl_slot::glDepthFunc);
        // z=0 vertices -> depth 0.5 < cleared 1.0, GREATER(0.5>1.0) is FALSE -> discard.
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW7 depth greater discard",h.center(),0x0000);
    }
    // ---------- QW7b: depth ALWAYS always draws ----------
    {
        Harness h; h.clear_black();
        h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::DEPTH_TEST}; h.call(igl_slot::glEnable);
        h.regs={ALWAYS}; h.call(igl_slot::glDepthFunc);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW7 depth always",h.center(),0xF800);
    }
    // ---------- QW7c: depth EQUAL boundary discard ----------
    {
        Harness h; h.clear_black();
        h.put_tri();
        h.regs={h.FX(0),h.FX(1),h.FX(0),h.FX(1)}; h.call(igl_slot::glColor4x); // green
        h.regs={glenum::DEPTH_TEST}; h.call(igl_slot::glEnable);
        h.regs={EQUAL}; h.call(igl_slot::glDepthFunc); // 0.5==1.0 false -> discard
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        check("QW7 depth equal discard",h.center(),0x0000);
    }
    // ---------- QW7d: blend src=ONE dst=ONE (additive) ----------
    {
        Harness h;
        // clear to dark red (0.5,0,0)
        h.regs={h.FX(0.5f),0,0,h.FX(1)}; h.call(igl_slot::glClearColorx);
        h.regs={COLOR_BIT|DEPTH_BIT}; h.call(igl_slot::glClear);
        h.put_tri();
        h.regs={h.FX(0.5f),0,0,h.FX(1)}; h.call(igl_slot::glColor4x); // add 0.5 red
        h.regs={glenum::BLEND}; h.call(igl_slot::glEnable);
        h.regs={ONE,ONE}; h.call(igl_slot::glBlendFunc);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        // 0.5+0.5=1.0 red -> 0xF800
        check("QW7 blend additive",h.center(),0xF800);
    }
    // ---------- QW7e: blend src=DST_COLOR dst=ZERO (modulate) ----------
    {
        Harness h;
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glClearColorx); // dst = red
        h.regs={COLOR_BIT|DEPTH_BIT}; h.call(igl_slot::glClear);
        h.put_tri();
        h.regs={h.FX(1),h.FX(1),h.FX(1),h.FX(1)}; h.call(igl_slot::glColor4x); // white src
        h.regs={glenum::BLEND}; h.call(igl_slot::glEnable);
        h.regs={DST_COLOR,ZERO}; h.call(igl_slot::glBlendFunc);
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        // src(1,1,1)*dst(1,0,0) + 0 = (1,0,0) red
        check("QW7 blend modulate",h.center(),0xF800);
    }
    // ---------- Safety: malformed glAlphaFuncx / glTexParameterx don't crash ----------
    {
        Harness h; h.clear_black();
        h.regs={0xDEADBEEF,0xFFFFFFFF}; h.call(igl_slot::glAlphaFuncx); // junk func/ref
        h.regs={0x0de1,0x9999,0x9999}; h.call(igl_slot::glTexParameterx); // junk pname
        h.regs={0,0xDEAD,0}; h.call(igl_slot::glTexParameterx); // no bound tex
        h.regs={0x0de1,BACK}; h.call(igl_slot::glCullFace);
        printf("[safety] malformed args handled without crash PASS\n");
    }

    // ---------- Default-state: GL_BLEND enabled WITHOUT glBlendFunc ----------
    // GLES1 §4.1.7: default blend function is (GL_ONE, GL_ZERO) => src*1 + dst*0
    // (source replaces). If RenderState.blend_src/dst default to (0,0) instead,
    // every fragment computes src*0+dst*0 = black — visible corruption for any
    // title that enables blending and relies on the default.
    {
        Harness h; h.clear_black();
        h.put_tri();
        h.regs={h.FX(1),h.FX(1),h.FX(1),h.FX(1)}; h.call(igl_slot::glColor4x); // white src
        h.regs={glenum::BLEND}; h.call(igl_slot::glEnable);                    // NO glBlendFunc
        h.regs={glenum::TRIANGLES,0,3}; h.call(igl_slot::glDrawArrays);
        // Default (ONE,ZERO) = "src substitui": src branco sobre dst preto => branco
        // 0xFFFF (o bug 0/0 -> ZERO/ZERO daria preto 0x0000).
        check("QWd default blend (ONE,ZERO) keeps src white",h.center(),0xFFFF);
    }
    // ---------- Default-state: glClear(0) is a no-op ----------
    // GL: glClear(0) must not touch the buffer. The impl used `mask==0 || mask&0x4000`
    // which turns glClear(0) into a full color clear. Test: fill red, then set the
    // clear color to green and call glClear(0) — a true no-op keeps red; the bug
    // (mask==0 treated as color clear) would overwrite it green.
    {
        Harness h;
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glClearColorx); // red clear
        h.regs={COLOR_BIT|DEPTH_BIT}; h.call(igl_slot::glClear);       // fill red
        h.regs={0,h.FX(1),0,h.FX(1)}; h.call(igl_slot::glClearColorx); // now green
        h.regs={0}; h.call(igl_slot::glClear);                          // glClear(0): must be no-op
        check("QWd glClear(0) is no-op (keeps red)",h.center(),0xF800);
    }

    printf(fails==0?"DONE ALL PASS\n":"DONE %d FAIL\n",fails);
    return fails==0?0:1;
}
