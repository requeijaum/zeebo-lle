// zeebo_harness.cpp — interactive LLE debug harness (C++23)
//
// Boots a Zeebo firmware image in Unicorn (ARM1176) and exposes a REPL for
// full debug: peek/poke of memory AND MMIO (with a device model), full CPU
// state, VA->PA translation (real ARM11 MMU map), single-step, breakpoints,
// disassembly (Capstone), and file-offset/vaddr/PA addressing.
//
// Build (host x86_64):
//   g++ -std=c++23 -O2 -o zeebo_harness zeebo_harness.cpp \
//       -I/usr/include -lunicorn -lcapstone
//
// Usage:
//   ./zeebo_harness firmware/openzeebo-zloader.bin 0x00a00000
//   ./zeebo_harness nand/1.1.2_APPSBL.bin 0x00000000
//   (with optional 3rd/4th args = NandController data/spare paths)
//
// REPL commands (case-insensitive):
//   help | regs | cpsr
//   peek <addr> [n]            read n words (dot/hex) from addr (VA unless - or #)
//   poke <addr> <val>          write word to addr
//   p8/p16/p32 ...             sized peek; wk <addr> <val> sized poke
//   mmio <on|off>              enable MMIO access logging
//   run [n_insns]              execute until breakpoint / n
//   bp <addr>  / cl | step [n]
//   dis <addr> [count]         disassemble from addr
//   vtop <va>                  translate VA->PA (real map, if loadable)
//   map <load|clear>           (built-in map is intialized for the loaded image)
//   d8/d16/d32 <addr> <len>    dump bytes
//   file <addr> [len]          show file-offset <-> addr (for a mapped blob)
//   reset | quit
// Address prefixes: `#` = add base-load; `&` = physical/PA (identity);
//   `f:<fileoff>` = file offset into the loaded blob.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <unicorn/unicorn.h>
#include <capstone/capstone.h>
#include "zeebo_devices.h"

using u8=uint8_t; using u16=uint16_t; using u32=uint32_t; using u64=uint64_t;

static const char* kProg="zeebo_harness";
// --- device model state ---
static std::map<u32,u32> g_sticky;      // MMIO config regs (write->read back)
static std::map<u32,u64> g_mmioCount;   // MMIO access counts by addr
static bool g_mmioLog=false;
static u64 g_dmovExecs=0;

static uc_engine* g_uc = nullptr;

// --- device models (functional) ---
static NandController* g_nand=nullptr;
static DMOVModel*     g_dmov=nullptr;
static const std::string g_nandData="/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2.bin";
static const std::string g_nandSpare="/home/rafaelfrequiao/projects/zeebo-lle/nand/1.1.2_spare.bin";
static void init_devices(){ if(!g_nand){ g_nand=new NandController(g_nandData,g_nandSpare); g_dmov=new DMOVModel(g_uc,*g_nand); } }

// --- loaded blob ---
static std::vector<u8> g_blob;
static u32 g_loadBase=0;
static bool g_blobLoaded=false;

// --- run control ---
static std::vector<u32> g_bps;
static u64 g_insnCount=0;
static u64 g_stepBudget=0;   // 0 = run until bp/forever (bounded)
static bool g_keepGoing=true;
static u32 g_lastPc=0;
static int g_pastText=0;

static u32 cpu_reg(int r){ u32 v=0; uc_reg_read(g_uc,r,&v); return v; }
static void cpu_reg_w(int r,u32 v){ uc_reg_write(g_uc,r,&v); }

static int hexchar(int c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static u64 parse_hex(const std::string&s){
    size_t i=0; if(s.size()>2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) i=2;
    u64 v=0; for(;i<s.size();i++){int h=hexchar(s[i]); if(h<0) break; v=(v<<4)|(u32)h;} return v;
}

// ---- Unicorn hooks ----
static void on_unmapped(uc_engine*uc, uc_mem_type type, uint64_t address, int size, int64_t value, void*ud){
    #ifdef DEBUG_UNMAP
    printf("  [unmapped] type=%d addr=%08llx\n",type,(unsigned long long)address);
    #endif
    uintptr_t page=address & ~0xFFFULL;
    uc_mem_map(uc,page,0x1000, UC_PROT_ALL);
}
static void on_mem(uc_engine*uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void*ud){
    if (addr < 0x80000000ULL) return; // RAM passes through (real memory)
    g_mmioCount[(u32)addr]++;
    if (g_mmioLog)
        printf("  [MMIO] %s 0x%08x sz=%d val=0x%llx\n", (type&UC_MEM_WRITE)?"W":"R",
               (u32)addr, size, (unsigned long long)value);

    // DMOV SD1 command/result/status/readback (SD1 base 0xa9400000)
    if (addr>=DMOV_SD1_BASE && addr<DMOV_SD1_BASE+0x400){
        u32 off=(u32)(addr-DMOV_SD1_BASE);
        if (type==UC_MEM_WRITE){
            // writing CMD_PTR (off+ch*4 == 0x00c for NAND chan 3) => run DMA
            if (off==dmov_reg(DMOV_CMD_PTR,DMOV_NAND_CHAN)-DMOV_SD1_BASE){
                if (!g_dmov) init_devices();
                g_dmov->exec_cmdptr((u32)value);
                g_dmovExecs=g_dmov->exec_count();
            }
            g_sticky[(u32)addr]=(u32)value;
        } else {
            u32 val=0; auto it=g_sticky.find((u32)addr); if(it!=g_sticky.end()) val=it->second;
            // result/status bands: return DONE
            if (off>=0x200&&off<0x210) val=3;      // RSLT_VALID|CMD_PTR_RDY
            else if (off>=0x40&&off<0x50) val=DMOV_RSLT_DONE;
            uc_mem_write(uc,(u32)addr,&val,sizeof(val));
        }
        return;
    }
    // NAND controller (0xa0a00000)
    if (addr>=NAND_BASE && addr<NAND_BASE+0x400){
        if (!g_nand) init_devices();
        u32 off=(u32)(addr-NAND_BASE);
        if (type==UC_MEM_WRITE){
            g_nand->write(off,(u32)value,size);
        } else {
            u32 v=g_nand->read(off,size);
            // drain-cursor reads are handled by DMOVModel; direct reads of buffer
            uc_mem_write(uc,(u32)addr,&v,sizeof(v));
        }
        return;
    }
    // UART1 console (0xa9a00000): SR@0x08 -> TX_READY|TX_EMPTY; TF@0x0C = TX char
    if (addr>=0xa9a00000 && addr<0xa9a01000){
        u32 off=(u32)(addr-0xa9a00000);
        if (type==UC_MEM_WRITE){
            if (off==0x0C){ putchar((char)value); fflush(stdout); } // TF = serial out
            g_sticky[(u32)addr]=(u32)value;
        } else {
            u32 v=(off==0x08) ? (0x14) : (g_sticky.count((u32)addr)?g_sticky[(u32)addr]:0);
            uc_mem_write(uc,(u32)addr,&v,sizeof(v));
        }
        return;
    }
    if (type == UC_MEM_WRITE){
        g_sticky[(u32)addr]=(u32)value;
    } else {
        u32 v=0;
        auto it=g_sticky.find((u32)addr);
        if (it!=g_sticky.end()) v=it->second;
        // GPT free-running counter advances so delay loops exit
        if (addr==0xc0100004){ v=64; }
        uc_mem_write(uc,(u32)addr,&v,sizeof(v));
    }
}
static void on_code(uc_engine*uc,uint64_t addr,uint32_t size,void*ud){
    g_insnCount++;
    // log the FIRST time execution leaves the loader text range (any addr, shows slide origin)
    if (!g_pastText && addr >= 0xa03700){   // code ends ~0xa03700 (BSS/END 0xa039a8)
        g_pastText=1;
        printf("  [LEAVE-TEXT] 0x%08x insn=%llu (prev 0x%08x)\n", (u32)addr,(unsigned long long)g_insnCount, g_lastPc);
    }
    g_lastPc = (u32)addr;
    if (std::find(g_bps.begin(),g_bps.end(),(u32)addr)!=g_bps.end()){
        printf("  [breakpoint] 0x%08x (insn #%llu)\n",(u32)addr,(unsigned long long)g_insnCount);
        uc_emu_stop(uc); return;
    }
    if (g_stepBudget && g_insnCount>=g_stepBudget) uc_emu_stop(uc);
}

static void install_hooks(){
    uc_hook hmm=0,hcode=0,hunm=0;
    uc_hook_add(g_uc,&hunm,UC_HOOK_MEM_READ_UNMAPPED|UC_HOOK_MEM_WRITE_UNMAPPED|UC_HOOK_MEM_FETCH_UNMAPPED,
                (void*)(on_unmapped),nullptr,0,~0ULL);
    uc_hook_add(g_uc,&hmm,UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,(void*)(on_mem),nullptr,0x80000000ULL,~0ULL);
    uc_hook_add(g_uc,&hcode,UC_HOOK_CODE,(void*)(on_code),nullptr,0,~0ULL);  // all addresses (begin=0,end=max) or matched addr 0 only
}

// initialize the ARM1176 core + RAM
static bool init_uc(){
    uc_err e=uc_open(UC_ARCH_ARM,UC_MODE_ARM,&g_uc);
    if(e!=UC_ERR_OK){printf("uc_open: %s\n",uc_strerror(e));return false;}
    return true;
}

// map the image blob at base + a generous RAM window
static void map_image(){
    if(!g_uc) init_uc();
    for(auto [base,size] : std::vector<std::pair<u32,u32>>{
            {0x00000000u,0x00a00000u},   // low RAM
            {0x00a00000u,0x00600000u},   // zloader/APPSBL region
            {0x02000000u,0x01000000u},   // RAM high (enlarge to cover any slide/derail range)
            {0x01000000u,0x01000000u},   // APPS ELF region + flash geometry table (0x1f00000+) flat region: 0x1000000-0x2000000
            {0x00c00000u,0x00400000u},   // heap (distinct)
        }){
        uc_mem_map(g_uc,base,size,UC_PROT_ALL);
    }
    // high windows
    for(u32 base : {0xff000000u,0xffe00000u}) uc_mem_map(g_uc,base,0x00400000u,UC_PROT_ALL);
    // peripherals (identity, RW)
    for(u32 base : {0xb8000000u,0xc0000000u,0xc0100000u,0xa9a00000u,0xaa600000u,0xa9700000u,0xa9400000u,0xa0a00000u,0xa9200000u,0xa9000000u,0xa8600000u,0xa0d00000u}){
        uc_mem_map(g_uc,base,0x10000,UC_PROT_ALL);
    }
    if(g_blobLoaded) uc_mem_write(g_uc,g_loadBase,g_blob.data(),g_blob.size());
}

// --- VA->PA translation using the REAL ARM11 map (parsed from console__zeebo__mmu.txt) ---
static std::map<u32,u32> g_sections;   // va_base>=1MB -> pa_base
static std::map<u32,u32> g_coarse;     // va_page(4K)->pa
static void build_default_mmu(){
    // Minimal internal transcript of the verified ARM11 entries
    auto sec=[](u32 va,u32 pa){g_sections[(va>>20)]=(pa>>20)*0x100000;};
    sec(0xf0000000,0x10000000); sec(0xf4000000,0x10000000);
    sec(0xc5300000,0xc0000000); sec(0xc5400000,0xc0100000); sec(0xc1d00000,0xaa600000);
    sec(0xc2a00000,0xa9700000); sec(0xc2f00000,0xa9200000); sec(0xc3200000,0xa8600000);
    sec(0xc2e00000,0xa9300000); sec(0xc2500000,0xaa200000); sec(0xc1400000,0xa0700000);
    sec(0xc1600000,0xa0500000); sec(0xc1800000,0xa0200000); sec(0xc1e00000,0xaa500000);
    sec(0xc2300000,0xa9900000); sec(0xc2900000,0xa9800000);
    sec(0x10200000,0x10200000); sec(0x10300000,0x10300000); sec(0x10400000,0x10400000);
    sec(0x10500000,0x10500000); sec(0x10600000,0x10600000); sec(0x10700000,0x10700000);
    sec(0x10a00000,0x10a00000); sec(0x11000000,0x11000000); sec(0x13000000,0x13000000);
    sec(0x14000000,0x14000000); sec(0x15600000,0x15600000); sec(0x16a00000,0x16a00000);
    auto coa=[](u32 va,u32 pa){g_coarse[va>>12]=pa;};
    coa(0xb0100000,0x100a3800); coa(0xb0400000,0x100afc00);
    coa(0xb0d00000,0x100a3400); coa(0xb0e00000,0x100a3c00);
    coa(0x10100000,0x100ad400); coa(0x11400000,0x100adc00);
    coa(0xe0000000,0x10090800); coa(0xff000000,0x10090000); coa(0xfff00000,0x10095800);
}
static u32 translate_va(u32 va){
    auto p=g_coarse.find(va>>12);
    if(p!=g_coarse.end()) return p->second+(va&0xFFF);
    auto s=g_sections.find(va>>20);
    if(s!=g_sections.end()) return s->second+(va&0xFFFFF);
    return va; // identity
}
static std::string va2pa_str(u32 va){
    u32 pa=translate_va(va);
    if(pa==va){ char b[32]; snprintf(b,sizeof b,"0x%08x(=identity)",pa); return b; }
    char b[32]; snprintf(b,sizeof b,"0x%08x",pa); return b;
}

// --- load a blob ---
static bool load_blob(const std::string&path,u32 base){
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f) return false;
    auto sz=f.tellg(); f.seekg(0); g_blob.resize((size_t)sz); f.read((char*)g_blob.data(),sz);
    g_loadBase=base; g_blobLoaded=true;
    if(g_uc) uc_mem_write(g_uc,base,g_blob.data(),g_blob.size());
    return true;
}

// --- memory peek/poke helpers (with MU translation-aware addr, prefixed) ---
static u32 resolve_addr(const std::string&tok){
    if(tok.empty()) return 0;
    if(tok[0]=='#') return g_loadBase + (u32)parse_hex(tok.substr(1));
    if(tok[0]=='&'){ u32 pa=(u32)parse_hex(tok.substr(1)); // physical: find a VA that maps->pa, else identity
        return pa; }
    if(tok[0]=='f'){ // file offset into blob
        if(!g_blobLoaded){printf("  no blob loaded\n");return 0;}
        return g_loadBase + (u32)parse_hex(tok.substr(2));
    }
    return (u32)parse_hex(tok);
}
static std::optional<u64> peek_uN(u32 addr,int bytes){
    u8 buf[8]={0};
    if(uc_mem_read(g_uc,addr,buf,bytes)!=UC_ERR_OK) return std::nullopt;
    u64 v=0; for(int i=bytes-1;i>=0;i--) v=(v<<8)|buf[i];
    return v;
}
static bool poke_uN(u32 addr,u64 val,int bytes){
    u8 buf[8]; for(int i=0;i<bytes;i++) buf[i]=(u8)(val>>(8*i));
    return uc_mem_write(g_uc,addr,buf,bytes)==UC_ERR_OK;
}

static void cmd_regs(){ 
    printf("  r0 =%08x r1 =%08x r2 =%08x r3 =%08x\n",cpu_reg(UC_ARM_REG_R0),cpu_reg(UC_ARM_REG_R1),cpu_reg(UC_ARM_REG_R2),cpu_reg(UC_ARM_REG_R3));
    printf("  r4 =%08x r5 =%08x r6 =%08x r7 =%08x\n",cpu_reg(UC_ARM_REG_R4),cpu_reg(UC_ARM_REG_R5),cpu_reg(UC_ARM_REG_R6),cpu_reg(UC_ARM_REG_R7));
    printf("  r8 =%08x r9 =%08x r10=%08x r11=%08x\n",cpu_reg(UC_ARM_REG_R8),cpu_reg(UC_ARM_REG_R9),cpu_reg(UC_ARM_REG_R10),cpu_reg(UC_ARM_REG_R11));
    printf("  r12=%08x sp =%08x lr =%08x pc =%08x\n",cpu_reg(UC_ARM_REG_R12),cpu_reg(UC_ARM_REG_SP),cpu_reg(UC_ARM_REG_LR),cpu_reg(UC_ARM_REG_PC));
    printf("  cpsr=%08x  (insn#%llu)\n",cpu_reg(UC_ARM_REG_CPSR),(unsigned long long)g_insnCount);
}
static void cmd_cpsr(){
    u32 c=cpu_reg(UC_ARM_REG_CPSR);
    const char* mode="?";
    switch(c&0x1f){ case 0x10:mode="USR";break; case 0x11:mode="FIQ";break; case 0x12:mode="IRQ";break;
        case 0x13:mode="SVC";break; case 0x17:mode="ABT";break; case 0x1b:mode="UND";break; case 0x1f:mode="SYS";break; }
    printf("  cpsr=%08x mode=%s N=%d Z=%d C=%d V=%d I=%d F=%d T=%d\n",
           c,mode,(c>>31)&1,(c>>30)&1,(c>>29)&1,(c>>28)&1,(c>>7)&1,(c>>6)&1,(c>>5)&1);
}
static void cmd_peek(const std::string&a){ 
    u32 addr=resolve_addr(a); 
    auto v=peek_uN(addr,4);
    if(v) printf("  [0x%08x](PA %s)= %08x\n",addr,va2pa_str(addr).c_str(),(u32)*v);
    else printf("  unreadable/not-mapped\n");
}
static void cmd_peekN(const std::string&a,int n){
    u32 addr=resolve_addr(a);
    printf("  0x%08x: ",addr);
    bool ok=true;
    for(int i=0;i<n;i++){ auto v=peek_uN(addr+i*4,4); if(!v){printf("?? ");ok=false;continue;} printf("%08x ",(u32)*v);}
    printf("\n");
}
static void cmd_poke(const std::string&a,const std::string&v){
    u32 addr=resolve_addr(a); u64 val=parse_hex(v);
    if(poke_uN(addr,val,4)) printf("  wrote %08x -> 0x%08x (see memory)\n",(u32)val,addr);
    else printf("  write failed\n");
}
static void cmd_dis(const std::string&a,int count){
    u32 addr=resolve_addr(a);
    csh cs; if(cs_open(CS_ARCH_ARM,CS_MODE_ARM,&cs)!=CS_ERR_OK){printf("  cs_open fail\n");return;}
    cs_option(cs,CS_OPT_DETAIL,CS_OPT_OFF);
    cs_insn* ins=cs_malloc(cs);
    int done=0;
    while(done<count){
        u8 code[4]; int rd=4;
        if(uc_mem_read(g_uc,addr,code,rd)!=UC_ERR_OK){ printf("  %08x: (unmapped)\n",addr); addr+=4; continue; }
        const uint8_t* cp = code; size_t avail=4; uint64_t a=addr;
        if(cs_disasm_iter(cs,&cp,&avail,&a,ins)){
            printf("  %08x: %-8s %s\n",ins->address,ins->mnemonic,ins->op_str);
            addr=(u32)(ins->address+ins->size);
        } else {
            printf("  %08x: ????\n",addr); addr+=4;
        }
        done++;
    }
    cs_free(ins,1); cs_close(&cs);
}
static void cmd_dump(const std::string&a,int len){
    u32 addr=resolve_addr(a);
    for(int row=0;row<len;row+=16){
        printf("  %08x: ",addr+row);
        u8 b[16]; bool ok=uc_mem_read(g_uc,addr+row,b,16)==UC_ERR_OK;
        for(int i=0;i<16;i++) printf("%02x ", ok?b[i]:0xFF);
        printf(" ");
        for(int i=0;i<16;i++){ if(!ok) printf("."); else printf("%c",(b[i]>=32&&b[i]<127)?b[i]:'.'); }
        printf("\n");
    }
}
static void cmd_bp(const std::string&a){ u32 addr=resolve_addr(a); g_bps.push_back(addr); printf("  bp @0x%08x\n",addr); }
static void cmd_bp_clear(){ g_bps.clear(); printf("  cleared\n"); }
static void cmd_run(const std::string&n){
    u64 count = n.empty()? 0 : parse_hex(n);
    uc_err e = uc_emu_start(g_uc, cpu_reg(UC_ARM_REG_PC), 0, 0, count); // 0=forever (bounded by budget hook)
    if(e!=UC_ERR_OK && e!=UC_ERR_HOOK) printf("  emu: %s\n",uc_strerror(e));
    printf("  [stopped] pc=0x%08x insn#%llu\n",cpu_reg(UC_ARM_REG_PC),(unsigned long long)g_insnCount);
}
static void cmd_step(int n){
    g_stepBudget = g_insnCount + n;
    uc_err e=uc_emu_start(g_uc,cpu_reg(UC_ARM_REG_PC),0,0,0);
    g_stepBudget=0;
    if(e!=UC_ERR_OK && e!=UC_ERR_HOOK) printf("  [step err %s]\n",uc_strerror(e));
    printf("  pc=0x%08x insn#%llu\n",cpu_reg(UC_ARM_REG_PC),(unsigned long long)g_insnCount);
}

static void cmd_file(const std::string&a){
    u32 addr=resolve_addr(a);
    if(!g_blobLoaded||addr<g_loadBase||addr>=g_loadBase+g_blob.size()){printf("  outside blob\n");return;}
    printf("  blob off 0x%x <- 0x%08x (vaddr)\n",addr-g_loadBase,addr);
    (void)parse_hex(a);
}

static void usage(){
    printf("commands: help regs cpsr peek [n] poke vtop mmio run [n] step [n] bp cl dis [cnt] d8/d16/d32 f[off] reset quit\n");
    printf("addr prefixes: '#'=loadbase+off  'f:'=fileoff into blob   hex by default (VA)\n");
    printf("e.g.  poke #0x8ec 1 ; peek #0x8ec ; dis #0x8ec 8 ; vtop f0000000\n");
}

static void eval(const std::string&line){
    std::istringstream ss(line); std::string cmd; ss>>cmd;
    std::transform(cmd.begin(),cmd.end(),cmd.begin(),[](unsigned char c){return (char)std::tolower(c);});
    if(cmd=="dev"){ printf("  DMOV execs=%llu NAND last_cmd=%#x cfg0=%#x cfg1=%#x id=%#x\n",
        (unsigned long long)(g_dmov?g_dmov->exec_count():0),
        g_nand?g_nand->debug_last_cmd():0, g_nand?g_nand->read(R_DEV0_CFG0,4):0,
        g_nand?g_nand->read(R_DEV0_CFG1,4):0, g_nand?g_nand->read(R_READ_ID,4):0); return; }
    if(cmd=="help"){ usage(); return; }
    if(cmd=="regs"){ cmd_regs(); return; }
    if(cmd=="cpsr"){ cmd_cpsr(); return; }
    if(cmd=="reset"){ // re-init
        if(g_uc) uc_close(g_uc); g_uc=nullptr; init_uc(); map_image(); install_hooks(); g_insnCount=0;
        printf("  core reset; image remapped\n"); return; }
    if(cmd=="quit"||cmd=="exit"){ g_keepGoing=false; return; }
    if(cmd=="mmio"){ std::string v; ss>>v; g_mmioLog=(v=="on"||v=="1"); printf("  mmio log=%d\n",(int)g_mmioLog); return; }
    if(cmd=="bp"){ std::string a; ss>>a; cmd_bp(a); return; }
    if(cmd=="cl"){ cmd_bp_clear(); return; }
    if(cmd=="vtop"){ std::string a; ss>>a; u32 va=(u32)parse_hex(a); printf("  VA 0x%08x -> PA %s\n",va,va2pa_str(va).c_str()); return; }
    if(cmd=="run"){ std::string n; ss>>n; cmd_run(n); return; }
    if(cmd=="step"){ int n=1; ss>>n; cmd_step(std::max(1,n)); return; }
    if(cmd=="pc"){ std::string a; ss>>a; u32 v=resolve_addr(a); cpu_reg_w(UC_ARM_REG_PC,v); printf("  pc=0x%08x\n",v); return; }
    if(cmd=="sp"){ std::string a; ss>>a; u32 v=(u32)parse_hex(a); cpu_reg_w(UC_ARM_REG_SP,v); printf("  sp=0x%08x\n",v); return; }
    if(cmd=="sreg"){ std::string r,v; int rn; if(!(r=="r0"||r=="r1"||r=="r2"||r=="r3"||r=="r4"||r=="r5"||r=="r6"||r=="r7"||r=="r8"||r=="r9"||r=="r10"||r=="r11"||r=="r12"||r=="lr"||r=="sp")){usage();return;} ss>>v; u64 val=parse_hex(v);
        int regs[]={UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_R7,UC_ARM_REG_R8,UC_ARM_REG_R9,UC_ARM_REG_R10,UC_ARM_REG_R11,UC_ARM_REG_R12,UC_ARM_REG_LR,UC_ARM_REG_SP};
        int idx= r[1]-'0'; if(r=="lr")idx=13; else if(r=="sp")idx=14; else { if(r.size()==2) idx=r[1]-'0'; }
        cpu_reg_w(regs[idx],(u32)val); printf("  %s=0x%08x\n",r.c_str(),(u32)val); return; }
    if(cmd=="peek"){ std::string a; int n=1; ss>>a; ss>>n; n = (n>32)?32:n; cmd_peekN(a,n); return; }
    if(cmd=="poke"){ std::string a,v; ss>>a; ss>>v; cmd_poke(a,v); return; }
    if(cmd=="d8"||cmd=="d16"||cmd=="d32"||cmd=="dump"){ std::string a; int len; ss>>a; if(!(ss>>len))len=16; bool isDump=(cmd=="dump"); cmd_dump(a,isDump?len:(cmd=="d8"?64:len)); return; }
    if(cmd=="dis"){ std::string a; int cnt; ss>>a; if(!(ss>>cnt))cnt=8; cmd_dis(a,cnt); return; }
    if(cmd=="file"){ std::string a; ss>>a; cmd_file(a); return; }
    usage(); 
}

int main(int argc,char**argv){
    std::string blob,base="0x00000000";
    if(argc>=2) blob=argv[1];
    if(argc>=3) base=argv[2];
    init_uc();
    build_default_mmu();
    init_devices();   // NandController + DMOVModel wired to g_uc
    if(!blob.empty()){
        if(!load_blob(blob,(u32)parse_hex(base))){ printf("failed to load %s\n",blob.c_str()); return 1; }
        printf("loaded %s @0x%08zx (%zu bytes)\n",blob.c_str(),(size_t)parse_hex(base),g_blob.size());
    }
    map_image();
    install_hooks();
    // set SP + start PC at loadBase (reset vector) by default
    cpu_reg_w(UC_ARM_REG_SP,0x00bff000);
    printf("%s: type 'help' for commands. pc=0x%08zx\n",kProg,(size_t)g_loadBase);
    std::string line;
    g_keepGoing=true;
    while(g_keepGoing){
        printf("> "); std::fflush(stdout);
        if(!std::getline(std::cin,line)) break;
        if(line.empty()) continue;
        eval(line);
    }
    if(g_uc) uc_close(g_uc);
    return 0;
}