// zeebo_devices_test.cpp — self-test for the C++ port of NandController+DMOVModel.
// Validates: FETCH_ID, PAGE_READ of the real dump, and a full DMA page read
// through the descriptor sequence (matches the Python mini_boot result).
#include <cstdio>
#include <cstring>
#include <vector>
#include <unicorn/unicorn.h>
#include "zeebo_devices.h"

static void expect(bool c,const char* what){ if(c) printf("  OK  %s\n",what); else { printf("  FAIL %s\n",what); } }

static const char* NDPATH="/home/rafaelfrequiao/projects/zeebo-lle/nand";
static std::vector<u8> readfile(const char* p){ std::vector<u8> v; NandController::read_file(p,v); return v; }

int main(){
    std::string data=std::string(NDPATH)+"/1.1.2.bin";
    std::string spare=std::string(NDPATH)+"/1.1.2_spare.bin";
    auto ref=readfile(data.c_str());

    // 1. standalone NandController tests
    {
        NandController nc(data,spare);
        nc.write(R_FLASH_CMD,CMD_FETCH_ID); nc.write(R_EXEC_CMD,1);
        expect(nc.read(R_READ_ID,4)==NAND_ID,"FETCH_ID latches 0x5580b1ad");
        nc.write(R_ADDR0,0); nc.write(R_FLASH_CMD,CMD_PAGE_READ); nc.write(R_EXEC_CMD,1);
        expect(std::memcmp(nc.buffer().data(),ref.data(),16)==0,"PAGE_READ[0] matches dump first16");
        nc.write(R_ADDR0,11712); nc.write(R_FLASH_CMD,CMD_PAGE_READ_ECC); nc.write(R_EXEC_CMD,1);
        expect(std::memcmp(nc.buffer().data(),ref.data()+11712*2048,4)==0,"PAGE_READ[11712]=APPSBL vector (18f09fe5)");
        expect((nc.read(R_FLASH_STATUS,4)&FS_READY),"status READY after read");
    }

    // 2. full DMA page read through DMOVModel (page 11712 = APPSBL), like mini_boot
    {
        uc_engine* uc; uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc);
        uc_mem_map(uc,0x00400000,0x100000,UC_PROT_ALL);   // io struct RAM
        uc_mem_map(uc,0x10000000,0x100000,UC_PROT_ALL);   // dest RAM
        NandController nc(data,spare);
        DMOVModel dm(uc,nc);
        u32 IO=0x00400000, CL=0x00401000, PTR=0x00402000, DEST=0x10000000;
        u32 page=11712;
        auto w=[&](u32 a,u32 v){ uc_mem_write(uc,a,(const void*)&v,4); };
        w(IO+0x00,0x33);                       // PAGE_READ_ECC
        w(IO+0x04,(page<<16)&0xFFFFFFFF);      // addr0=page<<16
        w(IO+0x08,(page>>16)&0xFF);            // addr1
        w(IO+0x0c,0|4);                        // chipsel
        w(IO+0x10,0xa25400c0); w(IO+0x14,0x0004745e); // cfg0/cfg1
        w(IO+0x18,1); w(IO+0x1c,0x203); w(IO+0x20,0);
        struct { u32 cmd,src,dst,len; } cmdseq[8] = {
            {5<<7, IO+0x00, NAND_BASE+0x00, 16},
            {0,    IO+0x10, NAND_BASE+0x20, 8},
            {0,    IO+0x18, NAND_BASE+0x10, 4},
            {4<<3, NAND_BASE+0x14, IO+0x24, 8},
            {0,    NAND_FLASH_BUFFER, DEST+0,    512},
            {0,    NAND_FLASH_BUFFER, DEST+512,  512},
            {0,    NAND_FLASH_BUFFER, DEST+1024, 512},
            {CMD_LC, NAND_FLASH_BUFFER, DEST+1536,512},
        };
        for(int i=0;i<8;i++) uc_mem_write(uc,CL+i*16,(const void*)&cmdseq[i],16);
        w(PTR,(CL>>3)|CMD_PTR_LP);
        u32 dv = ((PTR>>3))|CMD_PTR_LP;
        dm.exec_cmdptr(dv);
        u8 got[16]; uc_mem_read(uc,DEST,got,16);
        const u8* exp=ref.data()+page*2048;
        expect(std::memcmp(got,exp,16)==0,"DMOV DMA page read (11712) == dump");

        // also check FULL 2048 page
        u8 gotfull[2048]; uc_mem_read(uc,DEST,gotfull,2048);
        expect(std::memcmp(gotfull,ref.data()+page*2048,2048)==0,"DMOV full 2048B page matches dump");
        printf("  DMOV execs=%u  got[0..4]=%02x %02x %02x %02x\n",
               dm.exec_count(),got[0],got[1],got[2],got[3]);
        uc_close(uc);
    }
    printf("\nall device-model C++ tests done\n");
    return 0;
}