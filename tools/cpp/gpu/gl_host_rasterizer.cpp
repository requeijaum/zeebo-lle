// gl_host_rasterizer.cpp — Backend 2: OpenGL host renderer (Citra "HW renderer"
// analog). ESBOÇO (FASE 2 do GPU_TODO.md §6). Traduz a IGpuRasterizer para GL
// desktop, renderiza num FBO 640x480, lê de volta RGB565 e entrega ao MESMO
// fb_sink (não abre janela própria).
//
// GUARD DE COMPILAÇÃO: só entra no build quando -DZEEBO_GL_HOST e -lGL/GLEW estão
// presentes. Sem isso, este arquivo compila para NADA (o create_rasterizer em
// soft_rasterizer.cpp continua sendo o único símbolo). Assim o gpu_smoke headless
// não precisa de contexto GL, e o outro agente não herda dependência de GL.
//
// DECISÃO PENDENTE (GPU_TODO §8): fixed-function foi removido do GL core moderno.
// Três opções p/ GLES1.1 fixed-function no host:
//   (i)  contexto GL compat 1.x  — mais simples, deprecated;
//   (ii) ANGLE / GL ES no host   — mantém semântica GLES1 nativa;
//   (iii) ubershader que emula matriz+combine+dot3+luz — portável (recomendado).
// Este esboço assume (iii): um pipeline programável mínimo com um shader que
// reproduz o fixed-function GLES1.1 usado pelo Zeebo (multitexture+combine+dot3).
#ifdef ZEEBO_GL_HOST

#include "igpu_rasterizer.h"
#include <cstdio>
#include <unordered_map>
#include <vector>

// O include real de GL fica a cargo do build (GLEW/glad + contexto headless via
// EGL/OSMesa). Mantido abstrato no esboço para não fixar a escolha agora.
// #include <GL/glew.h>

namespace zeebo::gpu {

class GlHostRasterizer final : public IGpuRasterizer {
public:
    bool init() override {
        // TODO(FASE2): criar contexto GL headless (EGL surfaceless ou OSMesa),
        //   compilar o ubershader (ver §8/iii), criar FBO 640x480 RGB (color)
        //   + depth, VAO/VBO dinâmico p/ os vertex arrays convertidos.
        fb_.assign(kFbWidth*kFbHeight, 0);
        printf("[GlHost] STUB init — contexto GL/FBO ainda não criado (FASE 2)\n");
        return true; // stub: reporta ok mas NÃO é funcional (marcado no log)
    }
    void begin_frame() override { /* glBindFramebuffer(fbo_); */ }

    void set_viewport(int x,int y,int w,int h) override {
        vx_=x; vy_=y; vw_=w; vh_=h; /* glViewport(x,y,w,h) */
    }
    void clear_color(f32 r,f32 g,f32 b,f32 a) override {
        cr_=r; cg_=g; cb_=b; ca_=a; /* glClearColor */
    }
    void clear(u32 /*mask*/) override {
        // TODO: glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)
    }
    void set_state(const RenderState& s) override {
        st_=s;
        // TODO: mapear blend/depth/alpha/cull/shade -> glEnable/glBlendFunc/...
        //   alpha_test em GLES1 não existe em GL core -> emular no ubershader
        //   (discard por alpha_ref/func). combine/dot3 -> uniforms do ubershader.
    }
    void set_mvp(const Mat4& m) override { mvp_=m; /* uniform mat4 u_mvp */ }

    void tex_image_2d(u32 id,int w,int h,const void* rgba8) override {
        // data já é RGBA8 host (ATITC decodificado upstream, GPU_TODO §5).
        // TODO: glGenTextures/glBindTexture/glTexImage2D(GL_RGBA,GL_UNSIGNED_BYTE)
        (void)w;(void)h;(void)rgba8; textures_[id]=id;
    }
    void bind_texture(u32 unit,u32 id) override {
        // TODO: glActiveTexture(GL_TEXTURE0+unit); glBindTexture(2D, gltex)
        (void)unit;(void)id;
    }
    void delete_texture(u32 id) override { textures_.erase(id); /* glDeleteTextures */ }

    void draw(Prim p, const std::vector<Vertex>& v) override {
        // TODO: upload v -> VBO; glDrawArrays(prim_to_gl(p), 0, v.size())
        (void)p; last_draw_verts_=v.size();
    }
    void draw_indexed(Prim p, const std::vector<Vertex>& v,
                      const std::vector<u32>& idx) override {
        // Zeebulator expande índices; aqui podemos usar glDrawElements REAL (host
        // suporta), evitando o trade-off de expansão do caminho HLE.
        (void)p;(void)v; last_draw_verts_=idx.size();
    }

    void end_frame() override {
        // TODO: glReadPixels(GL_RGB, GL_UNSIGNED_SHORT_5_6_5) do FBO -> fb_.
        // Enquanto stub, mantém fb_ zerado (present neutro).
    }
    const u16* framebuffer_rgb565() const override { return fb_.data(); }

private:
    std::vector<u16> fb_;
    RenderState st_{}; Mat4 mvp_{};
    std::unordered_map<u32,u32> textures_;
    int vx_=0,vy_=0,vw_=kFbWidth,vh_=kFbHeight;
    f32 cr_=0,cg_=0,cb_=0,ca_=1;
    size_t last_draw_verts_=0;
};

std::unique_ptr<IGpuRasterizer> make_gl_host_rasterizer(){
    return std::make_unique<GlHostRasterizer>();
}

} // namespace zeebo::gpu

#endif // ZEEBO_GL_HOST
