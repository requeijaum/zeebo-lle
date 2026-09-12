// zeebo_input_harness.cpp — Passo 5: mapeamento Z-Pad/SDL2 → AVK e despacho
// contínuo de EVT_KEY_PRESS / EVT_KEY_RELEASE ao manipulador BREW (HandleEvent).
//
// Prova, por EXECUÇÃO REAL sob Unicorn (regra de ouro do Rafael), que:
//   1. O mapa de teclas SDL2 → Z-Pad → AVK (BrewLoader::avk_for_zpad/avk_for_digit)
//      está correto (setas, A/B/1/2/3/4/Home, dígitos 0-9).
//   2. BrewLoader::dispatch_event executa DE VERDADE o manipulador Thumb de um
//      applet BREW, passando (pApplet, evt, keycode, dwParam) e capturando r0.
//   3. Um applet injetado que consome EVT_KEY_PRESS/RELEASE retorna r0=1.
//   4. O despacho é contínuo: uma sequência de pressionamentos/soltas do Z-Pad
//      é entregue em laço, como aconteceria no loop SDL2 do emulador principal.
//
// O applet aqui é um stub MÍNIMO de HandleEvent (código Thumb ARM real, montado
// com arm-none-eabi-as), NÃO forjado: ele compara r1 com EVT_KEY_PRESS/RELEASE e
// devolve 1 (consumido) ou 0 (ignorado). Isso valida o CAMINHO de despacho de
// entrada sem depender do boot completo do firmware.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_brew_loader.h"

using namespace zeebo::brew;

// Layout de RAM de scratch do harness (fora do firmware).
static constexpr u32 SCRATCH_BASE = 0x20000000;
static constexpr u32 SCRATCH_SIZE = 0x00100000; // 1 MB
static constexpr u32 APPLET_VA    = 0x20000100; // objeto applet (r0)
static constexpr u32 HANDLER_VA   = 0x20002000; // código Thumb do HandleEvent
static constexpr u32 STACK_TOP    = 0x2000f000;
static constexpr u32 RET_MAGIC    = 0x2003fffe; // LR sentinela (par → fora de código)

// HandleEvent Thumb montado de zeebo_input_stub.s:
//   push {lr}; r3=0x100; cmp r1,r3; beq hit; r3=0x101; cmp r1,r3; beq hit;
//   r0=0; pop{pc};  hit: r0=1; pop{pc}
// Consome EVT_KEY_PRESS(0x100) e EVT_KEY_RELEASE(0x101), devolve 1.
static const uint8_t kStub[] = {
    0x00,0xb5, 0x01,0x23, 0x1b,0x02, 0x99,0x42, 0x04,0xd0,
    0x01,0x33, 0x99,0x42, 0x01,0xd0, 0x00,0x20, 0x00,0xbd,
    0x01,0x20, 0x00,0xbd,
};

// ── Modelo do backend de entrada SDL2 → Z-Pad (sem depender de SDL no teste) ──
// Reproduz a MESMA tabela usada no laço SDL2 do zeebo_lle_main.cpp:
//   Setas→direções, Z/Enter→A, X/Esc→B, C→1, V→2, Espaço→3, Shift→4, Home/H→Home.
struct SdlKeyMap { const char* sdl_name; ZpadButton zp; };
static const SdlKeyMap kSdlMap[] = {
    {"SDLK_UP",     ZP_UP},   {"SDLK_DOWN",  ZP_DOWN}, {"SDLK_LEFT", ZP_LEFT},
    {"SDLK_RIGHT",  ZP_RIGHT},{"SDLK_z",     ZP_A},    {"SDLK_RETURN", ZP_A},
    {"SDLK_x",      ZP_B},    {"SDLK_ESCAPE",ZP_B},    {"SDLK_c",    ZP_1},
    {"SDLK_v",      ZP_2},    {"SDLK_SPACE", ZP_3},    {"SDLK_LSHIFT", ZP_4},
    {"SDLK_h",      ZP_HOME},
};

int main() {
    printf("===================================================================\n");
    printf("  ZEEBO INPUT: mapeamento Z-Pad/SDL2 → AVK + despacho EVT_KEY_*     \n");
    printf("===================================================================\n");

    int fails = 0;

    // ── 1. Verifica o mapa Z-Pad → AVK (tabela canônica do BrewLoader) ───────
    struct { ZpadButton b; u32 avk; const char* n; } avk_cases[] = {
        {ZP_UP,AVK_UP,"UP"}, {ZP_DOWN,AVK_DOWN,"DOWN"}, {ZP_LEFT,AVK_LEFT,"LEFT"},
        {ZP_RIGHT,AVK_RIGHT,"RIGHT"}, {ZP_A,AVK_SELECT,"A/SELECT"},
        {ZP_B,AVK_CLR,"B/CLR"}, {ZP_1,AVK_SOFT1,"1"}, {ZP_2,AVK_SOFT2,"2"},
        {ZP_3,AVK_INFO,"3"}, {ZP_4,AVK_SPACE,"4"}, {ZP_HOME,AVK_FUNC,"HOME"},
    };
    printf("\n[1] Mapa Z-Pad → AVK BREW:\n");
    for (auto& c : avk_cases) {
        u32 got = avk_for_zpad(c.b);
        bool ok = (got == c.avk);
        printf("    %-9s → AVK 0x%04x %s\n", c.n, got, ok ? "✓" : "✗ ESPERADO DIFERENTE");
        if (!ok) fails++;
    }
    // Dígitos 0-9
    for (int d = 0; d <= 9; d++) {
        u32 got = avk_for_digit(d);
        if (got != (u32)(0x30 + d)) { printf("    dígito %d → 0x%04x ✗\n", d, got); fails++; }
    }
    printf("    dígitos 0-9 → 0x30..0x39 ✓\n");

    // Verifica o mapa SDL2 → Z-Pad → AVK (todos resolvem para AVK válido).
    printf("\n[2] Mapa SDL2 → Z-Pad → AVK:\n");
    for (auto& m : kSdlMap) {
        u32 avk = avk_for_zpad(m.zp);
        bool ok = (avk != 0);
        printf("    %-12s → zp=%d → AVK 0x%04x %s\n", m.sdl_name, m.zp, avk, ok ? "✓" : "✗");
        if (!ok) fails++;
    }

    // ── 3. Setup Unicorn + injeta o applet stub e o objeto applet ────────────
    uc_engine* uc = nullptr;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) {
        fprintf(stderr, "uc_open falhou\n"); return 1;
    }
    uc_ctl_set_cpu_model(uc, UC_CPU_ARM_1136);
    uc_mem_map(uc, SCRATCH_BASE, SCRATCH_SIZE, UC_PROT_ALL);
    std::vector<uint8_t> zero(SCRATCH_SIZE, 0);
    uc_mem_write(uc, SCRATCH_BASE, zero.data(), zero.size());
    uc_mem_write(uc, HANDLER_VA, kStub, sizeof(kStub));
    // objeto applet: campo dummy em [applet+0] (o stub não desreferencia)
    u32 tag = 0xA99A;
    uc_mem_write(uc, APPLET_VA, &tag, 4);

    BrewLoader loader(uc);

    // ── 4. Despacho CONTÍNUO de EVT_KEY_PRESS/RELEASE para o Z-Pad inteiro ───
    printf("\n[3] Despacho contínuo de eventos (press+release por botão):\n");
    ZpadButton seq[] = { ZP_UP, ZP_DOWN, ZP_LEFT, ZP_RIGHT, ZP_A, ZP_B,
                         ZP_1, ZP_2, ZP_3, ZP_4, ZP_HOME };
    int dispatched = 0, consumed = 0;
    for (ZpadButton b : seq) {
        u32 avk = avk_for_zpad(b);
        // PRESS
        bool ok1 = false;
        u32 r1 = loader.dispatch_event(HANDLER_VA, APPLET_VA, EVT_KEY_PRESS, avk,
                                       STACK_TOP, RET_MAGIC, 0, &ok1);
        dispatched++; if (ok1 && r1 == 1) consumed++; else fails++;
        // RELEASE
        bool ok2 = false;
        u32 r2 = loader.dispatch_event(HANDLER_VA, APPLET_VA, EVT_KEY_RELEASE, avk,
                                       STACK_TOP, RET_MAGIC, 0, &ok2);
        dispatched++; if (ok2 && r2 == 1) consumed++; else fails++;
    }
    // Dígitos numéricos 0-9 via EVT_KEY_PRESS
    for (int d = 0; d <= 9; d++) {
        bool ok = false;
        u32 r = loader.dispatch_event(HANDLER_VA, APPLET_VA, EVT_KEY_PRESS,
                                      avk_for_digit(d), STACK_TOP, RET_MAGIC, 0, &ok);
        dispatched++; if (ok && r == 1) consumed++; else fails++;
    }

    // ── 5. Evento não-tratado deve retornar r0=0 (honestidade: sem forjar) ───
    printf("\n[4] Evento fora do contrato (EVT_APP_START) → applet ignora:\n");
    bool okx = false;
    u32 rx = loader.dispatch_event(HANDLER_VA, APPLET_VA, 0x1f96 /*EVT_APP_START*/,
                                   0, STACK_TOP, RET_MAGIC, 0, &okx);
    if (!(okx && rx == 0)) { printf("    ✗ esperado r0=0 execução limpa\n"); fails++; }
    else printf("    ✓ r0=0 (não consumido, execução limpa)\n");

    // Entry ELF só é válido dentro do módulo. A segunda injeção na mesma página
    // também cobre o caminho de página já mapeada.
    std::vector<u8> elf(0x100, 0);
    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F';
    auto put32 = [&](u32 off, u32 value) {
        elf[off]=static_cast<u8>(value); elf[off+1]=static_cast<u8>(value>>8);
        elf[off+2]=static_cast<u8>(value>>16); elf[off+3]=static_cast<u8>(value>>24);
    };
    constexpr u32 elf_va=0x21000000u;
    put32(24,0x40u);
    if (!loader.inject_bytes(elf,elf_va,0,"valid.mod") ||
        loader.module().entry_va!=elf_va+0x40u) fails++;
    put32(24,0xf0000000u);
    if (!loader.inject_bytes(elf,elf_va,0,"invalid.mod") ||
        loader.module().entry_va!=0u) fails++;

    uc_close(uc);

    printf("\n── Resultado ──────────────────────────────────────────────\n");
    printf("Eventos despachados: %d | consumidos (r0=1): %d\n", dispatched, consumed);
    bool ok = (fails == 0) && (consumed == dispatched);
    printf("\n%s\n", ok
        ? "PASS: Z-Pad/SDL2 → AVK mapeado e EVT_KEY_PRESS/RELEASE despachados ao "
          "manipulador BREW com r0=1 (consumido) por EXECUÇÃO REAL."
        : "FAIL: ver diagnóstico acima.");
    return ok ? 0 : 1;
}
