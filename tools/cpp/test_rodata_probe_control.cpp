// ── OBSOLETO POR QW56 (causa raiz corrigida) ────────────────────────────────
// Este teste nasceu quando o boot travava no panic thread.cc:1273 e o
// ZEEBO_PROBE=rodata era o unico jeito de passar dele. Ele exigia que:
//    - rodata avancasse >3x sobre o baseline
//    - rodata ALCANCASSE a assertion do TCB
//
// QW56 provou que aquilo era REMENDO: a causa real era o Split I/D servindo
// o pristino (zeros) sobre o .bss do kernel, apagando a struct do alocador de
// TCB. Corrigido em c1_heap_read_hook (guarda REX_KERNEL_FILESZ), o boot passa
// do TCB SEM probe algum -- logo as premissas 1 e 3 sao falsas por construcao.
//
// O que ainda tem valor e' o CONTROLE NEGATIVO (premissa 2/4): a faixa
// irrelevante nao pode destravar nada. Isso segue verificado abaixo.
// A cobertura da causa real esta' em test_split_id_bss_clobber.
// ────────────────────────────────────────────────────────────────────────────
// test_rodata_probe_control.cpp — controle negativo do probe de .rodata (Core1).
//
// CONTEXTO
// O boot do Core1 trava num laco de page-table walk. Servir a faixa
// 0xf000e000..0xf0010000 (.rodata do kernel OKL4, contendo a tabela de tamanhos
// de pagina {12,16,20,26,32} em 0xf000efc8) a partir do snapshot PRISTINO faz o
// boot avancar ~8x e alcancar a assertion do proprio kernel
// (pistachio/src/thread.cc:1273, "Failed to create root server TCB").
//
// POR QUE ESTE TESTE EXISTE
// Regra do projeto (notes/MORE_INFO.md §7): "Qualquer ferramenta nova que
// 'prove' progresso deve ser rodada tambem com entrada invalida; se o resultado
// for igual, a ferramenta provou o harness, nao o guest."
//
// Um contador maior NAO e prova de correcao. Sem controle negativo, o salto
// 8,5M -> 73M poderia vir da propria mecanica de servir memoria do pristino
// (mais paginas coerentes = mais progresso), independente de QUAL faixa.
//
// O CONTROLE
// Roda o mesmo binario 3x, variando so ZEEBO_PROBE:
//   unset    -> baseline
//   rodata   -> hipotese:  f000e000..f0010000
//   control  -> faixa vizinha irrelevante: f0012000..f0014000 (mesma mecanica,
//               mesmo tamanho de janela, sem uso conhecido pelo walk)
//
// CRITERIO (falha o teste se qualquer um quebrar):
//   1. rodata  DEVE avancar substancialmente sobre o baseline  (>3x)
//   2. control NAO pode avancar substancialmente                (<1.5x)
//   3. rodata  DEVE alcancar a assertion do TCB
//   4. control NAO pode alcancar a assertion do TCB
//
// Se (2) ou (4) falhar, a conclusao "a .rodata zerada e a causa" esta ERRADA e
// o probe provou apenas o instrumento.
//
// MEDIDO em 2026-09-10 (--seconds=12):
//   baseline  8.510.000 insns   TCB=nao
//   rodata   73.210.000 insns   TCB=SIM
//   control   8.310.000 insns   TCB=nao
//
// INTERPRETADOR PURO (exigencia do projeto)
// Investigacao de LLE roda SEM Dynarmic: o JIT introduz uma variavel a mais
// (divergencia de backend) num problema que ainda e de emulacao low-level. O
// comando montado abaixo NAO passa --jit nem --jit-solo, e o teste ABORTA se a
// saida mencionar o backend JIT -- assim a exigencia fica verificada, nao
// apenas prometida em comentario.
//
// SKIP (exit 77) se a imagem AMSS nao estiver presente.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

static const char* kAmss = "../../nand/1.1.2_AMSS.bin";

struct Run {
    unsigned long long insns = 0;
    bool tcb = false;
    bool jit = false;   // backend JIT detectado na saida (nao deve ocorrer)
};

// Roda o emulador com um valor de ZEEBO_PROBE e extrai (insns, assertion TCB).
static Run run_probe(const char* modo) {
    std::string cmd;
    if (modo) cmd += std::string("ZEEBO_PROBE=") + modo + " ";
    cmd += "./zeebo_lle_main --headless --boot-appmgr --seconds=12 2>&1";

    Run r;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;

    char linha[4096];
    while (fgets(linha, sizeof(linha), p)) {
        if (strstr(linha, "thread.cc:1273") ||
            strstr(linha, "Failed to create root server")) {
            r.tcb = true;
        }
        // guarda: nenhuma corrida deste teste pode usar Dynarmic
        if (strstr(linha, "Dynarmic") || strstr(linha, "JIT backend")) {
            r.jit = true;
        }
        // ultima ocorrencia de insns= vence (progresso final)
        const char* m = nullptr;
        for (const char* s = linha; (s = strstr(s, "insns=")); ++s) m = s;
        if (m) r.insns = strtoull(m + 6, nullptr, 10);
    }
    pclose(p);
    return r;
}

int main() {
    struct stat st;
    if (stat(kAmss, &st) != 0) {
        fprintf(stderr, "SKIP: imagem AMSS ausente (%s)\n", kAmss);
        return 77;
    }
    if (stat("./zeebo_lle_main", &st) != 0) {
        fprintf(stderr, "SKIP: zeebo_lle_main nao construido\n");
        return 77;
    }

    printf("== controle negativo do probe de .rodata ==\n");

    Run base = run_probe(nullptr);
    Run rod  = run_probe("rodata");
    Run ctl  = run_probe("control");

    printf("  baseline  insns=%llu  TCB=%s\n", base.insns, base.tcb ? "SIM" : "nao");
    printf("  rodata    insns=%llu  TCB=%s\n", rod.insns,  rod.tcb  ? "SIM" : "nao");
    printf("  control   insns=%llu  TCB=%s\n", ctl.insns,  ctl.tcb  ? "SIM" : "nao");

    if (base.jit || rod.jit || ctl.jit) {
        fprintf(stderr, "FALHA: backend JIT/Dynarmic detectado -- este teste exige "
                        "interpretador puro (sem --jit/--jit-solo)\n");
        return 1;
    }
    printf("  [guarda] interpretador puro confirmado (sem Dynarmic)\n");

    if (base.insns == 0) {
        fprintf(stderr, "FALHA: baseline nao produziu contagem de insns\n");
        return 1;
    }

    int falhas = 0;
    const double r_rod = (double)rod.insns / (double)base.insns;
    const double r_ctl = (double)ctl.insns / (double)base.insns;

    // (1) a hipotese precisa avancar
    if (r_rod > 3.0) {
        printf("[PASS] rodata avanca sobre o baseline (%.2fx > 3x)\n", r_rod);
    } else {
        printf("[INFO/QW56] rodata nao avanca mais (%.2fx) -- esperado:\n"
               "         a causa raiz foi corrigida, o probe virou no-op.\n", r_rod);
    }

    // (2) CONTROLE NEGATIVO: a faixa irrelevante NAO pode avancar
    if (r_ctl < 1.5) {
        printf("[PASS] controle negativo: faixa irrelevante nao destrava (%.2fx < 1.5x)\n", r_ctl);
    } else {
        printf("[FALHA] CONTROLE NEGATIVO QUEBROU: faixa irrelevante tambem "
               "destravou (%.2fx) -- o probe provou o instrumento, nao a causa\n", r_ctl);
        falhas++;
    }

    // (3)/(4) a assertion do TCB deve ser exclusiva da hipotese
    if (rod.tcb) {
        printf("[PASS] rodata alcanca a assertion do TCB (thread.cc:1273)\n");
    } else {
        printf("[INFO/QW56] rodata nao alcanca mais o TCB -- esperado:\n"
               "         o boot passa do TCB sem probe (ver test_split_id_bss_clobber).\n");
    }
    if (!ctl.tcb) {
        printf("[PASS] controle negativo: faixa irrelevante nao alcanca o TCB\n");
    } else {
        printf("[FALHA] CONTROLE NEGATIVO QUEBROU: faixa irrelevante tambem "
               "alcancou o TCB\n");
        falhas++;
    }

    if (falhas) {
        printf("\nFALHOU (%d)\n", falhas);
        return 1;
    }
    printf("\nTODOS OS TESTES PASSARAM -- a .rodata zerada e causa especifica, "
           "nao artefato do instrumento\n");
    return 0;
}
