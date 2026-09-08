#!/usr/bin/env python3
"""
Zeebo LLE Debug Scripting Engine (Python Client)
Communicates with the emulator ControlServer via TCP JSON-RPC / NDJSON.
Allows setting breakpoints, inspecting/writing memory and registers,
and registering custom script hooks/stubs dynamically without modifying C++ source.
"""

import socket
import json
import time
import subprocess
import sys
import struct
import os

class ZeeboDebugClient:
    def __init__(self, host="127.0.0.1", port=48998):
        self.host = host
        self.port = port
        self.sock = None
        self.sock_timeout = 30.0
        # QW2: deduplicador de trace opcional. Quando anexado, um poke em
        # código executável invalida os PCs afetados (SMC), reemitindo-os.
        self._dedup = None

    def attach_dedup(self, dedup):
        """Anexa um TraceDeduplicator (QW2). poke() passará a invalidar a
        máscara de PCs no range escrito, tornando SMC visível no trace."""
        self._dedup = dedup
        return self

    def observe_pc(self, pc):
        """Atalho: registra um PC no dedup anexado. Retorna True se emitir,
        False se omitido; True (sem dedup) mantém comportamento verboso."""
        if self._dedup is None:
            return True
        return self._dedup.observe(pc)

    def connect(self, timeout=10.0):
        start = time.time()
        while time.time() - start < timeout:
            try:
                self.sock = socket.create_connection((self.host, self.port), timeout=self.sock_timeout)
                return True
            except (ConnectionRefusedError, OSError):
                time.sleep(0.1)
        raise TimeoutError(f"Could not connect to Zeebo LLE ControlServer at {self.host}:{self.port}")

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None

    def rpc(self, cmd_dict):
        if not self.sock:
            raise RuntimeError("Socket not connected")
        payload = (json.dumps(cmd_dict) + "\n").encode("utf-8")
        self.sock.sendall(payload)
        data = b""
        start_t = time.time()
        while b"\n" not in data:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            except socket.timeout:
                if time.time() - start_t > self.sock_timeout:
                    return {"ok": False, "error": "timeout"}
        resp_str = data.decode("utf-8").strip()
        if not resp_str:
            return {"ok": False, "error": "empty_response"}
        return json.loads(resp_str)

    def backtrace(self, core=0):
        return self.rpc({"cmd": "backtrace", "core": core})

    def peek(self, addr, size=4, core=0):
        return self.rpc({"cmd": "peek", "core": core, "addr": addr, "len": size})

    def poke(self, addr, val, size=4, core=0):
        resp = self.rpc({"cmd": "poke", "core": core, "addr": addr, "val": val, "len": size})
        # QW2: poke pode ser SMC. Invalida PCs no range para reemissão no trace.
        if self._dedup is not None:
            self._dedup.invalidate(addr, size=size)
        return resp

    def vram_stat(self):
        return self.rpc({"cmd": "vram_stat"})

    def ping(self):
        return self.rpc({"cmd": "ping"})

    def state(self):
        return self.rpc({"cmd": "state"})

    def pause(self):
        return self.rpc({"cmd": "pause"})

    def cont(self):
        return self.rpc({"cmd": "cont"})

    def step(self, core=0, ticks=1):
        return self.rpc({"cmd": "step", "core": core, "ticks": ticks})

    def reg(self, core=0, n=15):
        res = self.rpc({"cmd": "reg", "core": core, "n": n})
        return res.get("value", 0)

    def setreg(self, core=0, n=15, val=0):
        return self.rpc({"cmd": "setreg", "core": core, "n": n, "value": val})

    def read_mem(self, core=0, addr=0, length=16):
        res = self.rpc({"cmd": "read", "core": core, "addr": addr, "len": length})
        if res.get("ok") and "hex" in res:
            return bytes.fromhex(res["hex"])
        return b""

    def write_mem(self, core=0, addr=0, data_bytes=b""):
        return self.rpc({"cmd": "write", "core": core, "addr": addr, "hex": data_bytes.hex()})

    def bp(self, addr, core=0):
        return self.rpc({"cmd": "bp", "core": core, "addr": addr})

    def bpclear(self, addr, core=0):
        return self.rpc({"cmd": "bpclear", "core": core, "addr": addr})

    def set_hook(self, addr, action="stub_r0_0", core=0):
        """
        Actions:
          'stub_r0_0': emulates 'bl func; bx lr with r0=0'
          'stub_r0_1': emulates 'bl func; bx lr with r0=1'
          'step_pc_4': skips instruction (pc = pc + 4)
          'break': breaks execution (paused = true)
          'clear': removes hook
        """
        return self.rpc({"cmd": "hook", "core": core, "addr": addr, "action": action})

    def clear_hook(self, addr, core=0):
        return self.set_hook(addr, action="clear", core=core)

    def quit(self):
        return self.rpc({"cmd": "quit"})


def demo_script():
    port = 48995
    print(f"[Script] Launching zeebo_lle_main with control port {port}...")
    base_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.abspath(os.path.join(base_dir, "../.."))
    proc = subprocess.Popen(
        [os.path.join(base_dir, "zeebo_lle_main"), "--headless", f"--control-port={port}"],
        cwd=base_dir,
        stdout=None,
        stderr=None,
        text=True
    )
    
    dbg = ZeeboDebugClient(port=port)
    try:
        dbg.connect(timeout=5.0)
        print("[Script] Connected! Ping:", dbg.ping())
        st = dbg.state()
        print(f"[Script] Initial state: cycle={st.get('cycle')} c0_pc=0x{st.get('c0_pc',0):08x}")

        # KIP e UTCB inicial
        initial_utcb = 0xdff00000
        dbg.write_mem(core=0, addr=0xff000ff0, data_bytes=struct.pack("<I", initial_utcb))
        # Inicializa cabeçalho do UTCB e palavras de TCR / MR
        # MR0 (offset 0x40): msg tag (ex: 0x00000000 ou resposta IPC válida)
        dbg.write_mem(core=0, addr=0xdff00000, data_bytes=struct.pack("<I", 0x80000100))
        # No TCR do Iguana: [UTCB+0x40] é MR0 (tag) e [UTCB+0x44] é MR1 (label/opcode)
        # Para que o loop de despacho do servidor Iguana não dê erro:
        dbg.write_mem(core=0, addr=0xdff00040, data_bytes=struct.pack("<4I", 0x00000001, 0x00000016, 0x00000000, 0x00000000))

        # Set hooks dynamically via debug scripting engine
        print("[Script] Configuring dynamic script hooks...")
        # Stubs para serviços pesados que ainda não têm suporte em LLE
        stubs_0 = [
            0xb000d5b4, # mempool_init
            0xb00055dc,
            0xb000b1dc,
            0xb0001e80,
            0xb00056c4,
            0xb0004de4,
            0xb00070c8,
            0xb00017b8, # extensions_init
            0xb000ad3c, # rotina final pós-servidor
        ]
        for s in stubs_0:
            dbg.set_hook(s, action="stub_r0_0")
        
        # Se quisermos que o bi_execute (0xb00001fc) execute nativamente, desativamos o jump direto.
        # Por padrão, deixamos o jump:0xb000345c garantindo que o servidor inicie sem falhas de bi_execute.
        dbg.set_hook(0xb00001fc, action="jump:0xb000345c")

        # Breakpoint at 0xb00033f0 to catch Iguana immediately upon returning from L4_KernelInterface
        dbg.bp(0xb00033f0)
        print("[Script] Breakpoint set at 0xb00033f0. Resuming execution...")
        dbg.cont()

        hit_bp = False
        for i in range(100):
            time.sleep(0.05)
            st = dbg.state()
            pc = dbg.reg(core=0, n=15)
            c1_pc = dbg.reg(core=1, n=15)
            if not st.get("running"):
                print(f"[Script] Hit breakpoint/pause! c0_pc=0x{pc:08x} c1_pc=0x{c1_pc:08x} cycle={st.get('cycle')}")
                hit_bp = True
                break
            if i % 10 == 0:
                print(f"  [Poll {i:02d}] running={st.get('running')} c0_pc=0x{pc:08x} c1_pc=0x{c1_pc:08x} cycle={st.get('cycle')}")

        print(f"[Script] Execution milestone reached: hit_bp={hit_bp}")
        if hit_bp:
            dbg.bpclear(0xb00033f0)
            print("[Script] Stepping through server initialization in 0xb000aa94...")
            for s in range(500):
                dbg.step(core=0, ticks=1)
                pc = dbg.reg(core=0, n=15)
                if (pc >= 0x10137000 and pc < 0xb0000000):
                    print(f"[Script] >>> SUCCESS: Reached BREW space @ 0x{pc:08x}!")
                    break
                if pc == 0xb0003480:
                    print("[Script] Reached Iguana main() epilogue!")
                    break
                if pc == 0xb000c830 or (pc >= 0xb0046000 and s > 60):
                    r0 = dbg.reg(core=0, n=0)
                    r1 = dbg.reg(core=0, n=1)
                    r2 = dbg.reg(core=0, n=2)
                    r3 = dbg.reg(core=0, n=3)
                    sp = dbg.reg(core=0, n=13)
                    mr = dbg.read_mem(core=0, addr=0xdff00040, length=32)
                    mr_words = [hex(w) for w in struct.unpack("<8I", mr)] if len(mr) == 32 else []
                    print(f"  [IPC/Server Loop @ step {s:03d}] PC=0x{pc:08x} r0=0x{r0:08x} r1=0x{r1:08x} r2=0x{r2:08x} r3=0x{r3:08x} MRs={mr_words}")
                    # Despachamos o vetor BREW/AMSS!
                    print("[Script] Iguana server loop stable and waiting for user requests.")
                    print("[Script] Reading memory at 0x10137000...")
                    brew_mem = dbg.read_mem(core=0, addr=0x10137000, length=32)
                    print(f"[Script] Mem @ 0x10137000: {brew_mem.hex() if brew_mem else 'None'}")
                    print("[Script] Activating AMSS / BREW thread vector @ 0x10137000...")
                    res_set = dbg.setreg(core=0, n=15, val=0x10137000)
                    print(f"[Script] setreg result: {res_set}")
                    print(f"[Script] current PC after setreg: 0x{dbg.reg(core=0, n=15):08x}")
                    # Dá passos no espaço BREW até o runtime AEECShell
                    for step_brew in range(25):
                        res_step = dbg.step(core=0, ticks=1)
                        brew_pc = dbg.reg(core=0, n=15)
                        print(f"    [BREW Step {step_brew:02d}] PC=0x{brew_pc:08x}")
                        if brew_pc >= 0x1013a000 and brew_pc < 0xb0000000:
                            print(f"[Script] >>> SUCCESS: Reached AEECShell entry @ 0x{brew_pc:08x}!")
                            break
                    break

        dbg.quit()
    finally:
        dbg.close()
        try:
            proc.wait(timeout=2)
        except Exception:
            proc.terminate()
            proc.wait()

if __name__ == "__main__":
    demo_script()
