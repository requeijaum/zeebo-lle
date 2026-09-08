#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xdr_emu.py — Extração EMPÍRICA do layout wire do serializador xdr do AUDMGR
(QDSP5, firmware AMSS 1.1.2) via emulação Unicorn.

ABORDAGEM
=========
Em vez de ler o serializador à mão, nós o EXECUTAMOS sob Unicorn e observamos:
  - a ORDEM em que os campos da struct-fonte são LIDOS (UC_HOOK_MEM_READ), e
  - a ORDEM/TAMANHO/VALOR das escritas no wire (interceptando os helpers
    putlong/putbytes).

Os helpers 0x16e9c710 (XDR_PUTLONG) e 0x16e9b114 (XDR_PUTBYTES) são veneers de
import (ldr pc,[pc,#-4]) que saltam para código do RTOS não presente/mapeável.
Então NÃO os executamos: hookamos o endereço de entrada, capturamos os args e
forçamos um retorno (r0=1 sucesso, pc=lr). Isso nos dá o efeito de encoding sem
precisar do runtime SunRPC real.

Modelo SunRPC/ONCRPC:
  bool_t xdr_routine(XDR *xdrs, void *objp[, enum disc])
  XDR_PUTLONG(xdrs, &longval)   -> emite 4 bytes big-endian no wire
  XDR_PUTBYTES(xdrs, addr, len) -> emite len bytes crus no wire (padded p/ 4)

  putlong helper 0x16e9c710: ARM calling conv r0=xdrs, r1 = *ponteiro* p/ long
  putbytes helper 0x16e9b114: r0=xdrs, r1=addr, r2=len

Layout de memória do sandbox:
  BASE (0x163a8000)  : imagem inteira do AMSS, RX
  SRC  (0x40000000)  : struct-fonte de entrada (sentinelas), RW
  WIRE (0x41000000)  : buffer de saída wire (o que o encoder "emite"), RW
  VT   (0x42000000)  : fake XDR object + fake vtable, RW
  STK  (0x7ff00000)  : pilha, RW
"""
import struct, sys
from unicorn import *
from unicorn.arm_const import *

IMG  = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_AMSS.bin"
BASE = 0x163a8000

# helpers (endereços de entrada dos veneers de import)
PUTLONG_VENEER  = 0x16e9c710   # XDR_PUTLONG(xdrs, &long)  -> 4B big-endian
PUTBYTES_VENEER = 0x16e9b114   # XDR_PUTBYTES(xdrs, addr, len) -> len bytes

SRC  = 0x40000000
WIRE = 0x41000000
VT   = 0x42000000
STK  = 0x7ff00000
STK_SIZE = 0x00100000

# offsets dentro da região VT
FAKE_XDR   = VT + 0x000      # objeto XDR falso
FAKE_VTBL  = VT + 0x100      # vtable falsa (x_ops)
SENTINEL_RET = VT + 0x800    # endereço "trampolim" que hookamos p/ vtable calls

def page_down(x): return x & ~0xFFF
def page_up(x):   return (x + 0xFFF) & ~0xFFF


class XdrEmu:
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.img = open(IMG, "rb").read()
        self.reset_log()
        self._build()

    def reset_log(self):
        self.wire = bytearray()     # bytes emitidos, na ordem
        self.emits = []             # lista de (tipo, valor/len, detalhe)
        self.src_reads = []         # (offset_na_src, tamanho, pc)

    def _build(self):
        mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_ARM)
        self.mu = mu
        # imagem inteira RX
        img_base = page_down(BASE)
        img_len  = page_up(len(self.img) + (BASE - img_base))
        mu.mem_map(img_base, img_len, UC_PROT_READ | UC_PROT_EXEC)
        mu.mem_write(BASE, self.img)
        # regiões RW
        mu.mem_map(SRC,  0x10000, UC_PROT_ALL)
        mu.mem_map(WIRE, 0x10000, UC_PROT_ALL)
        mu.mem_map(VT,   0x10000, UC_PROT_ALL)
        mu.mem_map(STK,  STK_SIZE, UC_PROT_ALL)

        # fake XDR object: [xdrs+8] -> vtable; slots +0x58/+0x60 -> SENTINEL_RET
        mu.mem_write(FAKE_XDR + 0x00, struct.pack("<I", 0))        # x_op = XDR_ENCODE=0
        mu.mem_write(FAKE_XDR + 0x08, struct.pack("<I", FAKE_VTBL))
        # preenche vtable inteira apontando p/ um stub Thumb nosso (STUB) que faz
        # 'movs r0,#1; bx lr' — assim a blx da union flui pelo Unicorn (modo correto).
        # vtable -> SENTINEL_RET (hookado p/ capturar byptr) que também é 'movs r0,#1; bx lr'
        for i in range(0, 0x100, 4):
            mu.mem_write(FAKE_VTBL + i, struct.pack("<I", SENTINEL_RET | 1))
        # STUB genérico: movs r0,#1 ; bx lr  (Thumb) — retorno NATURAL via bx lr
        mu.mem_write(VT + 0xA00, struct.pack("<HH", 0x2001, 0x4770))
        # STUB no SENTINEL_RET: idem (as vtable calls caem aqui; _hk_code captura o arg)
        mu.mem_write(SENTINEL_RET & ~1, struct.pack("<HH", 0x2001, 0x4770))
        # Patch dos veneers de import putlong/putbytes: o word literal (veneer+4) passa
        # a apontar p/ o mesmo STUB (Thumb). blx -> STUB -> retorna r0=1 no modo certo.
        mu.mem_write(PUTLONG_VENEER + 4,  struct.pack("<I", (VT + 0xA00) | 1))
        mu.mem_write(PUTBYTES_VENEER + 4, struct.pack("<I", (VT + 0xA00) | 1))

        # hooks
        mu.hook_add(UC_HOOK_CODE, self._hk_code)
        mu.hook_add(UC_HOOK_MEM_READ, self._hk_read)
        mu.hook_add(UC_HOOK_MEM_UNMAPPED, self._hk_unmapped)

    # ---- hooks ----
    def _hk_read(self, mu, access, address, size, value, user):
        if SRC <= address < SRC + 0x10000:
            pc = mu.reg_read(UC_ARM_REG_PC)
            self.src_reads.append((address - SRC, size, pc))

    def _emit_putlong_byval(self, mu):
        # helper 0x16e9c710 = xdr_put_uint32(xdrs, VALUE). r1 = valor (não ponteiro).
        val = mu.reg_read(UC_ARM_REG_R1) & 0xFFFFFFFF
        self.wire += struct.pack(">I", val)  # XDR = big-endian no wire
        self.emits.append(("put_u32_byval", 4, val, None))
        if self.verbose:
            print(f"    [PUT_U32/val] val=0x{val:08x} -> wire+=BE(4)")

    def _emit_putlong_byptr(self, mu):
        # vtable x_putlong(xdrs, objp). r1 = ponteiro; lê 4 bytes.
        r1 = mu.reg_read(UC_ARM_REG_R1)
        raw = bytes(mu.mem_read(r1, 4))
        val = struct.unpack("<I", raw)[0]
        self.wire += struct.pack(">I", val)
        src_off = (r1 - SRC) if SRC <= r1 < SRC + 0x10000 else None
        self.emits.append(("put_u32_byptr", 4, val, src_off))
        if self.verbose:
            print(f"    [PUT_U32/ptr] *0x{r1:08x} src_off={src_off} val=0x{val:08x} -> wire+=BE(4)")

    def _emit_putbytes(self, mu):
        r1 = mu.reg_read(UC_ARM_REG_R1)  # addr
        r2 = mu.reg_read(UC_ARM_REG_R2)  # len
        raw = bytes(mu.mem_read(r1, r2))
        self.wire += raw  # putbytes: bytes crus, sem swap
        src_off = (r1 - SRC) if SRC <= r1 < SRC + 0x10000 else None
        self.emits.append(("putbytes", r2, raw.hex(), src_off))
        if self.verbose:
            print(f"    [PUTBYTES] addr=0x{r1:08x} src_off={src_off} len={r2} bytes={raw.hex()} -> wire+=raw")

    def _ret(self, mu, ok=1):
        # força retorno bool_t=ok, preservando o estado Thumb do LR
        lr = mu.reg_read(UC_ARM_REG_LR)
        mu.reg_write(UC_ARM_REG_R0, ok)
        # Se LR tem bit0=1 -> destino é Thumb. Unicorn seleciona modo pelo bit0 do PC
        # apenas em bx/blx reais; ao escrever PC diretamente precisamos setar o CPSR-T.
        if lr & 1:
            cpsr = mu.reg_read(UC_ARM_REG_CPSR) | (1 << 5)   # T=1
            mu.reg_write(UC_ARM_REG_CPSR, cpsr)
            mu.reg_write(UC_ARM_REG_PC, lr & ~1)
        else:
            cpsr = mu.reg_read(UC_ARM_REG_CPSR) & ~(1 << 5)  # T=0
            mu.reg_write(UC_ARM_REG_CPSR, cpsr)
            mu.reg_write(UC_ARM_REG_PC, lr & ~3)

    def _hk_code(self, mu, address, size, user):
        # Captura APENAS. O retorno é natural: cada veneer/SENTINEL agora aponta p/ um
        # stub Thumb 'movs r0,#1; bx lr', então o Unicorn volta sozinho no modo certo.
        # (Não reescrevemos PC/CPSR — era isso que causava o desync de thumb-mode.)
        a = address & ~1
        if a == (PUTLONG_VENEER & ~1):
            self._emit_putlong_byval(mu); return
        if a == (PUTBYTES_VENEER & ~1):
            self._emit_putbytes(mu); return
        if a == (SENTINEL_RET & ~1):
            self._emit_putlong_byptr(mu); return

    def _hk_unmapped(self, mu, access, address, size, value, user):
        pc = mu.reg_read(UC_ARM_REG_PC)
        print(f"  !! UNMAPPED access @0x{address:08x} size={size} pc=0x{pc:08x}")
        return False  # aborta

    # ---- runner ----
    def call(self, fn_va, src_bytes, disc=None, extra_r3=None, timeout=5_000_000):
        """Chama fn_va(r0=fake_xdr, r1=SRC[, r2=disc]) com a struct-fonte em SRC."""
        self.reset_log()
        mu = self.mu
        mu.mem_write(SRC, src_bytes.ljust(0x100, b"\x00"))
        # reset fake xdr op
        mu.mem_write(FAKE_XDR + 0x00, struct.pack("<I", 0))
        mu.reg_write(UC_ARM_REG_R0, FAKE_XDR)
        mu.reg_write(UC_ARM_REG_R1, SRC)
        if disc is not None:
            mu.reg_write(UC_ARM_REG_R2, disc)
        if extra_r3 is not None:
            mu.reg_write(UC_ARM_REG_R3, extra_r3)
        sp = STK + STK_SIZE - 0x1000
        mu.reg_write(UC_ARM_REG_SP, sp)
        # LR aponta p/ um endereço de parada conhecido: usamos SENTINEL_RET+0x10 mapeado c/ bx? 
        # Melhor: LR -> um endereço com 'bx lr' loop-guard. Usamos STOP marker.
        STOP = (VT + 0x900)
        mu.mem_write(STOP & ~1, struct.pack("<H", 0x4770))  # bx lr (nunca executa; paramos por hook)
        mu.reg_write(UC_ARM_REG_LR, STOP | 1)
        # hook de parada: quando PC == STOP, paramos a emulação
        self._stop_at = STOP & ~1
        h = mu.hook_add(UC_HOOK_CODE, self._hk_stop)
        thumb = bool(fn_va & 1) or True  # nossas fns são thumb
        start = fn_va | 1
        try:
            mu.emu_start(start, self._stop_at, timeout=timeout)
        except UcError as e:
            print(f"  UcError: {e} (pc=0x{mu.reg_read(UC_ARM_REG_PC):08x})")
        finally:
            mu.hook_del(h)
        return self.result()

    def _hk_stop(self, mu, address, size, user):
        if (address & ~1) == self._stop_at:
            mu.emu_stop()

    def result(self):
        return {
            "wire": bytes(self.wire),
            "emits": list(self.emits),
            "src_reads": list(self.src_reads),
        }


def hexdump(b):
    out = []
    for i in range(0, len(b), 16):
        chunk = b[i:i+16]
        hexs = " ".join(f"{x:02x}" for x in chunk)
        asci = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        out.append(f"  {i:04x}: {hexs:<48} {asci}")
    return "\n".join(out)


def report(title, res):
    print(f"\n===== {title} =====")
    print(f"src_reads (offset, size, pc):")
    for off, sz, pc in res["src_reads"]:
        print(f"    src+0x{off:02x}  size={sz}  pc=0x{pc:08x}")
    print(f"emits (na ordem):")
    for e in res["emits"]:
        print(f"    {e}")
    print(f"wire ({len(res['wire'])} bytes):")
    print(hexdump(res["wire"]))


# leafs conhecidos (dos anchors + desasm)
LEAF_A     = 0x16e425aa
LEAF_B     = 0x16e425d0
LEAF_C     = 0x16e425fa
LEAF_D     = 0x16e42626
UNION      = 0x16e42546
DISPATCH   = 0x17422ae4   # array de fn ptrs (4B); idx = proc


if __name__ == "__main__":
    emu = XdrEmu(verbose=True)

    # Sentinelas distintos e reconhecíveis
    # struct de 32 bytes com marcadores
    src = struct.pack("<8I",
        0xAAAA0001, 0xAAAA0002, 0xBBBB0003, 0xBBBB0004,
        0xCCCC0005, 0xCCCC0006, 0xDDDD0007, 0xDDDD0008)

    report("leafA @0x16e425aa", emu.call(LEAF_A, src))
    report("leafB @0x16e425d0", emu.call(LEAF_B, src))
    report("leafC @0x16e425fa", emu.call(LEAF_C, src))
    report("leafD @0x16e42626", emu.call(LEAF_D, src))
    # union: precisa de disc em r2 (0,1,5,6 são braços válidos)
    for disc in (0, 1, 5, 6):
        report(f"union @0x16e42546 disc={disc}", emu.call(UNION, src, disc=disc))
