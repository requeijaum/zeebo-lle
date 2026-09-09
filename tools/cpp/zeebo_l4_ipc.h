#ifndef ZEEBO_L4_IPC_H
#define ZEEBO_L4_IPC_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace zeebo_l4 {

// MsgTag do OKL4 2.1 (reutilizado do QW28)
struct MsgTag {
    uint32_t raw = 0;

    static MsgTag create(uint32_t untyped, uint32_t label, bool sndblock = false, bool rcvblock = false) {
        MsgTag t;
        t.raw = (untyped & 0x3fu) | ((label & 0xffffu) << 16);
        if (sndblock) t.raw |= (1u << 15);
        if (rcvblock) t.raw |= (1u << 14);
        return t;
    }

    uint32_t untyped() const { return raw & 0x3fu; }
    uint32_t label() const { return (raw >> 16) & 0xffffu; }
    bool is_error() const { return (raw & (1u << 15)) != 0; }
    bool is_notify() const { return (raw & (1u << 13)) != 0; }

    static MsgTag error_tag() {
        MsgTag t;
        t.raw = (1u << 15);
        return t;
    }

    static MsgTag nil_tag() {
        return MsgTag{0};
    }
};

// Objeto de Serviço Iguana registrado (para resolução de nomes / buffers)
struct ServiceDescriptor {
    std::string name;
    uint32_t    server_tid = 0;
    uint32_t    interface_id = 0;
    uint32_t    buffer_base = 0;
    uint32_t    buffer_size = 0;
};

// Mensagem IPC estruturada para troca entre threads de servidor
struct IpcMessage {
    uint32_t sender_tid = 0;
    uint32_t target_tid = 0;
    MsgTag   tag;
    std::vector<uint32_t> mr; // Message Registers (MR1..MRn)
};

// Tabela de Nomes e Serviços de Sistema (QW31)
class SystemServiceRegistry {
public:
    bool register_service(const std::string& name, uint32_t tid, uint32_t iface_id, uint32_t buf_base = 0, uint32_t buf_sz = 0) {
        if (name.empty() || tid == 0) return false;
        ServiceDescriptor desc{ name, tid, iface_id, buf_base, buf_sz };
        services_[name] = desc;
        by_tid_[tid] = desc;
        return true;
    }

    const ServiceDescriptor* lookup_by_name(const std::string& name) const {
        auto it = services_.find(name);
        if (it != services_.end()) return &it->second;
        return nullptr;
    }

    const ServiceDescriptor* lookup_by_tid(uint32_t tid) const {
        auto it = by_tid_.find(tid);
        if (it != by_tid_.end()) return &it->second;
        return nullptr;
    }

    size_t count() const { return services_.size(); }

    // QW33: Identifica se a thread informada é um ponto de entrada para o AMSS/BREW
    bool is_amss_thread(uint32_t tid) const {
        const auto* desc = lookup_by_tid(tid);
        return desc && desc->name == "amss";
    }

    uint32_t get_amss_entry() const {
        const auto* desc = lookup_by_name("amss");
        return desc ? desc->buffer_base : 0;
    }

    void clear() {
        services_.clear();
        by_tid_.clear();
    }

private:
    std::unordered_map<std::string, ServiceDescriptor> services_;
    std::unordered_map<uint32_t, ServiceDescriptor>    by_tid_;
};

} // namespace zeebo_l4

#endif // ZEEBO_L4_IPC_H
