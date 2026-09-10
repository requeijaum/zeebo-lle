// zeebo_elf.cpp — M2: parse an ELF (AMSS/APPS) read from the NAND partition and
// map its PT_LOAD segments into Unicorn RAM at the PHYSICAL addresses the real
// ARM11 MMU map defines. Then optionally run from e_entry.
//
// For the APPS image the ARM11 MMU (real dump) maps:  f0000000->10000000,
// 10xxxxxx identity, b0xxx->100a3xxx. So we translate each PT_LOAD vaddr to its
// PA via the same map translate_va() uses, and load there. The entry is then
// startable (M3) with VA->PA done by the host (identity in the loaded PA frames).
//
// Build: g++ -std=c++23 -O2 -o zeebo_elf zeebo_elf.cpp -lunicorn -lcapstone
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <unicorn/unicorn.h>
#include <capstone/capstone.h>
#include "zeebo_devices.h"

// Teto para alocacao guiada por p_memsz do arquivo (RAM do aparelho e bem menor).
static constexpr size_t kMaxSegBytes = 64u * 1024u * 1024u;

using u16=uint16_t;

// ---- ARM11 VA->PA map (real, from console__zeebo__mmu.txt, ARM11 section) ----
static std::map<u32,u32> gSec, gCoa;
static void build_arm11_map(){
    auto S=[](u32 va,u32 pa){gSec[va>>20]=(pa>>20)*0x100000;};
    S(0xf0000000,0x10000000);S(0xf4000000,0x10000000);S(0xf0100000,0x10100000);
    S(0xc5300000,0xc0000000);S(0xc5400000,0xc0100000);S(0xc1d00000,0xaa600000);
    S(0xc2a00000,0xa9700000);S(0xc2f00000,0xa9200000);S(0xc3200000,0xa8600000);
    S(0x10200000,0x10200000);S(0x10300000,0x10300000);S(0x10400000,0x10400000);
    S(0x10500000,0x10500000);S(0x10600000,0x10600000);S(0x10700000,0x10700000);
    S(0x10a00000,0x10a00000);S(0x11000000,0x11000000);S(0x13000000,0x13000000);
    S(0x14000000,0x14000000);S(0x15600000,0x15600000);S(0x16a00000,0x16a00000);
    auto C=[](u32 va,u32 pa){gCoa[va>>12]=pa;};
    C(0xb0100000,0x100a3800);C(0xb0400000,0x100afc00);
    C(0xb0d00000,0x100a3400);C(0xb0e00000,0x100a3c00);
    C(0x10100000,0x100ad400);C(0x11400000,0x100adc00);
    C(0xe0000000,0x10090800);C(0xff000000,0x10090000);C(0xfff00000,0x10095800);
}
static u32 va2pa(u32 va){ auto p=gCoa.find(va>>12); if(p!=gCoa.end()) return p->second+(va&0xFFF);
    auto s=gSec.find(va>>20); if(s!=gSec.end()) return s->second+(va&0xFFFFF); return va; }

static u16 rd16(const u8*d,size_t o){return (u16)d[o]|((u16)d[o+1]<<8);}
static u32 rd32(const u8*d,size_t o){return (u32)d[o]|((u32)d[o+1]<<8)|((u32)d[o+2]<<16)|((u32)d[o+3]<<24);}

int main(int argc,char**argv){
    if(argc<3){ printf("usage: %s <prog_file> <raw_bin_path> [phys]  (phys=load at PA not vaddr)\n",argv[0]); return 2;}
    std::string prog=argv[1], binpath=argv[2];
    bool physical = argc>3 && strcmp(argv[3],"phys")==0;
    std::vector<u8> d; NandController::read_file(binpath,d);
    if(d.size()<52 || rd32(d.data(),0)!=0x464C457Fu){ printf("not ELF: %s\n",binpath.c_str()); return 1; }

    build_arm11_map();
    u32 entry=rd32(d.data(),24), phoff=rd32(d.data(),28);
    u16 phent=rd16(d.data(),42), phnum=rd16(d.data(),44);
    printf("%s: ELF entry=0x%08x phnum=%u phent=%u\n",prog.c_str(),entry,phnum,phent);

    uc_engine* uc; uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc);
    // map a wide physical RAM for the PA frames (0x10000000.., 0xb0.., low)
    uc_mem_map(uc,0x00000000,0x00800000,UC_PROT_ALL);
    uc_mem_map(uc,0x00a00000,0x00600000,UC_PROT_ALL);
    uc_mem_map(uc,0x01000000,0x01000000,UC_PROT_ALL);   // 0x10000000-0x20000000
    uc_mem_map(uc,0x00c00000,0x00400000,UC_PROT_ALL);
    uc_mem_map(uc,0xff000000,0x00400000,UC_PROT_ALL);
    for(u32 bx : {0xb0000000u,0xc0000000u,0xa0a00000u,0xa9a00000u})
        uc_mem_map(uc,bx,0x10000,UC_PROT_ALL);

    int loads=0;
    for(int i=0;i<phnum;i++){
        size_t o=phoff+(size_t)i*phent;
        // Header do proprio arquivo: nao confiar em phoff/phent/phnum.
        if(o+4>d.size()){ printf("  [skip] phdr %d fora do arquivo (off=%zu size=%zu)\n", i, o, d.size()); continue; }
        u32 ptype=rd32(d.data(),o);
        if(ptype!=1) continue;
        u32 p_filesz=o+16<d.size()? rd32(d.data(),o+16):0;
        u32 p_vaddr=o+8<d.size()? rd32(d.data(),o+8):0;
        u32 p_memsz =o+20<d.size()? rd32(d.data(),o+20):0;
        u32 val=(o+4<d.size())?rd32(d.data(),o+4):0;  // p_offset
        u32 lmap = physical ? va2pa(p_vaddr) : p_vaddr;
        size_t nmem=p_memsz? p_memsz : p_filesz;
        if(!nmem) continue;
        // copy file bytes then zero bss tail
        // p_memsz vem do arquivo; recusa alocacao absurda em vez de tentar.
        if(nmem>kMaxSegBytes){ printf("  [skip] phdr %d memsz=%zu acima do teto\n", i, nmem); continue; }
        std::vector<u8> seg(nmem,0);
        if(p_filesz){
            // p_offset/p_filesz sao do arquivo: validar contra o buffer real.
            if((size_t)val>d.size()){ printf("  [skip] phdr %d p_offset=0x%x fora do arquivo\n", i, val); continue; }
            size_t avail=d.size()-(size_t)val;
            size_t cl=std::min({(size_t)p_filesz, seg.size(), avail});
            if(cl<(size_t)p_filesz) printf("  [warn] phdr %d truncado: %zu de %u bytes\n", i, cl, p_filesz);
            memcpy(seg.data(), d.data()+val, cl);
        }
        uc_mem_write(uc,lmap,seg.data(),seg.size());
        printf("  LOAD vaddr=0x%08x ->pa 0x%08x (%s) filesz=%u memsz=%u\n",
               p_vaddr,lmap, physical?"phys":"vaddr", p_filesz,nmem);
        loads++;
    }
    printf("mapped %d LOAD segs%s\n",loads, physical? " (at PA)":"");
    printf("entry=0x%08x -> pa 0x%08x\n",entry,va2pa(entry));
    uc_close(uc);
    return 0;
}