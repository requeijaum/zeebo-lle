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
    std::vector<u32> mem(0x200/4,0); // VA base = 0x1000
    float verts[3][2]={{0.0f,-0.8f},{-0.8f,0.8f},{0.8f,0.8f}}; // NDC [-1,1]; ver GPU_TODO §15
    size_t word=0;
    for(auto&v:verts){ mem[word++]=FX(v[0]); mem[word++]=FX(v[1]); }
    const u32 VTX_VA=0x1000, UV_VA=0x1080, TEX_VA=0x1100;
    for(int i=0;i<3;i++){
        mem[(UV_VA-VTX_VA)/4+i*2]=FX(0.25f);
        mem[(UV_VA-VTX_VA)/4+i*2+1]=FX(0.25f);
    }
    const u8 blue_rgba[16]={0,0,255,255, 0,0,255,255,
                            0,0,255,255, 0,0,255,255};
    std::memcpy(reinterpret_cast<u8*>(mem.data())+(TEX_VA-VTX_VA),blue_rgba,sizeof(blue_rgba));
    const u32 TEX565_VA=0x1120, IDX_VA=0x1140;
    const u16 red565[4]={0xf800,0xf800,0xf800,0xf800};
    std::memcpy(reinterpret_cast<u8*>(mem.data())+(TEX565_VA-VTX_VA),red565,sizeof(red565));
    const u16 sparse_indices[3]={50000,1,50000};
    std::memcpy(reinterpret_cast<u8*>(mem.data())+(IDX_VA-VTX_VA),sparse_indices,sizeof(sparse_indices));

    // GuestMachine falso: args por vetor, read() mapeia VA->nosso vetor mem.
    std::vector<u32> regs; u32 ret=0; size_t read_calls=0;
    GuestMachine gm;
    gm.arg=[&](int n){ return n<(int)regs.size()?regs[n]:0u; };
    gm.set_ret=[&](u32 r){ ret=r; };
    gm.read=[&](u32 va, void* dst, u32 size)->bool{
        ++read_calls;
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

    // Frontend IGL: upload RGBA8 + estado/array de textura devem chegar ao backend.
    regs={0x0de1,7}; hook.dispatch_igl(igl_slot::glBindTexture,gm);
    regs={0x0de1,0,glenum::RGBA,2,2,0,glenum::RGBA,glenum::UBYTE,TEX_VA};
    hook.dispatch_igl(igl_slot::glTexImage2D,gm);
    regs={0x0de1}; hook.dispatch_igl(igl_slot::glEnable,gm);
    regs={2,glenum::FIXED,0,UV_VA}; hook.dispatch_igl(igl_slot::glTexCoordPointer,gm);
    regs={glenum::TEXCOORD_ARRAY}; hook.dispatch_igl(igl_slot::glEnableClientState,gm);
    regs={0,0,0,FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx,gm);
    regs={0x4000}; hook.dispatch_igl(igl_slot::glClear,gm);
    regs={glenum::TRIANGLES,0,3}; hook.dispatch_igl(igl_slot::glDrawArrays,gm);
    center=rast->framebuffer_rgb565()[240*640+320];
    const bool texture_ok=center==0x001f;
    printf("[igl texture] center=0x%04X expect=0x001F %s\n",center,texture_ok?"PASS":"FAIL");

    regs={0x0de1,8}; hook.dispatch_igl(igl_slot::glBindTexture,gm);
    regs={0x0de1,0,glenum::RGB,2,2,0,glenum::RGB,0x8363,TEX565_VA};
    const bool upload565=hook.dispatch_igl(igl_slot::glTexImage2D,gm);
    regs={0,0,0,FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx,gm);
    regs={0x4000}; hook.dispatch_igl(igl_slot::glClear,gm);
    regs={glenum::TRIANGLES,0,3}; hook.dispatch_igl(igl_slot::glDrawArrays,gm);
    center=rast->framebuffer_rgb565()[240*640+320];
    const bool texture565_ok=upload565 && center==0xf800;
    printf("[igl rgb565 ] center=0x%04X expect=0xF800 %s\n",center,texture565_ok?"PASS":"FAIL");

    ret=0;
    const bool swap_ok=hook.dispatch_iegl(26,gm) && ret==1;
    printf("[iegl swap ] slot=26 handled=%d ret=%u %s\n",swap_ok,ret,swap_ok?"PASS":"FAIL");

    regs={0x0201}; const bool depth_func_ok=hook.dispatch_igl(igl_slot::glDepthFunc,gm);
    regs={1}; const bool depth_mask_ok=hook.dispatch_igl(igl_slot::glDepthMask,gm);
    regs={glenum::DEPTH_TEST}; const bool depth_enable_ok=hook.dispatch_igl(igl_slot::glEnable,gm);
    const bool depth_state_ok=depth_func_ok&&depth_mask_ok&&depth_enable_ok;
    printf("[igl depth ] frontend state handled=%d %s\n",depth_state_ok,depth_state_ok?"PASS":"FAIL");
    regs={0x0302,0x0303}; const bool blend_func_ok=hook.dispatch_igl(igl_slot::glBlendFunc,gm);
    regs={glenum::BLEND}; const bool blend_enable_ok=hook.dispatch_igl(igl_slot::glEnable,gm);
    const bool blend_state_ok=blend_func_ok&&blend_enable_ok;
    printf("[igl blend ] frontend state handled=%d %s\n",blend_state_ok,blend_state_ok?"PASS":"FAIL");

    const bool flush_ok = hook.dispatch_igl(igl_slot::glFlush, gm);
    const bool finish_ok = hook.dispatch_igl(igl_slot::glFinish, gm);
    const bool sync_ok = flush_ok && finish_ok;
    printf("[igl sync  ] glFlush=%d glFinish=%d %s\n", flush_ok, finish_ok, sync_ok?"PASS":"FAIL");

    regs={glenum::TEXTURE_2D}; hook.dispatch_igl(igl_slot::glDisable,gm);
    regs={glenum::DEPTH_TEST}; hook.dispatch_igl(igl_slot::glDisable,gm);
    regs={FX(1),0,0,FX(1)}; const bool color_call=hook.dispatch_igl(igl_slot::glColor4x,gm);
    regs={0,0,0,FX(1)}; hook.dispatch_igl(igl_slot::glClearColorx,gm);
    regs={0x4000}; hook.dispatch_igl(igl_slot::glClear,gm);
    regs={glenum::TRIANGLES,0,3}; hook.dispatch_igl(igl_slot::glDrawArrays,gm);
    center=rast->framebuffer_rgb565()[240*640+320];
    const bool current_color_ok=color_call&&center==0xf800;
    printf("[igl color ] center=0x%04X expect=0xF800 %s\n",center,current_color_ok?"PASS":"FAIL");

    IglHook strict_hook(*rast);
    regs={0,0,0,FX(1)}; strict_hook.dispatch_igl(igl_slot::glClearColorx,gm);
    regs={0x4000}; strict_hook.dispatch_igl(igl_slot::glClear,gm);
    regs={2,glenum::FIXED,0,VTX_VA}; strict_hook.dispatch_igl(igl_slot::glVertexPointer,gm);
    regs={glenum::TRIANGLES,0,3}; strict_hook.dispatch_igl(igl_slot::glDrawArrays,gm);
    center=rast->framebuffer_rgb565()[240*640+320];
    const bool pointer_state_ok=center==0;
    printf("[igl arrays] pointer sem Enable não desenha: %s\n",pointer_state_ok?"PASS":"FAIL");

    IglHook sparse_hook(*rast);
    regs={2,glenum::FIXED,0,VTX_VA}; sparse_hook.dispatch_igl(igl_slot::glVertexPointer,gm);
    regs={glenum::VERTEX_ARRAY}; sparse_hook.dispatch_igl(igl_slot::glEnableClientState,gm);
    read_calls=0;
    regs={glenum::TRIANGLES,3,glenum::USHORT,IDX_VA};
    const bool sparse_draw_handled=sparse_hook.dispatch_igl(igl_slot::glDrawElements,gm);
    const bool sparse_index_ok=sparse_draw_handled && read_calls<32;
    printf("[igl sparse] reads=%zu expect<32 %s\n",read_calls,sparse_index_ok?"PASS":"FAIL");

    printf("DONE\n");
    return (draw_ok && fixed_ok && red_ok && texture_ok && texture565_ok && swap_ok &&
            depth_state_ok && blend_state_ok && current_color_ok && pointer_state_ok &&
            sparse_index_ok) ? 0 : 1;
}
