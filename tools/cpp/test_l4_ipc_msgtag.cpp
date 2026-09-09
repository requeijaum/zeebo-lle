// test_l4_ipc_msgtag.cpp — QW28: Validação dos tipos de MsgTag e decodificação do L4_Ipc
// ---------------------------------------------------------------------------------------
// Clean-room, derivado de especificações públicas L4 / Pistachio (pistachio/include/ipc.h).

#include <cstdint>
#include <cassert>
#include <cstdio>

namespace zeebo_l4 {

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

} // namespace zeebo_l4

int main() {
    printf("=== Test L4 MsgTag IPC (RED) ===\n");

    // MsgTag com 2 palavras untyped, label 0x1234
    auto tag = zeebo_l4::MsgTag::create(2, 0x1234);
    assert(tag.untyped() == 2);
    assert(tag.label() == 0x1234);
    assert(!tag.is_error());

    // MsgTag de erro
    auto err = zeebo_l4::MsgTag::error_tag();
    assert(err.is_error());

    // MsgTag nil
    auto nil = zeebo_l4::MsgTag::nil_tag();
    assert(nil.raw == 0);
    assert(nil.untyped() == 0);
    assert(nil.label() == 0);

    printf("=== Test L4 MsgTag IPC: PASS ===\n");
    return 0;
}
