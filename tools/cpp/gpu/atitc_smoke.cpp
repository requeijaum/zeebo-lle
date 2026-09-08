// atitc_smoke.cpp — TEST-FIRST (RED before GREEN) for QW9 ATITC/ATC host decode.
//
// Written BEFORE the decoder or the IGL slot-15 path exist, so it MUST fail to
// build/run against the current tree (genuine RED). Provenance of expected
// vectors: derived by the INDEPENDENT oracle atc_oracle.py (see its header) from
// the Khronos AMD_compressed_ATC_texture enums/block sizes + the Guild Software
// 2012 public paper documenting the ATC block layout (Chainfire's public XDA
// docs; no RE, no NDA, no third-party decoder source reused).
//
// Coverage: RGB method0 + method1, explicit alpha, interpolated alpha, non-4x4
// dims/cropping, and malformed inputs (bad imageSize / zero dims / null pointer)
// that must NOT mutate state and must return false. Plus end-to-end slot 15
// (glCompressedTexImage2D) -> texture -> exact pixel/hash.
#include "atitc_decode.h"     // ABSENT until GREEN -> compile RED
#include "igl_hook.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
using namespace zeebo::gpu;

static uint32_t fnv1a(const uint8_t* d, size_t n){
    uint32_t h=0x811c9dc5u;
    for(size_t i=0;i<n;i++){ h^=d[i]; h*=0x01000193u; }
    return h;
}

// ---- Pinned vectors emitted by atc_oracle.py (fnv over full RGBA8 output) ----
static const uint8_t V_RGB_M0[8]={0x00,0x7c,0x1f,0x00,0xe4,0xe4,0xe4,0xe4};
static const uint32_t H_RGB_M0=0xc4cb9da5u;
static const uint8_t V_RGB_M1[8]={0x94,0xd2,0x8a,0x52,0x1b,0x1b,0x1b,0x1b};
static const uint32_t H_RGB_M1=0x0a3fb245u;
static const uint8_t V_EXPL[16]={0x0f,0xf0,0x0f,0xf0,0x0f,0xf0,0x0f,0xf0,
                                 0x00,0x7c,0x1f,0x00,0x00,0x00,0x00,0x00};
static const uint32_t H_EXPL=0xad427775u;
static const uint8_t V_INTP[16]={0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x7c,0x1f,0x00,0x55,0x55,0x55,0x55};
static const uint32_t H_INTP=0x36079025u;
static const uint8_t V_CROP[32]={0x00,0x7c,0x1f,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x7c,0x1f,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x7c,0x1f,0x00,0x00,0x00,0x00,0x00,
                                 0x00,0x7c,0x1f,0x00,0x00,0x00,0x00,0x00};
static const uint32_t H_CROP=0xb88f83fdu;

static int fails=0;
static bool check_decode(const char* name,u32 fmt,int w,int h,
                         const uint8_t* in,size_t insz,uint32_t expect){
    std::vector<uint8_t> out;
    bool ok=decode_atitc(fmt,w,h,in,insz,out);
    uint32_t got = ok ? fnv1a(out.data(),out.size()) : 0;
    bool pass = ok && out.size()==size_t(w)*h*4 && got==expect;
    printf("[dec %-8s] fmt=0x%04X %dx%d ok=%d fnv=0x%08x expect=0x%08x %s\n",
           name,fmt,w,h,ok,got,expect,pass?"PASS":"FAIL");
    if(!pass) fails++;
    return pass;
}

int main(){
    // 1) Positive decodes — exact hash over full RGBA8.
    check_decode("RGB_M0",glenum::ATITC_RGB, 4,4,V_RGB_M0,sizeof(V_RGB_M0),H_RGB_M0);
    check_decode("RGB_M1",glenum::ATITC_RGB, 4,4,V_RGB_M1,sizeof(V_RGB_M1),H_RGB_M1);
    check_decode("EXPL",  glenum::ATITC_RGBA_EXPLICIT, 4,4,V_EXPL,sizeof(V_EXPL),H_EXPL);
    check_decode("INTP",  glenum::ATITC_RGBA_INTERP,   4,4,V_INTP,sizeof(V_INTP),H_INTP);
    check_decode("CROP66",glenum::ATITC_RGB, 6,6,V_CROP,sizeof(V_CROP),H_CROP);

    // 1b) exact corner pixel of RGB_M0: texel0 must be pure red (t=0 -> color0).
    {
        std::vector<uint8_t> out;
        decode_atitc(glenum::ATITC_RGB,4,4,V_RGB_M0,sizeof(V_RGB_M0),out);
        bool px=out.size()>=4 && out[0]==0xff && out[1]==0x00 && out[2]==0x00 && out[3]==0xff;
        printf("[pixel   ] RGB_M0 texel0=%02x%02x%02x%02x expect=ff0000ff %s\n",
               out.size()>=4?out[0]:0,out.size()>=4?out[1]:0,
               out.size()>=4?out[2]:0,out.size()>=4?out[3]:0,px?"PASS":"FAIL");
        if(!px) fails++;
    }

    // 2) Malformed inputs: MUST return false and NOT write output (no fallback true).
    auto reject=[&](const char* why,u32 fmt,int w,int h,const uint8_t* in,size_t sz){
        std::vector<uint8_t> out(7,0xAB); // pre-seed to detect illegal mutation
        bool ok=decode_atitc(fmt,w,h,in,sz,out);
        bool untouched = out.size()==7 && out[0]==0xAB;
        bool pass = !ok && untouched;
        printf("[reject  ] %-18s ok=%d untouched=%d %s\n",why,ok,untouched,pass?"PASS":"FAIL");
        if(!pass) fails++;
    };
    reject("null_ptr",     glenum::ATITC_RGB,4,4,nullptr,8);
    reject("zero_w",       glenum::ATITC_RGB,0,4,V_RGB_M0,8);
    reject("zero_h",       glenum::ATITC_RGB,4,0,V_RGB_M0,8);
    reject("short_size",   glenum::ATITC_RGB,4,4,V_RGB_M0,7);   // needs 8
    reject("expl_shortsz", glenum::ATITC_RGBA_EXPLICIT,4,4,V_EXPL,15); // needs 16
    reject("bad_format",   0x1234,4,4,V_RGB_M0,8);

    // 3) End-to-end: slot 15 glCompressedTexImage2D -> IGpuRasterizer texture.
    // glCompressedTexImage2D(target,level,internalformat,width,height,border,imageSize,data)
    {
        auto rast=create_rasterizer(Backend::SoftwareRef);
        rast->init(); rast->begin_frame();
        IglHook hook(*rast);
        std::vector<uint8_t> mem(0x400,0);
        const u32 TEX_VA=0x100;
        std::memcpy(mem.data()+TEX_VA,V_RGB_M0,sizeof(V_RGB_M0));
        std::vector<u32> regs; u32 ret=0;
        GuestMachine gm;
        gm.arg=[&](int n){ return n<(int)regs.size()?regs[n]:0u; };
        gm.set_ret=[&](u32 r){ ret=r; };
        gm.read=[&](u32 va,void* dst,u32 size)->bool{
            if(va+size>mem.size()) return false;
            std::memcpy(dst,mem.data()+va,size); return true; };

        regs={glenum::TEXTURE_2D,9}; hook.dispatch_igl(igl_slot::glBindTexture,gm);
        regs={glenum::TEXTURE_2D,0,glenum::ATITC_RGB,4,4,0,8,TEX_VA};
        bool handled=hook.dispatch_igl(igl_slot::glCompressedTexImage2D,gm);
        printf("[slot15  ] glCompressedTexImage2D handled=%d %s\n",
               handled,handled?"PASS":"FAIL");
        if(!handled) fails++;

        // Malformed slot-15 (imageSize too small AND too large) must NOT be handled.
        IglHook hook2(*rast);
        regs={glenum::TEXTURE_2D,9}; hook2.dispatch_igl(igl_slot::glBindTexture,gm);
        regs={glenum::TEXTURE_2D,0,glenum::ATITC_RGB,4,4,0,7,TEX_VA}; // too small
        bool bad=hook2.dispatch_igl(igl_slot::glCompressedTexImage2D,gm);
        printf("[slot15sml] small imageSize handled=%d expect=0 %s\n",
               bad,!bad?"PASS":"FAIL");
        if(bad) fails++;
        // too-large imageSize: decoder alone would accept (data>=need); only the
        // strict imageSize==need check in slot 15 rejects it (load-bearing guard).
        regs={glenum::TEXTURE_2D,0,glenum::ATITC_RGB,4,4,0,9,TEX_VA}; // 9 != 8
        bool big=hook2.dispatch_igl(igl_slot::glCompressedTexImage2D,gm);
        printf("[slot15big] large imageSize handled=%d expect=0 %s\n",
               big,!big?"PASS":"FAIL");
        if(big) fails++;
    }

    printf("DONE fails=%d\n",fails);
    return fails==0?0:1;
}
