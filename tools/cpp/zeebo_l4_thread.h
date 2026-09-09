// zeebo_l4_thread.h — QW29: Tabela de Threads L4 e rastreamento de ativação via ExchangeRegisters
// ------------------------------------------------------------------------------------------------
// Clean-room, derivado de especificações públicas L4 e OKL4 2.1.1 (syscalls.h / thread.c).
// Rastreador determinístico de threads criadas e ativadas pelo Iguana BootInfo / microkernel.

#ifndef ZEEBO_L4_THREAD_H
#define ZEEBO_L4_THREAD_H

#include <cstdint>
#include <cstddef>
#include <map>
#include <vector>

namespace zeebo_l4 {

// Constantes de controle de L4_ExchangeRegisters (OKL4 2.1.1 syscalls.h)
constexpr uint32_t EXREGS_CTRL_HALT     = (1u << 0);
constexpr uint32_t EXREGS_CTRL_RECV     = (1u << 1);
constexpr uint32_t EXREGS_CTRL_SEND     = (1u << 2);
constexpr uint32_t EXREGS_CTRL_SP       = (1u << 3);
constexpr uint32_t EXREGS_CTRL_IP       = (1u << 4);
constexpr uint32_t EXREGS_CTRL_FLAGS    = (1u << 5);
constexpr uint32_t EXREGS_CTRL_UHANDLE  = (1u << 6);
constexpr uint32_t EXREGS_CTRL_TLS      = (1u << 7);
constexpr uint32_t EXREGS_CTRL_DELIVER  = (1u << 9);

struct ThreadInfo {
    uint32_t tid = 0;
    uint32_t sp  = 0;
    uint32_t ip  = 0;
    uint32_t flags = 0;
    bool     started = false;
    bool     active  = false;
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
        if (control & EXREGS_CTRL_DELIVER) {
            th.started = true;
            th.active = true;
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
