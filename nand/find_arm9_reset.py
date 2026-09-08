#!/usr/bin/env python3
# find_arm9_reset.py
# Localiza o vetor de reset real do ARM9/AMSS (REX RTOS) no 1.1.2_AMSS.bin.
#
# Estrategia:
#   1. Ler os PT_LOAD do super-ELF do AMSS (VA/PA/off/filesz).
#   2. Varrer o codigo procurando o preambulo classico de reset ARM:
#        msr cpsr_c, #0xd3   (SVC mode, IRQ+FIQ off)   -> 0xe321f0d3
#        ldr sp, =<addr>     (init do stack de supervisor/REX)
#        b   <boot/init>     (branch para rex_init)
#   3. Ranquear candidatos: msr d3 seguido de setup de SP e um branch tomado
#      dentro de ~24 instrucoes = forte candidato a reset vector.
#   4. Reportar VA, PA, SP inicial e destino do branch de boot.
#
# Uso: python3 find_arm9_reset.py
import struct, sys

IMG = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin"

try:
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
    from capstone.arm import ARM_OP_REG, ARM_OP_MEM, ARM_OP_IMM, ARM_REG_PC, ARM_REG_SP
    HAVE_CS = True
except ImportError:
    HAVE_CS = False


def load_segments(d):
    e_entry = struct.unpack_from("<I", d, 24)[0]
    phoff = struct.unpack_from("<I", d, 28)[0]
    phent = struct.unpack_from("<H", d, 42)[0]
    phnum = struct.unpack_from("<H", d, 44)[0]
    segs = []
    for i in range(phnum):
        o = phoff + i * phent
        typ = struct.unpack_from("<I", d, o)[0]
        off = struct.unpack_from("<I", d, o + 4)[0]
        va = struct.unpack_from("<I", d, o + 8)[0]
        pa = struct.unpack_from("<I", d, o + 12)[0]
        fs = struct.unpack_from("<I", d, o + 16)[0]
        ms = struct.unpack_from("<I", d, o + 20)[0]
        if typ == 1 and fs:
            segs.append(dict(off=off, va=va, pa=pa, fs=fs, ms=ms))
    return e_entry, segs


def off_to_va(segs, o):
    for s in segs:
        if s["off"] <= o < s["off"] + s["fs"]:
            return s["va"] + (o - s["off"]), s["pa"] + (o - s["off"])
    return None, None


def va_to_off(segs, va):
    for s in segs:
        if s["va"] <= va < s["va"] + s["fs"]:
            return s["off"] + (va - s["va"])
    return None


def find_msr_svc(d):
    """Retorna offsets de 'msr cpsr_<field>, #imm' com imm de modo SVC/IRQ-off.

    msr cpsr_<field>, #imm8 = 1110 00 1 10 0 10 <mask> 1111 <rot4> <imm8>
    Field mask varia: cpsr_c=0xe321f0.., cpsr_fc=0xe329f0.., etc.
    Mascara: cond+opcode fixos (0xe3.0f0..), bits de field-mask livres.
    """
    hits = []
    for o in range(0, len(d) - 4, 4):
        w = struct.unpack_from("<I", d, o)[0]
        # msr immediate: 0b1110_0011_0x10_xxxx_1111_rrrr_iiii_iiii
        # topo fixo 0xe3, byte de destino (SPSR/CPSR + field) em bits 22..16,
        # e o campo 0x_?0f0 identifica msr-imm p/ CPSR.
        if (w & 0xFFF0F000) == 0xe320F000 and (w & 0x00400000) == 0:
            imm = w & 0xFF
            hits.append((o, imm))
    return hits


def analyze(d, segs, o, imm):
    """Disassembla ~24 insns a partir do msr e procura ldr sp + branch tomado."""
    va, pa = off_to_va(segs, o)
    if va is None:
        return None
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = True
    code = d[o:o + 24 * 4]
    sp_init = None
    branch_dst = None
    has_mcr_mmu = False
    n = 0
    disasm_lines = []
    for ins in md.disasm(code, va):
        n += 1
        disasm_lines.append(f"    {ins.address:#010x}: {ins.mnemonic} {ins.op_str}")
        m = ins.mnemonic
        # mcr p15 -> setup de MMU/cache (assinatura forte de reset boot, nao de handler)
        if m.startswith("mcr") and "p15" in ins.op_str:
            has_mcr_mmu = True
        # ldr sp, =literal  (ldr sp, [pc, #imm])
        if m.startswith("ldr") and len(ins.operands) >= 2:
            dst, src = ins.operands[0], ins.operands[1]
            if (dst.type == ARM_OP_REG and dst.reg == ARM_REG_SP and
                    src.type == ARM_OP_MEM and src.mem.base == ARM_REG_PC):
                pc = (ins.address + 8) & 0xFFFFFFFF
                litva = pc + src.mem.disp
                lito = va_to_off(segs, litva)
                if lito is not None:
                    sp_init = struct.unpack_from("<I", d, lito)[0]
        # mov sp, #imm
        if m == "mov" and len(ins.operands) >= 2:
            dst, src = ins.operands[0], ins.operands[1]
            if dst.type == ARM_OP_REG and dst.reg == ARM_REG_SP and src.type == ARM_OP_IMM:
                sp_init = src.imm
        # branch tomado (b / bl) para fora do fluxo linear
        if m in ("b", "bl") and ins.operands and ins.operands[0].type == ARM_OP_IMM:
            branch_dst = ins.operands[0].imm
            break
        if n >= 24:
            break
    return dict(off=o, va=va, pa=pa, imm=imm, sp=sp_init,
                branch=branch_dst, mcr=has_mcr_mmu, disasm=disasm_lines)


def main():
    d = open(IMG, "rb").read()
    e_entry, segs = load_segments(d)
    print(f"[*] {IMG}")
    print(f"[*] e_entry (super-ELF) = {e_entry:#010x}")
    print(f"[*] {len(segs)} PT_LOAD segments\n")

    if not HAVE_CS:
        print("[!] capstone indisponivel — rode com o python do projeto.")
        sys.exit(2)

    cands = find_msr_svc(d)
    print(f"[*] {len(cands)} instrucoes 'msr cpsr_c, #imm' encontradas\n")

    ranked = []
    for o, imm in cands:
        r = analyze(d, segs, o, imm)
        if not r:
            continue
        score = 0
        if imm == 0xd3:
            score += 3
        if r["sp"] is not None:
            score += 3
        if r["branch"] is not None:
            score += 2
        if r["mcr"]:
            score += 5   # mcr p15 = MMU/cache init: assinatura de reset boot
        # e_entry do super-ELF = reset vector canonico
        if r["va"] == e_entry or r["pa"] == e_entry:
            score += 6
        # reset vectors ficam no inicio dos segmentos de codigo baixo (PA baixo)
        if r["pa"] is not None and r["pa"] < 0x00b00000:
            score += 1
        r["score"] = score
        ranked.append(r)

    ranked.sort(key=lambda r: (-r["score"], r["off"]))
    print("=== Candidatos a reset vector (rankeados) ===")
    for r in ranked[:8]:
        print(f"\n[score {r['score']}] off={r['off']:#x} VA={r['va']:#010x} PA={r['pa']:#010x} "
              f"msr#{r['imm']:#x} SP={r['sp'] and hex(r['sp'])} "
              f"branch->{r['branch'] and hex(r['branch'])}")
        for l in r["disasm"][:12]:
            print(l)

    best = ranked[0] if ranked else None
    if best:
        print("\n=== MELHOR CANDIDATO ===")
        print(f"  reset_vector VA = {best['va']:#010x}")
        print(f"  reset_vector PA = {best['pa']:#010x}")
        print(f"  SP inicial      = {best['sp'] and hex(best['sp'])}")
        print(f"  CPSR            = 0x{best['imm']:02X} (SVC, IRQ/FIQ off)")
        print(f"  branch boot     = {best['branch'] and hex(best['branch'])}")


if __name__ == "__main__":
    main()
