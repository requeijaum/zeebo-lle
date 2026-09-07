// gpu_display_integration.cpp — prova o HANDOFF GPU->display no build do emulador.
// Satisfaz GPU_TODO §7 FASE 1: "Ligar present → fb_sink (RGB565 640x480) já
// existente. Teste: clear → PPM", verificado por efeito (pixels no arquivo),
// NÃO por "chamada retornou". Usa o SoftRasterizer (oráculo de correção, sem
// GL host) e exporta PPM RGB565 640x480 — o mesmo formato que o fb_sink produz.
#include "igpu_rasterizer.h"
#include "igl_hook.h"
#include <cstdio>
#include <vector>
#include <cstring>
using namespace zeebo::gpu;

static void rgb565_to_rgb888(u16 px, u8& r, u8& g, u8& b){
    r = ((px>>11)&0x1f)*255/31; g=((px>>5)&0x3f)*255/63; b=(px&0x1f)*255/31; }

static bool export_ppm(const u16* fb, const char* path){
    FILE* fp=fopen(path,"wb"); if(!fp) return false;
    fprintf(fp,"P6\n%d %d\n255\n", kFbWidth, kFbHeight);
    std::vector<u8> rd(kFbWidth*kFbHeight*3);
    for(size_t i=0;i<(size_t)kFbWidth*kFbHeight;i++){
        u8 r,g,b; rgb565_to_rgb888(fb[i],r,g,b);
        rd[i*3+0]=r; rd[i*3+1]=g; rd[i*3+2]=b; }
    fwrite(rd.data(),1,rd.size(),fp); fclose(fp); return true; }

int main(){
    auto rast=create_rasterizer(Backend::SoftwareRef);
    if(!rast->init()){ printf("init FAIL\n"); return 1; }
    rast->begin_frame();
    IglHook hook(*rast);

    // Rede do fb_sink digitaliza 640x480 (mesma geometria do IGpuRasterizer).
    const int cx=kFbWidth/2, cy=kFbHeight/2;

    // F1: clear AZUL via IglHook (caminho real de produção: vtable IGL -> rast).
    auto FX=[](float f)->u32{ return u32(int32_t(f*65536.0f)); };
    std::vector<u32> regs;
    GuestMachine gm;
    gm.arg=[&](int n){ return n<(int)regs.size()?regs[n]:0u; };
    gm.set_ret=[](u32){};
    gm.read=[](u32,void*,u32)->bool{ return false; }; // sem arrays neste teste
    regs={FX(0),FX(0),FX(1),FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx, gm); // azul
    regs={0x4000};                   hook.dispatch_igl(igl_slot::glClear, gm);       // COLOR
    rast->end_frame();
    if(!export_ppm(rast->framebuffer_rgb565(), "/tmp/zeebo_gpu_f1_blue.ppm")){ printf("ppm1 FAIL\n"); return 1; }
    u16 b=rast->framebuffer_rgb565()[cy*kFbWidth+cx];
    bool f1 = (b==0x001F); // RGB565 azul puro (r,g=0, b=31)
    printf("[F1 clear-blue] center=0x%04X expect=0x001F %s\n", b, f1?"PASS":"FAIL");

    // F2: clear VERMELHO — pipeline vivo entre frames (begin/end + fb reset).
    rast->begin_frame();
    regs={FX(1),FX(0),FX(0),FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx, gm);
    regs={0x4000};                   hook.dispatch_igl(igl_slot::glClear, gm);
    rast->end_frame();
    if(!export_ppm(rast->framebuffer_rgb565(), "/tmp/zeebo_gpu_f2_red.ppm")){ printf("ppm2 FAIL\n"); return 1; }
    u16 r=rast->framebuffer_rgb565()[cy*kFbWidth+cx];
    bool f2 = (r==0xF800);
    printf("[F2 clear-red ] center=0x%04X expect=0xF800 %s\n", r, f2?"PASS":"FAIL");

    if(f1&&f2) printf("GPU->display integration PASS: /tmp/zeebo_gpu_f1_blue.ppm /tmp/zeebo_gpu_f2_red.ppm\n");
    else       { printf("GPU->display integration FAIL\n"); return 1; }
    return 0;
}