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
    // (mask 0x4100 = GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT; glClear(0) é no-op por
    //  GL, então "limpar tudo" precisa do mask real — não usar 0 como sentinela.)
    ras->begin_frame();
    ras->clear_color(1.f,0.f,0.f,1.f);
    ras->clear(0x4100);
    ras->end_frame();
    u16 c = px(*ras, 320,240);
    const bool t1 = c==0xF800;
    printf("[T1 clear-red] center=0x%04X expect=0xF800 %s\n", c, t1?"PASS":"FAIL");

    // Test 2: draw a green triangle covering center -> center becomes green-ish.
    ras->begin_frame();
    ras->clear_color(0,0,0,1); ras->clear(0x4100);
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

    // Test 5: textura RGBA8 ligada à unidade 0 deve modular a cor branca.
    ras->clear_color(0,0,0,1); ras->clear(0);
    const u8 blue_rgba[16]={0,0,255,255, 0,0,255,255,
                            0,0,255,255, 0,0,255,255};
    ras->tex_image_2d(7,2,2,blue_rgba);
    ras->bind_texture(0,7);
    RenderState textured{}; textured.tex_enabled[0]=1; textured.active_unit=0;
    ras->set_state(textured);
    std::vector<Vertex> textri(3);
    textri[0].x=-0.8f; textri[0].y=-0.8f; textri[0].u=0.25f; textri[0].v=0.25f;
    textri[1].x= 0.8f; textri[1].y=-0.8f; textri[1].u=0.25f; textri[1].v=0.25f;
    textri[2].x= 0.0f; textri[2].y= 0.8f; textri[2].u=0.25f; textri[2].v=0.25f;
    ras->draw(Prim::Triangles,textri);
    const bool t5 = px(*ras,320,240)==0x001f;
    printf("[T5 texture  ] center=0x%04X expect=0x001F %s\n",
           px(*ras,320,240),t5?"PASS":"FAIL");

    // Test 6: depth LESS preserva o triângulo próximo contra desenho distante.
    RenderState depth{}; depth.depth_test=true; depth.depth_func=0x0201; depth.depth_write=true;
    ras->set_state(depth); ras->clear_color(0,0,0,1); ras->clear(0x4100);
    std::vector<Vertex> near_tri=textri, far_tri=textri;
    for(auto&v:near_tri){ v.z=-0.5f; v.r=0;v.g=1;v.b=0; }
    for(auto&v:far_tri){ v.z=0.5f; v.r=1;v.g=0;v.b=0; }
    ras->draw(Prim::Triangles,near_tri);
    ras->draw(Prim::Triangles,far_tri);
    const u16 depth_px=px(*ras,320,240);
    const bool t6=((depth_px>>5)&0x3f)>0x30 && (depth_px>>11)==0;
    printf("[T6 depth    ] center=0x%04X expect=green %s\n",depth_px,t6?"PASS":"FAIL");

    // Test 7: SRC_ALPHA/ONE_MINUS_SRC_ALPHA compõe vermelho 50% sobre azul.
    RenderState blend{}; blend.blend=true; blend.blend_src=0x0302; blend.blend_dst=0x0303;
    ras->set_state(blend); ras->clear_color(0,0,1,1); ras->clear(0x4000);
    std::vector<Vertex> alpha_tri=textri;
    for(auto&v:alpha_tri){ v.r=1;v.g=0;v.b=0;v.a=0.5f; }
    ras->draw(Prim::Triangles,alpha_tri);
    const u16 blend_px=px(*ras,320,240);
    const u32 br=blend_px>>11, bg=(blend_px>>5)&0x3f, bb=blend_px&0x1f;
    const bool t7=br>=14&&br<=17&&bg==0&&bb>=14&&bb<=17;
    printf("[T7 blend    ] center=0x%04X expect=purple %s\n",blend_px,t7?"PASS":"FAIL");

    // Test 8: filtro linear padrão interpola os quatro texels no centro.
    const u8 corners[16]={255,0,0,255, 0,255,0,255,
                          0,0,255,255, 255,255,255,255};
    ras->tex_image_2d(9,2,2,corners); ras->bind_texture(0,9);
    RenderState linear{}; linear.tex_enabled[0]=1; ras->set_state(linear);
    ras->clear_color(0,0,0,1); ras->clear(0x4000);
    std::vector<Vertex> linear_tri=textri;
    for(auto&v:linear_tri){ v.u=0.5f;v.v=0.5f;v.r=v.g=v.b=v.a=1; }
    ras->draw(Prim::Triangles,linear_tri);
    const u16 linear_px=px(*ras,320,240);
    const u32 lr=linear_px>>11, lg=(linear_px>>5)&0x3f, lb=linear_px&0x1f;
    const bool t8=lr>=14&&lr<=17&&lg>=30&&lg<=33&&lb>=14&&lb<=17;
    printf("[T8 bilinear ] center=0x%04X expect=gray %s\n",linear_px,t8?"PASS":"FAIL");

    printf("DONE\n");
    return (t1 && green && t3 && t4 && t5 && t6 && t7 && t8) ? 0 : 1;
}
