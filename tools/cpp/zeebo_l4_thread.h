// zeebo_l4_thread.h — QW29: Tabela de Threads L4 e rastreamento de ativação via ExchangeRegisters
// ------------------------------------------------------------------------------------------------
// Clean-room, derivado de especificações públicas L4 e OKL4 2.1.1 (syscalls.h / thread.c).
// Rastreador determinístico de threads criadas e ativadas pelo Iguana BootInfo / microkernel.

#ifndef ZEEBO_L4_THREAD_H
#define ZEEBO_L4_THREAD_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <map>
#include <vector>

namespace zeebo_l4 {

// Contexto de CPU ARM completo de uma thread (Bug 2). Uma troca cooperativa de
// thread deve salvar/restaurar TODOS os registradores banked-user (r0..r12),
// sp, lr, pc e o CPSR (que carrega o bit T = modo Thumb). Guardar só sp/ip/flags
// perdia r0..r12 e o modo de instrução da thread suspensa.
struct CpuContext {
    uint32_t r[13] = {0}; // r0..r12
    uint32_t sp    = 0;   // r13
    uint32_t lr    = 0;   // r14
    uint32_t pc    = 0;   // r15
    uint32_t cpsr  = 0;   // inclui bit T (5) = Thumb, e flags de condição
};

// Constantes de controle de L4_ExchangeRegisters (OKL4 2.1.1 syscalls.h)
constexpr uint32_t EXREGS_CTRL_HALT     = (1u << 0);
constexpr uint32_t EXREGS_CTRL_RECV     = (1u << 1);
constexpr uint32_t EXREGS_CTRL_SEND     = (1u << 2);
constexpr uint32_t EXREGS_CTRL_SP       = (1u << 3);
constexpr uint32_t EXREGS_CTRL_IP       = (1u << 4);
constexpr uint32_t EXREGS_CTRL_FLAGS    = (1u << 5);
constexpr uint32_t EXREGS_CTRL_UHANDLE  = (1u << 6);
constexpr uint32_t EXREGS_CTRL_TLS      = (1u << 7);
// Bits de halt/resume da fonte OKL4 2.1.1-fix7 (pistachio/include/syscalls.h
// + src/exregs.cc): HALTFLAG (1<<8) = "aplicar bit HALT"; se HALTFLAG setado e
// HALT (1<<0) limpo, a thread halted é RESUMIDA (start). thread_start usa
// exatamente isso (control observado no firmware = 0x11e). DELIVER (1<<9) é o
// caminho alternativo de entrega direta; o firmware do Zeebo não o usa aqui.
constexpr uint32_t EXREGS_CTRL_HALTFLAG = (1u << 8);
constexpr uint32_t EXREGS_CTRL_DELIVER  = (1u << 9);

struct ThreadInfo {
    uint32_t tid = 0;
    uint32_t sp  = 0;
    uint32_t ip  = 0;
    uint32_t flags = 0;
    bool     started = false;
    bool     active  = false;
    // Bug 1/2: cada thread pertence a um space_id (L4_SpaceId_t). O SID
    // determina QUAL conjunto de traduções VA->host (VTLB por espaço) vale
    // enquanto a thread executa. 0 = ainda não atribuído.
    uint32_t space_id = 0;
    // Bug 2: contexto de CPU completo salvo na última suspensão cooperativa.
    CpuContext ctx{};
    bool       has_context = false;
};

class ThreadTable {
public:
    ThreadTable() = default;

    void set_current_tid(uint32_t tid) {
        current_tid_ = tid;
    }

    uint32_t current_tid() const {
        return current_tid_;
    }

    // Bug 1/2: associa uma thread ao seu space_id (L4_SpaceId_t). Cria a entrada
    // se ainda não existir (o kernel pode fixar o SID de uma thread antes de
    // ativá-la via ExchangeRegisters).
    void set_thread_space(uint32_t tid, uint32_t space_id) {
        if (tid == 0) return;
        auto& th = threads_[tid];
        th.tid = tid;
        th.space_id = space_id;
    }

    uint32_t thread_space(uint32_t tid) const {
        auto it = threads_.find(tid);
        return it != threads_.end() ? it->second.space_id : 0;
    }

    // Bug 2: salva o contexto de CPU completo de uma thread na suspensão.
    void save_context(uint32_t tid, const CpuContext& ctx) {
        if (tid == 0) return;
        auto& th = threads_[tid];
        th.tid = tid;
        th.ctx = ctx;
        th.has_context = true;
        // Mantém sp/ip espelhados para compatibilidade com callers antigos.
        th.sp = ctx.sp;
        th.ip = ctx.pc;
    }

    // Bug 2: restaura o contexto salvo. Retorna false se a thread nunca foi
    // suspensa (sem contexto para restaurar) — o caller deve então usar o
    // ip/sp inicial do ExchangeRegisters em vez de um contexto lixo.
    bool load_context(uint32_t tid, CpuContext* out) const {
        auto it = threads_.find(tid);
        if (it == threads_.end() || !it->second.has_context) return false;
        if (out) *out = it->second.ctx;
        return true;
    }

    bool on_exchange_registers(uint32_t dest, uint32_t control, uint32_t new_sp, uint32_t new_ip, uint32_t flags) {
        if (dest == 0) return false;

        auto& th = threads_[dest];
        th.tid = dest;

        if (control & EXREGS_CTRL_SP) {
            th.sp = new_sp;
        }
        if (control & EXREGS_CTRL_IP) {
            th.ip = new_ip;
        }
        if (control & EXREGS_CTRL_FLAGS) {
            th.flags = flags;
        }
        // Ativação real: o kernel OKL4 (exregs.cc:326-338) trata "resume" quando
        // HALTFLAG está setado e HALT limpo — é assim que thread_start inicia os
        // servidores iniciais (control=0x11e no firmware). DELIVER é o caminho
        // legado de entrega direta. Ambos ativam a thread.
        bool resume = (control & EXREGS_CTRL_HALTFLAG) && !(control & EXREGS_CTRL_HALT);
        bool halt   = (control & EXREGS_CTRL_HALTFLAG) &&  (control & EXREGS_CTRL_HALT);
        if ((control & EXREGS_CTRL_DELIVER) || resume) {
            th.started = true;
            th.active = true;
        }
        if (halt) {
            th.active = false;
        }
        return true;
    }

    const ThreadInfo* get_thread(uint32_t tid) const {
        auto it = threads_.find(tid);
        if (it != threads_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    ThreadInfo* get_thread_mut(uint32_t tid) {
        auto it = threads_.find(tid);
        if (it != threads_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    uint32_t pick_next_thread(uint32_t current) {
        if (threads_.empty()) return 0;
        // Procura próxima thread ativa em round-robin
        auto it = threads_.upper_bound(current);
        for (auto cur = it; cur != threads_.end(); ++cur) {
            if (cur->second.active && cur->first != current) return cur->first;
        }
        for (auto cur = threads_.begin(); cur != it; ++cur) {
            if (cur->second.active && cur->first != current) return cur->first;
        }
        // Se nenhuma outra ativa, mantém a atual se ativa
        auto self = threads_.find(current);
        if (self != threads_.end() && self->second.active) return current;
        return 0;
    }

    size_t count() const {
        return threads_.size();
    }

    void clear() {
        threads_.clear();
        current_tid_ = 0;
    }

    std::vector<uint32_t> get_active_threads() const {
        std::vector<uint32_t> list;
        for (const auto& kv : threads_) {
            if (kv.second.active) list.push_back(kv.first);
        }
        return list;
    }

private:
    uint32_t current_tid_ = 0;
    std::map<uint32_t, ThreadInfo> threads_;
};

} // namespace zeebo_l4

#endif // ZEEBO_L4_THREAD_H
