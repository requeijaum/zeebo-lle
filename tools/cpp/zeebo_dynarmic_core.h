// zeebo_dynarmic_core.h — dynarmic A32 backend para o Core0 do Zeebo LLE (opt-in).
#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <array>
#include <span>

namespace zeebo::jit {

// Callbacks de memória: resolvem guest_addr -> bytes usando a VTLB LUT do LLE
// (RAM) e o modelo de periférico (MMIO). Injetados pelo ZeeboLLESystem.
struct MemoryBridge {
    void* user_data = nullptr;

    // Memória normal (RAM / VTLB)
    uint8_t  (*read8)(void* ud, uint32_t addr)  = nullptr;
    uint16_t (*read16)(void* ud, uint32_t addr) = nullptr;
    uint32_t (*read32)(void* ud, uint32_t addr) = nullptr;
    void (*write8)(void* ud, uint32_t addr, uint8_t val)   = nullptr;
    void (*write16)(void* ud, uint32_t addr, uint16_t val) = nullptr;
    void (*write32)(void* ud, uint32_t addr, uint32_t val) = nullptr;

    // Periféricos / MMIO
    bool     (*is_peripheral)(void* ud, uint32_t addr) = nullptr;
    uint32_t (*read_peripheral)(void* ud, uint32_t addr, int size) = nullptr;
    void     (*write_peripheral)(void* ud, uint32_t addr, int size, uint32_t val) = nullptr;

    // Code fetch opcional / monitor
    void     (*on_code)(void* ud, uint32_t pc) = nullptr;
};

// Identidade e estado de reset do CP15.
//
// Os valores vem do fonte do QEMU (target/arm/tcg/cpu32.c), que e a MESMA
// definicao usada pelo Unicorn (fork do QEMU) e portanto pelo nosso motor
// interpretado. Manter os dois backends com a mesma identidade e pre-requisito
// para o lockstep valer.
//
// Padrao = ARM1136, alinhado ao HARDWARE REAL. O log de boot do Linux
// 2.6.29-zeebo publicado pelo TripleOxygen (pastebin.com/raw/pdVwuLUV) imprime
//   "CPU: ARMv6-compatible processor [4117b362] revision 2 (ARMv6TEJ)"
// ou seja MIDR 0x4117b362 = part 0xb36 (ARM1136), variant 1, revision 2.
// Antes o projeto usava arm1176 (part 0xb76), que era divergencia de fato --
// e contradizia o proprio TRM que temos em maos (ARM1136 r1p5).
//
// O Unicorn nao expoe um modelo com revision 2 E variant 1 ao mesmo tempo:
//   arm1136_r2: midr=0x4107b362 (variant 0)  arm1136: midr=0x4117b363 (rev 3)
//   arm1176:    midr=0x410fb767 (part errado)
// Usamos UC_CPU_ARM_1136, que erra o alvo em UM bit (revision 3 vs 2) e acerta
// part e variant. Medido: a ISA observavel e identica entre 1136 e 1176 (unica
// diferenca e FPSID, tambem so na revisao), entao a troca e de identidade, nao
// de comportamento.
//
// Este campo e a identidade do motor RECOMPILADO; o interpretado recebe a mesma
// via uc_ctl_set_cpu_model. test_jit_cp15_reset cruza os dois e FALHA se
// divergirem -- foi ele que pegou esta troca pela metade.
//   ctr=0x01dd20d2 e reset_sctlr=0x00050078 em todos os tres.
struct Cp15Ids {
    // 0x4117b363 = UC_CPU_ARM_1136 do Unicorn. Difere do silicio real
    // (0x4117b362) apenas na revision; ver nota acima.
    uint32_t midr = 0x4117B363; // ARM1136 (part 0xb36), casa com UC_CPU_ARM_1136
    uint32_t ctr  = 0x01DD20D2; // idem
    // Valor de RESET do registrador de controle do sistema (SCTLR, c1,c0,0).
    // NAO e zero: traz W/P/D/L ligados. Um banco zerado fazia o boot perder
    // esses bits no primeiro read-modify-write.
    uint32_t sctlr_reset = 0x00050078;
};

class DynarmicCore {
public:
    explicit DynarmicCore(MemoryBridge bridge, Cp15Ids cp15 = {});
    ~DynarmicCore();

    void set_regs_zero();
    void set_cpsr(uint32_t cpsr);
    void set_pc(uint32_t pc);
    void set_sp(uint32_t sp);
    void set_lr(uint32_t lr);

    uint32_t pc() const;
    uint32_t cpsr() const;
    uint32_t sp() const;
    uint32_t lr() const;

    uint32_t reg(unsigned i) const;
    void set_reg(unsigned i, uint32_t val);

    // Sincronização entre Unicorn e Dynarmic
    void sync_to_unicorn(void* uc_engine_ptr) const;
    void sync_from_unicorn(void* uc_engine_ptr);

    // Executa fatias de até `max_insns`. Retorna o número de instruções/ticks consumidos.
    uint64_t run(uint64_t max_insns);
    uint64_t total_ticks() const;
    void halt_execution();
    // Interrompe a fatia atual a partir de dentro de on_code_exec.
    void halt_from_hook();
    bool step_one_insn();

    // Falha da ultima fatia executada. Sem isto, uma instrucao invalida ou um
    // fallback de interpretador viram laco silencioso: o PC fica parado e o
    // contador de instrucoes sobe indefinidamente, sem qualquer diagnostico.
    struct Fault {
        bool raised = false;         // excecao (instrucao indefinida etc.)
        bool interpreter_fallback = false;
        uint32_t pc = 0;
        uint32_t kind = 0;           // Dynarmic::A32::Exception, como inteiro
    };
    Fault take_fault();

    // Dispatcher de SVC chamado em tempo real
    std::function<void(uint32_t swi)> on_svc;

    // Gancho por INSTRUCAO EXECUTADA. Diferente de MemoryBridge::on_code, que
    // e disparado pela TRADUCAO de bloco (uma vez por bloco compilado, e nao a
    // cada execucao): aqui o callback recebe o PC real do motor em execucao e
    // pode alterar registradores via este mesmo objeto. Quando definido, run()
    // passa a avancar instrucao a instrucao -- mais lento, porem correto.
    std::function<void(uint32_t pc)> on_code_exec;

    // Habilita fastmem se uma tabela de ponteiros host for providenciada
    void enable_page_table(std::array<std::uint8_t*, 1 << (32 - 12)>* pt);

    // Invalida cache JIT para um intervalo de código
    void invalidate_cache(uint32_t addr, size_t size);

    void clear_cache();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeebo::jit
