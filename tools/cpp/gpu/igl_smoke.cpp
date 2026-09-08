// igl_smoke.cpp — prova o produtor CERTO (IglHook) por FRAMEBUFFER, sem Unicorn.
// Um GuestMachine falso serve args de registrador e uma "memória" host como se
// fosse a memória emulada. Roda uma sequência real GLES1.x: viewport, clearcolor,
// clear, um triângulo verde via glDrawArrays lendo um array de vértices GLfixed.
#include "igl_hook.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace zeebo::gpu;

int main(){
    auto rast = create_rasterizer(Backend::SoftwareRef);
    rast->init(); rast->begin_frame();
    IglHook hook(*rast);

    // --- "memória emulada" host: um array de 3 vértices XY em GLfixed (16.16) ----
    // Triângulo cobrindo o centro em NDC-ish (o soft rasterizer usa coords diretas).
    auto FX=[](float f)->u32{ return u32(int32_t(f*65536.0f)); };
    std::vector<u32> mem; // VA base = 0x1000, cada vértice = 2 fixed (x,y)
    float verts[3][2]={{0.0f,-0.8f},{-0.8f,0.8f},{0.8f,0.8f}}; // NDC [-1,1]; ver GPU_TODO §15
    for(auto&v:verts){ mem.push_back(FX(v[0])); mem.push_back(FX(v[1])); }
    const u32 VTX_VA=0x1000;

    // GuestMachine falso: args por vetor, read() mapeia VA->nosso vetor mem.
    std::vector<u32> regs; u32 ret=0;
    GuestMachine gm;
    gm.arg=[&](int n){ return n<(int)regs.size()?regs[n]:0u; };
    gm.set_ret=[&](u32 r){ ret=r; };
    gm.read=[&](u32 va, void* dst, u32 size)->bool{
        if(va<VTX_VA) return false;
        u32 off=va-VTX_VA; if(off+size>mem.size()*4) return false;
        std::memcpy(dst, (uint8_t*)mem.data()+off, size); return true; };

    // 1) glViewport(0,0,640,480)
    regs={0,0,640,480}; hook.dispatch_igl(igl_slot::glViewport, gm);
    // 2) glClearColorx(0,0,0,1) — preto
    regs={0,0,0,FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx, gm);
    // 3) glClear(COLOR_BUFFER_BIT=0x4000)
    regs={0x4000}; hook.dispatch_igl(igl_slot::glClear, gm);
    // 4) set vertex array manualmente (glVertexPointer ainda placeholder) para
    //    provar o caminho assemble->draw. No real, glVertexPointer preencheria isto.
    //    Aqui injetamos via um dispatch equivalente: usamos glDrawArrays direto,
    //    mas o array precisa estar 'enabled' -> exercitamos por reflexão de estado:
    //    como glVertexPointer é -1, montamos o estado com uma chamada de teste.
    //    (test-only hook: acessa o mesmo caminho que o slot real preencherá.)
    // Para não depender do slot placeholder, o smoke valida CLEAR (efeito garantido)
    // e a decodificação de GLfixed via read_component.
    // 4) glVertexPointer(2, GL_FIXED, 0, VTX_VA) — agora índice real (78)
    regs={2, glenum::FIXED, 0, VTX_VA}; hook.dispatch_igl(igl_slot::glVertexPointer, gm);
    // 5) glEnableClientState(GL_VERTEX_ARRAY)
    regs={glenum::VERTEX_ARRAY}; hook.dispatch_igl(igl_slot::glEnableClientState, gm);
    // 6) glDrawArrays(GL_TRIANGLES, 0, 3) — deve pintar o centro (dentro do triângulo)
    regs={glenum::TRIANGLES, 0, 3}; bool drew=hook.dispatch_igl(igl_slot::glDrawArrays, gm);
    rast->end_frame();
    const u16* fb=rast->framebuffer_rgb565();
    u16 center=fb[240*640+320];
    // triângulo desenhado com cor default (branco, verts sem color array) -> != preto
    const bool draw_ok = drew && center!=0x0000;
    printf("[igl draw  ] handled=%d center=0x%04X (!=0 dentro do tri) %s\n",
           drew, center, draw_ok?"PASS":"FAIL");

    // Valida a conversão GLfixed->float (o ponto mais sutil da ABI real).
    // vert[0].x = 0.0 em NDC agora; testa o componente y[0] = -0.8.
    float fy = gm.read_component(VTX_VA+4, glenum::FIXED); // vert0.y = -0.8
    const bool fixed_ok = fy>-0.81&&fy<-0.79;
    printf("[igl fixed ] read_component=%.2f expect=-0.80 %s\n", fy, fixed_ok?"PASS":"FAIL");

    // Segundo frame: clear vermelho confirma pipeline vivo.
    rast->begin_frame();
    regs={FX(1),0,0,FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx, gm);
    regs={0x4000}; hook.dispatch_igl(igl_slot::glClear, gm);
    rast->end_frame();
    center=rast->framebuffer_rgb565()[240*640+320];
    const bool red_ok = center==0xF800;
    printf("[igl red   ] center=0x%04X expect=0xF800 %s\n", center, red_ok?"PASS":"FAIL");
    printf("DONE\n");
    return (draw_ok && fixed_ok && red_ok) ? 0 : 1;
}
