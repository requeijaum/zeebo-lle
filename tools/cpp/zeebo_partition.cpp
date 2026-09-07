// zeebo_partition.cpp — M1: read a full NAND partition via the functional DMOV
// DMA model in C++ and verify byte-for-byte against the raw dump. Port of
// tools/mini_boot_read_partition.py.
//
// Build: g++ -std=c++23 -O2 -o zeebo_partition zeebo_partition.cpp -lunicorn
// Run:   ./zeebo_partition [AMSS|APPS]
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <unicorn/unicorn.h>
#include "zeebo_devices.h"

static const char* NDPATH="/home/rafaelfrequiao/projects/zeebo-lle/nand";
static std::vector<u8> readfile(const char* p){ std::vector<u8> v; NandController::read_file(p,v); return v; }

static void wram(uc_engine*uc,u32 a,u32 v){ uc_mem_write(uc,a,(const void*)&v,4); }

// one page read through the descriptor list nand.c emits
static void read_page(uc_engine*uc,DMOVModel&dm,NandController&nc,u32 page,u32 dest){
    const u32 IO=0x00400000, CL=0x00401000, PTR=0x00402000;
    wram(uc,IO+0x00,0x33);                        // NAND_CMD_PAGE_READ_ECC
    wram(uc,IO+0x04,(page<<16)&0xFFFFFFFFu);      // addr0
    wram(uc,IO+0x08,(page>>16)&0xFFu);            // addr1
    wram(uc,IO+0x0c,0|4);                         // chipsel
    wram(uc,IO+0x10,0xa25400c0); wram(uc,IO+0x14,0x0004745e);
    wram(uc,IO+0x18,1); wram(uc,IO+0x1c,0x203); wram(uc,IO+0x20,0);
    struct { u32 cmd,src,dst,len; } s[8]={
        {5<<7,IO+0x00,NAND_BASE+0x00,16},
        {0,    IO+0x10,NAND_BASE+0x20,8},
        {0,    IO+0x18,NAND_BASE+0x10,4},
        {4<<3, NAND_BASE+0x14,IO+0x24,8},
        {0,    NAND_FLASH_BUFFER,dest,512},
        {0,    NAND_FLASH_BUFFER,dest+512,512},
        {0,    NAND_FLASH_BUFFER,dest+1024,512},
        {CMD_LC,NAND_FLASH_BUFFER,dest+1536,512},
    };
    for(int i=0;i<8;i++) uc_mem_write(uc,CL+i*16,(const void*)&s[i],16);
    wram(uc,PTR,(CL>>3)|CMD_PTR_LP);
    dm.exec_cmdptr(((PTR>>3))|CMD_PTR_LP);
}

static int run(const char* which,u32 start_blk,u32 nblocks){
    auto raw=readfile((std::string(NDPATH)+"/1.1.2.bin").c_str());
    uc_engine* uc; uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc);
    uc_mem_map(uc,0x00400000,0x100000,UC_PROT_ALL);
    uc_mem_map(uc,0x10000000,0x0400000,UC_PROT_ALL);
    NandController nc(std::string(NDPATH)+"/1.1.2.bin",std::string(NDPATH)+"/1.1.2_spare.bin");
    DMOVModel dm(uc,nc);
    const u32 DEST=0x10000000;
    std::vector<u8> out;
    unsigned total_pages=nblocks*64;
    out.reserve((size_t)total_pages*2048);
    for(unsigned blk=0;blk<nblocks;blk++){
        for(unsigned p=0;p<64;p++){
            u32 page=(start_blk+blk)*64+p;
            read_page(uc,dm,nc,page,DEST);   // DMOV drain-cursor yields full 2048
            u8 buf[2048]; uc_mem_read(uc,DEST,buf,2048);
            out.insert(out.end(),buf,buf+2048);
        }
        if(blk%32==0) printf("  blk %u/%u (%u pages)\n",blk,nblocks,out.size()/2048);
    }
    u32 first_page=start_blk*64;
    const u8* exp=raw.data()+ (size_t)first_page*2048;
    bool match = out.size()==(size_t)total_pages*2048 &&
                 std::memcmp(out.data(),exp,out.size())==0;
    printf("\n== %s: %zu bytes read (%u pages) ==\n",which,out.size(),total_pages);
    printf("  first16 got: ");
    for(int i=0;i<16;i++) printf("%02x",out[i]); printf("\n");
    printf("  first16 exp: ");
    for(int i=0;i<16;i++) printf("%02x",exp[i]); printf("\n");
    printf("  byte-identical: %s\n", match?"YES":"NO");
    if(!match){ for(size_t i=0;i<out.size();i++) if(out[i]!=exp[i]){printf("  first diff @%zu\n",i);break;} }
    printf("  DMOV execs=%u\n",dm.exec_count());
    uc_close(uc);
    return match?0:1;
}

int main(int argc,char**argv){
    std::string w = argc>1? argv[1] : "AMSS";
    if(w=="AMSS") return run("AMSS",0x012,0x0a5);
    if(w=="APPS") return run("APPS",0x0e6,0x0a9);
    printf("usage: %s [AMSS|APPS]\n",argv[0]);
    return 2;
}