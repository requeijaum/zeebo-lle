// zeebo_devices.h — C++23 port of the Zeebo LLE device models:
//   NandController (MSM7201A EBI2 flash controller, dump-backed)
//   DMOVModel     (functional ADM/DMOV DMA, executes nand.c descriptor lists)
// Ported faithfully from tools/nand_controller.py + tools/dmov_model.py.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <unicorn/unicorn.h>

using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;

// ---- NAND register offsets (nand.h) ----
enum {
    NAND_BASE=0xA0A00000,
    R_FLASH_CMD=0x0000, R_ADDR0=0x0004, R_ADDR1=0x0008, R_CHIP_SELECT=0x000C,
    R_EXEC_CMD=0x0010, R_FLASH_STATUS=0x0014, R_BUFFER_STATUS=0x0018,
    R_DEV0_CFG0=0x0020, R_DEV0_CFG1=0x0024, R_READ_ID=0x0040, R_READ_STATUS=0x0044,
    R_CONFIG_DATA=0x0050, R_CONFIG=0x0054, R_CONFIG_MODE=0x0058, R_CONFIG_STATUS=0x0060,
    R_DEV_CMD_VLD=0x00AC, R_EBI2_ECC_BUF_CFG=0x00F0, R_FLASH_BUFFER=0x0100,
    CMD_SOFT_RESET=0x01, CMD_PAGE_READ=0x32, CMD_PAGE_READ_ECC=0x33, CMD_PAGE_READ_ALL=0x34,
    CMD_FETCH_ID=0x0B, CMD_STATUS=0x0C, CMD_RESET=0x0D,
    FS_READY=1u<<6, FS_OP_ERR=1u<<4,
    PAGE_DATA=2048, PAGE_SPARE=64, PAGE_FULL=2112,
    NAND_ID=0x5580b1ad,
};

class NandController {
public:
    NandController(const std::string& data_path, const std::string& spare_path="",
                   u32 nand_id=NAND_ID)
        : nand_id_(nand_id), status_(FS_READY), last_cmd_(0), id_latched_(0) {
        read_file(data_path, data_blob_);
        if (!spare_path.empty()) read_file(spare_path, blob_);
        else blob_ = data_blob_;
        // real CFG defaults (else flash_read_config returns -1 -> boot stalls)
        reg_[R_DEV0_CFG0]=0xa25400c0;
        reg_[R_DEV0_CFG1]=0x0004745e;
        buffer_.assign(PAGE_FULL,0);
    }
    static bool read_file(const std::string&p, std::vector<u8>&out){
        std::ifstream f(p,std::ios::binary|std::ios::ate);
        if(!f) return false;
        auto sz=f.tellg(); f.seekg(0); out.resize((size_t)sz); f.read((char*)out.data(),sz);
        return true;
    }

    u32 read(u32 off, int size){
        if(off==R_FLASH_STATUS||off==R_READ_STATUS) return status_;
        if(off==R_BUFFER_STATUS) return 0;
        if(off==R_READ_ID) return id_latched_;
        if(off==R_CONFIG_STATUS) return 1;
        if(off>=R_FLASH_BUFFER&&off<R_FLASH_BUFFER+PAGE_FULL){
            u32 i=off-R_FLASH_BUFFER;
            u32 v=0;
            for(int k=3;k>=0 && (int)(i+k)<(int)buffer_.size();k--) v=(v<<8)|buffer_[i+k];
            return v;
        }
        auto it=reg_.find(off); return it==reg_.end()?0:it->second;
    }
    void write(u32 off, u32 val, int size=4){
        reg_[off]=val&0xFFFFFFFF;
        if(off==R_FLASH_CMD) last_cmd_=val&0xFF;
        else if(off==R_EXEC_CMD) execute_(last_cmd_);
        else if(off>=R_FLASH_BUFFER&&off<R_FLASH_BUFFER+PAGE_FULL){
            u32 i=off-R_FLASH_BUFFER;
            for(int k=0;k<size && (int)(i+k)<(int)buffer_.size();k++) buffer_[i+k]=(u8)(val>>(8*k));
        }
    }
    // access to the page buffer (for DMOV cursor reads at byte granularity)
    u8 buffer_byte(u32 idx) const { return idx<buffer_.size()?buffer_[idx]:0; }
    const std::vector<u8>& buffer() const { return buffer_; }
    u32 debug_last_cmd() const { return last_cmd_; }

private:
    void execute_(u32 cmd){
        if(cmd==CMD_FETCH_ID){ id_latched_=nand_id_; status_=FS_READY; return; }
        if(cmd==CMD_PAGE_READ||cmd==CMD_PAGE_READ_ECC||cmd==CMD_PAGE_READ_ALL){
            u32 addr0=reg_[R_ADDR0], addr1=reg_[R_ADDR1];
            u32 page = addr0>=0x10000 ? (addr0>>16)|((addr1&0xFF)<<8) : addr0;
            size_t npages = data_blob_.size()/PAGE_DATA;
            if(page<npages){
                for(int i=0;i<PAGE_DATA;i++) buffer_[i]=data_blob_[page*PAGE_DATA+i];
                status_=FS_READY;
            } else status_=FS_OP_ERR;
            return;
        }
        status_=FS_READY;  // reset/status/unknown -> ready
    }
    u32 nand_id_; u32 status_; u32 last_cmd_; u32 id_latched_;
    std::vector<u8> data_blob_, blob_, buffer_;
    std::map<u32,u32> reg_;
};

// ---- DMOV (dmov.h) ----
enum {
    DMOV_SD1_BASE=0xa9400000,
    DMOV_CMD_PTR=0x000, DMOV_RSLT=0x040, DMOV_STATUS=0x200, DMOV_CONFIG=0x300,
    DMOV_NAND_CHAN=3,
    CMD_PTR_LP=1u<<31, CMD_LC=1u<<31,
    NAND_FLASH_BUFFER=NAND_BASE+0x100,
    DMOV_RSLT_DONE=0x80000002,
};
static u32 dmov_reg(u32 off,int ch){ return DMOV_SD1_BASE+off+(ch<<2); }

class DMOVModel {
public:
    DMOVModel(uc_engine* uc, NandController& nand): uc_(uc), nand_(nand){
        exec_count_=0; buf_cursor_=0; last_chan_=0;
    }
    u32 exec_cmdptr(u32 regval){
        exec_count_++;
        u32 pptr=(regval&0x7FFFFFFF)<<3;
        for(int idx=0;idx<16;idx++){
            u32 p=ram32(pptr);
            u32 cmdlist=(p&0x7FFFFFFF)<<3;
            bool plast=(p&CMD_PTR_LP)!=0;
            run_cmdlist(cmdlist);
            if(plast) break;
            if(p==0) break;
            pptr=cmdlist;
        }
        last_chan_=DMOV_NAND_CHAN;
        return DMOV_RSLT_DONE;
    }
    u32 exec_count() const{return exec_count_;}

private:
    u32 ram32(u32 a){ u8 b[4]={0,0,0,0}; uc_mem_read(uc_,a,b,4); 
        return (u32)b[0]|((u32)b[1]<<8)|((u32)b[2]<<16)|((u32)b[3]<<24); }
    bool is_nand(u32 a){ return a>=NAND_BASE && a<NAND_BASE+0x400; }
    void run_cmdlist(u32 base){
        for(int i=0;i<64;i++){
            u32 e=base+i*16;
            u32 cmd=ram32(e), src=ram32(e+4), dst=ram32(e+8), ln=ram32(e+12);
            exec_descriptor(cmd,src,dst,ln);
            if(cmd&CMD_LC) break;
        }
    }
    void exec_descriptor(u32 cmd,u32 src,u32 dst,u32 ln){
        bool sn=is_nand(src), dn=is_nand(dst);
        int length=std::min((int)ln,2048);
        if(sn){
            u32 off=src-NAND_BASE;
            if(src==NAND_FLASH_BUFFER){
                // drain-cursor window: each DMA reads the NEXT `len` bytes
                for(int i=0;i<length;i+=4){
                    u32 cur=buf_cursor_;
                    u32 v=nand_.read(off+cur+i,4);
                    u8 b[4]={(u8)v,(u8)(v>>8),(u8)(v>>16),(u8)(v>>24)};
                    uc_mem_write(uc_,dst+i,b,4);
                }
                buf_cursor_=(buf_cursor_+length)%2048;
            } else {
                u32 v=nand_.read(off,std::min((int)ln,4));
                u8 b[4]={(u8)v,(u8)(v>>8),(u8)(v>>16),(u8)(v>>24)};
                uc_mem_write(uc_,dst,b,4);
            }
            return;
        }
        if(dn){
            u32 off=dst-NAND_BASE;
            if(dst==NAND_FLASH_BUFFER){
                for(int i=0;i<length;i+=4){ u32 v=ram32(src+i); nand_.write(off+i,v,4); }
            } else if(ln==16){
                for(int i=0;i<16;i+=4){
                    if((dst&~0x3)!=dst) break; // unaligned guard
                    u32 v=ram32(src+i);
                    nand_.write(off+i,v,4);
                    if((off+i)>=0x400) break;
                }
            } else {
                if(dst==NAND_BASE+0x10) buf_cursor_=0;  // EXEC resets cursor
                u32 v=ram32(src);
                nand_.write(off,v,std::min((int)ln,4));
            }
            return;
        }
        // RAM->RAM copy
        int n=std::min((int)ln,0x1000);
        u8 buf[0x1000];
        if(uc_mem_read(uc_,src,buf,n)==UC_ERR_OK) uc_mem_write(uc_,dst,buf,n);
    }
    uc_engine* uc_; NandController& nand_;
    u32 exec_count_, buf_cursor_, last_chan_;
};