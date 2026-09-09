// test_l4_ipc_dispatch.cpp — QW31: TDD para despacho IPC, MsgTag e SystemServiceRegistry
// ----------------------------------------------------------------------------------------
// Validação determinística isolada (sem dependência de NAND).

#include <cassert>
#include <cstdio>
#include "zeebo_l4_ipc.h"

int main() {
    printf("=== Test L4 IPC Dispatch & Service Registry (QW31) ===\n");

    zeebo_l4::SystemServiceRegistry reg;
    assert(reg.count() == 0);

    // Registro inválido (nome vazio ou tid 0)
    assert(!reg.register_service("", 6, 0x1000));
    assert(!reg.register_service("ig_naming", 0, 0x1000));
    assert(reg.count() == 0);

    // Registro do ig_naming (thread 6, VA 0xb0100000)
    bool ok_naming = reg.register_service("ig_naming", 6, 0x01, 0xb0100000, 0x10000);
    assert(ok_naming);
    assert(reg.count() == 1);

    // Registro do quartz_servers (thread 13, VA 0xb0300000)
    bool ok_quartz = reg.register_service("quartz_servers", 13, 0x02, 0xb0300000, 0x20000);
    assert(ok_quartz);
    assert(reg.count() == 2);

    // Registro do AMSS / BREW (thread 23, VA 0x10137000)
    bool ok_amss = reg.register_service("amss", 23, 0x03, 0x10137000, 0x800000);
    assert(ok_amss);
    assert(reg.count() == 3);

    // Lookup por nome
    const auto* s_naming = reg.lookup_by_name("ig_naming");
    assert(s_naming != nullptr);
    assert(s_naming->server_tid == 6);
    assert(s_naming->buffer_base == 0xb0100000);

    const auto* s_quartz = reg.lookup_by_name("quartz_servers");
    assert(s_quartz != nullptr);
    assert(s_quartz->server_tid == 13);
    assert(s_quartz->buffer_base == 0xb0300000);

    const auto* s_amss = reg.lookup_by_name("amss");
    assert(s_amss != nullptr);
    assert(s_amss->server_tid == 23);
    assert(s_amss->buffer_base == 0x10137000);

    // Lookup inexistente
    assert(reg.lookup_by_name("nonexistent") == nullptr);
    assert(reg.lookup_by_tid(999) == nullptr);

    // Lookup por TID
    const auto* by_tid6 = reg.lookup_by_tid(6);
    assert(by_tid6 != nullptr);
    assert(by_tid6->name == "ig_naming");

    const auto* by_tid13 = reg.lookup_by_tid(13);
    assert(by_tid13 != nullptr);
    assert(by_tid13->name == "quartz_servers");

    const auto* by_tid23 = reg.lookup_by_tid(23);
    assert(by_tid23 != nullptr);
    assert(by_tid23->name == "amss");

    // MsgTag integração
    auto tag = zeebo_l4::MsgTag::create(4, 0x16);
    assert(tag.untyped() == 4);
    assert(tag.label() == 0x16);
    assert(!tag.is_error());

    // QW32: Teste de IpcMessage para handshake AMSS
    zeebo_l4::IpcMessage msg;
    msg.sender_tid = 6;  // ig_naming
    msg.target_tid = 23; // amss
    msg.tag = tag;
    msg.mr = {0x00000016, 0x10137000, 0x00000000, 0x00000001};
    assert(msg.sender_tid == 6);
    assert(msg.target_tid == 23);
    assert(msg.mr.size() == 4);
    assert(msg.mr[0] == 0x16);
    assert(msg.mr[1] == 0x10137000);

    // QW33: Validação de identificação da thread e ponto de entrada AMSS/BREW
    assert(reg.is_amss_thread(23));
    assert(!reg.is_amss_thread(6));
    assert(!reg.is_amss_thread(13));
    assert(!reg.is_amss_thread(999));
    assert(reg.get_amss_entry() == 0x10137000);

    printf("=== Test L4 IPC Dispatch & Service Registry: PASS ===\n");
    return 0;
}
