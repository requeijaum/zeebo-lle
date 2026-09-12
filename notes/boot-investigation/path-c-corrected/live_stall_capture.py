#!/usr/bin/env python3
import sys, os, json, time
sys.path.insert(0, "/tmp/zeebo-boot-replay/tools/cpp")
from zeebo_debug_scripting import ZeeboDebugClient
PORT=int(os.environ.get("ZEEBO_PORT","49103"))
OUT=os.path.dirname(os.path.abspath(__file__))
def regs(c):
    names={13:"sp",14:"lr",15:"pc"}
    return {names.get(n,f"r{n}"):c.reg(0,n) for n in range(16)}
def rd(c,a,l):
    try: return c.read_mem(0,a,l).hex()
    except Exception as e: return f"ERR:{e}"
def main():
    c=ZeeboDebugClient(port=PORT); c.connect(timeout=15)
    for _ in range(3):
        try:
            c.pause(); break
        except Exception: time.sleep(0.3)
    st=c.state()
    r=regs(c)
    d={"port":PORT,"live_stall_state":st,"regs_core0":r}
    try: d["backtrace"]=c.backtrace(0)
    except Exception as e: d["backtrace"]=f"ERR:{e}"
    pc=r.get("pc")
    d["mem"]={
        "stall_ip@d708_bytes": rd(c,0xb000d708,16),
        "stall_d6dc_bytes": rd(c,0xb000d6dc,16),
        "minpage_caller_d494": rd(c,0xb000d494,16),
        "pre_kip_c738": rd(c,0xb000c738,16),
        "cached_minpage_b0041284": rd(c,0xb0041284,16),
        "KIP_c8": rd(c,0xf0f000c8,8),
    }
    sp=r.get("sp")
    if isinstance(sp,int) and sp: d["mem"]["saved_stack@sp"]=rd(c,sp,64)
    lr=r.get("lr")
    if isinstance(lr,int) and lr: d["mem"]["code@lr"]=rd(c,lr & ~1,12)
    with open(os.path.join(OUT,"live_stall_capture.json"),"w") as f:
        json.dump(d,f,indent=2,default=str)
    print(json.dumps({"pc":hex(pc) if isinstance(pc,int) else pc,
        "insns":st.get("c0_insns"),"lr":hex(lr) if isinstance(lr,int) else lr},indent=2))
    print("wrote live_stall_capture.json")
if __name__=="__main__": main()
