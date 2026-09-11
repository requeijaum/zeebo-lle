#!/usr/bin/env python3
import sys, os, time, subprocess, struct, hashlib
sys.path.insert(0, "/home/rafaelfrequiao/projects/zeebo-lle/tools/cpp")
from zeebo_debug_scripting import ZeeboDebugClient

BASE="/home/rafaelfrequiao/projects/zeebo-lle/nand"
port=49209
proc = subprocess.Popen(
    ["./zeebo_lle_main","--headless",f"--control-port={port}","--seconds=600",
     f"{BASE}/1.1.2.bin", f"{BASE}/1.1.2_APPS.bin", f"{BASE}/1.1.2_AMSS.bin"],
    cwd="/tmp/zeebo-qw99-scatter/tools/cpp",
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
dbg=ZeeboDebugClient(port=port)
def snap(tag):
    r=[dbg.reg(core=0,n=i) for i in range(16)]
    print(f"[{tag}] pc=0x{r[15]:08x} r0(src)=0x{r[0]:08x} r1(dst)=0x{r[1]:08x} r2(end)=0x{r[2]:08x} r3(ctl)=0x{r[3]:08x} r4(len)=0x{r[4]:08x} r5=0x{r[5]:08x} sp=0x{r[13]:08x} lr=0x{r[14]:08x}")
    return r
try:
    dbg.connect(timeout=15)
    time.sleep(6)
    dbg.pause()
    print("=== sample A ===")
    a=snap("A")
    dbg.cont(); time.sleep(2); dbg.pause()
    print("=== sample B (2s later) ===")
    b=snap("B")
    dbg.cont(); time.sleep(2); dbg.pause()
    print("=== sample C (4s later) ===")
    c=snap("C")
    # r1 growth per second
    print("r1 delta B-A =0x%08x, C-B=0x%08x"%((b[1]-a[1])&0xffffffff,(c[1]-b[1])&0xffffffff))
    print("r4 (len) A=0x%08x B=0x%08x C=0x%08x"%(a[4],b[4],c[4]))
    # is dst beyond end r2?
    print("r1>r2? A=%s B=%s C=%s"%(a[1]>a[2],b[1]>b[2],c[1]>c[2]))
    # what is at src r0 (few bytes)
    for tag,r in [("A",a),("B",b),("C",c)]:
        try:
            m=dbg.read_mem(core=0,addr=r[0]&~3,length=16)
            print("src[%s]@0x%08x="%(tag,r[0]),m.hex())
        except Exception as e: print("src read err",e)
    dbg.quit()
finally:
    dbg.close()
    try: proc.wait(timeout=3)
    except Exception: proc.terminate()
