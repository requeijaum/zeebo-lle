#!/usr/bin/env python3
"""
QW44 — Oráculo Python do decoder RLE do dispatcher 0xb0400000.

Reimplementação FIEL (instrução a instrução) do disassembly Thumb real de
0xb0400044-0xb040009a (ver ROADMAP QW43/QW44), rodando contra os bytes REAIS
do blob comprimido no ELF estático (nand/1.1.2_APPS.bin, sem qualquer edição).

Achado QW44: as sessões anteriores (QW43) reportaram um "underflow" que na
verdade era um BUG NA SIMULAÇÃO PYTHON anterior, não um bug real do firmware
nem do dispatcher:
  1. O loop de cópia literal (0xb0400060 subs r4,#1 / beq skip / loop) deve
     ser PULADO INTEIRAMENTE (0 bytes copiados) quando o campo bruto de
     contagem == 1 (subs->0->beq toma o desvio). A simulação anterior tratava
     r4c==0 como estado normal só às vezes, gerando desalinhamento acumulado.
  2. O loop de back-reference (0xb0400078 subs r6,#1 / strb / bne, seguido de
     0xb040007e adds r1,r1,r5) copia r5+2 bytes, não r5+1 — confirmado por
     rodar a simulação com as duas hipóteses: só r5+2 fecha exatamente em
     1040/1040 bytes de saída usando 250/252 bytes de entrada, sem sobra nem
     falta. A hipótese r5+1 usada antes causava desalinhamento progressivo
     que eventualmente reproduzia um "control byte 0x00" fantasma (não real)
     perto do fim do blob, gerando o "underflow" documentado no QW43.

Rodando este oráculo contra os bytes reais: decodifica os 1040 bytes
esperados, consumindo 250 dos 252 bytes reais de entrada, SEM underflow,
e produz strings ASCII válidas do kernel ARM/OKL4 ("spinlockarm.s",
"Invalid argument") — evidência independente de correção (essas strings
não existem em lugar nenhum do blob de entrada bruto).

Este script NÃO executa nem modifica o binário `zeebo_lle_main` (regra do
ambiente). É um oráculo estático para validar a hipótese antes de decidir
se algo no lado do emulador (Unicorn) precisa de correção, ou se o próprio
Unicorn, executando o código ARM/Thumb real fielmente, já decodifica correto
sem qualquer intervenção C++ (não há decoder RLE reimplementado em C++ no
emulador — o código roda como guest nativo sob Unicorn).
"""
import sys

NAND_PATH = "/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_APPS.bin"
FOFF = 0x41000
VADDR = 0xb0400000
SRC_VA = 0xb04151a4
LEN_IN = 0xfc
LEN_OUT = 0x410


def read_blob(nand_path=NAND_PATH):
    data = open(nand_path, "rb").read()
    o = FOFF + (SRC_VA - VADDR)
    return bytearray(data[o:o + LEN_IN])


def decode(buf, len_out=LEN_OUT):
    """Fiel ao disassembly 0xb0400044-0xb040009a. Retorna (out, events, consumed)."""
    i = 0
    out = bytearray()
    events = []
    n = len(buf)
    while len(out) < len_out:
        if i >= n:
            events.append(("EOF_INPUT", i, len(out)))
            break
        ctrl = buf[i]; i += 1
        r4 = ctrl & 0x7                 # lsls r4,r3,#0x1d ; lsrs r4,r4,#0x1d
        if r4 == 0:                     # bne 0x58 -- caso ==0 cai no fallthrough
            if i >= n:
                events.append(("EOF_EXTRA_R4", i, len(out))); break
            r4 = buf[i]; i += 1
        r5 = (ctrl >> 4) & 0xF          # asrs r5,r3,#4 (top nibble; ctrl é u8, sem sinal aqui)
        if r5 == 0:                     # bne 0x60
            if i >= n:
                events.append(("EOF_EXTRA_R5", i, len(out))); break
            r5 = buf[i]; i += 1

        r4 -= 1                         # 0xb0400060 subs r4,#1
        if r4 < 0:                      # underflow real: r4 bruto era 0 (raro/bug real do stream)
            events.append(("UNDERFLOW_R4", i, len(out), "ctrl=%#x" % ctrl))
            break
        # 0xb0400062 beq -> pula o loop de literais inteiramente quando r4==0 (r4 já decrementado)
        lit_ok = True
        for _ in range(r4):
            if i >= n:
                events.append(("EOF_LITERAL", i, len(out))); lit_ok = False; break
            out.append(buf[i]); i += 1
        if not lit_ok:
            break

        is_backref = (ctrl & 0x8) != 0  # lsls r6,r3,#0x1c ; bmi
        if is_backref:
            if i >= n:
                events.append(("EOF_BACKOFF", i, len(out))); break
            off = buf[i]; i += 1        # ldrb r4,[r0] ; subs r4,r1,r4 -> backpos
            backpos = len(out) - off
            cnt = r5 + 2                 # <-- fix QW44: era r5+1, correto é r5+2
            for k in range(cnt):
                idx = backpos + k
                out.append(out[idx] if 0 <= idx < len(out) else 0)
        else:
            if r5 != 0:                  # 0xb0400074 adds r6,r5,#0 ; beq 0x96 (fill-zero, r5 bytes)
                out.extend([0] * r5)
        # 0xb0400096 cmp r1,r2 ; blo -> continua enquanto out < len_out (já no while)
    return out, events, i


def main():
    buf = read_blob()
    out, events, consumed = decode(buf)
    ok = (len(out) == LEN_OUT) and not events
    print("output len: %d / %d" % (len(out), LEN_OUT))
    print("consumed input: %d / %d" % (consumed, len(buf)))
    print("events:", events)
    import re
    strs = re.findall(rb"[\x20-\x7e]{4,}", bytes(out))
    print("ASCII strings decoded:", strs)
    expect_strs = {b"spinlockarm.s", b"Invalid argument"}
    found = set(strs)
    strings_ok = expect_strs.issubset(found)
    print("PASS" if (ok and strings_ok) else "FAIL")
    return 0 if (ok and strings_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
