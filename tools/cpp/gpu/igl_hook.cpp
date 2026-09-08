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

// obj (x,y,z,1) -> clip = mvp*v -> NDC = clip/w. Z preservado p/ depth (não usado ainda).
void IglHook::transform_vertex(const Mat4& mvp, Vertex& v){
    const f32* m = mvp.m; // col-major
    f32 x=v.x,y=v.y,z=v.z,w=1;
    f32 cx = m[0]*x+m[4]*y+m[8]*z+m[12]*w;
    f32 cy = m[1]*x+m[5]*y+m[9]*z+m[13]*w;
    f32 cz = m[2]*x+m[6]*y+m[10]*z+m[14]*w;
    f32 cw = m[3]*x+m[7]*y+m[11]*z+m[15]*w;
    if (cw != 0){ v.x=cx/cw; v.y=cy/cw; v.z=cz/cw; }
    else { v.x=0; v.y=0; v.z=0; } // w=0: ponte no infinito; fora da view
}

std::vector<Vertex> IglHook::assemble(GuestMachine& gm, int first, int count){
    std::vector<Vertex> v; v.reserve(count);
    // Composite mvp = projection * modelview (GL: clip = proj * (modelview * obj)).
    Mat4 mvp_; mat_mul(mvp_, projection_, modelview_);
    auto stride=[&](const GuestArray& a){ return a.stride? a.stride
                     : a.size*glenum::type_size(a.type); };
    for(int e=first; e<first+count; ++e){
        Vertex vx{};
        vx.r=current_color_.r; vx.g=current_color_.g;
        vx.b=current_color_.b; vx.a=current_color_.a;
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
        transform_vertex(mvp_, vx);   // obj -> NDC (vértice pronto p/ o rasterizer)
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
    if(slot==glColor4x){
        current_color_.r=fixed_to_float(gm.arg(0));
        current_color_.g=fixed_to_float(gm.arg(1));
        current_color_.b=fixed_to_float(gm.arg(2));
        current_color_.a=fixed_to_float(gm.arg(3));
        return true;
    }
    if(slot==glActiveTexture || slot==igl_slot::glClientActiveTexture){
        const u32 requested=gm.arg(0);
        state_.active_unit=requested>=glenum::TEXTURE0 ?
            std::min<u32>(requested-glenum::TEXTURE0,1) : 0;
        return true;
    }
    if(slot==glDepthFunc){ state_.depth_func=gm.arg(0); return true; }
    if(slot==glDepthMask){ state_.depth_write=gm.arg(0)!=0; return true; }
    if(slot==glBlendFunc){ state_.blend_src=gm.arg(0); state_.blend_dst=gm.arg(1); return true; }
    if(slot==glBindTexture){
        const u32 unit=std::min<u32>(state_.active_unit,1);
        bound_tex_[unit]=gm.arg(1);
        rast_.bind_texture(unit,bound_tex_[unit]);
        return true;
    }
    if(slot==glEnable || slot==glDisable){
        const bool on=slot==glEnable;
        switch(gm.arg(0)){
            case glenum::TEXTURE_2D: state_.tex_enabled[std::min<u32>(state_.active_unit,1)]=on; break;
            case glenum::DEPTH_TEST: state_.depth_test=on; break;
            case glenum::BLEND: state_.blend=on; break;
            case glenum::ALPHA_TEST: state_.alpha_test=on; break;
            case glenum::CULL_FACE: state_.cull=on; break;
        }
        return true;
    }
    if(slot==glTexImage2D){
        const u32 level=gm.arg(1), width=gm.arg(3), height=gm.arg(4);
        const u32 format=gm.arg(6), type=gm.arg(7), pixels=gm.arg(8);
        const bool rgba8=format==glenum::RGBA && type==glenum::UBYTE;
        const bool rgb565=format==glenum::RGB && type==glenum::USHORT_565;
        if(level!=0 || width==0 || height==0 || width>4096 || height>4096 ||
           (!rgba8 && !rgb565) || pixels==0) return false;
        const uint64_t texels=static_cast<uint64_t>(width)*height;
        const uint64_t bytes=texels*(rgba8?4:2);
        if(bytes>64*1024*1024) return false;
        std::vector<u8> raw(static_cast<size_t>(bytes));
        if(!gm.read(pixels,raw.data(),static_cast<u32>(bytes))) return false;
        std::vector<u8> rgba;
        if(rgba8){
            rgba=std::move(raw);
        } else {
            rgba.resize(static_cast<size_t>(texels)*4);
            for(size_t i=0;i<static_cast<size_t>(texels);++i){
                const u16 p=static_cast<u16>(raw[i*2] | (static_cast<u16>(raw[i*2+1])<<8));
                rgba[i*4]=static_cast<u8>(((p>>11)&31)*255/31);
                rgba[i*4+1]=static_cast<u8>(((p>>5)&63)*255/63);
                rgba[i*4+2]=static_cast<u8>((p&31)*255/31);
                rgba[i*4+3]=255;
            }
        }
        rast_.tex_image_2d(bound_tex_[std::min<u32>(state_.active_unit,1)],
                           static_cast<int>(width),static_cast<int>(height),rgba.data());
        return true;
    }
    // --- gl*Pointer: guardam VA guest + formato; lidos SÓ no draw (assemble). ---
    // Assinatura real: gl{Vertex,TexCoord}Pointer(size,type,stride,ptr);
    //                  glColorPointer(size,type,stride,ptr) idem;
    //                  glNormalPointer(type,stride,ptr) — SEM size (sempre 3).
    if(slot==glVertexPointer){
        vtx_={vtx_.enabled, gm.arg(3), int(gm.arg(0)), gm.arg(1), int(gm.arg(2))}; return true; }
    if(slot==glColorPointer){
        col_={col_.enabled, gm.arg(3), int(gm.arg(0)), gm.arg(1), int(gm.arg(2))}; return true; }
    if(slot==glTexCoordPointer){
        tex_={tex_.enabled, gm.arg(3), int(gm.arg(0)), gm.arg(1), int(gm.arg(2))}; return true; }
    if(slot==glNormalPointer){
        nrm_={nrm_.enabled, gm.arg(2), 3, gm.arg(0), int(gm.arg(1))}; return true; }
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
    // --- Matriz fixed-function (GPU_TODO §15): alimenta modelview/projection. ---
    if(slot==glMatrixMode){ matrix_mode_ = gm.arg(0); return true; }
    if(slot==glLoadIdentity){ mat_identity(cur_matrix()); return true; }
    if(slot==glLoadMatrixx){ Mat4 m; read_matrix(gm, gm.arg(0), m); cur_matrix()=m; return true; }
    if(slot==glMultMatrixx){ Mat4 m, t; read_matrix(gm, gm.arg(0), m);
        mat_mul(t, cur_matrix(), m); cur_matrix()=t; return true; } // cur = cur * m
    if(slot==glPushMatrix){
        int& depth = cur_stack_depth();
        if(depth<4) cur_stack()[depth++]=cur_matrix();
        return true;
    }
    if(slot==glPopMatrix){
        int& depth = cur_stack_depth();
        if(depth>0) cur_matrix()=cur_stack()[--depth];
        return true;
    }
    if(slot==glRotatex){ Mat4 r; mat_rotate(r,fixed_to_float(gm.arg(0)),
        fixed_to_float(gm.arg(1)),fixed_to_float(gm.arg(2)),fixed_to_float(gm.arg(3)));
        Mat4 t; mat_mul(t, cur_matrix(), r); cur_matrix()=t; return true; }
    if(slot==glScalex){ Mat4 s; mat_scale(s,fixed_to_float(gm.arg(0)),
        fixed_to_float(gm.arg(1)),fixed_to_float(gm.arg(2)));
        Mat4 t; mat_mul(t, cur_matrix(), s); cur_matrix()=t; return true; }
    if(slot==glTranslatex){ Mat4 s; mat_translate(s,fixed_to_float(gm.arg(0)),
        fixed_to_float(gm.arg(1)),fixed_to_float(gm.arg(2)));
        Mat4 t; mat_mul(t, cur_matrix(), s); cur_matrix()=t; return true; }
    if(slot==glFrustumx){ Mat4 p; mat_frustum(p,fixed_to_float(gm.arg(0)),
        fixed_to_float(gm.arg(1)),fixed_to_float(gm.arg(2)),fixed_to_float(gm.arg(3)),
        fixed_to_float(gm.arg(4)),fixed_to_float(gm.arg(5)));
        Mat4 t; mat_mul(t, cur_matrix(), p); cur_matrix()=t; return true; }
    if(slot==glOrthox){ Mat4 p; mat_ortho(p,fixed_to_float(gm.arg(0)),
        fixed_to_float(gm.arg(1)),fixed_to_float(gm.arg(2)),fixed_to_float(gm.arg(3)),
        fixed_to_float(gm.arg(4)),fixed_to_float(gm.arg(5)));
        Mat4 t; mat_mul(t, cur_matrix(), p); cur_matrix()=t; return true; }
    if(slot==glDrawArrays){                       // glDrawArrays(mode,first,count)
        const u32 mode=gm.arg(0), first_raw=gm.arg(1), count_raw=gm.arg(2);
        if (count_raw > 1'000'000u || first_raw > 0x7fffffffu - count_raw) {
            gm.set_ret(0);
            return false;
        }
        const int first=static_cast<int>(first_raw), count=static_cast<int>(count_raw);
        rast_.set_mvp(mvp_); rast_.set_state(state_);
        rast_.draw(glenum::to_prim(mode), assemble(gm, first, count));
        return true;
    }
    if(slot==glDrawElements){                      // glDrawElements(mode,count,type,idx*)
        const u32 mode=gm.arg(0), count_raw=gm.arg(1), type=gm.arg(2), idx_va=gm.arg(3);
        if (count_raw > 1'000'000u || (type != glenum::UBYTE && type != glenum::USHORT)) {
            gm.set_ret(0);
            return false;
        }
        const int count=static_cast<int>(count_raw);
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
    if(slot==eglSwapBuffers){
        rast_.end_frame(); rast_.begin_frame(); gm.set_ret(1); return true;
    }
    gm.set_ret(1);   // EGL lifecycle: devolver sucesso é seguro (handles sentinela)
    return false;
}

} // namespace zeebo::gpu
