// zeebo_slide_detector.h — Detector de NOP-slide/derail do Core1 SEM hotspot.
//
// PROBLEMA (bug 8): o detector antigo vivia no UC_HOOK_CODE (uma vez POR
// INSTRUCAO) e fazia um `uc_mem_read(uc, pc, &insn, 4)` incondicional em cada
// insn so para decidir se era control-flow. Isso e' o fetch mais quente do
// Core1: dobra o custo de cada instrucao interpretada.
//
// OBSERVACAO QUE ELIMINA O HOTSPOT
// Um NOP-slide autentico NAO contem NENHUM branch tomado por centenas de
// instrucoes. No Unicorn, um bloco basico termina exatamente num branch/escrita
// de PC. Logo:
//   - codigo legitimo (loops de init, bl/beq/pop pc) => MUITOS blocos PEQUENOS;
//   - um slide/derail => UM (ou poucos blocos CONTIGUOS) bloco ENORME, linear.
// Portanto a fronteira de bloco JA E' a deteccao de control-flow: o
// UC_HOOK_BLOCK entrega (start, size) e o numero de instrucoes lineares sai de
// size/4 — sem ler um unico opcode no caminho quente.
//
// A deteccao de "area zerada/NOP" continua possivel, mas fora do hot path:
// amostra-se no maximo UMA palavra por bloco (e so quando ja ha suspeita de
// run linear), nao uma por instrucao.
#pragma once
#include <cstdint>

namespace zeebo {

struct SlideDetector {
    using u32 = uint32_t;

    // Config (cacheada fora do hot path; nao muda durante a execucao).
    u32 slide_limit = 256;   // insns lineares consecutivas SEM branch => derail

    // Estado.
    u32  linear_run = 0;     // insns lineares acumuladas em blocos contiguos
    u32  prev_end   = 0;     // fim (start+size) do bloco anterior; 0 = nenhum
    bool tripped    = false; // ja disparou (evita spam)

    // Chamado uma vez POR BLOCO BASICO (UC_HOOK_BLOCK). `insn_count` = size/4.
    // Retorna true no exato bloco em que o detector dispara.
    //
    // Blocos ARM tem tamanho multiplo de 4; insn_count = size/4. Blocos
    // contiguos (start == prev_end) representam uma corrida linear sem branch
    // tomado interrompida apenas pelo limite de traducao do Unicorn, entao
    // acumulam. Um branch tomado leva a um start != prev_end, o que zera a run.
    bool on_block(u32 start, u32 insn_count) {
        if (tripped) return false;
        if (insn_count == 0) insn_count = 1;

        if (prev_end != 0 && start == prev_end) {
            linear_run += insn_count;      // bloco emenda no anterior: mesma run
        } else {
            linear_run = insn_count;       // salto/branch tomado: nova run
        }
        prev_end = start + insn_count * 4;

        if (linear_run >= slide_limit) {
            tripped = true;
            return true;
        }
        return false;
    }
};

} // namespace zeebo
