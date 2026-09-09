#include "zeebo_l4_thread.h"
#include <cassert>
#include <cstdio>

int main() {
    printf("=== Test L4 Thread Table (RED) ===\n");
    zeebo_l4::ThreadTable tt;
    assert(tt.count() == 0);

    // Registra ativação via ExchangeRegisters: dest=0x14 (thread 20), control=0x11e (DELIVER|IP|SP)
    // EXREGS_CONTROL_SP (1<<3 = 8), EXREGS_CONTROL_IP (1<<4 = 16), EXREGS_CONTROL_DELIVER (1<<9 = 512)
    uint32_t dest = 0x14;
    uint32_t control = (1 << 3) | (1 << 4) | (1 << 9);
    uint32_t sp = 0xb01ffff0;
    uint32_t ip = 0xb0100000;
    uint32_t flags = 0;

    bool updated = tt.on_exchange_registers(dest, control, sp, ip, flags);
    assert(updated == true);
    assert(tt.count() == 1);

    const zeebo_l4::ThreadInfo* t = tt.get_thread(dest);
    assert(t != nullptr);
    assert(t->tid == dest);
    assert(t->sp == sp);
    assert(t->ip == ip);
    assert(t->started == true);

    // Segunda chamada sem flags de SP/IP não deve sobrescrever SP/IP
    tt.on_exchange_registers(dest, 0, 0x1234, 0x5678, 0);
    assert(t->sp == sp);
    assert(t->ip == ip);

    // Teste negativo: dest nulo não registra thread
    bool updated_zero = tt.on_exchange_registers(0, control, sp, ip, flags);
    assert(updated_zero == false);
    assert(tt.count() == 1);

    // Teste de seleção de próxima thread (round-robin cooperativo)
    uint32_t t2 = 0x20;
    tt.on_exchange_registers(t2, control, 0xb02ffff0, 0xb0200000, flags);
    assert(tt.count() == 2);
    assert(tt.pick_next_thread(dest) == t2);
    assert(tt.pick_next_thread(t2) == dest);

    // Teste negativo: se t2 for marcada inativa, pick_next_thread deve retornar dest
    zeebo_l4::ThreadInfo* t2_info = tt.get_thread_mut(t2);
    assert(t2_info != nullptr);
    t2_info->active = false;
    assert(tt.pick_next_thread(dest) == dest);
    assert(tt.pick_next_thread(t2) == dest);

    // Restaura e valida wrap-around
    t2_info->active = true;
    assert(tt.pick_next_thread(dest) == t2);

    printf("=== Test L4 Thread Table: PASS ===\n");
    return 0;
}
