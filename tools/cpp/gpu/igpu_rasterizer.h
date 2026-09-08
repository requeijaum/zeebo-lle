// igpu_rasterizer.h — GPU abstraction for zeebo-lle (Citra-style RasterizerInterface)
// Skeleton (FASE 1 do GPU_TODO.md). One interface, two backends, two producers.
// NADA aqui desenha "de verdade" ainda — stubs com efeito verificável (clear->fb).
#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <array>

namespace zeebo::gpu {

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using f32 = float;

// Zeebo native display: VGA 640x480 4:3 ONLY, RGB565 (docs/hardware-map.md).
constexpr int kFbWidth  = 640;
constexpr int kFbHeight = 480;

enum class Prim : u32 { Points, Lines, LineStrip, Triangles, TriStrip, TriFan };

// Host-native vertex after guest arrays are read+converted (see gl_hle semantics).
struct Vertex {
    f32 x=0,y=0,z=0,w=1;      // clip/obj coords (fixed-function transformed by matrix)
    f32 r=1,g=1,b=1,a=1;
    f32 u=0,v=0;               // texunit 0
    f32 u1=0,v1=0;             // texunit 1 (multitexture)
};

struct RenderState {
    bool blend=false;   u32 blend_src=0, blend_dst=0;
    bool depth_test=false; u32 depth_func=0; bool depth_write=true;
    bool alpha_test=false; u32 alpha_func=0; f32 alpha_ref=0;
    bool cull=false;    u32 cull_face=0x0405; // GLES1 default GL_CULL_FACE_MODE = GL_BACK
    u32  shade_model=0; // flat/smooth
    std::array<u32,2> tex_enabled{0,0};
    u32  active_unit=0;
};

// GL ES 1.1 fixed-function matrix stacks live in the frontend; the rasterizer
// receives already-composed 4x4 (modelview*projection) per draw when needed.
struct Mat4 { f32 m[16]; };

// The single abstraction. GlHostRasterizer and SoftRasterizer implement it.
// Fed by IglHook (FASE 2) now, by Pm4Decoder (FASE 4) later — same interface.
class IGpuRasterizer {
public:
    virtual ~IGpuRasterizer() = default;

    virtual bool init() = 0;
    virtual void begin_frame() = 0;

    virtual void set_viewport(int x,int y,int w,int h) = 0;
    virtual void clear_color(f32 r,f32 g,f32 b,f32 a) = 0;
    virtual void clear(u32 mask) = 0;              // GL_COLOR_BUFFER_BIT etc.
    virtual void set_state(const RenderState& s) = 0;
    virtual void set_mvp(const Mat4& mvp) = 0;

    // Texture: id is the guest GL name; data already decoded to RGBA8 host-side
    // (ATITC etc. handled upstream — docs/file-formats.md).
    virtual void tex_image_2d(u32 id,int w,int h,const void* rgba8) = 0;
    virtual void tex_parameter(u32 unit,u32 pname,u32 param) = 0;
    virtual void bind_texture(u32 unit,u32 id) = 0;
    virtual void delete_texture(u32 id) = 0;

    virtual void draw(Prim p, const std::vector<Vertex>& verts) = 0;
    virtual void draw_indexed(Prim p, const std::vector<Vertex>& verts,
                              const std::vector<u32>& indices) = 0;

    // Presents into a 640x480 RGB565 buffer for the EXISTING fb_sink pipeline.
    virtual void end_frame() = 0;
    virtual const u16* framebuffer_rgb565() const = 0;  // kFbWidth*kFbHeight
};

enum class Backend { SoftwareRef, GlHost };

// Factory (rasterizer_factory.cpp). Each backend exposes its own maker below;
// the factory picks one, guarded by ZEEBO_GL_HOST so headless builds need no GL.
std::unique_ptr<IGpuRasterizer> create_rasterizer(Backend b);
std::unique_ptr<IGpuRasterizer> make_soft_rasterizer();
#ifdef ZEEBO_GL_HOST
std::unique_ptr<IGpuRasterizer> make_gl_host_rasterizer();
#endif

} // namespace zeebo::gpu
