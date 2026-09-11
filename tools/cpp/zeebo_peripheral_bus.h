#pragma once
// zeebo_peripheral_bus.h — QW99 Part 2 Bug 5: a minimal, guest-visible MMIO
// decoder that wires real guest loads/stores to the VIC and GPT device models,
// plus a deterministic virtual-time source and a non-reentrant ARM IRQ
// lifecycle (pending / enabled / in_service / acknowledge-vector / EOI).
//
// WHY THIS EXISTS
// ---------------
// Before this seam, guest writes to the interrupt controller and timer landed
// in flat Unicorn RAM: gpt.enable()/set_match() and vic.enable_line()/ack were
// never called, so no model state ever changed and no IRQ was ever delivered by
// the actual firmware path. `PeripheralBus` is the decoder the Core0/Core1 MMIO
// hooks call, so a guest-style store to ENABLE/MATCH/INTENABLE/ACK mutates the
// model, and a guest-style load of COUNT/STATUS reads it back — the production
// path a test can exercise, not a lambda copy.
//
// REGISTER MAP — PRIMARY SOURCE, NOT INVENTED
// -------------------------------------------
// The VIC base 0xC0000000 and the register OFFSETS below are transcribed from
// the OpenZeebo zloader headers that cite Qualcomm doc "80-VE113-1 A" for the
// MSM7200/MSM7201A:
//   docs/remote/openzeebo/tools/zloader/include/msm7k/vic.h  (pp 218-228)
//   docs/remote/openzeebo/tools/zloader/include/msm7k/gpt.h  (pp 229-231)
//
// GPT BASE — RESOLVED FROM THE REAL FIRMWARE, NOT THE HEADER
// ---------------------------------------------------------
// The zloader header (older MSM7200) puts the GPT at 0xC0100000, but that
// address COLLIDES with the Zeebo MSM7201A MSM_CSR window (doorbell/ProcComm at
// 0xC0100400). A literal scan of the shipped NAND firmware 1.1.2 (primary
// evidence, not a header) is decisive: the timer bank is referenced at
// 0xC5000000 — 545 refs in APPS + 590 in AMSS (bank at 0xC5000100) — while the
// header's 0xC0100000 appears only 13/5 times. So on this SoC the GPT lives at
// 0xC5000000. Because the base is SoC/firmware-specific but the register layout
// is shared, the base addresses are runtime-configurable members (defaults are
// the header/primary values for the standalone test; zeebo_lle_main overrides
// gpt_base to the firmware-resolved 0xC5000000). Only the low interrupt word
// (lines 0..31, STATUS0/EN0/CLEAR0) and GPT0 are modelled; that is the honest
// scope, labelled where behaviour is [model] beyond the register offset.
//
// The VIC_IRQ_VEC_RD acknowledge-on-read / EOI-on-write behaviour is [model]:
// the offsets 0xF00/0xF20 are primary-source (pending int # / pending vector),
// but their exact ack/EOI side effects on this SoC are not captured in a
// primary source we hold, so we implement the standard vectored-controller
// contract (read the vector = take the interrupt into service; write it = EOI)
// and label it as inferred.

#include <cstdint>

#include "zeebo_vic_irq.h"
#include "zeebo_gpt_timer.h"

#ifdef ZEEBO_VIC_WITH_UNICORN
#include <unicorn/unicorn.h>
#endif

namespace zeebo {

// ---- MSM7201A MMIO map (see file header) ----
// PB_*_BASE below are the DEFAULT/reference bases (header/primary-source) used
// by the standalone decoder test. The production integration overrides the
// per-instance base members (vic_base / gpt_base) so the GPT decodes at the
// firmware-resolved 0xC5000000 instead of the colliding 0xC0100000.
enum : uint32_t {
    PB_VIC_BASE          = 0xC0000000u,
    PB_VIC_SIZE          = 0x00001000u,
    PB_GPT_BASE          = 0xC0100000u, // reference (test) base; firmware = 0xC5000000
    PB_GPT_SIZE          = 0x00000100u,

    // Firmware-resolved GPT base for the shipped MSM7201A NAND 1.1.2 (see header).
    PB_GPT_BASE_FW       = 0xC5000000u,

    // VIC register offsets (vic.h, pp 218-228)
    PB_VIC_IRQ_STATUS0   = 0x0000, // masked pending = pending & enable (read)
    PB_VIC_RAW_STATUS0   = 0x0010, // raw pending latch (read)
    PB_VIC_INT_CLEAR0    = 0x0018, // write mask -> clear pending (ACK/EOI latch)
    PB_VIC_INT_EN0       = 0x0028, // write mask -> set enable bits (INTENABLE)
    PB_VIC_INT_ENCLEAR0  = 0x0040, // write mask -> clear enable bits (INTENCLEAR)
    PB_VIC_IRQ_VEC_RD    = 0x0F00, // read: pending int #, takes it into service [model]
    // EOI: write to VIC_IRQ_VEC_RD ends service of the current line [model]

    // GPT register offsets (gpt.h, pp 229-231)
    PB_GPT_MATCH_VAL     = 0x0000, // write MATCH compare
    PB_GPT_COUNT_VAL     = 0x0004, // read counter (read MUST NOT advance it)
    PB_GPT_ENABLE        = 0x0008, // bit0 = GPT_ENABLE_EN, bit1 = CLR_ON_MATCH_EN
    PB_GPT_CLEAR         = 0x000C, // write -> reset counter, re-arm match
};

enum : uint32_t {
    PB_GPT_ENABLE_EN            = 1u, // gpt.h GPT_ENABLE_EN
    PB_GPT_ENABLE_CLR_ON_MATCH  = 2u, // gpt.h GPT_ENABLE_CLR_ON_MATCH_EN
};

// The GPT0 hardware IRQ line into the VIC. INT_GP_TIMER is line 8 on the model
// bank; the concrete SoC line index is [model] (no primary-source line table
// held), but the routing (timer MATCH -> a fixed VIC line) is structural.
enum : unsigned { PB_INT_GP_TIMER = 8 };

// PeripheralBus: the decoder seam. Owns the VIC + GPT models and a deterministic
// virtual clock. All guest MMIO for those devices flows through mmio_read/write;
// the flat RAM behind these windows is never the source of truth again.
struct PeripheralBus {
    VicState vic;
    GptTimer gpt;

    // Per-instance decode bases. Default to the reference bases; production sets
    // gpt_base = PB_GPT_BASE_FW (0xC5000000) to avoid the MSM_CSR collision.
    uint32_t vic_base = PB_VIC_BASE;
    uint32_t gpt_base = PB_GPT_BASE;

    // --- Deterministic virtual time (independent of dual-core retired counts) ---
    //
    // The old model advanced the timer by `Core0.insns + Core1.insns`, which is
    // non-deterministic: it depends on how the two Unicorn engines happened to
    // interleave and how many instructions each retired per slice (branchy code,
    // NOP-slides, and hook-driven PC edits all perturb it). Here virtual time is
    // a monotonic counter advanced by a FIXED quantum per scheduler slice via
    // tick_slice(), so a given number of slices always yields the same emulated
    // time regardless of what either core actually executed. Tests and the boot
    // path both advance it the same way, so a guest deadline busy-wait always
    // terminates after the same number of slices.
    uint64_t virtual_ticks = 0;
    uint32_t ticks_per_slice = 4096; // model quantum; calibrate against observed polling

    unsigned gpt_line = PB_INT_GP_TIMER;

    // Advance virtual time by exactly one scheduler slice and drive the GPT from
    // that virtual time (NOT from retired instruction counts). If the timer
    // crosses MATCH while enabled, latch its line pending in the VIC. Returns
    // true iff the GPT fired on this slice.
    bool tick_slice() { return tick(ticks_per_slice); }

    // Advance by an explicit virtual quantum (testable seam).
    bool tick(uint32_t vticks) {
        virtual_ticks += vticks;
        if (gpt.advance(vticks)) {
            vic.raise_line(gpt_line);
            return true;
        }
        return false;
    }

    // ---- Guest MMIO decode ----
    // Returns true if `addr` belongs to a modelled device (decode consumed it).
    // On a modelled read, *out receives the model value; unmodelled offsets in a
    // modelled window read back 0 (and are still "consumed" so flat RAM cannot
    // shadow the device).
    bool mmio_write(uint32_t addr, uint32_t val) {
        if (addr >= vic_base && addr < vic_base + PB_VIC_SIZE) {
            switch (addr - vic_base) {
                case PB_VIC_INT_CLEAR0:   vic.ack_mask(val);            return true;
                case PB_VIC_INT_EN0:      vic.enable_mask(val);         return true;
                case PB_VIC_INT_ENCLEAR0: vic.disable_mask(val);        return true;
                case PB_VIC_IRQ_VEC_RD:   vic.eoi_current();            return true;
                default:                  return true; // consumed, no side effect
            }
        }
        if (addr >= gpt_base && addr < gpt_base + PB_GPT_SIZE) {
            switch (addr - gpt_base) {
                case PB_GPT_MATCH_VAL: gpt.set_match(val); return true;
                case PB_GPT_ENABLE:
                    gpt.clr_on_match = (val & PB_GPT_ENABLE_CLR_ON_MATCH) != 0;
                    if (val & PB_GPT_ENABLE_EN) gpt.enable(); else gpt.disable();
                    return true;
                case PB_GPT_CLEAR:     gpt.clear(); return true;
                default:               return true;
            }
        }
        return false;
    }

    bool mmio_read(uint32_t addr, uint32_t* out) {
        if (addr >= vic_base && addr < vic_base + PB_VIC_SIZE) {
            switch (addr - vic_base) {
                case PB_VIC_IRQ_STATUS0: *out = vic.masked_pending(); return true;
                case PB_VIC_RAW_STATUS0: *out = vic.pending;          return true;
                case PB_VIC_IRQ_VEC_RD:  *out = vic.vector_and_ack(); return true;
                default:                 *out = 0;                    return true;
            }
        }
        if (addr >= gpt_base && addr < gpt_base + PB_GPT_SIZE) {
            switch (addr - gpt_base) {
                case PB_GPT_COUNT_VAL: *out = gpt.count; return true; // read: no advance
                case PB_GPT_MATCH_VAL: *out = gpt.match; return true;
                case PB_GPT_ENABLE:    *out = gpt.enabled ? (uint32_t)PB_GPT_ENABLE_EN : 0u; return true;
                default:               *out = 0; return true;
            }
        }
        return false;
    }

#ifdef ZEEBO_VIC_WITH_UNICORN
    // Deliver at most ONE pending, enabled, not-yet-in-service IRQ into `uc`,
    // between emu slices. Non-reentrant: a line already in service will NOT be
    // redelivered (that would overwrite LR_irq/SPSR_irq before the handler runs
    // its return). Returns true iff an exception was taken.
    bool deliver_irq(uc_engine* uc, uint32_t vector_base) {
        return vic_deliver_next(uc, vic, vector_base);
    }
#endif
};

} // namespace zeebo
