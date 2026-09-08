#!/usr/bin/env python3
"""RED regression: L4_KernelInterface wrapper saved-frame preservation.

Exercises the ACTUAL, unmodified production handler in zeebo_lle_main via a
real cold boot over the ControlServer debugger. No toy handler.

ABI contract (independent, from refs/okl4-2.1.1-fix7/.../kernelinterface.spp):

    L4_KernelInterface:
        stmfd sp!, {r4-r6, lr}   ; 0xb000c720 push saved frame
        ...
        swi 0x14                 ; handled by emulator at 0xb000c738
        cmp r4,#0 / strne r1,[r4]  ; outputs go to CALLER pointers [r4]/[r5]/[r6]
        ...
        ldmfd sp!, {r4-r6, pc}   ; 0xb000c754 restore r4-r6, return

The kernel returns ApiVersion/ApiFlags/KernelId in r1/r2/r3 and writes them
ONLY through the caller-supplied pointers [r4],[r5],[r6]. The saved-register
slots on the stack (sp+0/4/8 == ip+0/4/8) hold the caller's r4,r5,r6 and MUST
survive untouched so the epilogue pop restores them. In this firmware the
caller's r4 holds the page-cache pointer 0xb0041284, later consumed by
l4e_min_pagesize / largest_aligned_fpage.

PASS  : after the c754 pop, r4 == 0xb0041284 (saved frame preserved).
FAIL  : r4 == 0x0000000c (api_version leaked into the saved slot -> corruption).
"""
import os, sys, subprocess, time, socket, json, signal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "cpp"))
from zeebo_debug_scripting import ZeeboDebugClient  # noqa: E402

NAND = "/home/rafaelfrequiao/projects/zeebo-lle/nand"
EMU  = os.path.join(ROOT, "tools", "cpp", "zeebo_lle_main")
PORT = 49101
EXPECTED_PAGE_CACHE = 0xb0041284
C754 = 0xb000c754   # ldmfd sp!, {r4-r6, pc}

def main():
    args = [EMU, "--control-port=%d" % PORT, "--headless", "--seconds=0",
            os.path.join(NAND, "1.1.2.bin"),
            os.path.join(NAND, "1.1.2_APPS.bin"),
            os.path.join(NAND, "1.1.2_AMSS.bin")]
    print("[boot] " + " ".join(args))
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        dbg = ZeeboDebugClient(port=PORT)
        dbg.connect(timeout=15.0)
        # Verify the live wrapper bytes so we know APPS (not AMSS) is loaded and
        # the KernelInterface wrapper is present at the expected VA.
        b0 = dbg.peek(0xb000c738, 4).get("val", 0) & 0xffffffff
        b4 = dbg.peek(0xb000c73c, 4).get("val", 0) & 0xffffffff
        live = ("%08x%08x" % (b0, b4))
        print("[live] c738 bytes = %s (expect 140000ef000054e3-order)" % live)
        # 0xb000c738 = 'ef000014' (swi), 0xb000c73c = 'e3540000' (cmp r4,#0) little-endian words
        if b0 != 0xef000014:
            print("[ABORT] wrong image at 0xb000c738: 0x%08x (AMSS-as-APPS defect?)" % b0)
            return 3
        dbg.bp(0xb000c720, core=0)   # stmfd sp!, {r4-r6, lr}  (entry)
        dbg.bp(C754, core=0)         # ldmfd sp!, {r4-r6, pc}  (epilogue)
        dbg.cont()
        deadline = time.time() + 60
        entry_r4 = entry_r5 = entry_r6 = None
        r4 = r5 = r6 = None
        pc = 0
        seen_entry = False
        while time.time() < deadline:
            pc = dbg.reg(core=0, n=15) & 0xffffffff
            if pc == 0xb000c720 and not seen_entry:
                entry_r4 = dbg.reg(core=0, n=4) & 0xffffffff
                entry_r5 = dbg.reg(core=0, n=5) & 0xffffffff
                entry_r6 = dbg.reg(core=0, n=6) & 0xffffffff
                seen_entry = True
                print("[entry c720] r4=0x%08x r5=0x%08x r6=0x%08x"
                      % (entry_r4, entry_r5, entry_r6))
                dbg.cont()
                time.sleep(0.05)
                continue
            if pc == C754 and seen_entry:
                # At c754 (pre-pop) the wrapper's working r4-r6 are NOT the saved
                # values (the body did `mov r4,r0`). The saved frame is in memory
                # at [sp]/[sp+4]/[sp+8] and is what ldmfd will restore into r4-r6.
                sp = dbg.reg(core=0, n=13) & 0xffffffff
                r4 = dbg.peek(sp + 0, 4).get("val", 0) & 0xffffffff
                r5 = dbg.peek(sp + 4, 4).get("val", 0) & 0xffffffff
                r6 = dbg.peek(sp + 8, 4).get("val", 0) & 0xffffffff
                break
            time.sleep(0.05)
        if not seen_entry or r4 is None:
            print("[ABORT] did not observe both c720 and c754 (pc=0x%08x)" % pc)
            return 3
        print("[epilogue c754] r4=0x%08x r5=0x%08x r6=0x%08x" % (r4, r5, r6))
        preserved = (r4 == entry_r4 and r5 == entry_r5 and r6 == entry_r6)
        result = {
            "test": "kernelinterface_saved_frame",
            "abi": "ldmfd restores caller r4-r6; outputs go to [r4]/[r5]/[r6] not saved slots",
            "entry":   {"r4": "0x%08x" % entry_r4, "r5": "0x%08x" % entry_r5, "r6": "0x%08x" % entry_r6},
            "epilogue":{"r4": "0x%08x" % r4, "r5": "0x%08x" % r5, "r6": "0x%08x" % r6},
            "live_c738": live,
            "pass": preserved,
        }
        open(os.path.join(HERE, "kernelinterface_stack_result.json"), "w").write(
            json.dumps(result, indent=2) + "\n")
        if preserved:
            print("PASS: saved frame preserved across L4_KernelInterface wrapper")
            return 0
        print("FAIL: saved r4-r6 corrupted by handler (kernel outputs leaked "
              "into saved stack slots ip+0/4/8)")
        return 1
    finally:
        try: proc.send_signal(signal.SIGINT)
        except Exception: pass
        try: proc.wait(timeout=5)
        except Exception: proc.kill()

if __name__ == "__main__":
    sys.exit(main())
