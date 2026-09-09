// test_exregs_activation.cpp — RED->GREEN para a ativação de thread via
// L4_ExchangeRegisters com o control REAL observado no firmware 1.1.2.
//
// Evidência viva (ZEEBO_SYSCALL_HIST=1, boot --boot-appmgr):
//   [EXREGS] dest=0x80008001 control=0x0000011e new_ip=0xb0100000 (ig_naming)
//   [EXREGS] dest=0x8000c001 control=0x0000011e new_ip=0xb0300000 (quartz_servers)
//   [EXREGS] dest=0x80010001 control=0x0000011e new_ip=0x10137000 (AMSS/BREW)
//
// control=0x11e = bits {1,2,3,4,8} = RECV|SEND|SP|IP|HALTFLAG.
// Contra a fonte OKL4 2.1.1-fix7 (pistachio/include/syscalls.h + src/exregs.cc):
//   EXREGS_CONTROL_HALT     (1<<0)
//   EXREGS_CONTROL_SP       (1<<3)
//   EXREGS_CONTROL_IP       (1<<4)
//   EXREGS_CONTROL_HALTFLAG (1<<8)
//   EXREGS_CONTROL_DELIVER  (1<<9)
// exregs.cc:326-338: se HALTFLAG setado e HALT limpo => a thread halted é
// RESUMIDA (start). thread_start cria a thread halted e a resume com esse
// control. O bit DELIVER (1<<9) NUNCA é usado pelo firmware neste ponto, logo
// gate de ativação por DELIVER deixa TODA thread inativa e nenhum handoff
// jamais ocorre (Core0 preso em 0xb000c834).
//
// RED (produção antiga só ativa em DELIVER): control=0x11e => active=false.
// GREEN (fix): control=0x11e (HALTFLAG sem HALT) => active=true.

#include "zeebo_l4_thread.h"
#include <cassert>
#include <cstdio>

int main() {
    printf("=== Test ExchangeRegisters activation (control REAL do firmware) ===\n");

    // Control exato observado no boot real para os 3 servidores iniciais.
    const uint32_t CTRL_FIRMWARE = 0x11e;

    // Sanidade: o control real NÃO contém o bit DELIVER (1<<9).
    assert((CTRL_FIRMWARE & (1u << 9)) == 0);
    // ...mas contém HALTFLAG (1<<8) sem HALT (1<<0) => resume/start.
    assert((CTRL_FIRMWARE & (1u << 8)) != 0);
    assert((CTRL_FIRMWARE & (1u << 0)) == 0);

    zeebo_l4::ThreadTable tt;

    // AMSS/BREW: dest do EXREGS real, new_ip=0x10137000.
    uint32_t dest = 0x80010001;
    uint32_t sp = 0xb0e1ff0c;
    uint32_t ip = 0x10137000;

    bool updated = tt.on_exchange_registers(dest, CTRL_FIRMWARE, sp, ip, 0);
    assert(updated == true);

    const zeebo_l4::ThreadInfo* t = tt.get_thread(dest);
    assert(t != nullptr);
    assert(t->ip == ip);
    assert(t->sp == sp);

    // NÚCLEO DO TESTE: com o control REAL do firmware, a thread deve ficar
    // ATIVA (resume via HALTFLAG). Sem isso, pick_next_thread nunca retorna
    // um alvo e o boot fica preso para sempre.
    assert(t->started == true);
    assert(t->active == true);

    // Uma segunda thread ativada pelo mesmo control permite round-robin real.
    uint32_t dest2 = 0x80008001;
    tt.on_exchange_registers(dest2, CTRL_FIRMWARE, 0xb0147f24, 0xb0100000, 0);
    assert(tt.get_thread(dest2)->active == true);

    tt.set_current_tid(dest2);
    assert(tt.pick_next_thread(dest2) == dest); // handoff possível

    // Negativo: HALT explícito (HALTFLAG|HALT) NÃO deve ativar (para a thread).
    zeebo_l4::ThreadTable tt2;
    uint32_t ctrl_halt = (1u << 8) | (1u << 0); // HALTFLAG|HALT
    tt2.on_exchange_registers(0x1234, ctrl_halt, 0, 0x1000, 0);
    assert(tt2.get_thread(0x1234)->active == false);

    printf("=== Test ExchangeRegisters activation: PASS ===\n");
    return 0;
}
