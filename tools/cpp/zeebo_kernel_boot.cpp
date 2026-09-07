// zeebo_kernel_boot.cpp — boot the compiled OKL4 L4e ARM kernel (refs/okl4-arm-build/
// arm-kernel.elf) under unicorn. Loads ELF LOADs at their VA (kernel is linked at
// 0xf0000000 virtual, 0xa0100000 phys — pleb2 map), maps phys RAM + IO areas,
// runs from _start, logs MMIO (kernel console) + control transfers + SVC.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <unicorn/unicorn.h>

using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;
static std::map<u32,u16> g_sticky;
static u64 g_insn=0, g_budget=0; static u32 g_last=0; static int g_log=1;

static u16 rd16(const u8*d,size_t o){return (u16)d[o]|((u16)d[o+1]<<8);}
static u32 rd32(const u8*d,size_t o){return (u32)d[o]|((u32)d[o+1]<<8)|((u32)d[o+2]<<16)|((u32)d[o+3]<<24);}
static u32 rreg(uc_engine*uc,int r){ u32 v=0; uc_reg_read(uc,r,&v); return v; }
static void wreg(uc_engine*uc,int r,u32 v){ uc_reg_write(uc,r,&v); }

// pleb2 phys addresses (kernel's XSCALE_DEV_PHYS = 0x40000000)
static const u32 PLEB_RAM    = 0xa0000000; // RAM_START..0xa2000000
static const u32 XSCALE_DEV  = 0x40000000; // CONSOLE 0x100000, INTERRUPT 0xd00000, TIMER 0xa00000
static const u32 CONSOLE_P   = XSCALE_DEV+0x100000;   // uart/console phys
static const u32 INTR_P      = XSCALE_DEV+0xd00000;
static const u32 TIMER_P     = XSCALE_DEV+0xa00000;

static bool saw_idle = false;

static void on_mem(uc_engine*uc,uc_mem_type type,uint64_t ad,int sz,int64_t val,void*ud){
    (void)ud;
    // MMIO read hook
    if (type == UC_MEM_READ && ad >= XSCALE_DEV && ad < XSCALE_DEV + 0x1000000) {
        // Return 0 for read into destination register
        u32 v = 0;
        u32 pc = rreg(uc, UC_ARM_REG_PC);
        int reg = (pc == 0xf001c168) ? UC_ARM_REG_R3 : ((pc == 0xf001c178) ? UC_ARM_REG_R2 : UC_ARM_REG_R2);
        uc_reg_write(uc, reg, &v);
    }
    // pleb2 console: the kernel's early out_char writes to CONSOLE_P+... 
    // Model console TX: any write to [CONSOLE_P, CONSOLE_P+0x100) -> print low byte
    if ((type&UC_MEM_WRITE) && ad>=CONSOLE_P && ad<CONSOLE_P+0x200){
        unsigned char c=(unsigned char)val;
        putchar(c); fflush(stdout);
        return;
    }
    // Only log if it's outside RAM and outside kernel ELF LOAD
    if (ad >= 0xa0000000 && ad < 0xa2000000) return;
    if (ad >= 0xf0000000 && ad < 0xf0020000) return;
    if (g_log) printf("    [MMIO] %s 0x%08llx val=0x%llx (pc%08x)\n",
        (type&UC_MEM_WRITE)?"W":"R",ad,(unsigned long long)val,rreg(uc,UC_ARM_REG_PC));
}
static void on_code(uc_engine*uc,uint64_t ad,uint32_t,void*ud){
    (void)ud; g_insn++;
    if (ad == 0xf001cb98) {
        // Just before MMU enable, populate L1 page table for KTCB at 0xe00
        for (int i = 0; i < 16; ++i) {
            u32 desc = (0xa1000000 + i * 0x100000) | 0xc12;
            uc_mem_write(uc, 0xa0110000 + (0xe00 + i) * 4, &desc, 4);
        }
    }
    if (ad >= 0xf0002ea4 && ad <= 0xf0002ee4 && !saw_idle) {
        printf(">>> KERNEL ENTERED IDLE THREAD LOOP (SCHEDULER RUNNING!) pc=%08x insn#%llu <<<\n",
            (u32)ad, (unsigned long long)g_insn);
        saw_idle = true;
    }
    g_last=(u32)ad;
    if (g_budget && g_insn>=g_budget) uc_emu_stop(uc);
}

int main(int argc,char**argv){
    if(argc<2){ printf("usage: %s <kernel.elf> [insns]\n",argv[0]); return 2; }
    FILE*f=fopen(argv[1],"rb"); if(!f){perror("open");return 1;}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    std::vector<u8> d(n); if(fread(d.data(),1,n,f)!=(size_t)n){perror("read");return 1;} fclose(f);
    u32 entry=rd32(d.data(),24), phoff=rd32(d.data(),28);
    u16 phent=rd16(d.data(),42), phnum=rd16(d.data(),44);
    printf("ELF %s: entry=0x%08x phnum=%d\n",argv[1],entry,(int)phnum);

    uc_engine*uc; uc_err er=uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc);
    if(er){printf("uc_open err\n");return 1;}
    // map phys RAM pleb2 (0xa0000000..0xa2000000) + the ELF VA regions + IO area
    uc_mem_map(uc,PLEB_RAM,0x2000000,UC_PROT_ALL);                       // phys RAM 32MB
    for(int i=0;i<phnum;i++){
        size_t o=phoff+i*phent; if(rd32(d.data(),o)!=1) continue;
        u32 pv=rd32(d.data(),o+8), off=rd32(d.data(),o+4);
        u32 fs=rd32(d.data(),o+16), ms=rd32(d.data(),o+20);
        u32 nmem=ms?ms:fs; if(!nmem) continue;
        // map VA (kernel linked at 0xf0000000)
        if(uc_mem_map(uc,pv&~0xFFFu,((nmem+0xFFF)&~0xFFFu)+0x1000,UC_PROT_ALL)!=UC_ERR_OK){
            // overlap tolerated
        }
        std::vector<u8> seg(nmem,0);
        if(fs){size_t cl=std::min((size_t)fs,seg.size()); memcpy(seg.data(),d.data()+off,cl);}
        uc_mem_write(uc,pv,seg.data(),seg.size());
        // ALSO load segment at its PhysAddr (0xa0100000) so virt_to_phys(&platform_memory) reads it!
        u32 paddr=rd32(d.data(),o+12);
        if (paddr) {
            uc_mem_write(uc,paddr,seg.data(),seg.size());
            printf("   LOAD pa=0x%08x va=0x%08x off=0x%x fs=0x%x ms=0x%x nmem=0x%x\n",paddr,pv,off,fs,ms,nmem);
        } else {
            printf("   LOAD va=0x%08x off=0x%x fs=0x%x ms=0x%x nmem=0x%x\n",pv,off,fs,ms,nmem);
        }
    }
    // Zero out the KIP memory descriptor count so it starts clean!
    // KIP is at 0xf000d000 (pa 0xa010d000), num_descriptors is at offset +0x54 (16-bit)
    u16 zero16 = 0;
    uc_mem_write(uc, 0xa010d054, &zero16, 2);
    uc_mem_write(uc, 0xf000d054, &zero16, 2);
    // Map 0xfd000000 window (pleb2 / xscale interrupt controller registers)
    uc_mem_map(uc,0xfd000000,0x100000,UC_PROT_ALL);
    // Map address 0 (page 0 / vector table / null pointer protection)
    uc_mem_map(uc,0x00000000,0x10000,UC_PROT_ALL);

    // Map KTCB area: KTCB_AREA_START is 0xe0000000 on ARM
    uc_mem_map(uc,0xe0000000,0x01000000,UC_PROT_ALL); // 16MB KTCB area

    // pleb2 IODEVICE_VADDR = IO_AREA0_VADDR; map phys devices at XSCALE_DEV (0x40000000)
    uc_mem_map(uc,XSCALE_DEV,0x2000000,UC_PROT_ALL);                     // phys devices window 32MB
    // Also map XSCALE_CLOCKS at 0x41300000
    uc_mem_map(uc,0x41000000,0x1000000,UC_PROT_ALL);
    // Install L1 page table mapping for 0xe0000000 (KTCB) in physical memory
    // In pleb2, 0xa0110000 is TTB. Each L1 entry is 1MB.
    // 0xe0000000 corresponds to index 0xe00 (3584).
    // Let's create a 1MB section entry pointing to physical RAM 0xa1000000:
    // Section descriptor format: [31:20] base PA, [19:12]=0, [11:10]=AP(11=r/w), [9]=0, [8:5]=domain(0), [4]=1, [3:2]=C,B(00), [1:0]=02 (Section)
    // 0xa1000000 | (3 << 10) | (1 << 4) | 2 = 0xa1000c12
    u32 sec_desc = 0xa1000c12;
    // Map multiple MBs from 0xe00 to 0xe10
    for (int i = 0; i < 16; ++i) {
        u32 desc = (0xa1000000 + i * 0x100000) | 0xc12;
        uc_mem_write(uc, 0xa0110000 + (0xe00 + i) * 4, &desc, 4);
    }
    // Map above KTCB space from 0xe1000000 up to 0xf0000000
    uc_mem_map(uc,0xe1000000,0x0f000000,UC_PROT_ALL);

    // Just before boot, pre-populate physical page table for KTCB at 0xa0113800
    // so when MMU turns on, virtual addresses 0xe0000000..0xe0ffffff resolve to physical RAM
    // (handled dynamically in on_code hook at MMU transition)

    u64 budget = argc>2? strtoull(argv[2],0,0):5000000;
    g_budget=budget;
    uc_hook hc=0,hm=0;
    uc_hook_add(uc,&hc,UC_HOOK_CODE,(void*)(on_code),nullptr,0,~0ULL);
    uc_hook_add(uc,&hm,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE|UC_HOOK_MEM_FETCH_UNMAPPED|UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED,(void*)(on_mem),nullptr,0,~0ULL);
    printf("booting kernel @0x%08x (budget=%llu)\n",entry,(unsigned long long)budget);
    er=uc_emu_start(uc,entry,0,0,0);
    printf("stopped pc=0x%08x insn#%llu (err=%s)\n",rreg(uc,UC_ARM_REG_PC),(unsigned long long)g_insn,
           er?uc_strerror(er):"ok");
    printf("  r0=%08x r1=%08x r2=%08x sp=%08x lr=%08x\n",rreg(uc,UC_ARM_REG_R0),rreg(uc,UC_ARM_REG_R1),
           rreg(uc,UC_ARM_REG_R2),rreg(uc,UC_ARM_REG_SP),rreg(uc,UC_ARM_REG_LR));
    uc_close(uc);
    return 0;
}