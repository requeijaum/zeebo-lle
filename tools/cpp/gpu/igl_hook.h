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

namespace zeebo::gpu {

// --- Khronos GL enums necessários (valores públicos, iguais ao gl_types.h) ------
namespace glenum {
constexpr u32 BYTE=0x1400, UBYTE=0x1401, SHORT=0x1402, USHORT=0x1403,
              FLOAT=0x1406, FIXED=0x140C;
constexpr u32 VERTEX_ARRAY=0x8074, NORMAL_ARRAY=0x8075, COLOR_ARRAY=0x8076,
              TEXCOORD_ARRAY=0x8078;
constexpr u32 POINTS=0x0000, LINES=0x0001, LINE_STRIP=0x0003,
              TRIANGLES=0x0004, TRI_STRIP=0x0005, TRI_FAN=0x0006;
constexpr u32 RGB=0x1907, RGBA=0x1908;
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
    u32 bound_tex_[2]{0,0};

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
              glClear=7, glClearColorx=8, glClearDepthx=9, glColor4x=12,
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
// IEGL: 28 slots. Índices exatos dos egl* ainda a extrair do array IEGL do
// gl_hle.cpp (não lido nesta rodada); marcados -1 até lá. eglSwapBuffers tratado
// por nome quando o índice for fixado.
constexpr int eglSwapBuffers=-1, eglMakeCurrent=-1, eglQueryString=-1;
}

} // namespace zeebo::gpu
