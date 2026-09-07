// zeebo_boot.cpp — M3: boot an AMSS/APPS image loaded at its PHYSICAL PAs, run
// from e_entry, capture where control goes (first INTR/syscall / transfers).
// Clean C API (uc_reg_read/uc_reg_write, not Python).
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <unicorn/unicorn.h>
#include "zeebo_devices.h"

using u16=uint16_t;
static std::map<u32,u32> gSec,gCoa;
static int gSvcCount=0;
static u32 g_lastSvcp=0;
static u32 va2pa(u32 va){ auto p=gCoa.find(va>>12); if(p!=gCoa.end()) return p->second+(va&0xFFF);
    auto s=gSec.find(va>>20); if(s!=gSec.end()) return s->second+(va&0xFFFFF); return va; }
static void build_arm11_map(){
    auto S=[](u32 v,u32 p){gSec[v>>20]=(p>>20)*0x100000;};
    S(0xf0000000,0x10000000);S(0xf4000000,0x10000000);S(0xf0100000,0x10100000);
    S(0x10200000,0x10200000);S(0x10300000,0x10300000);S(0x10400000,0x10400000);S(0x10500000,0x10500000);
    S(0x10600000,0x10600000);S(0x10700000,0x10700000);S(0x10a00000,0x10a00000);S(0x11000000,0x11000000);
    auto C=[](u32 v,u32 p){gCoa[v>>12]=p;};
    C(0xb0100000,0x100a3800);C(0xb0400000,0x100afc00);C(0xb0d00000,0x100a3400);C(0xb0e00000,0x100a3c00);
    C(0x10100000,0x100ad400);C(0x11400000,0x100adc00);
}
static u16 rd16(const u8*d,size_t o){return (u16)d[o]|((u16)d[o+1]<<8);}
static u32 rd32(const u8*d,size_t o){return (u32)d[o]|((u32)d[o+1]<<8)|((u32)d[o+2]<<16)|((u32)d[o+3]<<24);}
static void map_all(uc_engine*uc){
    uc_mem_map(uc,0x00000000,0x00800000,UC_PROT_ALL);
    uc_mem_map(uc,0x00a00000,0x00600000,UC_PROT_ALL);
    uc_mem_map(uc,0x01000000,0x01000000,UC_PROT_ALL);
    uc_mem_map(uc,0x00c00000,0x00400000,UC_PROT_ALL);
    uc_mem_map(uc,0xff000000,0x00400000,UC_PROT_ALL);
    for(u32 bx : {0xb0000000u,0xc0000000u,0xa0a00000u,0xaa600000u,0xa9700000u,0xa9400000u,0xa9a00000u,0xa9200000u})
        uc_mem_map(uc,bx,0x10000,UC_PROT_ALL);
}
static u32 rreg(uc_engine*uc,int r){ u32 v=0; uc_reg_read(uc,r,&v); return v; }

struct Ctx{ u64 n=0; u32 last=0; int xfers=0; uc_engine*uc=nullptr; u64 budget=0; };
static void code_hook(uc_engine*uc,uint64_t ad,uint32_t,void*ud){
    auto* st=(Ctx*)ud; st->n++;
    if (ad == 0x171bb7a2) {
        // Overwrite r0 with a heap block (e.g. 0x17700000)
        u32 fake_heap = 0x17700000;
        uc_reg_write(uc, UC_ARM_REG_R0, &fake_heap);
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        return;
    }
    if (ad == 0x17420fb0) {
        // e1013092: swp r3, r2, [r1]
        // In emulated uniprocessor without other hardware, simulate lock acquisition:
        u32 zero = 0;
        uc_mem_write(uc, rreg(uc, UC_ARM_REG_R1), &zero, 4);
    }
    if (ad == 0x17420fb4) {
        static int count = 0;
        if (++count == 1) {
            printf("LOOP AT 0x17420fb4 hit! r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n",
                rreg(uc, UC_ARM_REG_R0), rreg(uc, UC_ARM_REG_R1),
                rreg(uc, UC_ARM_REG_R2), rreg(uc, UC_ARM_REG_R3),
                rreg(uc, UC_ARM_REG_SP), rreg(uc, UC_ARM_REG_LR));
        }
    }
    if (ad == 0x00b346ba) {
        // Satisfy the delay condition immediately
        u32 target = rreg(uc, UC_ARM_REG_R1);
        uc_mem_write(uc, 0xc5000108, &target, 4);
    }
    if (ad == 0x00d10570 || ad == 0x00d1058c) {
        // e7ffa5f5 is the breakpoint / undefined instruction trap
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        u32 sp = rreg(uc, UC_ARM_REG_SP);
        u32 st_dump[4];
        uc_mem_read(uc, sp, st_dump, sizeof(st_dump));
        printf("L4 PANIC / TRAP at 0x%08x! Bypassing... Caller LR = 0x%08x, sp=%08x (sp[0]=%08x sp[1]=%08x)\n", (u32)ad, lr, sp, st_dump[0], st_dump[1]);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        u32 cpsr;
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        return;
    }
    if (ad == 0x00d1054c || ad == 0x00d10550) {
        // Return success/timeout elapsed from the timer wait function
        // r0 = 0 (success)
        u32 zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        uc_reg_write(uc, UC_ARM_REG_PC, &lr);
        u32 cpsr;
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        return;
    }
    // At 0x00b346d8:
    /*
    if (ad == 0x00b346d8) {
        static int b3d8_hits = 0;
        if (b3d8_hits++ < 5) {
            printf("AT 0x00b346d8: r0=%08x r3=%08x r4=%08x lr=%08x sp=%08x\n",
                rreg(uc, UC_ARM_REG_R0), rreg(uc, UC_ARM_REG_R3),
                rreg(uc, UC_ARM_REG_R4), rreg(uc, UC_ARM_REG_LR),
                rreg(uc, UC_ARM_REG_SP));
        }
    }
    */
    // Clean up probes: remove EXITING 0x00b346e4 printf to keep log clean
    /*
    if (ad == 0x00b346e4) {
        ...
    }
    */

    // Clean up delay loop hook at 0x1730f482 to not flood
    if (ad == 0x1730f482) {
        static int f482_hits = 0;
        if (f482_hits++ == 0) {
            printf("EXITING DELAY LOOP at 1730f482 (suppressing further messages)\n");
        }
        u32 sp = rreg(uc, UC_ARM_REG_SP);
        u32 regs[6];
        uc_mem_read(uc, sp, regs, sizeof(regs));
        uc_reg_write(uc, UC_ARM_REG_R3, &regs[0]);
        uc_reg_write(uc, UC_ARM_REG_R4, &regs[1]);
        uc_reg_write(uc, UC_ARM_REG_R5, &regs[2]);
        uc_reg_write(uc, UC_ARM_REG_R6, &regs[3]);
        uc_reg_write(uc, UC_ARM_REG_R7, &regs[4]);
        u32 new_pc = regs[5] & ~1;
        uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
        u32 cpsr = rreg(uc, UC_ARM_REG_CPSR);
        if (regs[5] & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        sp += 24;
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        u32 ret = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &ret);
        return;
    }
    // In delay loop at 0x00b346ba:
    // When called, return 0 in r0 to signify completion/ready
    if (ad == 0x00b346ba || ad == 0x00b346c4) {
        static int b3ba_hits = 0;
        if (b3ba_hits++ == 0) {
            printf("EXITING 0x00b346ba/c4 FUNCTION directly (suppressing further messages)\n");
        }
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        u32 new_pc = lr & ~1;
        uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
        u32 cpsr = rreg(uc, UC_ARM_REG_CPSR);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        u32 zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }

    // Clean up excessive logging at 0x00b346dc
    /*
    if (ad == 0x00b346dc) {
        printf("AT 0x00b346dc: r3=%08x cpsr=%08x\n", rreg(uc, UC_ARM_REG_R3), rreg(uc, UC_ARM_REG_CPSR));
    }
    */
    // Target of BL at 0x16ef0b1c is 0x16ef0a9c:
    // Force return value r0 = 1 when returning from 0x16ef0a9c (or at 0x16ef0b20)
    if (ad == 0x16ef0b20) {
        u32 one = 1;
        uc_reg_write(uc, UC_ARM_REG_R0, &one);
    }
    // Suppress rex_wait after first print
    if (ad == 0x1730f442) {
        static int f_rex_wait = 0;
        u32 mask = rreg(uc, UC_ARM_REG_R0);
        if (f_rex_wait++ == 0) {
            printf("REX_WAIT called at 0x1730f442: mask=%08x lr=%08x (suppressing further logs)\n", mask, rreg(uc, UC_ARM_REG_LR));
        }
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        u32 new_pc = lr & ~1;
        uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
        u32 cpsr = rreg(uc, UC_ARM_REG_CPSR);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        uc_reg_write(uc, UC_ARM_REG_R0, &mask);
        return;
    }
    /*
    // Return 3 from 0x16ef0a82:
    if (ad == 0x16ef0a82) {
        static int f_a82 = 0;
        if (f_a82++ == 0) {
            printf("CALLED 0x16ef0a82: returning status 3!\n");
        }
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        u32 new_pc = lr & ~1;
        uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
        u32 cpsr = rreg(uc, UC_ARM_REG_CPSR);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        u32 three = 3;
        uc_reg_write(uc, UC_ARM_REG_R0, &three);
        return;
    }
    */
    // Clean up flood print at 0x16ef0b2c
    // Let's remove the print at 0x16ef0b2c
    /*
    if (ad >= 0x16ef0b2c && ad <= 0x16ef0b3a) {
        ...
    }
    */
    // Read literal at 0x16ef0a9c: pc=0x16ef0aa0 + 0xdc*4 = 0x16ef0aa0 + 0x370 = 0x16ef0e10!
    // 0x16ef0a9e: ldrb r0, [r0, #1]
    if (ad == 0x16ef0a9c) {
        static int f_a9c = 0;
        if (f_a9c++ == 0) {
            printf("CALL 0x16ef0a9c: returning status 0 (ready/unlocked)!\n");
        }
        u32 lr = rreg(uc, UC_ARM_REG_LR);
        u32 new_pc = lr & ~1;
        uc_reg_write(uc, UC_ARM_REG_PC, &new_pc);
        u32 cpsr = rreg(uc, UC_ARM_REG_CPSR);
        if (lr & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        u32 zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        return;
    }
    // Track ONCRPC dispatch / registered services
    // 0x16ef0e30 is referenced near 0x16ef0bbe as RPC program/version info.
    // Let's hook entries into 0x16ef0b70..0x16ef0bd0 to see RPC transactions
    if (ad == 0x16ef0b70) {
        static int f_rpc_reg = 0;
        if (f_rpc_reg++ < 5) {
            printf("ONCRPC REGISTER / HANDLER AT 0x16ef0b70: r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x\n",
                rreg(uc, UC_ARM_REG_R0), rreg(uc, UC_ARM_REG_R1),
                rreg(uc, UC_ARM_REG_R2), rreg(uc, UC_ARM_REG_R3),
                rreg(uc, UC_ARM_REG_LR));
        }
    }
    // Clean up temporary probes, keep cleanly instrumented
    // Record findings in notes/FINDINGS.md

    st->last=(u32)ad;
    if(st->n>=st->budget) uc_emu_stop(uc);
}
static void mmio_hook(uc_engine*uc,uc_mem_type type,uint64_t ad,int sz,int64_t val,void*ud){
    (void)uc;(void)type;(void)sz;(void)ud;
    // log writes to high (unmapped periph) addresses to catch the derail target
    if(type==UC_MEM_WRITE && (ad>=0x80000000|| (ad&0xFF000000)==0xA0000000||(ad&0xFF000000)==0xB0000000||(ad&0xFF000000)==0xC0000000)){
        if (ad != 0xc500010c) printf("  MMIO-W 0x%08x = 0x%llx (pc 0x%08x)\n",(u32)ad,(unsigned long long)val,rreg(uc,UC_ARM_REG_PC));
        // Allow writes to peripherals without stopping unconditionally
    } else if(type==UC_MEM_READ && (ad>=0x80000000)){
        if (ad == 0xc5000108) {
            // Virtual timer ticker: increment simulated timer count
            static u32 virt_timer = 100000;
            virt_timer += 5000; // increment by 5000 ticks (~5ms equivalent)
            uc_mem_write(uc, 0xc5000108, &virt_timer, 4);
        } else if (ad != 0xff000ff0) {
            printf("  MMIO-R 0x%08x (pc 0x%08x)\n",(u32)ad,rreg(uc,UC_ARM_REG_PC));
        }
    }
}
static void unmap_hook(uc_engine*u,uc_mem_type t,uint64_t ad,int sz,int64_t val,void*){
    printf("  [UNMAPPED] %s 0x%08llx sz=%d val=0x%llx (pc 0x%08x)\n",(t&UC_MEM_WRITE)?"W":(t&UC_MEM_READ)?"R":"FETCH",
           ad,sz,(unsigned long long)val,rreg(u,UC_ARM_REG_PC)); uc_emu_stop(u);
}
static void intr_hook(uc_engine*uc,uint32_t,int,void*ud){
    (void)ud;
    u32 pc=rreg(uc,UC_ARM_REG_PC);
    u8 b[4]; int off=pc-4;
    if(uc_mem_read(uc,off,b,4)!=UC_ERR_OK){ printf("  !! INTR @0x%08x (unreadable svc)\n",off); uc_emu_stop(uc); return; }
    u32 w=rd32(b,0); u32 imm=w&0xFFFFFF;
    static const char* names[]={"ipc","thread_switch","thread_control","?3","exchange_reg","schedule",
        "map_control","space_control","?8","?9","cache","?11","security","lipc","?14","?15"};
    const char* nm = (imm<=0x28 && (imm%4)==0) ? names[imm/4] : "?";
    u32 sp=rreg(uc,UC_ARM_REG_SP);
    u32 ip=rreg(uc,UC_ARM_REG_R12);      // caller SP saved by: mov ip,sp
    u32 lr=rreg(uc,UC_ARM_REG_LR);       // return address
    u32 r0=rreg(uc,UC_ARM_REG_R0), r1=rreg(uc,UC_ARM_REG_R1), r2=rreg(uc,UC_ARM_REG_R2);
    if (imm == 6 || imm == 0x646fe) {
        printf("  L4 SYSCALL 6 (thread_switch / yield / wait) called! lr=%08x\n", lr);
        // Emulate successful return: r0 = 0
        u32 zero = 0;
        uc_reg_write(uc, UC_ARM_REG_R0, &zero);
        // Pop {r4, pc} manually from ip:
        u32 r4 = 0, pc = 0;
        uc_mem_read(uc, ip, &r4, 4);
        uc_mem_read(uc, ip + 4, &pc, 4);
        ip += 8;
        uc_reg_write(uc, UC_ARM_REG_R4, &r4);
        uc_reg_write(uc, UC_ARM_REG_SP, &ip);
        uc_reg_write(uc, UC_ARM_REG_PC, &pc);
        u32 cpsr;
        uc_reg_read(uc, UC_ARM_REG_CPSR, &cpsr);
        if (pc & 1) cpsr |= (1 << 5); else cpsr &= ~(1 << 5);
        uc_reg_write(uc, UC_ARM_REG_CPSR, &cpsr);
        return;
    }
    printf("  !! SVC #0x%x (%s) lr=%08x ip=%08x r0=%08x r1=%08x r2=%08x [insn#%d]\n",
           imm,nm,lr,ip,r0,r1,r2,(unsigned long long)gSvcCount);
    if (++gSvcCount > 10000){ printf("  ... svc budget exceeded\n"); uc_emu_stop(uc); return; }
    // ---- minimal L4e kernel shim: emulate handler returning to caller ----
    // L4e syscall return convention: kernel restores caller SP (IP) and returns
    // to the SVC return address (LR). Write zero outputs to the r0/r1/r2 ptr slots
    // if they point into mapped RAM, then resume at LR with SP=IP.
    // emulate successful return: r0 = 0 (L4_OK-ish), outputs cleared
    u32 ret=0; uc_reg_write(uc,UC_ARM_REG_R0,&ret);
    // write 0 to each non-zero output pointer (results area on caller stack)
    // (only if it looks like a stack/ram pointer)
    for (u32 outp : {r1,r2}){
        if (outp>=0x00800000 && outp<0x18000000){
            u32 z=0; uc_mem_write(uc,outp,&z,4); (void)z;
        }
    }
    // restore stack and return to the instruction right AFTER the svc (thunk continues)
    uc_reg_write(uc,UC_ARM_REG_SP,&ip);
    u32 cpsr_target = (u32)(off+4);
    uc_reg_write(uc,UC_ARM_REG_PC,&cpsr_target);
    g_lastSvcp = off;
    // restore the caller mode from SPSR_svc (kernel convention: movs pc,lr)
    u32 spsr=0; uc_reg_read(uc,UC_ARM_REG_SPSR,&spsr);
    if (spsr) uc_reg_write(uc,UC_ARM_REG_CPSR,&spsr);
}

int main(int argc,char**argv){
    if(argc<2){ printf("usage: %s <elf.bin> [insns]\n",argv[0]); return 2;}
    std::vector<u8> d; NandController::read_file(argv[1],d);
    u32 entry=rd32(d.data(),24), phoff=rd32(d.data(),28);
    u16 phent=rd16(d.data(),42), phnum=rd16(d.data(),44);
    build_arm11_map();
    uc_engine* uc; uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc);
    uc_ctl_set_cpu_model(uc,UC_CPU_ARM_1176);
    map_all(uc);
    // AMSS high vaddrs form one contiguous DRAM window; map as a single region
    uc_mem_map(uc,0x16e00000,0x17a60000-0x16e00000,UC_PROT_ALL);
    // Map handshake / token buffer region (0x20000000 window)
    uc_mem_map(uc,0x20000000,0x1000000,UC_PROT_ALL);
    // Map hardware MMIO window around 0xc5000000
    uc_mem_map(uc,0xc5000000,0x01000000,UC_PROT_ALL);
    u32 t_init = 100000;
    uc_mem_write(uc, 0xc5000108, &t_init, 4);
    for(int i=0;i<phnum;i++){
        size_t o=phoff+i*phent; if(rd32(d.data(),o)!=1) continue;
        u32 pv=rd32(d.data(),o+8), off=rd32(d.data(),o+4);
        u32 fs=rd32(d.data(),o+16), ms=rd32(d.data(),o+20);
        size_t nmem=ms?ms:fs; if(!nmem) continue;
        u32 va=va2pa(pv);
        // map target VA page; tolerate overlap (contiguous LOADs share RAM)
        if (uc_mem_map(uc,va&~0xFFFu,((nmem+0xFFF)&~0xFFFu)+0x1000,UC_PROT_ALL)!=UC_ERR_OK){
            // already mapped (overlapping LOAD); just write
        }
        std::vector<u8> seg(nmem,0);
        if(fs){ size_t cl=std::min((size_t)fs,seg.size()); memcpy(seg.data(),d.data()+off,cl); }
        uc_mem_write(uc,va,seg.data(),seg.size());
    }
    u32 sp=0x00bff000; uc_reg_write(uc,UC_ARM_REG_SP,&sp);
    u64 budget = argc>2? strtoull(argv[2],0,0):500000;
    printf("%s: entry=0x%08x (pa 0x%08x) budget=%llu\n",argv[1],entry,va2pa(entry),(unsigned long long)budget);
    Ctx st; st.uc=uc; st.budget=budget;
    uc_hook hc=0,hI=0;
    uc_hook_add(uc,&hc,UC_HOOK_CODE,(void*)(code_hook),&st,1,0);
    uc_hook_add(uc,&hI,UC_HOOK_INTR,(void*)(intr_hook),nullptr,1,0);
    uc_hook hM=0;
    uc_hook_add(uc,&hM,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE|UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED,
                (void*)(mmio_hook),nullptr,0,~0ULL);
    auto unmap=[](uc_engine*u,uc_mem_type t,uint64_t ad,int sz,int64_t val,void*){
            printf("  [UNMAPPED] %s 0x%08llx sz=%d val=0x%llx (pc 0x%08x)\n",(t&UC_MEM_WRITE)?"W":(t&UC_MEM_READ)?"R":"FETCH",
                   ad,sz,(unsigned long long)val,rreg(u,UC_ARM_REG_PC)); uc_emu_stop(u);
        };
    uc_hook hU=0;
    uc_hook_add(uc,&hU,UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,(void*)(unmap_hook),nullptr,0,~0ULL);
    
    // Fix 0xff000ff0 pointer and structure:
    // 0xff000ff0 points to a control block at 0x177f0000
    // and offset +0xa has a non-zero byte so the check at 0x00d10538 passes
    // In addition, at offset +0x4 and +0x8, provide valid handler/function pointers!
    // Offset +0x8 should point to an array of pointers to valid functions (like a dummy return 0x1730f32e = bx lr)
    u32 ctrl_ptr = 0x177f0000;
    uc_mem_write(uc, 0xff000ff0, &ctrl_ptr, 4);
    u8 flag_byte = 1;
    uc_mem_write(uc, ctrl_ptr + 0xa, &flag_byte, 1);

    u32 table_ptr = 0x177f0100;
    uc_mem_write(uc, ctrl_ptr + 0x4, &table_ptr, 4);
    uc_mem_write(uc, ctrl_ptr + 0x8, &table_ptr, 4);
    // Fill table_ptr with pointers to bx lr (0x1730f32f in Thumb)
    u32 func_ptr = 0x1730f32f; // Thumb mode bx lr
    for (int i = 0; i < 32; i++) {
        uc_mem_write(uc, table_ptr + i*8, &func_ptr, 4);
        uc_mem_write(uc, table_ptr + i*8 + 4, &func_ptr, 4);
    }

    uc_err er=uc_emu_start(uc,entry,0,0,0);
    printf("stopped pc=0x%08x insn#%llu (err=%s)\n",rreg(uc,UC_ARM_REG_PC),(unsigned long long)st.n,
           er?uc_strerror(er):"ok");
    printf("  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x sp=%08x lr=%08x\n",
           rreg(uc,UC_ARM_REG_R0),rreg(uc,UC_ARM_REG_R1),rreg(uc,UC_ARM_REG_R2),rreg(uc,UC_ARM_REG_R3),
           rreg(uc,UC_ARM_REG_R4),rreg(uc,UC_ARM_REG_R5),rreg(uc,UC_ARM_REG_SP),rreg(uc,UC_ARM_REG_LR));
    u32 cpsr2=0; uc_reg_read(uc,UC_ARM_REG_CPSR,&cpsr2);
    printf("  cpsr=%08x (T=%d mode=%d)\n",cpsr2,(cpsr2>>5)&1,cpsr2&0x1F);
    u32 r6=0,r7=0,r8=0,r9=0,r10=0,r11=0; uc_reg_read(uc,UC_ARM_REG_R6,&r6);uc_reg_read(uc,UC_ARM_REG_R7,&r7);
    uc_reg_read(uc,UC_ARM_REG_R8,&r8);uc_reg_read(uc,UC_ARM_REG_R9,&r9);uc_reg_read(uc,UC_ARM_REG_R10,&r10);uc_reg_read(uc,UC_ARM_REG_R11,&r11);
    printf("  r6=%08x r7=%08x r8=%08x fp=%08x r10=%08x r11=%08x\n",r6,r7,r8,r9,r10,r11);
    uc_close(uc);
    return 0;
}