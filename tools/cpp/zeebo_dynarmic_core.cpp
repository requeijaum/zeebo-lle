// zeebo_dynarmic_core.cpp — dynarmic A32 backend implementation
#include "zeebo_dynarmic_core.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>

#include <unicorn/unicorn.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace zeebo::jit {

class ZeeboCoprocessor final : public Dynarmic::A32::Coprocessor {
public:
    using Coprocessor = Dynarmic::A32::Coprocessor;
    using CoprocReg = Dynarmic::A32::CoprocReg;

    explicit ZeeboCoprocessor(Cp15Ids ids) : ids_(ids) {
        scratch_ = 0;
        scratch2_ = 0;
        midr_val_ = ids.midr;
        ctr_val_  = ids.ctr;
        apply_reset_state(ids.sctlr_reset);
    }

    static std::uint64_t NopFn(void*, std::uint32_t, std::uint32_t) { return 0; }

    // Banco de registradores CP15 com estado real.
    //
    // Antes, TODA escrita era descartada (NopFn) e toda leitura fora de
    // MIDR/CTR devolvia zero. O boot do Core0 escreve o controle do sistema
    // (c1) e a base da tabela de paginas (c2) e depois LE esses valores de
    // volta: o motor interpretado devolvia o valor escrito, o recompilado
    // devolvia 0. Primeira divergencia observada entre os backends:
    // `MRC p15,0,r0,c2,c0,0` (TTBR0) devolvendo 0 em vez de 0x1001c000.
    //
    // Guarda por (opc1, CRn, CRm, opc2) para nao confundir registradores
    // diferentes que compartilham o mesmo CRn.
    static constexpr unsigned kBankSize = 8 * 16 * 16 * 8;
    static unsigned slot(unsigned opc1, unsigned CRn, unsigned CRm, unsigned opc2) {
        return ((opc1 & 7u) << 11) | ((CRn & 15u) << 7) | ((CRm & 15u) << 3) | (opc2 & 7u);
    }

    std::uint32_t* bank_slot(unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) {
        return &bank_[slot(opc1, (unsigned)CRn, (unsigned)CRm, opc2)];
    }

    // Aplica o estado de RESET do CP15. O banco nasce zerado, mas o hardware
    // nao: o registrador de controle do sistema (SCTLR, c1,c0,0) tem valor de
    // reset 0x00050078 (bits W/P/D/L ligados) segundo o fonte do QEMU, que e a
    // mesma definicao usada pelo Unicorn. Sem isto, a primeira leitura de SCTLR
    // devolvia 0 e o boot perdia esses bits no read-modify-write.
    void apply_reset_state(std::uint32_t sctlr_reset) {
        *bank_slot(0, static_cast<CoprocReg>(1), static_cast<CoprocReg>(0), 0) = sctlr_reset;
    }

    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg,
                                                     CoprocReg, CoprocReg, unsigned) override {
        return Callback{&NopFn, std::nullopt};
    }

    CallbackOrAccessOneWord CompileSendOneWord(bool /*two*/, unsigned opc1, CoprocReg CRn,
                                               CoprocReg CRm, unsigned opc2) override {
        // MCR: o destino e o proprio slot do banco, entao o valor escrito
        // persiste e uma leitura posterior o devolve.
        return bank_slot(opc1, CRn, CRm, opc2);
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override {
        return Callback{&NopFn, std::nullopt};
    }

    CallbackOrAccessOneWord CompileGetOneWord(bool /*two*/, unsigned opc1, CoprocReg CRn,
                                              CoprocReg CRm, unsigned opc2) override {
        // MRC p15, 0, Rd, c0, c0, 0 => Main ID Register (MIDR)
        if (opc1 == 0 && (unsigned)CRn == 0 && (unsigned)CRm == 0 && opc2 == 0) {
            return &midr_val_;
        }
        // MRC p15, 0, Rd, c0, c0, 1 => Cache Type Register (CTR)
        if (opc1 == 0 && (unsigned)CRn == 0 && (unsigned)CRm == 0 && opc2 == 1) {
            return &ctr_val_;
        }
        // Demais registradores: devolve o que foi escrito (zero se nunca).
        return bank_slot(opc1, CRn, CRm, opc2);
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override {
        scratch_ = 0;
        scratch2_ = 0;
        return std::array<std::uint32_t*, 2>{&scratch_, &scratch2_};
    }

    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg,
                                             std::optional<std::uint8_t>) override {
        return Callback{&NopFn, std::nullopt};
    }

    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg,
                                              std::optional<std::uint8_t>) override {
        return Callback{&NopFn, std::nullopt};
    }

private:
    Cp15Ids ids_;
    std::uint32_t midr_val_ = 0;
    std::uint32_t ctr_val_  = 0;
    std::uint32_t bank_[kBankSize] = {};
    std::uint32_t scratch_  = 0;
    std::uint32_t scratch2_ = 0;
};

struct DynarmicCore::Impl final : public Dynarmic::A32::UserCallbacks {
    MemoryBridge bridge;
    Cp15Ids cp15;
    Dynarmic::A32::UserConfig config;
    std::unique_ptr<Dynarmic::A32::Jit> jit;

    uint64_t ticks_left = 0;
    uint64_t ticks_consumed_total = 0;
    bool svc_hit_this_block = false;
    // Sinalizado quando o gancho por instrucao pede parada (ex.: apos desviar o
    // PC para tratar uma chamada interceptada).
    bool halted_by_hook = false;

    Impl(MemoryBridge b, Cp15Ids ids) : bridge(b), cp15(ids) {
        config.callbacks = this;
        config.arch_version = Dynarmic::A32::ArchVersion::v6K;

        auto cp15_coproc = std::make_shared<ZeeboCoprocessor>(ids);
        config.coprocessors[15] = cp15_coproc;

        auto nop_other = std::make_shared<ZeeboCoprocessor>(ids);
        for (size_t i = 0; i < 15; i++) {
            config.coprocessors[i] = nop_other;
        }

        jit = std::make_unique<Dynarmic::A32::Jit>(config);
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t addr) override {
        if (bridge.on_code) {
            bridge.on_code(bridge.user_data, addr);
        }
        return MemoryRead32(addr);
    }

    std::uint8_t MemoryRead8(std::uint32_t addr) override {
        if (bridge.is_peripheral && bridge.is_peripheral(bridge.user_data, addr)) {
            return (std::uint8_t)bridge.read_peripheral(bridge.user_data, addr, 1);
        }
        if (bridge.read8) {
            return bridge.read8(bridge.user_data, addr);
        }
        return 0;
    }

    std::uint16_t MemoryRead16(std::uint32_t addr) override {
        if (bridge.is_peripheral && bridge.is_peripheral(bridge.user_data, addr)) {
            return (std::uint16_t)bridge.read_peripheral(bridge.user_data, addr, 2);
        }
        if (bridge.read16) {
            return bridge.read16(bridge.user_data, addr);
        }
        return (std::uint16_t)MemoryRead8(addr) | ((std::uint16_t)MemoryRead8(addr + 1) << 8);
    }

    std::uint32_t MemoryRead32(std::uint32_t addr) override {
        if (bridge.is_peripheral && bridge.is_peripheral(bridge.user_data, addr)) {
            return bridge.read_peripheral(bridge.user_data, addr, 4);
        }
        if (bridge.read32) {
            return bridge.read32(bridge.user_data, addr);
        }
        return (std::uint32_t)MemoryRead16(addr) | ((std::uint32_t)MemoryRead16(addr + 2) << 16);
    }

    std::uint64_t MemoryRead64(std::uint32_t addr) override {
        return (std::uint64_t)MemoryRead32(addr) | ((std::uint64_t)MemoryRead32(addr + 4) << 32);
    }

    void MemoryWrite8(std::uint32_t addr, std::uint8_t val) override {
        if (bridge.is_peripheral && bridge.is_peripheral(bridge.user_data, addr)) {
            bridge.write_peripheral(bridge.user_data, addr, 1, val);
            return;
        }
        if (bridge.write8) {
            bridge.write8(bridge.user_data, addr, val);
        }
    }

    void MemoryWrite16(std::uint32_t addr, std::uint16_t val) override {
        if (bridge.is_peripheral && bridge.is_peripheral(bridge.user_data, addr)) {
            bridge.write_peripheral(bridge.user_data, addr, 2, val);
            return;
        }
        if (bridge.write16) {
            bridge.write16(bridge.user_data, addr, val);
            return;
        }
        MemoryWrite8(addr, (std::uint8_t)val);
        MemoryWrite8(addr + 1, (std::uint8_t)(val >> 8));
    }

    void MemoryWrite32(std::uint32_t addr, std::uint32_t val) override {
        if (bridge.is_peripheral && bridge.is_peripheral(bridge.user_data, addr)) {
            bridge.write_peripheral(bridge.user_data, addr, 4, val);
            return;
        }
        if (bridge.write32) {
            bridge.write32(bridge.user_data, addr, val);
            return;
        }
        MemoryWrite16(addr, (std::uint16_t)val);
        MemoryWrite16(addr + 2, (std::uint16_t)(val >> 16));
    }

    void MemoryWrite64(std::uint32_t addr, std::uint64_t val) override {
        MemoryWrite32(addr, (std::uint32_t)val);
        MemoryWrite32(addr + 4, (std::uint32_t)(val >> 32));
    }

    uint32_t last_swi_num = 0;
    void CallSVC(std::uint32_t swi) override {
        svc_hit_this_block = true;
        last_swi_num = swi;
        // Pára o quantum no SVC para tratar de forma síncrona
        ticks_left = 0;
    }

    // Guarda a ultima excecao para o chamador poder relatar. Descartar isso
    // fazia uma instrucao invalida virar laco silencioso: o quantum terminava,
    // o laco principal reiniciava a execucao no mesmo PC e o contador subia
    // para sempre sem que nada indicasse a falha.
    bool exception_pending = false;
    std::uint32_t exception_pc = 0;
    Dynarmic::A32::Exception exception_kind{};

    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exc) override {
        exception_pending = true;
        exception_pc = pc;
        exception_kind = exc;
        ticks_left = 0;
    }

    bool fallback_pending = false;
    std::uint32_t fallback_pc = 0;

    // Interpreta a instrucao CPS (habilita/desabilita IRQ, FIQ, abort e troca
    // de modo). O Dynarmic decodifica CPS mas delega ao interpretador, que este
    // projeto nao acopla -- entao ela chegava aqui como fallback e parava o
    // Core0 no boot do kernel (pc=0xf0003adc, opcode=0xf10800c0 = "cpsie if").
    // Executar a semantica sobre o CPSR e avancar o PC deixa o boot seguir.
    //
    // Encoding A1: 1111 0001 0000 imod M 0 0000 000 A I F 0 mode
    bool try_execute_cps(std::uint32_t pc) {
        const auto fetched = MemoryReadCode(pc);
        if (!fetched) return false;
        const std::uint32_t insn = *fetched;
        if ((insn & 0xFFF1FE20u) != 0xF1000000u) return false;

        const std::uint32_t imod = (insn >> 18) & 0x3u;
        const bool change_mode = ((insn >> 17) & 0x1u) != 0;
        const std::uint32_t mode = insn & 0x1Fu;
        const std::uint32_t affected =
            (((insn >> 8) & 1u) ? 0x100u : 0u) |   // A (abort)
            (((insn >> 7) & 1u) ? 0x80u : 0u) |    // I (IRQ)
            (((insn >> 6) & 1u) ? 0x40u : 0u);     // F (FIQ)

        std::uint32_t cpsr = jit->Cpsr();
        if (imod == 0b10) {
            cpsr &= ~affected;          // CPSIE: habilita = limpa a mascara
        } else if (imod == 0b11) {
            cpsr |= affected;           // CPSID: desabilita = seta a mascara
        }
        if (change_mode) {
            cpsr = (cpsr & ~0x1Fu) | mode;
        }
        jit->SetCpsr(cpsr);
        jit->Regs()[15] = pc + 4;
        return true;
    }

    void InterpreterFallback(std::uint32_t pc, std::size_t /*num_instructions*/) override {
        // CPS e a unica instrucao de sistema que o boot precisa e que o
        // Dynarmic nao traduz; tratada aqui, o fluxo continua normalmente.
        if (try_execute_cps(pc)) return;

        // Sem interpretador acoplado, continuar daqui repetiria a mesma
        // instrucao indefinidamente. Registra e para o quantum.
        fallback_pending = true;
        fallback_pc = pc;
        ticks_left = 0;
    }

    void AddTicks(std::uint64_t ticks) override {
        ticks_consumed_total += ticks;
        ticks_left = (ticks > ticks_left) ? 0 : (ticks_left - ticks);
    }

    std::uint64_t GetTicksRemaining() override {
        return ticks_left;
    }
};

DynarmicCore::DynarmicCore(MemoryBridge bridge, Cp15Ids cp15)
    : impl_(std::make_unique<Impl>(bridge, cp15)) {}

DynarmicCore::~DynarmicCore() = default;

DynarmicCore::Fault DynarmicCore::take_fault() {
    Fault f;
    f.raised = impl_->exception_pending;
    f.interpreter_fallback = impl_->fallback_pending;
    f.pc = impl_->exception_pending ? impl_->exception_pc : impl_->fallback_pc;
    f.kind = static_cast<uint32_t>(impl_->exception_kind);
    impl_->exception_pending = false;
    impl_->fallback_pending = false;
    return f;
}

void DynarmicCore::set_regs_zero() {
    impl_->jit->Regs().fill(0);
}

void DynarmicCore::set_cpsr(uint32_t cpsr) {
    impl_->jit->SetCpsr(cpsr);
}

void DynarmicCore::set_pc(uint32_t pc) {
    impl_->jit->Regs()[15] = pc;
}

void DynarmicCore::set_sp(uint32_t sp) {
    impl_->jit->Regs()[13] = sp;
}

void DynarmicCore::set_lr(uint32_t lr) {
    impl_->jit->Regs()[14] = lr;
}

uint32_t DynarmicCore::pc() const {
    return impl_->jit->Regs()[15];
}

uint32_t DynarmicCore::cpsr() const {
    return impl_->jit->Cpsr();
}

uint32_t DynarmicCore::sp() const {
    return impl_->jit->Regs()[13];
}

uint32_t DynarmicCore::lr() const {
    return impl_->jit->Regs()[14];
}

uint32_t DynarmicCore::reg(unsigned i) const {
    if (i < 16) return impl_->jit->Regs()[i];
    return 0;
}

void DynarmicCore::set_reg(unsigned i, uint32_t val) {
    if (i < 16) impl_->jit->Regs()[i] = val;
}

void DynarmicCore::sync_to_unicorn(void* uc_engine_ptr) const {
    if (!uc_engine_ptr) return;
    uc_engine* uc = static_cast<uc_engine*>(uc_engine_ptr);
    static const int reg_ids[16] = {
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
    };
    for (int i = 0; i < 16; ++i) {
        uint32_t val = impl_->jit->Regs()[i];
        uc_reg_write(uc, reg_ids[i], &val);
    }
    uint32_t cpsr_val = impl_->jit->Cpsr();
    uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr_val);
}

void DynarmicCore::sync_from_unicorn(void* uc_engine_ptr) {
    if (!uc_engine_ptr) return;
    uc_engine* uc = static_cast<uc_engine*>(uc_engine_ptr);
    static const int reg_ids[16] = {
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
    };
    for (int i = 0; i < 16; ++i) {
        uint32_t val = 0;
        uc_reg_read(uc, reg_ids[i], &val);
        impl_->jit->Regs()[i] = val;
    }
    uint32_t cpsr_val = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr_val);
    impl_->jit->SetCpsr(cpsr_val);
}

void DynarmicCore::enable_page_table(std::array<std::uint8_t*, 1 << (32 - 12)>* pt) {
    impl_->config.page_table = pt;
    impl_->jit = std::make_unique<Dynarmic::A32::Jit>(impl_->config);
}

void DynarmicCore::invalidate_cache(uint32_t addr, size_t size) {
    impl_->jit->InvalidateCacheRange(addr, size);
}

void DynarmicCore::clear_cache() {
    impl_->jit->ClearCache();
}

uint64_t DynarmicCore::total_ticks() const {
    return impl_->ticks_consumed_total;
}

void DynarmicCore::halt_execution() {
    impl_->jit->HaltExecution();
}

void DynarmicCore::halt_from_hook() {
    impl_->halted_by_hook = true;
}

bool DynarmicCore::step_one_insn() {
    impl_->svc_hit_this_block = false;
    impl_->ticks_left = 1;
    impl_->jit->Step();
    if (impl_->svc_hit_this_block) {
        if (on_svc) {
            on_svc(impl_->last_swi_num);
        }
    }
    // Retornava true incondicionalmente, entao quem chamava em laco nao tinha
    // como perceber uma instrucao invalida e repetia o passo indefinidamente.
    return !(impl_->exception_pending || impl_->fallback_pending);
}

uint64_t DynarmicCore::run(uint64_t max_insns) {
    uint64_t start_ticks = impl_->ticks_consumed_total;
    uint64_t target_ticks = start_ticks + max_insns;
    uint64_t quantum = 64;

    // Com gancho por instrucao ativo nao da para executar blocos inteiros: o
    // callback precisa ver cada PC executado e poder desviar o fluxo.
    if (on_code_exec) {
        while (impl_->ticks_consumed_total < target_ticks) {
            on_code_exec(impl_->jit->Regs()[15]);
            if (impl_->halted_by_hook) { impl_->halted_by_hook = false; break; }
            impl_->svc_hit_this_block = false;
            impl_->ticks_left = 1;
            impl_->jit->Step();
            // Uma instrucao invalida nao consome ticks: sem esta saida o laco
            // repete o mesmo PC para sempre (o que travava o boot em silencio).
            if (impl_->exception_pending || impl_->fallback_pending) break;
            if (impl_->svc_hit_this_block && on_svc) {
                on_svc(impl_->last_swi_num);
                break;
            }
        }
        return impl_->ticks_consumed_total - start_ticks;
    }

    while (impl_->ticks_consumed_total < target_ticks) {
        impl_->svc_hit_this_block = false;
        uint64_t remaining = target_ticks - impl_->ticks_consumed_total;
        impl_->ticks_left = std::min(quantum, remaining);

        impl_->jit->Run();

        // Idem para o caminho de blocos: sem ticks consumidos e sem esta
        // saida, target_ticks nunca e atingido e run() nao retorna.
        if (impl_->exception_pending || impl_->fallback_pending) break;

        if (impl_->svc_hit_this_block) {
            // Se houve SVC, executa o dispatcher registrado passando o número real
            if (on_svc) {
                on_svc(impl_->last_swi_num);
            }
            break;
        }
    }

    return impl_->ticks_consumed_total - start_ticks;
}

} // namespace zeebo::jit
