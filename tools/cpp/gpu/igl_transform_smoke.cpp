// igl_transform_smoke.cpp — PROVA o transform fixed-function (mvp+viewport),
// a correção do GPU_TODO §15 [ERRO-3]. Sem este transform, um glTranslatex não
// move nada (coords cruas iam direto ao rasterizer). Aqui validamos por PIXEL:
// um triângulo centrado desenhado, depois deslocado +0.5 NDC via modelview,
// deve trocar exatamente os pixels que pinta. Verificado por framebuffer.
#include "igl_hook.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace zeebo::gpu;

int main(){
    auto rast=create_rasterizer(Backend::SoftwareRef);
    rast->init();
    IglHook hook(*rast);
    auto FX=[](float f)->u32{ return u32(int32_t(f*65536.0f)); };

    // "memória" guest: 3 vértices XY em GLfixed (NDC input).
    std::vector<u32> mem;
    float verts[3][2]={{-0.3f,-0.3f},{0.3f,-0.3f},{0.0f,0.3f}}; // triângulo centrado
    for(auto&v:verts){ mem.push_back(FX(v[0])); mem.push_back(FX(v[1])); }
    const u32 VTX_VA=0x2000;

    std::vector<u32> regs;
    GuestMachine gm;
    gm.arg=[&](int n){ return n<(int)regs.size()?regs[n]:0u; };
    gm.set_ret=[](u32){};
    gm.read=[&](u32 va, void* d, u32 sz)->bool{
        if(va<VTX_VA) return false; u32 off=va-VTX_VA;
        if(off+sz>mem.size()*4) return false; std::memcpy(d,(uint8_t*)mem.data()+off,sz); return true; };

    auto clear_black=[&](){
        regs={FX(0),FX(0),FX(0),FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx, gm);
        regs={0x4000};                   hook.dispatch_igl(igl_slot::glClear, gm); };
    auto set_tri=[&](){
        regs={2,glenum::FIXED,0,VTX_VA}; hook.dispatch_igl(igl_slot::glVertexPointer, gm);
        regs={glenum::VERTEX_ARRAY};     hook.dispatch_igl(igl_slot::glEnableClientState, gm); };
    auto draw_tri=[&](){ regs={glenum::TRIANGLES,0,3}; hook.dispatch_igl(igl_slot::glDrawArrays, gm); };
    auto px=[&](int x,int y){ return rast->framebuffer_rgb565()[y*kFbWidth+x]; };
    auto wrt=[&](const char* p, const u16* fb){
        FILE* fp=fopen(p,"wb"); if(!fp)return; fprintf(fp,"P6\n%d %d\n255\n",kFbWidth,kFbHeight);
        std::vector<u8> rd(kFbWidth*kFbHeight*3);
        auto conv=[](u16 px,u8&r,u8&g,u8&b){r=((px>>11)&0x1f)*255/31;g=((px>>5)&0x3f)*255/63;b=(px&0x1f)*255/31;};
        for(size_t i=0;i<(size_t)kFbWidth*kFbHeight;i++){u8 r,g,b;conv(fb[i],r,g,b);rd[i*3]=r;rd[i*3+1]=g;rd[i*3+2]=b;}
        fwrite(rd.data(),1,rd.size(),fp); fclose(fp); };

    rast->begin_frame();
    // Identity default (mvp = identity). Viewport = full 640x480 (default).
    clear_black(); set_tri(); draw_tri(); rast->end_frame();
    u16 c0=px(320,240);   // centro: dentro do triângulo -> branco
    bool f1 = (c0 != 0x0000);
    printf("[T1 no-translate ] center(320,240)=0x%04X !=0 %s\n", c0, f1?"PASS":"FAIL");
    wrt("/tmp/zeebo_igl_t1.ppm", rast->framebuffer_rgb565());

    // glTranslatex(+0.5,0,0) na pilha modelview -> o tri deve deslocar 160px p/ dir.
    rast->begin_frame();
    clear_black(); set_tri();
    regs={FX(0.5f),FX(0),FX(0)}; hook.dispatch_igl(igl_slot::glTranslatex, gm);
    draw_tri(); rast->end_frame();
    u16 old_center = px(320,240);   // o centro ORIGINAL agora é fundo (fora do tri)
    u16 new_center = px(480,240);   // centro DESLOCADO (x=0.5 NDC -> 480) dentro
    bool f2 = (old_center == 0x0000) && (new_center != 0x0000);
    printf("[T2 translate+0.5] old(320,240)=0x%04X(black) new(480,240)=0x%04X(white) %s\n",
           old_center, new_center, f2?"PASS":"FAIL");
    wrt("/tmp/zeebo_igl_t2.ppm", rast->framebuffer_rgb565());

    if(f1&&f2) printf("fixed-function transform PASS (mvp+viewport): /tmp/zeebo_igl_t1.ppm /tmp/zeebo_igl_t2.ppm\n");
    else { printf("fixed-function transform FAIL\n"); return 1; }
    return 0;
}