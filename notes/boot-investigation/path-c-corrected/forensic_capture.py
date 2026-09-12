#!/usr/bin/env python3
# Cold-boot bp-driven capture. Emulator starts paused at cycle 0.
import sys, os, json, time
sys.path.insert(0, "/tmp/zeebo-boot-replay/tools/cpp")
from zeebo_debug_scripting import ZeeboDebugClient
PORT=int(os.environ.get("ZEEBO_PORT","49103")); OUT=os.path.dirname(os.path.abspath(__file__))
TARGETS=[("pre_kip_hook",0xb000c738),("minpage_caller_lr",0xb000d498),
         ("stall_d6dc",0xb000d6dc),("stall_d708",0xb000d708)]
def regs(c):
    nm={13:"sp",14:"lr",15:"pc"}; return {nm.get(n,f"r{n}"):c.reg(0,n) for n in range(16)}
def rd(c,a,l):
    try: return c.read_mem(0,a,l).hex()
    except Exception as e: return f"ERR:{e}"
def snap(c,lbl):
    r=regs(c); pc=r["pc"]
    d={"label":lbl,"regs":r,"state":c.state()}
    try: d["backtrace"]=c.backtrace(0)
    except Exception as e: d["backtrace"]=f"ERR:{e}"
    m={"ip+0/4/8":rd(c,pc,12) if isinstance(pc,int) else None,
       "d6dc":rd(c,0xb000d6dc,16),"d494":rd(c,0xb000d494,16),
       "c738":rd(c,0xb000c738,16),"b0041284":rd(c,0xb0041284,16),"KIP_c8":rd(c,0xf0f000c8,8)}
    sp=r.get("sp"); lr=r.get("lr")
    if isinstance(sp,int) and sp: m["saved_stack@sp"]=rd(c,sp,64)
    if isinstance(lr,int) and lr: m["code@lr(d498caller)"]=rd(c,lr&~1,12)
    d["mem"]=m; return d
def wait(c,tgt,to=90):
    t0=time.time()
    while time.time()-t0<to:
        st=c.state()
        if st.get("c0_pc")==tgt: return True,st
        if not st.get("running"):
            # hit some bp; check which
            return (st.get("c0_pc")==tgt),st
        time.sleep(0.1)
    return False,c.state()
def main():
    c=ZeeboDebugClient(port=PORT); c.connect(timeout=15)
    res={"port":PORT,"targets":{k:hex(v) for k,v in TARGETS},"captures":{},"initial":c.state()}
    for _,a in TARGETS: c.bp(a,0)
    c.cont()
    seen=set()
    for _ in range(12):
        st=None; t0=time.time(); hitpc=None
        while time.time()-t0<90:
            st=c.state()
            if not st.get("running"):
                hitpc=st.get("c0_pc"); break
            time.sleep(0.1)
        if hitpc is None:
            res["captures"]["_timeout"]={"state":c.state()}; break
        lbl=next((k for k,a in TARGETS if a==hitpc),f"pc_{hex(hitpc)}")
        if lbl in seen or hitpc not in [a for _,a in TARGETS]:
            # unexpected stop (crash/invalid). record and stop
            res["captures"]["_stopped_"+(lbl)]={"snap":snap(c,lbl)}
            break
        seen.add(lbl)
        res["captures"][lbl]={"reached":True,"insns":st.get("c0_insns"),"snap":snap(c,lbl)}
        print(f"REACHED {lbl} pc={hex(hitpc)} insns={st.get('c0_insns')}")
        c.bpclear(hitpc,0); c.cont()
        if len(seen)==len(TARGETS): break
    with open(os.path.join(OUT,"forensic_capture.json"),"w") as f:
        json.dump(res,f,indent=2,default=str)
    print("wrote forensic_capture.json; reached:",sorted(seen))
    try: c.quit()
    except Exception: pass
if __name__=="__main__": main()
