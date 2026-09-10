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

// IDs CP15 reais para o ARM1136EJ-S do MSM7201A
struct Cp15Ids {
    uint32_t midr = 0x4107B362; // ARM1136 family (part 0xB36)
    uint32_t ctr  = 0x1D152152;
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
