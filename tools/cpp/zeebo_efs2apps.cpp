// zeebo_efs2apps.cpp — Validate & instrument 0:EFS2APPS (blk 0x191, off 0x3220000)
// access through the NandController/DMOVModel (EBI2 + ADM/DMOV) path.
//
// Goal: prove that the same hardware DMA descriptor list the ARM11 FTL/NAND
// driver emits can read the EFS2APPS filesystem region AND the appmgr/ZeeboApp/
// CoreApp string+resource pages inside 0:APPS, byte-for-byte identical to the
// raw dump. This is the read path the BREW IFILE layer depends on to resolve
// fs:/mif/*, fs:/mod/*/* — no modem EFS-RPC involved.
//
// Build: g++ -std=c++23 -O2 -o zeebo_efs2apps zeebo_efs2apps.cpp -lunicorn
// Run:   ./zeebo_efs2apps
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <unicorn/unicorn.h>
#include "zeebo_devices.h"

static const char* NDPATH="/home/rafaelfrequiao/projects/zeebo-lle/nand";
static std::vector<u8> readfile(const std::string& p){ std::vector<u8> v; NandController::read_file(p,v); return v; }
static void wram(uc_engine*uc,u32 a,u32 v){ uc_mem_write(uc,a,(const void*)&v,4); }

// Emit the exact descriptor list nand.c uses to DMA one 2048B page to `dest`.
static void read_page(uc_engine*uc,DMOVModel&dm,u32 page,u32 dest){
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

static int g_pass=0, g_fail=0;
static void check(bool c,const char* what){ if(c){printf("  OK   %s\n",what);g_pass++;} else {printf("  FAIL %s\n",what);g_fail++;} }

// Read a byte-exact window [off,off+len) via successive DMA page reads, return it.
static std::vector<u8> dma_read_range(uc_engine*uc,DMOVModel&dm,u32 DEST,u64 off,u32 len){
    std::vector<u8> out; out.reserve(len);
    u64 first=off/2048, last=(off+len-1)/2048;
    for(u64 pg=first; pg<=last; pg++){
        read_page(uc,dm,(u32)pg,DEST);
        u8 buf[2048]; uc_mem_read(uc,DEST,buf,2048);
        u64 pstart=pg*2048;
        for(u32 i=0;i<2048;i++){ u64 abs=pstart+i; if(abs>=off && abs<off+len) out.push_back(buf[i]); }
    }
    return out;
}

int main(){
    std::string data=std::string(NDPATH)+"/1.1.2.bin";
    std::string spare=std::string(NDPATH)+"/1.1.2_spare.bin";
    auto ref=readfile(data);
    printf("== EFS2APPS DMA access validation ==\n");
    printf("dump size=%zu (%zu blocks of 128KiB)\n", ref.size(), ref.size()/0x20000);

    uc_engine* uc; uc_open(UC_ARCH_ARM,UC_MODE_ARM,&uc);
    uc_mem_map(uc,0x00400000,0x100000,UC_PROT_ALL);   // io/cmdlist/ptr scratch
    uc_mem_map(uc,0x10000000,0x100000,UC_PROT_ALL);   // DMA destination
    NandController nc(data,spare);
    DMOVModel dm(uc,nc);
    const u32 DEST=0x10000000;

    const u64 EFS2APPS_OFF=0x3220000; // blk 0x191
    const u64 APPS_OFF=0x1CC0000;     // blk 0xe6

    // 1. Partition-boundary sanity: first page of 0:EFS2APPS via DMA == dump.
    {
        auto got=dma_read_range(uc,dm,DEST,EFS2APPS_OFF,2048);
        check(got.size()==2048 && std::memcmp(got.data(),ref.data()+EFS2APPS_OFF,2048)==0,
              "0:EFS2APPS first page (blk 0x191 @0x3220000) DMA == dump");
    }
    // 2. First page of 0:APPS ELF via DMA == dump (holds appmgr code + fs:/ strings).
    {
        auto got=dma_read_range(uc,dm,DEST,APPS_OFF,2048);
        check(got.size()==2048 && std::memcmp(got.data(),ref.data()+APPS_OFF,2048)==0,
              "0:APPS first page (blk 0xe6 @0x1cc0000) DMA == dump");
    }

    // 3. Marker strings/dirents readable via the DMA path, byte-identical.
    struct M { const char* name; u64 off; const char* expect; };
    M markers[]={
        {"fs:/mif/brewappmgr.mif (0:APPS)",     0x2c285ee, "fs:/mif/brewappmgr.mif"},
        {"fs:/mod/brewappmgr/appmgrls.bar",     0x2fe9ffc, "fs:/mod/brewappmgr/appmg"},
        {"fs:/mod/brewappmgr/appmgrln.bar",     0x2fea01c, "fs:/mod/brewappmgr/appmg"},
        {"ZeeboApp AEEAppletNew log (0:APPS)",  0x212e2e8, "ZeeboApp: AEEAppletNew(A"},
        {"fs:/mod/coreapp/coreapp_qvga.bar",    0x1efd3bf, "fs:/mod/coreapp/coreapp_"},
        {"reksio.mod dirent (0:EFS2APPS)",      0x32606fb, "reksio.mod"},
        {"ZeeboApp ref inside EFS2APPS region", 0x3dc9051, "ZeeboApp"},
    };
    for(auto&m:markers){
        u32 len=(u32)strlen(m.expect);
        auto got=dma_read_range(uc,dm,DEST,m.off,len);
        bool ok = got.size()==len && std::memcmp(got.data(),m.expect,len)==0
                  && std::memcmp(got.data(),ref.data()+m.off,len)==0;
        char buf[128]; snprintf(buf,sizeof buf,"%s @0x%llx",m.name,(unsigned long long)m.off);
        check(ok,buf);
    }

    // 4. Bulk integrity: DMA-read 8 blocks spanning the EFS2APPS module/dirent
    //    region and compare byte-for-byte (proves sustained multi-page access).
    {
        u64 span_off=EFS2APPS_OFF; u32 span_len=8*0x20000; // 8 blocks = 1MiB
        bool allok=true; u64 mism=0;
        for(u32 p=0;p<(span_len/2048);p++){
            u64 pg=(span_off/2048)+p;
            read_page(uc,dm,(u32)pg,DEST);
            u8 b[2048]; uc_mem_read(uc,DEST,b,2048);
            if(std::memcmp(b,ref.data()+pg*2048,2048)!=0){ allok=false; mism++; }
        }
        char buf[128]; snprintf(buf,sizeof buf,"EFS2APPS 1MiB (%u pages) DMA byte-identical (mismatch pages=%llu)",
                                span_len/2048,(unsigned long long)mism);
        check(allok,buf);
    }

    printf("\nDMOV execs=%u  pass=%d fail=%d\n",dm.exec_count(),g_pass,g_fail);
    uc_close(uc);
    return g_fail?1:0;
}
