#!/usr/bin/env python3
# Path B campaign: seeded differential + property fuzz. Writes JSON artifacts to path-b/.
import json, os, time, random, hashlib, struct, ctypes
import fuzz_pathb as H

OUT=H.OUT
SEED=0xB007
random.seed(SEED)
firmware_sha=hashlib.sha256(H.D).hexdigest()
# record actual runtime versions
try:
    uc_raw=ctypes.CDLL('libunicorn.so.2').uc_version(None,None)
except Exception:
    import unicorn; maj,mnr=unicorn.uc_version() if hasattr(unicorn,'uc_version') else (0,0); uc_raw=None
env={'firmware_sha256':firmware_sha,'seed':hex(SEED),
     'unicorn_py':__import__('unicorn').__version__,
     'capstone_py':__import__('capstone').__version__,
     'libunicorn_uc_version_raw':hex(uc_raw) if isinstance(uc_raw,int) else None}

t0=time.time()
report={'env':env,'suites':{}}

# ---------- SUITE 1: minpage differential (actual bytes vs CTZ contract) ----------
s1={'name':'minpage_ctz_differential','counterexamples':[],'n':0,'pass':0}
seed_vals=[0x01111006, 0x00000000, 0x00000400, 0xffffffff, 0x3ff, 0x400,
           0x01000000, 0x80000000, 0x00000c00, 0xdeadbeef]
rand_vals=[random.getrandbits(32) for _ in range(200)]
for v in seed_vals+rand_vals:
    s1['n']+=1
    model=H.model_minpage(v)
    if model is None:
        # firmware would infinite-loop (masked==0): contract hazard, skip live (bounded) but record
        act=H.run_minpage(v, max_insns=3000)
        if act[0]=='TIMEOUT':
            s1['pass']+=1  # actual also hangs -> matches "hazard" contract prediction
        else:
            s1['counterexamples'].append({'in':hex(v),'model':None,'actual':act})
        continue
    act=H.run_minpage(v)
    if act[0]=='OK' and act[1]==model:
        s1['pass']+=1
    else:
        s1['counterexamples'].append({'in':hex(v),'model':model,'actual':[act[0],act[1]]})
report['suites']['minpage']=s1
print('S1 minpage n=%d pass=%d cex=%d %.1fs'%(s1['n'],s1['pass'],len(s1['counterexamples']),time.time()-t0),flush=True)

# ---------- SUITE 2: largest_aligned_fpage differential + boundary ----------
s2={'name':'largest_fpage_differential','counterexamples':[],'n':0,'pass':0}
PAGEINFO=0x01111006  # minpage=12
# seeded boundary cases: aligned bases, unaligned, hi<lo, tiny ranges, whole-space, wrap
seed_cases=[
    (0xb0d00000,0xb0d00000,0xb6d00000),  # canonical mempool 96MB
    (0xb0d00000,0xb0d00000,0xb0d00000),  # empty range hi==lo
    (0xb0d00000,0xb6d00000,0xb0d00000),  # hi<lo
    (0xb0d00000,0xb0d00000,0xb0d01000),  # 4K
    (0xb0d00000,0xb0d00000,0xb0d00800),  # < min page (2K) => below minsz
    (0x00000000,0x00000000,0xffffffff),  # whole space
    (0xb0d00001,0xb0d00000,0xb6d00000),  # unaligned addr
    (0x10000000,0x10000000,0x10100000),  # 1MB region
    (0xffff0000,0xffff0000,0xffffffff),  # top-of-space edge
]
rand_cases=[]
for _ in range(400):
    base=random.getrandbits(32)&~0xfff
    lo=base
    hi=(lo+random.choice([0x1000,0x100000,0x1000000,random.getrandbits(24)]))&0xffffffff
    addr=random.choice([lo, lo|random.getrandbits(12), random.getrandbits(32)])
    rand_cases.append((addr&0xffffffff,lo,hi))
for addr,lo,hi in seed_cases+rand_cases:
    s2['n']+=1
    m_fp,m_k=H.model_largest_fpage(addr,lo,hi,12)
    act=H.run_largest_fpage(addr,lo,hi,PAGEINFO,max_insns=40000)
    if act[0]=='OK' and act[1]==(m_fp&0xffffffff):
        s2['pass']+=1
    else:
        s2['counterexamples'].append({'addr':hex(addr),'lo':hex(lo),'hi':hex(hi),
            'model_fpage':hex(m_fp),'model_k':m_k,'actual':[act[0],hex(act[1]) if act[1] is not None else None]})
report['suites']['largest_fpage']=s2
print('S2 largest_fpage n=%d pass=%d cex=%d %.1fs'%(s2['n'],s2['pass'],len(s2['counterexamples']),time.time()-t0),flush=True)

# ---------- SUITE 3: KernelInterface wrapper ABI / saved-register (MODELED hooks) ----------
# Property: after the call, caller-saved r4,r5,r6 sentinels are RESTORED (push/pop contract),
# SP is balanced, and the ONLY memory writes are to the 3 output ptrs (strne guarded by nonzero).
s3={'name':'wrapper_abi_saved_regs_MODELED','counterexamples':[],'n':0,'pass':0,
    'note':'hooks are MODELED, not production integration (Path A owns real integration)'}
OUT4,OUT5,OUT6=0x00205000,0x00205010,0x00205020  # inside DATA_BASE(0x200000)+0x10000
def check_wrapper(mode, out4,out5,out6, sent, rets):
    r=H.run_wrapper(out4,out5,out6, *sent, *rets, mode)
    if 'err' in r: return ('ERR',r)
    ok=True; why=[]
    # SP balanced back to caller frame top
    if r['sp']!=H.STACK_BASE+H.STACK_SIZE-0x200: ok=False; why.append('sp_unbalanced=%x'%r['sp'])
    # callee-saved sentinels restored
    if r['r4']!=sent[0]: ok=False; why.append('r4_clobbered=%x'%r['r4'])
    if r['r5']!=sent[1]: ok=False; why.append('r5_clobbered=%x'%r['r5'])
    if r['r6']!=sent[2]: ok=False; why.append('r6_clobbered=%x'%r['r6'])
    # allowed write set: out ptrs get ret1/2/3 iff ptr!=0
    if out4 and r['w4']!=rets[0]: ok=False; why.append('w4=%x'%r['w4'])
    if out5 and r['w5']!=rets[1]: ok=False; why.append('w5=%x'%r['w5'])
    if out6 and r['w6']!=rets[2]: ok=False; why.append('w6=%x'%r['w6'])
    return (ok,why)
sent=(0x0041284,0xcafe0005,0xcafe0006)  # r4 resembles page-cache ptr 0xb0041284 low bits
rets=(0x11111111,0x22222222,0x33333333)
# 3a: good hook -> must PASS (property holds)
for _ in range(30):
    s3['n']+=1
    ok,why=check_wrapper('good',OUT4,OUT5,OUT6,sent,rets)
    if ok is True: s3['pass']+=1
    else: s3['counterexamples'].append({'mode':'good','why':why})
# 3b: null out-ptr variants (strne must skip write) -> good hook still PASS
for combo in [(0,OUT5,OUT6),(OUT4,0,OUT6),(OUT4,OUT5,0),(0,0,0)]:
    s3['n']+=1
    ok,why=check_wrapper('good',*combo,sent,rets)
    if ok is True: s3['pass']+=1
    else: s3['counterexamples'].append({'mode':'good_null','combo':[hex(x) for x in combo],'why':why})
# 3c: MODELED bad hooks -> demonstrate the primary hypothesis violates the ABI contract
for mode in ('bad_stack','bad_reg'):
    s3['n']+=1
    ok,why=check_wrapper(mode,OUT4,OUT5,OUT6,sent,rets)
    # expected to FAIL the property; record as a hypothesis-confirming counterexample
    s3['counterexamples'].append({'mode':mode+'(MODELED_expected_fail)','property_violated':(ok is not True),'why':why})
report['suites']['wrapper_abi']=s3
print('S3 wrapper n=%d pass=%d recorded=%d %.1fs'%(s3['n'],s3['pass'],len(s3['counterexamples']),time.time()-t0),flush=True)

# ---------- minimize genuine counterexamples (only differential suites S1/S2) ----------
def minimize_fpage(c):
    # shrink range toward smallest still-diverging window
    addr=int(c['addr'],16); lo=int(c['lo'],16); hi=int(c['hi'],16)
    def diverges(a,l,h):
        m_fp,_=H.model_largest_fpage(a,l,h,12)
        act=H.run_largest_fpage(a,l,h,PAGEINFO,max_insns=40000)
        return not(act[0]=='OK' and act[1]==(m_fp&0xffffffff))
    if not diverges(addr,lo,hi): return None
    step=(hi-lo)//2
    while step>=0x1000:
        if diverges(addr,lo,lo+step): hi=lo+step
        step//=2
    return {'addr':hex(addr),'lo':hex(lo),'hi':hex(hi)}
genuine=[]
for c in s2['counterexamples']:
    mn=minimize_fpage(c)
    genuine.append({'orig':c,'minimized':mn})
report['minimized_fpage_counterexamples']=genuine

report['elapsed_s']=round(time.time()-t0,2)
report['totals']={'minpage':[s1['n'],s1['pass'],len(s1['counterexamples'])],
                  'largest_fpage':[s2['n'],s2['pass'],len(s2['counterexamples'])],
                  'wrapper':[s3['n'],s3['pass']]}
open(os.path.join(OUT,'campaign_report.json'),'w').write(json.dumps(report,indent=2))
print('WROTE',os.path.join(OUT,'campaign_report.json'),'elapsed',report['elapsed_s'])
print('GENUINE differential counterexamples: minpage=%d largest_fpage=%d'%(
      len(s1['counterexamples']),len(s2['counterexamples'])))
