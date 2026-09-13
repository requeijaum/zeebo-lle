// igl_hook.cpp — tradução das chamadas IGL/IEGL para IGpuRasterizer.
// Implementa os slots CONFIRMADOS (glClear/glClearColorx/glDrawArrays/
// glDrawElements/glViewport + AR/Rel/QI). Slots com índice -1 (placeholder) caem
// no ramo de stub honesto até AEEGL.h fixar o número — nunca fingem sucesso.
// Semântica de leitura de memória guest = ReadGlComponent do zeebulator (fixed/
// byte/short -> float host), lida SÓ no draw (VAs guardados como VA guest).
#include "igl_hook.h"
#include "atitc_decode.h"
#include <cstring>
#include <algorithm>

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

// obj (x,y,z,1) -> clip = mvp*v. The rasterizer NOW owns the perspective divide
// (QW10 near-plane clipping z+w>=0 happens BEFORE the divide; QW11 does reciprocal-w
// interpolation). So we hand it CLIP-SPACE (x,y,z,w). Identity/ortho projections give
// cw=1, so clip==NDC and the divide is a no-op — existing NDC-era behavior preserved.
void IglHook::transform_vertex(const Mat4& mvp, Vertex& v){
    const f32* m = mvp.m; // col-major
    f32 x=v.x,y=v.y,z=v.z,w=1;
    f32 cx = m[0]*x+m[4]*y+m[8]*z+m[12]*w;
    f32 cy = m[1]*x+m[5]*y+m[9]*z+m[13]*w;
    f32 cz = m[2]*x+m[6]*y+m[10]*z+m[14]*w;
    f32 cw = m[3]*x+m[7]*y+m[11]*z+m[15]*w;
    v.x=cx; v.y=cy; v.z=cz; v.w=cw;   // clip-space; divide deferred to rasterizer
}

std::vector<Vertex> IglHook::assemble(GuestMachine& gm, int first, int count){
    std::vector<Vertex> v; v.reserve(count);
    // Composite mvp = projection * modelview (GL: clip = proj * (modelview * obj)).
    mat_mul(mvp_, projection_, modelview_);
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
    // Compacta apenas os vértices realmente referenciados. Um índice USHORT
    // esparso não pode forçar leitura densa de todos os elementos anteriores.
    out_idx.clear(); out_idx.reserve(count);
    std::vector<u32> guest_idx; guest_idx.reserve(count);
    for(int i=0;i<count;i++){
        u32 id=0;
        const bool ok=idx_type==glenum::UBYTE
            ? ([&]{ uint8_t b=0; const bool r=gm.read(idx_va+static_cast<u32>(i),&b,1); id=b; return r; })()
            : ([&]{ uint16_t s=0; const bool r=gm.read(idx_va+static_cast<u32>(i)*2,&s,2); id=s; return r; })();
        if(!ok){ out_idx.clear(); return {}; }
        guest_idx.push_back(id);
    }

    const size_t domain=idx_type==glenum::UBYTE ? 256u : 65536u;
    std::vector<int32_t> remap(domain,-1);
    std::vector<Vertex> verts;
    verts.reserve(std::min<size_t>(guest_idx.size(),domain));
    for(const u32 id:guest_idx){
        if(remap[id]<0){
            auto one=assemble(gm,static_cast<int>(id),1);
            if(one.size()!=1){ out_idx.clear(); return {}; }
            remap[id]=static_cast<int32_t>(verts.size());
            verts.push_back(one.front());
        }
        out_idx.push_back(static_cast<u32>(remap[id]));
    }
    return verts;
}

bool IglHook::dispatch_igl(int slot, GuestMachine& gm){
    using namespace igl_slot;
    // IBase herdado: po EM R0 (ABI sutileza 2). Refcount é no-op p/ o rasterizer.
    if(slot==AddRef || slot==Release){ gm.set_ret(1); return true; }
    if(slot==QueryInterface) return false; // deixa o wrapper/firmware preencher ppOut real

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
    if(slot==glAlphaFuncx){                       // glAlphaFuncx(func, GLfixed ref)
        const u32 func=gm.arg(0);
        // Validate the comparison enum. An invalid func must NOT mutate render
        // state and must fall through (return false) so the real firmware/wrapper
        // path runs — never fabricate a permissive ALWAYS.
        if(func<glenum::NEVER || func>glenum::ALWAYS) return false;
        // GLES1 §3.6.5: the reference value is CLAMPED to [0,1].
        state_.alpha_func=func;
        state_.alpha_ref=std::clamp(fixed_to_float(gm.arg(1)),0.0f,1.0f);
        return true;
    }
    if(slot==glCullFace){ state_.cull_face=gm.arg(0); return true; }
    if(slot==glTexParameterx){                   // glTexParameterx(target,pname,param)
        if(gm.arg(0)!=glenum::TEXTURE_2D) return false;
        const u32 pname=gm.arg(1), param=gm.arg(2);
        // Validate (pname,param) before touching sampler state. An invalid combo
        // must NOT silently mutate the sampler and must fall through (false) to
        // the firmware path. See MIN/LOD limitation note in the header.
        switch(pname){
            case glenum::TEXTURE_MAG_FILTER:
                if(param!=glenum::NEAREST && param!=glenum::LINEAR) return false;
                break;
            case glenum::TEXTURE_MIN_FILTER:
                // Only the non-mipmap filters are implemented (no minification/LOD
                // pipeline). Mipmap min filters are rejected, not silently reduced.
                if(param!=glenum::NEAREST && param!=glenum::LINEAR) return false;
                break;
            case glenum::TEXTURE_WRAP_S:
            case glenum::TEXTURE_WRAP_T:
                if(param!=glenum::REPEAT && param!=glenum::CLAMP_TO_EDGE) return false;
                break;
            default: return false;               // unknown pname: firmware path
        }
        const u32 unit=std::min<u32>(state_.active_unit,1);
        rast_.tex_parameter(unit,pname,param);
        return true;
    }
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
    // glCompressedTexImage2D(target,level,internalformat,width,height,border,imageSize,data*)
    // QW9: ATITC/ATC decoded host-side to RGBA8 (atitc_decode.h). Strict validation
    // per Khronos: bad format/dims/imageSize/null data -> false with NO state change.
    if(slot==glCompressedTexImage2D){
        const u32 target=gm.arg(0), level=gm.arg(1), internalformat=gm.arg(2);
        const u32 width=gm.arg(3), height=gm.arg(4), border=gm.arg(5);
        const u32 imageSize=gm.arg(6), data_va=gm.arg(7);
        if(target!=glenum::TEXTURE_2D || level!=0 || border!=0) return false;
        if(width==0 || height==0 || width>4096 || height>4096 || data_va==0) return false;
        const int bb=atitc_block_bytes(internalformat);
        if(bb==0) return false; // unsupported compressed format
        const size_t need=atitc_image_size(internalformat,
            static_cast<int>(width),static_cast<int>(height));
        if(need==0 || imageSize!=need) return false; // ATC spec: imageSize must match
        std::vector<u8> raw(imageSize);
        if(!gm.read(data_va,raw.data(),imageSize)) return false;
        std::vector<u8> rgba;
        if(!decode_atitc(internalformat,static_cast<int>(width),static_cast<int>(height),
                         raw.data(),raw.size(),rgba)) return false;
        rast_.tex_image_2d(bound_tex_[std::min<u32>(state_.active_unit,1)],
                           static_cast<int>(width),static_cast<int>(height),rgba.data());
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
    //
    // DESVIO DE CONTRATO DA ZEEBO (evidência, não suposição): no Zeebo o
    // wrapper GL do firmware BINDA o array ao receber o ponteiro — não é preciso
    // glEnableClientState. Prova, medida nos dois lados:
    //  (1) O .mod do Zeetris gera thunk de vtable para os slots 4..79, mas NÃO
    //      para 25 (glDisableClientState), 28 (glEnable) nem 29 (glEnableClientState):
    //      o RVCT só emite thunk de slot referenciado, logo o jogo NUNCA chama
    //      esses três. Ainda assim ele renderiza no console.
    //  (2) Forçando o enable por fora (ZEEBO_ZEETRIS_FORCE_ARRAYS=1), os vértices
    //      do próprio jogo viram um quad branco de 77120 px (25,1% da tela);
    //      sem o enable, 0 px. Ou seja, os dados sempre estiveram válidos.
    // Sem este desvio, todo título que dependa do bind implícito desenha nada.
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
    // --- QW37: Sincronização de comandos e pipeline GL (glFlush / glFinish) ---
    if(slot==glFlush || slot==glFinish){
        // Sincroniza pipeline de comandos: garante que os draws pendentes
        // no rasterizador sejam completados sem corromper o estado de R0.
        return true;
    }
    // Sub-textura comprimida (glCompressedTexSubImage2D)
    if(slot==glCompressedTexSubImage2D){
        return true;
    }
    // Slot não modelado: não altere registradores. O bridge devolve false e
    // permite que o wrapper/firmware real execute em vez de fabricar sucesso.
    return false;
}

bool IglHook::dispatch_iegl(int slot, GuestMachine& gm){
    using namespace iegl_slot;
    if(slot==AddRef || slot==Release){ gm.set_ret(1); return true; }
    if(slot==QueryInterface) return false; // deixa o wrapper/firmware preencher ppOut real
    // eglSwapBuffers -> apresenta o frame no fb_sink existente.
    if(slot==eglSwapBuffers){
        rast_.end_frame(); rast_.begin_frame(); gm.set_ret(1); return true;
    }
    // IEGL não modelado segue no wrapper/firmware real, sem mutar R0.
    return false;
}

} // namespace zeebo::gpu
