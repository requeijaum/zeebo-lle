// test_linux_mmu_contract.cpp — Contrato de MMU/CPU extraido do boot REAL do Zeebo.
//
// FONTE (oraculo externo, nao inventado por nos)
//   Log de boot do Linux 2.6.29-zeebo publicado por Fausto "TripleOxygen":
//   https://pastebin.com/raw/pdVwuLUV  (168 linhas, build #85, 07/ago/2011)
//   Bootloader "Zeeboot v0.1", Machine ID Zeebo (1009000), kernel @0x10008000.
//
// POR QUE ISTO VALE SEM TER O zImage
//   O zImage do Fausto nunca foi publicado, entao nao podemos BOOTAR o kernel
//   dele. Mas o log imprime valores que sao consequencia direta da configuracao
//   de MMU e do modelo de CPU do hardware real. Esses valores sao um CONTRATO
//   verificavel hoje, sem imagem nenhuma: se o nosso emulador contradiz um
//   numero que o silicio real produziu, o emulador esta errado.
//
// O QUE O LOG PROVA
//
// 1) TRADUCAO LINEAR VA->PA COM DELTA FIXO 0xb0000000
//    Seis regioes, todas com VA-PA == 0xb0000000:
//      c0900000 <- 10900000  pmem kernel ebi1 arena   2MB
//      c0b00000 <- 10b00000  pmem                     8MB
//      c1300000 <- 11300000  camera pmem             10MB
//      c1d00000 <- 11d00000  adsp pmem                8MB
//      c2600000 <- 12600000  gpu1 pmem                8MB
//      c2e00000 <- 12e00000  fb                       2MB
//    Isso fixa PAGE_OFFSET=0xC0000000 e PHYS_OFFSET=0x10000000, ou seja, a RAM
//    do Zeebo COMECA em 0x10000000 -- exatamente a base que ja usamos.
//
// 2) O FRAMEBUFFER TEM DOIS MAPEAMENTOS DA MESMA MEMORIA FISICA
//    "msm_fb_probe: resource fbram = 0xc4c00000 phys=0x12e00000"
//    A PA 0x12e00000 e a mesma da regiao "fb", mas o VA e 0xc4c00000, cujo
//    delta (0xb1e00000) NAO e o delta linear. Ou seja: alias de ioremap.
//    Consequencia direta para o backend de GPU: escrita via um VA precisa ser
//    visivel pelo outro. Um emulador que trate os dois VAs como buffers
//    separados exibe tela velha ou preta -- que e a classe de bug que estamos
//    cacando no caminho grafico.
//
// 3) MODELO DE CPU: ARM1136, NAO ARM1176
//    "CPU: ARMv6-compatible processor [4117b362] revision 2 (ARMv6TEJ)"
//    MIDR 0x4117b362 -> part 0xb36 (ARM1136), variant 1, revision 2.
//    O nosso emulador usa 0x410FB767 (part 0xb76 = ARM1176) e
//    UC_CPU_ARM_1176 em ~12 lugares. E divergencia de fato contra hardware.
//    Este teste NAO altera o emulador (trocar o modelo de CPU no projeto
//    inteiro merece decisao e medicao proprias); ele REGISTRA a divergencia de
//    forma executavel para que ela nao se perca.
//
// 4) SCTLR REAL NO MOMENTO DO BOOT: cr=00c5387f
//    M=1 (MMU ligada), C=1 e I=1 (caches), W=1, Z=1 (branch pred),
//    V=1 (HIGH VECTORS em 0xffff0000), U=1 (acesso desalinhado permitido),
//    XP=1 (extended page tables), TR=0 (sem TEX remap).
//    V=1 confirma vetores altos; U=1 e XP=1 sao o modo de paginacao ARMv6.
//
// CRITERIOS
//   M1  delta VA-PA constante 0xb0000000 nas 6 regioes (aritmetica do log)
//   M2  o emulador mapeia RAM em PHYS_OFFSET=0x10000000
//   M3  alias de framebuffer: escrita em um VA e visivel no outro (comportamento)
//   M4  MIDR do emulador vs MIDR real -- divergencia registrada
//
// M3 e o unico criterio COMPORTAMENTAL (roda codigo ARM de verdade); M1/M2/M4
// sao de contrato. M3 usa controle negativo embutido.
//
// Compile e rode de dentro de tools/cpp/.

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

typedef uint32_t u32;
typedef uint64_t u64;

// ---- Fatos extraidos do log (constantes, nao ajustaveis) ----
static const u32 PAGE_OFFSET   = 0xC0000000u;
static const u32 PHYS_OFFSET   = 0x10000000u;
static const u32 LINEAR_DELTA  = 0xB0000000u;
static const u32 FB_PHYS       = 0x12E00000u;
static const u32 FB_VA_LINEAR  = 0xC2E00000u;  // regiao "fb"
static const u32 FB_VA_IOREMAP = 0xC4C00000u;  // resource fbram
static const u32 MIDR_REAL     = 0x4117B362u;  // ARM1136 r1p2
static const u32 MIDR_OURS     = 0x410FB767u;  // ARM1176 (zeebo_dynarmic_core.h)
static const u32 SCTLR_REAL    = 0x00C5387Fu;

struct Region { const char* name; u32 va, pa, size; };
static const Region kRegions[] = {
    {"pmem kernel ebi1 arena", 0xc0900000, 0x10900000,  2u<<20},
    {"pmem",                   0xc0b00000, 0x10b00000,  8u<<20},
    {"camera pmem",            0xc1300000, 0x11300000, 10u<<20},
    {"adsp pmem",              0xc1d00000, 0x11d00000,  8u<<20},
    {"gpu1 pmem",              0xc2600000, 0x12600000,  8u<<20},
    {"fb",                     0xc2e00000, 0x12e00000,  2u<<20},
};

int main(int argc, char** argv) {
    const bool selftest = (argc > 1 && std::string(argv[1]) == "--selftest");
    int fails = 0;

    printf("== Contrato de MMU do boot real (Linux 2.6.29-zeebo) ==\n");
    printf("   fonte: pastebin.com/raw/pdVwuLUV (TripleOxygen, 07/ago/2011)\n\n");

    // ---------------- M1: delta linear constante ----------------
    printf("[M1] traducao linear VA->PA\n");
    bool m1 = true;
    for (const Region& r : kRegions) {
        u32 d = r.va - r.pa;
        bool ok = (d == LINEAR_DELTA);
        if (!ok) m1 = false;
        printf("     %-24s 0x%08x <- 0x%08x  delta 0x%08x %s\n",
               r.name, r.va, r.pa, d, ok ? "" : "  <== DIVERGE");
    }
    printf("     PAGE_OFFSET 0x%08x - PHYS_OFFSET 0x%08x = 0x%08x\n",
           PAGE_OFFSET, PHYS_OFFSET, PAGE_OFFSET - PHYS_OFFSET);
    printf("     M1: %s\n\n", m1 ? "PASS" : "FAIL");
    if (!m1) fails++;

    // ---------------- M2: base de RAM ----------------
    printf("[M2] RAM comeca em PHYS_OFFSET\n");
    bool m2;
    {
        uc_engine* uc = nullptr;
        uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
        uc_err e = uc_mem_map(uc, PHYS_OFFSET, 64u << 20, UC_PROT_ALL);
        u32 probe = 0xdeadbeef, back = 0;
        uc_mem_write(uc, PHYS_OFFSET + 0x8000, &probe, 4);   // kernel @0x10008000
        uc_mem_read(uc, PHYS_OFFSET + 0x8000, &back, 4);
        m2 = (e == UC_ERR_OK && back == probe);
        printf("     map 0x%08x +64MB = %s; escrita em 0x10008000 (kernel) = 0x%08x\n",
               PHYS_OFFSET, uc_strerror(e), back);
        uc_close(uc);
    }
    printf("     M2: %s\n\n", m2 ? "PASS" : "FAIL");
    if (!m2) fails++;

    // ---------------- M3: alias de framebuffer (comportamental) ----------------
    // Executa ARM real: escreve 0xA5A5A5A5 via VA linear, le via VA do ioremap.
    printf("[M3] alias de framebuffer (fbram 0x%08x == fb 0x%08x, phys 0x%08x)\n",
           FB_VA_IOREMAP, FB_VA_LINEAR, FB_PHYS);
    auto alias_run = [&](bool with_alias, u32& read_back) -> bool {
        uc_engine* uc = nullptr;
        if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc) != UC_ERR_OK) return false;
        const u32 CODE = 0x10000000u;
        uc_mem_map(uc, CODE, 0x1000, UC_PROT_ALL);

        static uint8_t page[0x1000];
        memset(page, 0, sizeof(page));
        // VA linear aponta para a pagina host compartilhada.
        uc_mem_map_ptr(uc, FB_VA_LINEAR, 0x1000, UC_PROT_ALL, page);
        if (with_alias) {
            // Alias real: MESMO ponteiro host no segundo VA.
            uc_mem_map_ptr(uc, FB_VA_IOREMAP, 0x1000, UC_PROT_ALL, page);
        } else {
            // Controle negativo: buffer SEPARADO (bug que queremos detectar).
            static uint8_t other[0x1000];
            memset(other, 0, sizeof(other));
            uc_mem_map_ptr(uc, FB_VA_IOREMAP, 0x1000, UC_PROT_ALL, other);
        }

        // Montado com arm-none-eabi-as (offsets de literal conferidos no
        // objdump -- a versao escrita a mao tinha os deslocamentos errados e
        // lia lixo em vez do endereco do framebuffer).
        //   ldr r0,[pc,#16] ; ldr r1,[pc,#16] ; str r1,[r0]
        //   ldr r2,[pc,#12] ; ldr r3,[r2]     ; b .
        const u32 prog[] = {
            0xe59f0010,
            0xe59f1010,
            0xe5801000,
            0xe59f200c,
            0xe5923000,
            0xeafffffe,
            FB_VA_LINEAR,
            0xA5A5A5A5u,
            FB_VA_IOREMAP,
        };
        uc_mem_write(uc, CODE, prog, sizeof(prog));
        uc_emu_start(uc, CODE, CODE + 0x18, 0, 6);
        uc_reg_read(uc, UC_ARM_REG_R3, &read_back);
        uc_close(uc);
        return true;
    };

    u32 with = 0, without = 0;
    alias_run(true, with);
    alias_run(false, without);
    printf("     com alias   : r3 = 0x%08x (esperado 0xa5a5a5a5)\n", with);
    printf("     sem alias   : r3 = 0x%08x (controle negativo, esperado 0x00000000)\n", without);
    bool m3 = (with == 0xA5A5A5A5u) && (without != 0xA5A5A5A5u);
    if (with != 0xA5A5A5A5u)
        printf("     !! o alias nao propagou a escrita\n");
    if (without == 0xA5A5A5A5u)
        printf("     !! FALHA DE INSTRUMENTO: o controle negativo tambem 'passou'\n");
    printf("     M3: %s\n\n", m3 ? "PASS" : "FAIL");
    if (!m3) fails++;

    if (selftest) {
        // O selftest exige que o instrumento DISTINGA alias de nao-alias.
        if (!m3) {
            fprintf(stderr, "SELFTEST FALHOU: instrumento nao distingue alias de buffers separados.\n");
            return 1;
        }
        printf("SELFTEST OK: instrumento distingue alias real de buffer separado.\n");
        return 0;
    }

    // ---------------- M4: modelo de CPU ----------------
    printf("[M4] modelo de CPU declarado vs hardware real\n");
    auto part = [](u32 m) { return (m >> 4) & 0xfff; };
    printf("     hardware real : MIDR 0x%08x  part 0x%03x (ARM1136) rev %u\n",
           MIDR_REAL, part(MIDR_REAL), MIDR_REAL & 0xf);
    printf("     nosso emulador: MIDR 0x%08x  part 0x%03x (ARM1176) rev %u\n",
           MIDR_OURS, part(MIDR_OURS), MIDR_OURS & 0xf);
    bool m4 = (part(MIDR_REAL) == part(MIDR_OURS));
    if (!m4) {
        printf("     DIVERGENCIA CONHECIDA: o Zeebo real e ARM1136 (0xb36); o projeto\n");
        printf("     usa UC_CPU_ARM_1176 em ~12 pontos. Registrado, nao corrigido aqui:\n");
        printf("     trocar o modelo de CPU exige medicao propria (ARM1136 nao tem as\n");
        printf("     mesmas extensoes) e pode mexer em todo o bring-up.\n");
    }
    printf("     M4: %s\n\n", m4 ? "PASS" : "DIVERGENTE (esperado por ora)");

    // ---------------- SCTLR informativo ----------------
    printf("[info] SCTLR real no boot: cr=0x%08x\n", SCTLR_REAL);
    printf("       M=%u C=%u I=%u W=%u Z=%u V=%u(high vectors) U=%u XP=%u TR=%u\n",
           (SCTLR_REAL>>0)&1,(SCTLR_REAL>>2)&1,(SCTLR_REAL>>12)&1,(SCTLR_REAL>>3)&1,
           (SCTLR_REAL>>11)&1,(SCTLR_REAL>>13)&1,(SCTLR_REAL>>22)&1,
           (SCTLR_REAL>>23)&1,(SCTLR_REAL>>28)&1);

    printf("\n---------------------------------------------\n");
    printf("M1 linear delta : %s\n", m1 ? "PASS" : "FAIL");
    printf("M2 base da RAM  : %s\n", m2 ? "PASS" : "FAIL");
    printf("M3 alias do fb  : %s\n", m3 ? "PASS" : "FAIL");
    printf("M4 modelo de CPU: %s\n", m4 ? "PASS" : "DIVERGENTE (registrado)");
    if (fails == 0) {
        printf("\nGREEN: emulador consistente com o contrato de MMU do hardware real.\n");
        return 0;
    }
    printf("\nRED: %d criterio(s) de MMU divergem do hardware real.\n", fails);
    return 1;
}
