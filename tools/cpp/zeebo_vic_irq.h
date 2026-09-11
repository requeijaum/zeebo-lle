#pragma once
// zeebo_vic_irq.h — Minimal, guest-visible MSM VIC (Vectored Interrupt
// Controller) model + real ARM IRQ exception delivery (bug 5).
//
// Scope (honest): this is NOT a full MSM7201A VIC. It models exactly the
// behaviour a guest can observe and depend on for a single interrupt path:
//   * a per-line PENDING latch (set by a device raising its line),
//   * a per-line ENABLE mask (INTENABLE / INTENCLEAR),
//   * software ACK / End-Of-Interrupt that clears the pending latch,
//   * the derived IRQ assertion = (pending & enable) != 0.
//
// The delivery routine turns an asserted, unmasked line into a *real* ARM IRQ
// exception on a Unicorn engine. It MUST only be called between uc_emu_start
// slices (never from inside a hook) so the engine state is quiescent and safe
// to mutate. It preserves the banked IRQ context exactly as ARMv5/v6 hardware
// does: LR_irq = return address, SPSR_irq = CPSR, CPSR -> IRQ mode with I set,
// PC -> vector_base + 0x18. If the I bit is already set (IRQs masked) delivery
// is refused and the line stays pending, matching hardware.

#include <cstdint>

#ifdef ZEEBO_VIC_WITH_UNICORN
#include <unicorn/unicorn.h>
#endif

namespace zeebo {

using vic_u32 = uint32_t;

// ARM CPSR mode/flag bits we touch.
enum : vic_u32 {
    CPSR_MODE_MASK = 0x1f,
    CPSR_MODE_IRQ  = 0x12,
    CPSR_I_BIT     = 1u << 7, // IRQ disable
    CPSR_T_BIT     = 1u << 5, // Thumb
    CPSR_F_BIT     = 1u << 6, // FIQ disable
};

struct VicState {
    vic_u32 pending = 0; // INTPENDING: lines latched by devices
    vic_u32 enable  = 0; // INTENABLE:  lines allowed to reach the core

    // A device asserts its interrupt line: latch it pending.
    void raise_line(unsigned n) {
        if (n < 32) pending |= (1u << n);
    }
    // Guest enables/disables lines (INTENABLE / INTENCLEAR).
    void enable_line(unsigned n)  { if (n < 32) enable  |=  (1u << n); }
    void disable_line(unsigned n) { if (n < 32) enable  &= ~(1u << n); }

    // Guest acknowledges / signals End-Of-Interrupt: clear the pending latch.
    void ack_line(unsigned n) { if (n < 32) pending &= ~(1u << n); }
    void ack_mask(vic_u32 m)  { pending &= ~m; }

    // Is any enabled line pending? (the wire into the CPU core)
    bool irq_asserted() const { return (pending & enable) != 0; }

    // Lowest asserted+enabled line number, or -1 if none.
    int active_line() const {
        vic_u32 a = pending & enable;
        if (!a) return -1;
        for (int i = 0; i < 32; ++i) if (a & (1u << i)) return i;
        return -1;
    }
};

#ifdef ZEEBO_VIC_WITH_UNICORN
// Deliver a real ARM IRQ exception into `uc`, honouring the CPSR I mask and the
// supplied vector base. Returns true if an exception was actually taken.
//
// Preconditions: call ONLY between uc_emu_start slices. `uc` PC must already
// hold the address of the instruction that would run next (post-slice PC).
//
// vector_base is 0x00000000 for low vectors or 0xffff0000 for high vectors
// (CP15 c1 V bit). The IRQ vector lives at vector_base + 0x18.
inline bool vic_deliver_irq(uc_engine* uc, const VicState& vic,
                            uint32_t vector_base) {
    if (!uc || !vic.irq_asserted()) return false;

    uint32_t cpsr = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    if (cpsr & CPSR_I_BIT) return false; // IRQs masked -> stays pending

    uint32_t pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);

    // ARM IRQ entry: LR_irq = address of interrupted insn + 4. The interrupted
    // instruction is the one at PC (execution was suspended before it), so the
    // return-from-IRQ (SUBS PC, LR, #4) lands back on PC exactly.
    uint32_t lr_irq = pc + 4;
    uint32_t spsr_irq = cpsr;

    // Switch to IRQ mode with IRQs masked and Thumb cleared (ARM state on entry).
    uint32_t new_cpsr = (cpsr & ~(CPSR_MODE_MASK | CPSR_T_BIT)) |
                        CPSR_MODE_IRQ | CPSR_I_BIT;
    uc_reg_write(uc, UC_ARM_REG_CPSR, &new_cpsr);
    // Now that we are in IRQ mode, LR/SPSR refer to the banked IRQ copies.
    uc_reg_write(uc, UC_ARM_REG_LR,      &lr_irq);
    uc_reg_write(uc, UC_ARM_REG_SPSR,    &spsr_irq);

    uint32_t vec = vector_base + 0x18;
    uc_reg_write(uc, UC_ARM_REG_PC, &vec);
    return true;
}
#endif // ZEEBO_VIC_WITH_UNICORN

} // namespace zeebo
