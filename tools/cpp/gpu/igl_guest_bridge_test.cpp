// igl_guest_bridge_test.cpp — valida a resolução DETERMINÍSTICA de gpIGL/gpIEGL.
//
// Por que este teste existe: o 1.1.2_APPS.bin é um ELF STRIPPED (e_shnum=0).
// gpIGL/gpIEGL NÃO são exports estáticos — são globais que a init EGL/GL do guest
// preenche em runtime. Logo NÃO há VA honesto para hardcodar. A abordagem correta
// (implementada em IglGuestBridge::resolve_from_object) é ler a vtable a partir do
// OBJETO de interface vivo (obj[0]=&vtable) e VALIDAR estruturalmente que cada slot
// aponta para código executável do firmware antes de ligar ao IglHook.
//
// Este teste monta uma memória Unicorn com os MESMOS intervalos executáveis reais
// do 1.1.2_APPS.bin (extraídos dos program headers), constrói uma vtable legítima
// e uma falsa, e prova que:
//   (1) a vtable legítima é ACEITA e ligada (fn_map povoado);
//   (2) uma vtable com ponteiro fora de código é REJEITADA (honestidade);
//   (3) obj[0]==0 (interface ainda não inicializada pelo guest) é REJEITADA.
#include "igl_guest_bridge.h"
#include "igpu_rasterizer.h"
#include <cstdio>
#include <cstring>
#include <vector>
using namespace zeebo::gpu;

static int g_fail = 0;
#define CHECK(cond, msg) do{ if(!(cond)){ printf("  [FAIL] %s\n", msg); ++g_fail; } \
                             else printf("  [ok]   %s\n", msg); }while(0)

int main() {
    // Intervalos executáveis REAIS do 1.1.2_APPS.bin (program headers com flag X).
    // Só precisamos de UM segmento contíguo para a prova; usamos o do wrapper GLES
    // (blob identity-mapped 0x1013a000..0x1140c000, onde vive QGLToolsAPI/QEGLTools).
    const u32 CODE_LO = 0x1013a000, CODE_HI = 0x1140c000;
    std::vector<std::pair<u32,u32>> code_ranges = {
        {0xf0000000, 0xf001b4f0}, {0xb0000000, 0xb000f207},
        {CODE_LO, CODE_HI},
    };

    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        printf("[FATAL] uc_open falhou\n"); return 2;
    }
    // Mapeia uma página de DADOS para as vtables (fora dos ranges de código).
    const u32 DATA = 0x30000000;
    uc_mem_map(uc, DATA, 0x1000, UC_PROT_ALL);

    // (A) vtable IGL LEGÍTIMA: 80 ponteiros dentro de [CODE_LO,CODE_HI), com bit
    // Thumb setado (como o firmware real, código Thumb). Dois slots nulos (funções
    // não implementadas no wrapper) — devem ser tolerados.
    const u32 IGL_VTBL = DATA + 0x100;
    for (int s = 0; s < 80; ++s) {
        u32 fn = (s == 5 || s == 40) ? 0u : (CODE_LO + (u32)s * 0x40 + 1);
        uc_mem_write(uc, IGL_VTBL + (u32)s * 4, &fn, 4);
    }
    // (B) vtable IEGL LEGÍTIMA: 28 ponteiros.
    const u32 IEGL_VTBL = DATA + 0x400;
    for (int s = 0; s < 28; ++s) {
        u32 fn = CODE_LO + 0x2000 + (u32)s * 0x30 + 1;
        uc_mem_write(uc, IEGL_VTBL + (u32)s * 4, &fn, 4);
    }
    // (C) vtable FALSA: ponteiros para longe do código (região de dados/heap).
    const u32 FAKE_VTBL = DATA + 0x700;
    for (int s = 0; s < 80; ++s) { u32 fn = 0x50000000 + (u32)s * 4; uc_mem_write(uc, FAKE_VTBL + (u32)s*4, &fn, 4); }

    // Objetos de interface vivos: obj[0] = &vtable (padrão IBase da BREW).
    const u32 IGL_OBJ = DATA + 0x000; uc_mem_write(uc, IGL_OBJ,  &IGL_VTBL,  4);
    const u32 IEGL_OBJ= DATA + 0x004; uc_mem_write(uc, IEGL_OBJ, &IEGL_VTBL, 4);
    const u32 FAKE_OBJ= DATA + 0x008; uc_mem_write(uc, FAKE_OBJ, &FAKE_VTBL, 4);
    const u32 NULL_OBJ= DATA + 0x00c; u32 z = 0; uc_mem_write(uc, NULL_OBJ, &z, 4);

    auto rast = create_rasterizer(Backend::SoftwareRef);
    IglHook hook(*rast);
    IglGuestBridge bridge(hook);
    bridge.set_code_ranges(code_ranges);

    printf("== Teste 1: resolução determinística da vtable IGL a partir do objeto vivo ==\n");
    bool ok_igl = bridge.resolve_from_object(uc, IGL_OBJ, /*is_igl=*/true);
    CHECK(ok_igl, "vtable IGL legítima ACEITA");
    CHECK(bridge.igl_vtable_va() == IGL_VTBL, "igl_vtable_va() == VA lido de obj[0]");
    CHECK(bridge.bound(), "fn_map povoado (funções ligadas ao dispatch)");

    printf("== Teste 2: resolução determinística da vtable IEGL ==\n");
    bool ok_iegl = bridge.resolve_from_object(uc, IEGL_OBJ, /*is_igl=*/false);
    CHECK(ok_iegl, "vtable IEGL legítima ACEITA");
    CHECK(bridge.iegl_vtable_va() == IEGL_VTBL, "iegl_vtable_va() == VA lido de obj[0]");

    printf("== Teste 3: rejeição honesta de vtable FALSA (ponteiros fora de código) ==\n");
    IglGuestBridge b2(hook); b2.set_code_ranges(code_ranges);
    CHECK(!b2.resolve_from_object(uc, FAKE_OBJ, true), "vtable falsa REJEITADA");
    CHECK(!b2.bound(), "nada ligado após rejeição");

    printf("== Teste 4: rejeição de objeto não-inicializado (obj[0]==0) ==\n");
    CHECK(!b2.resolve_from_object(uc, NULL_OBJ, true), "obj[0]==0 REJEITADO (guest ainda não fez init)");

    printf("== Teste 5: validate_vtable como predicado puro ==\n");
    CHECK(bridge.validate_vtable(uc, IGL_VTBL, 80) == 78, "IGL: 78/80 slots-código (2 nulos tolerados)");
    CHECK(bridge.validate_vtable(uc, IEGL_VTBL, 28) == 28, "IEGL: 28/28 slots-código");
    CHECK(bridge.validate_vtable(uc, FAKE_VTBL, 80) == 0,  "FAKE: reprovada (0)");

    printf("== Teste 6: dispatch usa PC alinhado do Unicorn e não engole slots reais ==\n");
    bridge.set_guest_running(true);
    const u32 IEGL_ADDREF_FN = (CODE_LO + 0x2000 + 0 * 0x30) & ~1u;
    const u32 IEGL_QI_FN = (CODE_LO + 0x2000 + 2 * 0x30) & ~1u;
    const u32 IEGL_QUERYSTRING_FN = (CODE_LO + 0x2000 + 7 * 0x30) & ~1u;
    const u32 IEGL_SWAP_FN = (CODE_LO + 0x2000 + 26 * 0x30) & ~1u;
    CHECK(bridge.on_code(uc, IEGL_ADDREF_FN), "PC Thumb alinhado encontra slot IEGL tratado");
    u32 sentinel = 0xdeadbeef; uc_reg_write(uc, UC_ARM_REG_R0, &sentinel);
    CHECK(!bridge.on_code(uc, IEGL_QI_FN), "QueryInterface não modelado continua no firmware real");
    CHECK(!bridge.on_code(uc, IEGL_QUERYSTRING_FN), "slot IEGL não modelado continua no firmware real");
    u32 after = 0; uc_reg_read(uc, UC_ARM_REG_R0, &after);
    CHECK(after == sentinel, "slot não modelado não corrompe argumentos guest");
    CHECK(bridge.on_code(uc, IEGL_SWAP_FN), "eglSwapBuffers legado slot 26 é interceptado");

    uc_close(uc);
    printf("\n%s (%d falhas)\n", g_fail ? "FALHOU" : "TODOS OS TESTES PASSARAM", g_fail);
    return g_fail ? 1 : 0;
}
