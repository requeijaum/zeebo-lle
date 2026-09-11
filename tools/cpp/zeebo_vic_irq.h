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

    // Non-reentrant lifecycle: a delivered line is "in service" until the guest
    // signals EOI. While in service, that line MUST NOT be redelivered — doing
    // so would re-enter the IRQ vector and overwrite the banked LR_irq/SPSR_irq
    // saved for the handler still running, corrupting its return. Modelled as a
    // one-deep in-service latch (single active IRQ), matching how the boot code
    // handles one interrupt at a time before EOI. [model: single in-service slot]
    vic_u32 in_service = 0;      // bit mask of lines currently in service
    int     in_service_line = -1;// the line whose vector was read (for EOI)

    // A device asserts its interrupt line: latch it pending.
    void raise_line(unsigned n) {
        if (n < 32) pending |= (1u << n);
    }
    // Guest enables/disables lines (INTENABLE / INTENCLEAR).
    void enable_line(unsigned n)  { if (n < 32) enable  |=  (1u << n); }
    void disable_line(unsigned n) { if (n < 32) enable  &= ~(1u << n); }
    void enable_mask(vic_u32 m)   { enable |=  m; }
    void disable_mask(vic_u32 m)  { enable &= ~m; }

    // Guest acknowledges / signals End-Of-Interrupt: clear the pending latch.
    void ack_line(unsigned n) { if (n < 32) { pending &= ~(1u << n); in_service &= ~(1u << n); } }
    void ack_mask(vic_u32 m)  { pending &= ~m; in_service &= ~m; }

    // Is any enabled line pending? (the wire into the CPU core)
    bool irq_asserted() const { return (pending & enable) != 0; }

    // Masked pending as the guest reads it at VIC_IRQ_STATUS0.
    vic_u32 masked_pending() const { return pending & enable; }

    // Lowest asserted+enabled line number, or -1 if none.
    int active_line() const {
        vic_u32 a = pending & enable;
        if (!a) return -1;
        for (int i = 0; i < 32; ++i) if (a & (1u << i)) return i;
        return -1;
    }

    // Lowest asserted+enabled line that is NOT already in service, or -1.
    int deliverable_line() const {
        vic_u32 a = (pending & enable) & ~in_service;
        if (!a) return -1;
        for (int i = 0; i < 32; ++i) if (a & (1u << i)) return i;
        return -1;
    }

    // VIC_IRQ_VEC_RD read: take the highest-priority deliverable line into
    // service and return its number. [model] Standard vectored-controller
    // contract; the read acknowledges (moves the line to in-service) but does
    // NOT clear its pending latch — EOI does. Returns ~0u if none.
    vic_u32 vector_and_ack() {
        int l = deliverable_line();
        if (l < 0) return ~0u;
        in_service |= (1u << l);
        in_service_line = l;
        return (vic_u32)l;
    }

    // VIC_IRQ_VEC_RD write / VIC end-of-interrupt: retire the line last taken
    // into service and clear its pending latch. [model] Pairs with
    // vector_and_ack() / with an explicit ack_mask from the handler.
    void eoi_current() {
        if (in_service_line >= 0) {
            vic_u32 b = (1u << in_service_line);
            in_service &= ~b;
            pending    &= ~b;
            in_service_line = -1;
        }
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

// Non-reentrant delivery: deliver the highest-priority pending+enabled line that
// is NOT already in service, taking it into service so it cannot be redelivered
// (and thus cannot overwrite the banked LR_irq/SPSR_irq of a handler still
// running) until the guest signals EOI. Honours the CPSR I mask exactly like
// vic_deliver_irq. Returns true iff an exception was taken.
inline bool vic_deliver_next(uc_engine* uc, VicState& vic, uint32_t vector_base) {
    if (!uc) return false;
    int line = vic.deliverable_line();
    if (line < 0) return false; // nothing new to deliver (or all in service)

    uint32_t cpsr = 0;
    uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
    if (cpsr & CPSR_I_BIT) return false; // IRQs masked -> stays pending

    uint32_t pc = 0;
    uc_reg_read(uc, UC_ARM_REG_PC, &pc);
    uint32_t lr_irq   = pc + 4;
    uint32_t spsr_irq = cpsr;

    uint32_t new_cpsr = (cpsr & ~(CPSR_MODE_MASK | CPSR_T_BIT)) |
                        CPSR_MODE_IRQ | CPSR_I_BIT;
    uc_reg_write(uc, UC_ARM_REG_CPSR, &new_cpsr);
    uc_reg_write(uc, UC_ARM_REG_LR,   &lr_irq);
    uc_reg_write(uc, UC_ARM_REG_SPSR, &spsr_irq);

    uint32_t vec = vector_base + 0x18;
    uc_reg_write(uc, UC_ARM_REG_PC, &vec);

    // Move the line into service so the next deliver_next before EOI is a no-op.
    vic.in_service |= (1u << line);
    vic.in_service_line = line;
    return true;
}

// Member convenience wrapper (declared out-of-class to keep the struct free of
// the Unicorn dependency when built host-only).
inline bool vic_state_deliver_next(VicState& v, uc_engine* uc, uint32_t vb) {
    return vic_deliver_next(uc, v, vb);
}
#endif // ZEEBO_VIC_WITH_UNICORN

} // namespace zeebo
