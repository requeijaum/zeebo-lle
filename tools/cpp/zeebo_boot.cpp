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
    if(st->last && ad!=st->last+4 && st->xfers<25){
        printf("  xfer 0x%08x -> 0x%08x (insn#%llu)\n",st->last,(u32)ad,(unsigned long long)st->n);
        st->xfers++;
    }
    st->last=(u32)ad;
    if(st->n>=st->budget) uc_emu_stop(uc);
}
static void mmio_hook(uc_engine*uc,uc_mem_type type,uint64_t ad,int sz,int64_t val,void*ud){
    (void)uc;(void)type;(void)sz;(void)ud;
    // log writes to high (unmapped periph) addresses to catch the derail target
    if(type==UC_MEM_WRITE && (ad>=0x80000000|| (ad&0xFF000000)==0xA0000000||(ad&0xFF000000)==0xB0000000||(ad&0xFF000000)==0xC0000000)){
        printf("  MMIO-W 0x%08x = 0x%llx (pc 0x%08x)\n",(u32)ad,(unsigned long long)val,rreg(uc,UC_ARM_REG_PC));
        if((u32)ad>=0xB0000000) uc_emu_stop(uc);
    } else if(type==UC_MEM_READ && (ad>=0x80000000)){
        printf("  MMIO-R 0x%08x (pc 0x%08x)\n",(u32)ad,rreg(uc,UC_ARM_REG_PC));
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
    printf("  !! SVC #0x%02x (%s) lr=%08x ip=%08x r0=%08x r1=%08x r2=%08x [insn#%llu]\n",
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