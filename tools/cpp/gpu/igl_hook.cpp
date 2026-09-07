// igl_hook.cpp — tradução das chamadas IGL/IEGL para IGpuRasterizer.
// Implementa os slots CONFIRMADOS (glClear/glClearColorx/glDrawArrays/
// glDrawElements/glViewport + AR/Rel/QI). Slots com índice -1 (placeholder) caem
// no ramo de stub honesto até AEEGL.h fixar o número — nunca fingem sucesso.
// Semântica de leitura de memória guest = ReadGlComponent do zeebulator (fixed/
// byte/short -> float host), lida SÓ no draw (VAs guardados como VA guest).
#include "igl_hook.h"
#include <cstring>

namespace zeebo::gpu {

// GLfixed (16.16) -> float; e conversões por tipo (espelha gl_types.h ReadGlComponent).
static inline float fixed_to_float(u32 v){ return int32_t(v) / 65536.0f; }

float GuestMachine::read_component(u32 va, u32 type) const {
    switch(type){
        case glenum::FLOAT: { u32 w=0; read(va,&w,4); float f; std::memcpy(&f,&w,4); return f; }
        case glenum::FIXED: { u32 w=0; read(va,&w,4); return fixed_to_float(w); }
        case glenum::BYTE:  { int8_t b=0;  read(va,&b,1); return b; }
        case glenum::UBYTE: { uint8_t b=0; read(va,&b,1); return b/255.0f; } // color-norm
        case glenum::SHORT: { int16_t s=0; read(va,&s,2); return s; }
        case glenum::USHORT:{ uint16_t s=0;read(va,&s,2); return s; }
        default: return 0.0f;
    }
}

void IglHook::read_matrix(GuestMachine& gm, u32 va, Mat4& out){
    for(int i=0;i<16;i++) out.m[i] = gm.read_component(va + i*4, glenum::FIXED);
}

std::vector<Vertex> IglHook::assemble(GuestMachine& gm, int first, int count){
    std::vector<Vertex> v; v.reserve(count);
    auto stride=[&](const GuestArray& a){ return a.stride? a.stride
                     : a.size*glenum::type_size(a.type); };
    for(int e=first; e<first+count; ++e){
        Vertex vx{};
        if(vtx_.enabled){ u32 p=vtx_.va + e*stride(vtx_);
            vx.x=gm.read_component(p, vtx_.type);
            vx.y=gm.read_component(p+glenum::type_size(vtx_.type), vtx_.type);
            if(vtx_.size>=3) vx.z=gm.read_component(p+2*glenum::type_size(vtx_.type), vtx_.type);
        }
        if(col_.enabled){ u32 p=col_.va + e*stride(col_);
            vx.r=gm.read_component(p, col_.type);
            vx.g=gm.read_component(p+glenum::type_size(col_.type), col_.type);
            vx.b=gm.read_component(p+2*glenum::type_size(col_.type), col_.type);
            if(col_.size>=4) vx.a=gm.read_component(p+3*glenum::type_size(col_.type), col_.type);
        }
        if(tex_.enabled){ u32 p=tex_.va + e*stride(tex_);
            vx.u=gm.read_component(p, tex_.type);
            vx.v=gm.read_component(p+glenum::type_size(tex_.type), tex_.type);
        }
        v.push_back(vx);
    }
    return v;
}

std::vector<Vertex> IglHook::assemble_indexed(GuestMachine& gm, u32 idx_va, int count,
        u32 idx_type, std::vector<u32>& out_idx){
    // Lê índices da memória guest; monta o range max e reusa assemble por índice.
    out_idx.clear(); out_idx.reserve(count);
    u32 maxi=0;
    for(int i=0;i<count;i++){
        u32 id=0;
        if(idx_type==glenum::UBYTE){ uint8_t b=0; gm.read(idx_va+i,&b,1); id=b; }
        else { uint16_t s=0; gm.read(idx_va+i*2,&s,2); id=s; }
        out_idx.push_back(id); if(id>maxi) maxi=id;
    }
    return assemble(gm, 0, int(maxi)+1);
}

bool IglHook::dispatch_igl(int slot, GuestMachine& gm){
    using namespace igl_slot;
    // IBase herdado: po EM R0 (ABI sutileza 2). Refcount é no-op p/ o rasterizer.
    if(slot==AddRef || slot==Release){ gm.set_ret(1); return true; }
    if(slot==QueryInterface){ gm.set_ret(0); return true; }

    // Slots gl*: R0 = PRIMEIRO ARGUMENTO REAL (não po).
    if(slot==glViewport){                       // glViewport(x,y,w,h)
        rast_.set_viewport(int(gm.arg(0)),int(gm.arg(1)),int(gm.arg(2)),int(gm.arg(3)));
        return true;
    }
    if(slot==glClearColorx){                     // glClearColorx(r,g,b,a) em GLfixed
        rast_.clear_color(fixed_to_float(gm.arg(0)),fixed_to_float(gm.arg(1)),
                          fixed_to_float(gm.arg(2)),fixed_to_float(gm.arg(3)));
        return true;
    }
    if(slot==glClear){                           // glClear(mask)
        rast_.clear(gm.arg(0)); return true;
    }
    // --- gl*Pointer: guardam VA guest + formato; lidos SÓ no draw (assemble). ---
    // Assinatura real: gl{Vertex,TexCoord}Pointer(size,type,stride,ptr);
    //                  glColorPointer(size,type,stride,ptr) idem;
    //                  glNormalPointer(type,stride,ptr) — SEM size (sempre 3).
    if(slot==glVertexPointer){
        vtx_={true, gm.arg(3), int(gm.arg(0)), gm.arg(1), int(gm.arg(2))}; return true; }
    if(slot==glColorPointer){
        col_={true, gm.arg(3), int(gm.arg(0)), gm.arg(1), int(gm.arg(2))}; return true; }
    if(slot==glTexCoordPointer){
        tex_={true, gm.arg(3), int(gm.arg(0)), gm.arg(1), int(gm.arg(2))}; return true; }
    if(slot==glNormalPointer){
        nrm_={true, gm.arg(2), 3, gm.arg(0), int(gm.arg(1))}; return true; }
    // --- glEnable/DisableClientState(array): liga/desliga o array correspondente. ---
    if(slot==glEnableClientState || slot==glDisableClientState){
        bool on = (slot==glEnableClientState);
        switch(gm.arg(0)){
            case glenum::VERTEX_ARRAY:   vtx_.enabled=on; break;
            case glenum::COLOR_ARRAY:    col_.enabled=on; break;
            case glenum::TEXCOORD_ARRAY: tex_.enabled=on; break;
            case glenum::NORMAL_ARRAY:   nrm_.enabled=on; break;
        }
        return true;
    }
    if(slot==glDrawArrays){                       // glDrawArrays(mode,first,count)
        u32 mode=gm.arg(0); int first=int(gm.arg(1)), count=int(gm.arg(2));
        rast_.set_mvp(mvp_); rast_.set_state(state_);
        rast_.draw(glenum::to_prim(mode), assemble(gm, first, count));
        return true;
    }
    if(slot==glDrawElements){                      // glDrawElements(mode,count,type,idx*)
        u32 mode=gm.arg(0); int count=int(gm.arg(1)); u32 type=gm.arg(2), idx_va=gm.arg(3);
        std::vector<u32> idx;
        auto verts=assemble_indexed(gm, idx_va, count, type, idx);
        rast_.set_mvp(mvp_); rast_.set_state(state_);
        rast_.draw_indexed(glenum::to_prim(mode), verts, idx);
        return true;
    }
    // slot placeholder (-1) ou não traduzido: stub HONESTO — não finge sucesso de
    // desenho, apenas devolve 0 e sinaliza "não tratado plenamente" via return true
    // (consumido, mas sem efeito). Marcado p/ implementar quando AEEGL.h fixar índices.
    gm.set_ret(0);
    return false;   // false = slot reconhecido como IGL mas ainda não modelado
}

bool IglHook::dispatch_iegl(int slot, GuestMachine& gm){
    using namespace iegl_slot;
    if(slot==AddRef || slot==Release){ gm.set_ret(1); return true; }
    if(slot==QueryInterface){ gm.set_ret(0); return true; }
    // eglSwapBuffers -> apresenta o frame no fb_sink existente.
    if(slot==eglSwapBuffers && eglSwapBuffers>=0){
        rast_.end_frame(); rast_.begin_frame(); gm.set_ret(1); return true;
    }
    gm.set_ret(1);   // EGL lifecycle: devolver sucesso é seguro (handles sentinela)
    return false;
}

} // namespace zeebo::gpu
