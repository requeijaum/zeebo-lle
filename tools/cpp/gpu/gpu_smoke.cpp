// gpu_smoke.cpp — FASE 1 verification: prove the pipeline by FRAMEBUFFER, never
// by "call returned" (regra de ouro, FINDINGS 5a). Exercises interface + PM4 walk
// with SYNTHETIC data (no guest needed — MAP_CONTROL still blocks real execution).
// Build: see gpu/Makefile.gpu (isolated; does not touch the main Makefile).
#include "igpu_rasterizer.h"
#include "pm4_adreno.h"
#include <cstdio>
#include <vector>

using namespace zeebo::gpu;

static u16 px(const IGpuRasterizer& r,int x,int y){ return r.framebuffer_rgb565()[y*kFbWidth+x]; }

int main(){
    auto ras = create_rasterizer(Backend::SoftwareRef);
    if(!ras->init()){ printf("init FAIL\n"); return 1; }

    // Test 1: clear to pure red -> expect 0xF800 everywhere.
    ras->begin_frame();
    ras->clear_color(1.f,0.f,0.f,1.f);
    ras->clear(0);
    ras->end_frame();
    u16 c = px(*ras, 320,240);
    const bool t1 = c==0xF800;
    printf("[T1 clear-red] center=0x%04X expect=0xF800 %s\n", c, t1?"PASS":"FAIL");

    // Test 2: draw a green triangle covering center -> center becomes green-ish.
    ras->begin_frame();
    ras->clear_color(0,0,0,1); ras->clear(0);
    std::vector<Vertex> tri(3);
    for(auto&v:tri){ v.r=0;v.g=1;v.b=0; }
    tri[0].x=-0.8f; tri[0].y=-0.8f;
    tri[1].x= 0.8f; tri[1].y=-0.8f;
    tri[2].x= 0.0f; tri[2].y= 0.8f;
    ras->draw(Prim::Triangles, tri);
    ras->end_frame();
    c = px(*ras, 320,240);
    bool green = ((c>>5)&0x3F) > 0x30 && (c>>11)==0 && (c&0x1F)==0;
    printf("[T2 draw-tri ] center=0x%04X %s\n", c, green?"PASS":"FAIL");

    // Test 3: PM4 walker on a synthetic A2xx stream: 1 type0 (2 regs) + 1 DrawIndx2.
    std::vector<u32> ring;
    // type0 header: type=0, base=0x2000, count(N-1)=1 -> body 2 dwords
    ring.push_back((0u<<30)|(1u<<16)|0x2000); ring.push_back(0xAAAA); ring.push_back(0xBBBB);
    // type3 header: type=3, op=DrawIndx2(0x36), count(N-1)=0 -> body 1 dword
    ring.push_back((3u<<30)|(0u<<16)|(0x36<<8)); ring.push_back(3 /*vtx count*/);
    int regs=0, draws=0;
    Pm4Sink sink;
    sink.write_reg=[&](u32,u32){ ++regs; };
    sink.draw=[&](bool,u32 n,Prim){ ++draws; printf("   [pm4] draw n=%u\n",n); };
    u32 pk = pm4_walk(ring.data(), ring.size(), sink);
    const bool t3 = pk==2&&regs==2&&draws==1;
    printf("[T3 pm4-walk ] packets=%u regs=%d draws=%d %s\n",
           pk, regs, draws, t3?"PASS":"FAIL");

    // Test 4: strip de quatro vértices deve formar o quad inteiro, inclusive
    // o canto superior direito que não pertence ao primeiro triângulo.
    ras->clear_color(0,0,0,1); ras->clear(0);
    std::vector<Vertex> strip(4);
    for(auto&v:strip){ v.r=1;v.g=1;v.b=1; }
    strip[0].x=-0.8f; strip[0].y=-0.8f;
    strip[1].x= 0.8f; strip[1].y=-0.8f;
    strip[2].x=-0.8f; strip[2].y= 0.8f;
    strip[3].x= 0.8f; strip[3].y= 0.8f;
    ras->draw(Prim::TriStrip, strip);
    const bool t4 = px(*ras,480,120)!=0;
    printf("[T4 tri-strip] top-right=%s\n", t4?"PASS":"FAIL");

    printf("DONE\n");
    return (t1 && green && t3 && t4) ? 0 : 1;
}
