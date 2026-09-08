// igl_hook.h — THE CORRECT PRODUCER (FASE 1 real). Substitui pm4_adreno.h como
// alvo: a evidência do firmware (GPU_TODO §13) provou que o 3D é offload MPU->QDSP5
// atrás de uma fachada OpenGL ES 1.1 ATI-Imageon — NÃO há ring buffer PM4 do lado
// ARM11. A fronteira observável/hookável é a vtable IGL/IEGL que o `.mod` do jogo
// despacha via os ponteiros globais gpIGL/gpIEGL (wrapper GLES_1x.c/EGL_1x.c da
// Qualcomm linkado estaticamente no módulo). Hookar aqui = Citra/Dolphin "HW mode".
//
// ABI REAL (confirmada no corpus: brew-abi.md + zeebulator gl_hle.h/.cpp vs AEEGL.h):
//  (1) IGL = 80 slots: AddRef=0, Release=1, QueryInterface=2, depois 77 gl* (3..79)
//      na ordem declarada em AEEGL.h. IEGL = 28 slots: AR/Rel/QI + 25 egl*.
//  (2) ARMADILHA CENTRAL: os slots gl*/egl* NÃO recebem o ponteiro da interface (po)
//      em R0. O macro real `IGL_glClear(p,a)` expande p/ `vtbl->glClear(a)` — passa
//      só `a`. Logo R0 = PRIMEIRO ARGUMENTO REAL, não `po`. Só AddRef/Release/
//      QueryInterface (slots herdados de IBase) seguem po-em-R0.
//  (3) Ponteiros de array (glVertexPointer etc.) são VAs ARM do guest — guardados
//      como (ptr/type/size/stride) e lidos da memória emulada SÓ no draw, via
//      ReadGlComponent (semântica por-tipo GLES1.x). ATITC decodificado host-side.
#pragma once
#include "igpu_rasterizer.h"
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <cmath>

namespace zeebo::gpu {

// --- Khronos GL enums necessários (valores públicos, iguais ao gl_types.h) ------
namespace glenum {
constexpr u32 BYTE=0x1400, UBYTE=0x1401, SHORT=0x1402, USHORT=0x1403,
              FLOAT=0x1406, FIXED=0x140C;
constexpr u32 VERTEX_ARRAY=0x8074, NORMAL_ARRAY=0x8075, COLOR_ARRAY=0x8076,
              TEXCOORD_ARRAY=0x8078;
constexpr u32 POINTS=0x0000, LINES=0x0001, LINE_STRIP=0x0003,
              TRIANGLES=0x0004, TRI_STRIP=0x0005, TRI_FAN=0x0006;
constexpr u32 RGB=0x1907, RGBA=0x1908, USHORT_565=0x8363;
constexpr u32 TEXTURE_2D=0x0de1, DEPTH_TEST=0x0b71, BLEND=0x0be2,
              ALPHA_TEST=0x0bc0, CULL_FACE=0x0b44, TEXTURE0=0x84c0;
constexpr u32 ATITC_RGB=0x8C92, ATITC_RGBA=0x8C93;
inline int type_size(u32 t){ switch(t){case BYTE:case UBYTE:return 1;
    case SHORT:case USHORT:return 2; case FLOAT:case FIXED:return 4; default:return 4;} }
inline Prim to_prim(u32 mode){ switch(mode){case POINTS:return Prim::Points;
    case LINES:return Prim::Lines; case LINE_STRIP:return Prim::LineStrip;
    case TRI_STRIP:return Prim::TriStrip; case TRI_FAN:return Prim::TriFan;
    default:return Prim::Triangles;} }
}

// --- Abstração da máquina guest (implementada por Unicorn no LLE real) -----------
// Mantida abstrata de propósito: torna o hook testável SEM Unicorn (smoke test),
// e não acopla o produtor gráfico ao core ARM (regra do §0 / revisão concorrente).
struct GuestMachine {
    // Registradores de argumento no momento da chamada. ATENÇÃO (ABI sutileza 2):
    // para slots gl*/egl*, arg(0)=R0 é o PRIMEIRO ARGUMENTO REAL, não `po`.
    // AAPCS: args 0..3 em R0..R3, args >=4 na pilha (SP+((n-4)*4)).
    std::function<u32(int n)> arg;
    // Lê `size` bytes da memória emulada (VA guest) para `dst`. No LLE = uc_mem_read.
    std::function<bool(u32 va, void* dst, u32 size)> read;
    // Escreve o valor de retorno (R0). void => não chamar.
    std::function<void(u32 r0)> set_ret;
    // Lê um float GLES1.x de um componente em memória guest, por tipo (ReadGlComponent).
    float read_component(u32 va, u32 type) const;
};

// Estado de um array de vértices (guardado como VA guest — lido só no draw).
struct GuestArray {
    bool enabled=false;
    u32  va=0;          // ponteiro guest (NÃO host)
    int  size=0;        // componentes por elemento (2,3,4)
    u32  type=glenum::FLOAT;
    int  stride=0;      // 0 = tightly packed
};

namespace glenum2 { // GL fixed-function matrix enums (públicos, AEEGL.h)
constexpr u32 MODELVIEW=0x1700, PROJECTION=0x1701;
}

// Minimal column-major (convenção GL) 4x4 math — necesario p/ o transform
// fixed-function (GPU_TODO §15 ERRO-3). Identidade = {1,0,0,0,...} como m[16].
inline void mat_identity(Mat4& o){ float z[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    for(int i=0;i<16;i++) o.m[i]=z[i]; }
inline void mat_mul(Mat4& o, const Mat4& a, const Mat4& b){
    // o = a*b (col-major): o[i*4+j] = sum_k a[k*4+j] * b[i*4+k]
    float t[16];
    for(int c=0;c<4;c++) for(int r=0;r<4;r++){
        float s=0; for(int k=0;k<4;k++) s += a.m[k*4+r]*b.m[c*4+k];
        t[c*4+r]=s; }
    for(int i=0;i<16;i++) o.m[i]=t[i];
}
inline void mat_frustum(Mat4& o, float l,float r,float b,float t,float n,float f){
    // GL column-major perspective + ortho (clip space). Zeno no [-n,-f]->NDC[-1,1].
    o.m[0]=2*n/(r-l); o.m[1]=0;        o.m[2]=0;                       o.m[3]=0;
    o.m[4]=0;         o.m[5]=2*n/(t-b);o.m[6]=0;                       o.m[7]=0;
    o.m[8]=(r+l)/(r-l); o.m[9]=(t+b)/(t-b); o.m[10]=-(f+n)/(f-n);      o.m[11]=-1;
    o.m[12]=0; o.m[13]=0; o.m[14]=-(2*f*n)/(f-n);                      o.m[15]=0;
}
inline void mat_ortho(Mat4& o, float l,float r,float b,float t,float n,float f){
    o.m[0]=2/(r-l); o.m[1]=0; o.m[2]=0; o.m[3]=0;
    o.m[4]=0; o.m[5]=2/(t-b); o.m[6]=0; o.m[7]=0;
    o.m[8]=0; o.m[9]=0; o.m[10]=-2/(f-n); o.m[11]=0;
    o.m[12]=-(r+l)/(r-l); o.m[13]=-(t+b)/(t-b); o.m[14]=-(f+n)/(f-n); o.m[15]=1;
}
inline void mat_rotate(Mat4& o, float deg, float ax,float ay,float az){
    const float PI=3.14159265358979f, rad=deg*PI/180.f, s=sinf(rad), c=cosf(rad);
    float x=ax, y=ay, z=az, len=sqrtf(x*x+y*y+z*z);
    if(len>1e-6f){ x/=len; y/=len; z/=len; }
    float xx=x*x, yy=y*y, zz=z*z, xy=x*y, xz=x*z, yz=y*z, xs=x*s, ys=y*s, zs=z*s;
    o.m[0]=xx*(1-c)+c;      o.m[1]=xy*(1-c)+zs; o.m[2]=xz*(1-c)-ys; o.m[3]=0;
    o.m[4]=xy*(1-c)-zs;      o.m[5]=yy*(1-c)+c; o.m[6]=yz*(1-c)+xs; o.m[7]=0;
    o.m[8]=xz*(1-c)+ys;      o.m[9]=yz*(1-c)-xs;o.m[10]=zz*(1-c)+c; o.m[11]=0;
    o.m[12]=0; o.m[13]=0; o.m[14]=0; o.m[15]=1;
}
inline void mat_scale(Mat4& o, float x,float y,float z){
    mat_identity(o); o.m[0]=x; o.m[5]=y; o.m[10]=z; }
inline void mat_translate(Mat4& o, float x,float y,float z){
    mat_identity(o); o.m[12]=x; o.m[13]=y; o.m[14]=z; }

// O produtor. Recebe (slot, GuestMachine) a cada chamada interceptada na vtable
// IGL/IEGL e traduz para IGpuRasterizer. Espelha zeebulator::GlHle mas escreve no
// NOSSO IGpuRasterizer (não num GlBackend próprio).
class IglHook {
public:
    explicit IglHook(IGpuRasterizer& rast) : rast_(rast) {}

    // Ponto de entrada: chamado pelo dispatcher da vtable IGL. Retorna true se o
    // slot foi tratado (mesmo que como stub honesto). slot é o índice na vtable.
    bool dispatch_igl(int slot, GuestMachine& gm);
    bool dispatch_iegl(int slot, GuestMachine& gm);

private:
    IGpuRasterizer& rast_;
    // Estado fixed-function acumulado entre chamadas (transform/state/arrays).
    GuestArray vtx_, col_, tex_, nrm_;
    RenderState state_{};
    Mat4 mvp_{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    // Stacks fixed-function (GPU_TODO §15): modelview && projection, com
    // matriz corrente por glMatrixMode. Composite mvp = projection * modelview,
    // aplicado por vértice em assemble() (obj -> clip -> NDC via w-divide).
    Mat4 modelview_{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    Mat4 projection_{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
    Mat4 modelview_stack_[4];
    Mat4 projection_stack_[4];
    int  modelview_depth_=0;
    int  projection_depth_=0;
    u32  matrix_mode_=glenum2::MODELVIEW;
    u32  bound_tex_[2]{0,0};
    Vertex current_color_{};

    Mat4& cur_matrix(){ return matrix_mode_==glenum2::PROJECTION ? projection_ : modelview_; }
    Mat4* cur_stack(){ return matrix_mode_==glenum2::PROJECTION ? projection_stack_ : modelview_stack_; }
    int& cur_stack_depth(){ return matrix_mode_==glenum2::PROJECTION ? projection_depth_ : modelview_depth_; }

    // Aplica mvp=projection*modelview a um vértice obj -> NDC (clip/w).
    static void transform_vertex(const Mat4& mvp, Vertex& v);

    // Monta os vértices host lendo os arrays guest no range [first, first+count).
    std::vector<Vertex> assemble(GuestMachine& gm, int first, int count);
    // Idem, mas indexado (glDrawElements): lê índices da memória guest por `type`.
    std::vector<Vertex> assemble_indexed(GuestMachine& gm, u32 idx_va, int count,
                                          u32 idx_type, std::vector<u32>& out_idx);
    void read_matrix(GuestMachine& gm, u32 va, Mat4& out);
};

// Nomes de slot — TODOS confirmados contra a tabela real de 80 slots em
// zeebulator gl_hle.cpp (linhas 597-680), que por sua vez foi verificada contra o
// AEEGL.h genuíno da Qualcomm (OpenGL ES Extension for BREW SDK 4.x). Ordem exata.
namespace igl_slot {
constexpr int AddRef=0, Release=1, QueryInterface=2;
constexpr int glActiveTexture=3, glAlphaFuncx=4, glBindTexture=5, glBlendFunc=6,
              glClear=7, glClearColorx=8, glClearDepthx=9, glClientActiveTexture=11,
              glColor4x=12,
              glColorPointer=14, glCompressedTexImage2D=15, glCullFace=19,
              glDeleteTextures=20, glDepthFunc=21, glDepthMask=22, glDisable=24,
              glDisableClientState=25, glDrawArrays=26, glDrawElements=27,
              glEnable=28, glEnableClientState=29, glFrustumx=35, glGenTextures=36,
              glLoadIdentity=46, glLoadMatrixx=47, glMatrixMode=51, glMultMatrixx=52,
              glNormalPointer=55, glOrthox=56, glPopMatrix=60, glPushMatrix=61,
              glRotatex=63, glScalex=65, glTexCoordPointer=71, glTexEnvx=72,
              glTexImage2D=74, glTexParameterx=75, glTranslatex=77,
              glVertexPointer=78, glViewport=79;
}
namespace iegl_slot {
constexpr int AddRef=0, Release=1, QueryInterface=2;
// IEGL legado (28 slots). Ordem usada pelo Zeebx e compatível com a tabela
// pública EGL 1.0; o bridge ainda exige uma vtable viva validada no firmware.
constexpr int eglGetError=3, eglGetDisplay=4, eglInitialize=5, eglTerminate=6,
              eglQueryString=7, eglGetProcAddress=8, eglGetConfigs=9,
              eglChooseConfig=10, eglGetConfigAttrib=11,
              eglCreateWindowSurface=12, eglCreatePixmapSurface=13,
              eglCreatePbufferSurface=14, eglDestroySurface=15,
              eglQuerySurface=16, eglCreateContext=17, eglDestroyContext=18,
              eglMakeCurrent=19, eglGetCurrentContext=20,
              eglGetCurrentSurface=21, eglGetCurrentDisplay=22,
              eglQueryContext=23, eglWaitGL=24, eglWaitNative=25,
              eglSwapBuffers=26, eglCopyBuffers=27;
}

} // namespace zeebo::gpu
