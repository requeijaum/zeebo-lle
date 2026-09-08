// gl_alpha_sampler_smoke.cpp — QW16: strict pixel/state TDD for GLES1 alpha-test
// and texture-sampler hardening in the IGL dispatch (igl_hook.cpp).
//
// Hardening proven here (each by an EXACT RGB565 pixel and/or the dispatch
// return value — no Unicorn):
//   * glAlphaFuncx clamps the GLfixed reference to [0,1] (GLES1 §3.6.5).
//   * glAlphaFuncx rejects an invalid comparison enum WITHOUT mutating render
//     state, and returns false so the real firmware/wrapper path still runs.
//   * glTexParameterx validates (pname,param): MAG only NEAREST/LINEAR; MIN only
//     the implemented non-mipmap NEAREST/LINEAR (state-only, no LOD path — see
//     limitation note below); WRAP_S/WRAP_T only REPEAT/CLAMP_TO_EDGE. An invalid
//     pname or param is REJECTED: it must not silently mutate the sampler state
//     and must return false (firmware path preserved).
//   * Valid, in-range calls keep their existing pixel behavior unchanged.
//
// MIN/LOD LIMITATION (documented, intentional): the software rasterizer has no
// minification / mip-LOD pipeline. GL_TEXTURE_MIN_FILTER is therefore accepted
// ONLY for the non-mipmap NEAREST/LINEAR values and stored state-only (it selects
// the magnification-style sampler). The four *_MIPMAP_* min filters are rejected
// rather than silently treated as a base-level filter, so guest code that needs
// real mipmapping falls through to the firmware path instead of being lied to.
#include "igl_hook.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace zeebo::gpu;

namespace {
constexpr u32 NEVER=0x0200, LESS=0x0201, GREATER=0x0204, GEQUAL=0x0206, ALWAYS=0x0207;
constexpr u32 MAG_FILTER=0x2800, MIN_FILTER=0x2801, WRAP_S=0x2802, WRAP_T=0x2803;
constexpr u32 NEAREST=0x2600, LINEAR=0x2601, REPEAT=0x2901, CLAMP_TO_EDGE=0x812F;
constexpr u32 CLAMP_LEGACY=0x2900;                 // GL_CLAMP: not in GLES1 -> reject
constexpr u32 NEAREST_MIPMAP_NEAREST=0x2700;       // mipmap min filter -> reject (no LOD)
constexpr u32 LINEAR_MIPMAP_LINEAR=0x2703;         // mipmap min filter -> reject (no LOD)
constexpr u32 COLOR_BIT=0x4000, DEPTH_BIT=0x0100;

struct Harness {
    std::unique_ptr<IGpuRasterizer> rast = create_rasterizer(Backend::SoftwareRef);
    IglHook hook{*rast};
    std::vector<u32> mem = std::vector<u32>(0x400/4, 0);
    std::vector<u32> regs; u32 ret=0; bool handled=false;
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
    bool call(int slot){ handled=hook.dispatch_igl(slot,gm); return handled; }
    u16 center(){ return rast->framebuffer_rgb565()[240*640+320]; }
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
    // Textured full-screen triangle sampling UV=uv at every vertex.
    void put_textured_tri(float uv){
        const u32 TEX=BASE+0x100, UV=BASE+0x180;
        const u8 tx[16]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
        std::memcpy((u8*)mem.data()+(TEX-BASE),tx,16);
        regs={0x0de1,7}; call(igl_slot::glBindTexture);
        regs={0x0de1,0,glenum::RGBA,2,2,0,glenum::RGBA,glenum::UBYTE,TEX};
        call(igl_slot::glTexImage2D);
        for(int i=0;i<3;i++){ mem[(UV-BASE)/4+i*2]=FX(uv); mem[(UV-BASE)/4+i*2+1]=FX(uv); }
        regs={2,glenum::FIXED,0,UV}; call(igl_slot::glTexCoordPointer);
        regs={glenum::TEXCOORD_ARRAY}; call(igl_slot::glEnableClientState);
        put_tri();
        regs={FX(1),FX(1),FX(1),FX(1)}; call(igl_slot::glColor4x);
        regs={0x0de1}; call(igl_slot::glEnable);
    }
    void draw(){ regs={glenum::TRIANGLES,0,3}; call(igl_slot::glDrawArrays); }
};

int fails=0;
void check(const char* tag,u16 got,u16 want){
    bool ok=got==want; if(!ok) ++fails;
    printf("[%s] got=0x%04X want=0x%04X %s\n",tag,got,want,ok?"PASS":"FAIL");
}
void check_bool(const char* tag,bool got,bool want){
    bool ok=got==want; if(!ok) ++fails;
    printf("[%s] got=%d want=%d %s\n",tag,got,want,ok?"PASS":"FAIL");
}
}

int main(){
    // ---- A1: alpha ref > 1 is clamped to 1.0 (GEQUAL, alpha=1.0, ref=2.0) ----
    // clamp -> 1.0>=1.0 TRUE -> draw red. Unclamped -> 1.0>=2.0 FALSE -> black.
    {
        Harness h; h.clear_black(); h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(1)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::ALPHA_TEST}; h.call(igl_slot::glEnable);
        h.regs={GEQUAL,h.FX(2.0f)}; h.call(igl_slot::glAlphaFuncx);
        h.draw();
        check("QW16 alpha ref>1 clamp",h.center(),0xF800);
    }
    // ---- A2: alpha ref < 0 is clamped to 0.0 (GREATER, alpha=0.0, ref=-0.5) ----
    // clamp -> 0.0>0.0 FALSE -> discard(black). Unclamped -> 0.0>-0.5 TRUE -> red.
    {
        Harness h; h.clear_black(); h.put_tri();
        h.regs={0,0,0,0}; h.call(igl_slot::glColor4x); // alpha 0, but color red via ...
        h.regs={h.FX(1),0,0,0}; h.call(igl_slot::glColor4x); // red rgb, alpha 0
        h.regs={glenum::ALPHA_TEST}; h.call(igl_slot::glEnable);
        h.regs={GREATER,h.FX(-0.5f)}; h.call(igl_slot::glAlphaFuncx);
        h.draw();
        check("QW16 alpha ref<0 clamp",h.center(),0x0000);
    }
    // ---- A3: valid alpha func returns true (handled) ----
    {
        Harness h;
        check_bool("QW16 alpha valid handled",
            (h.regs={GREATER,h.FX(0.5f)}, h.call(igl_slot::glAlphaFuncx)), true);
    }
    // ---- A4: invalid alpha func rejected: state preserved + returns false ----
    // First set a valid GREATER/0.5 that discards a 0.25-alpha fragment (black).
    // Then an invalid enum must NOT overwrite the func to something permissive and
    // must return false (firmware path). Fragment stays discarded -> black.
    {
        Harness h; h.clear_black(); h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(0.25f)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::ALPHA_TEST}; h.call(igl_slot::glEnable);
        h.regs={GREATER,h.FX(0.5f)}; h.call(igl_slot::glAlphaFuncx); // 0.25>0.5 false
        bool r=(h.regs={0x9999,h.FX(0.0f)}, h.call(igl_slot::glAlphaFuncx)); // invalid
        h.draw();
        check_bool("QW16 alpha invalid returns false",r,false);
        check("QW16 alpha invalid no-mutate",h.center(),0x0000);
    }
    // ---- B1: invalid WRAP param must not mutate sampler state (rejected) ----
    // Establish NEAREST+CLAMP (white at UV=1.1). Then WRAP_S=GL_CLAMP(0x2900) is
    // invalid for GLES1: must be rejected, clamp_s stays true -> still white.
    // The unhardened path sets clamp_s=(param==0x812F)=false -> REPEAT -> not white.
    {
        Harness h; h.clear_black();
        h.put_textured_tri(1.1f);
        h.regs={0x0de1,MAG_FILTER,NEAREST}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,MIN_FILTER,NEAREST}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,WRAP_S,CLAMP_TO_EDGE}; h.call(igl_slot::glTexParameterx);
        h.regs={0x0de1,WRAP_T,CLAMP_TO_EDGE}; h.call(igl_slot::glTexParameterx);
        bool r=(h.regs={0x0de1,WRAP_S,CLAMP_LEGACY}, h.call(igl_slot::glTexParameterx));
        h.draw();
        check_bool("QW16 wrap invalid returns false",r,false);
        check("QW16 wrap invalid no-mutate",h.center(),0xFFFF);
    }
    // ---- B2: mipmap MIN filter rejected (no LOD path) ----
    {
        Harness h;
        check_bool("QW16 min mipmap rejected",
            (h.regs={0x0de1,MIN_FILTER,NEAREST_MIPMAP_NEAREST},
             h.call(igl_slot::glTexParameterx)), false);
        check_bool("QW16 min mipmap-lin rejected",
            (h.regs={0x0de1,MIN_FILTER,LINEAR_MIPMAP_LINEAR},
             h.call(igl_slot::glTexParameterx)), false);
    }
    // ---- B3: invalid MAG param rejected ----
    {
        Harness h;
        check_bool("QW16 mag mipmap rejected",
            (h.regs={0x0de1,MAG_FILTER,NEAREST_MIPMAP_NEAREST},
             h.call(igl_slot::glTexParameterx)), false);
    }
    // ---- B4: unknown pname rejected ----
    {
        Harness h;
        check_bool("QW16 pname invalid rejected",
            (h.regs={0x0de1,0x9999,NEAREST}, h.call(igl_slot::glTexParameterx)), false);
    }
    // ---- B5: valid param combos handled (return true) ----
    {
        Harness h; h.put_textured_tri(0.4f); // binds a texture so unit has an id
        check_bool("QW16 mag linear ok",
            (h.regs={0x0de1,MAG_FILTER,LINEAR}, h.call(igl_slot::glTexParameterx)), true);
        check_bool("QW16 min nearest ok",
            (h.regs={0x0de1,MIN_FILTER,NEAREST}, h.call(igl_slot::glTexParameterx)), true);
        check_bool("QW16 wrap repeat ok",
            (h.regs={0x0de1,WRAP_S,REPEAT}, h.call(igl_slot::glTexParameterx)), true);
        check_bool("QW16 wrap clamp ok",
            (h.regs={0x0de1,WRAP_T,CLAMP_TO_EDGE}, h.call(igl_slot::glTexParameterx)), true);
    }
    // ---- B6: non-TEXTURE_2D target still rejected (unchanged behavior) ----
    {
        Harness h;
        check_bool("QW16 bad target rejected",
            (h.regs={0x0de0,MAG_FILTER,NEAREST}, h.call(igl_slot::glTexParameterx)), false);
    }
    // ---- C1: valid alpha ref in-range keeps existing behavior (pass) ----
    {
        Harness h; h.clear_black(); h.put_tri();
        h.regs={h.FX(1),0,0,h.FX(0.9f)}; h.call(igl_slot::glColor4x);
        h.regs={glenum::ALPHA_TEST}; h.call(igl_slot::glEnable);
        h.regs={GEQUAL,h.FX(0.5f)}; h.call(igl_slot::glAlphaFuncx);
        h.draw();
        check("QW16 alpha in-range pass",h.center(),0xF800);
    }
    (void)NEVER; (void)LESS; (void)ALWAYS;

    printf(fails==0?"DONE ALL PASS\n":"DONE %d FAIL\n",fails);
    return fails==0?0:1;
}
