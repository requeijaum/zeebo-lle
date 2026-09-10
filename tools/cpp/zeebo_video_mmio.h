// zeebo_video_mmio.h — modelos de MDDI e Adreno 130 (extraidos de
// zeebo_lle_main.cpp sem alteracao de comportamento, para que os testes
// exercitem o codigo real do runtime em vez de uma copia).
#pragma once
#include <cstdint>
#include <map>

#ifndef ZEEBO_VIDEO_MMIO_TYPES
#define ZEEBO_VIDEO_MMIO_TYPES
using u32 = uint32_t;
#endif

enum : u32 {
    ZV_MSM_MDDI_BASE   = 0xaa600000,
    ZV_MDDI_SIZE       = 0x00010000,
    ZV_ADRENO130_BASE  = 0xa0000000,
    ZV_ADRENO130_SIZE  = 0x00100000,
    ZV_CHIP_ID_YAMATO  = 0x01030000,
};

#ifndef CHIP_ID_YAMATO
#define CHIP_ID_YAMATO ZV_CHIP_ID_YAMATO
#endif

// ---- Unified Hardware Models ----

// 1. Virtual MDDI Engine
class UnifiedMDDI {
public:
    UnifiedMDDI() : status_(0x21), version_(0x00000102), pri_ptr_(0), frame_count_(0) {}
    u32 read(u32 off) {
        if (off == 0x0004) return version_;
        if (off == 0x0028) return status_;
        if (off == 0x0008) return pri_ptr_;
        auto it = regs_.find(off); return it != regs_.end() ? it->second : 0;
    }
    void write(u32 off, u32 val) {
        regs_[off] = val;
        if (off == 0x0000) { // CMD
            if ((val & 0xff00) == 0x0200) status_ |= 0x01; // POWER_UP -> LINK_ACTIVE
            else if ((val & 0xff00) == 0x0400) status_ = 0x21; // RESET
        } else if (off == 0x0008) { // PRI_PTR
            pri_ptr_ = val;
            frame_count_++;
            status_ |= 0x20; // PRI_LIST_DONE
        }
    }
    u32 frame_count() const { return frame_count_; }
private:
    u32 status_, version_, pri_ptr_, frame_count_;
    std::map<u32, u32> regs_;
};

// 2. Virtual Adreno 130 GPU Engine
class UnifiedAdreno130 {
public:
    UnifiedAdreno130() : status_(1), rb_rptr_(0), rb_wptr_(0), int_status_(0), int_en_(0), draws_(0), fb_dirty_(false) {}
    u32 read(u32 off) {
        if (off == 0x0000) return CHIP_ID_YAMATO;
        if (off == 0x0004) return 0x00000001; // Rev 1
        if (off == 0x0110) return status_;
        if (off == 0x0108) return rb_rptr_;
        if (off == 0x010c) return rb_wptr_;
        if (off == 0x0120) return int_status_;
        auto it = regs_.find(off); return it != regs_.end() ? it->second : 0;
    }
    void write(u32 off, u32 val) {
        regs_[off] = val;
        if (off == 0x010c) { // WPTR
            rb_wptr_ = val;
            draws_ += (rb_wptr_ >= rb_rptr_) ? (rb_wptr_ - rb_rptr_) : (0x10000 - rb_rptr_ + rb_wptr_);
            rb_rptr_ = rb_wptr_;
            fb_dirty_ = true;
            if (int_en_ & 1) int_status_ |= 1;
        } else if (off == 0x0124) int_en_ = val;
        else if (off == 0x0128) int_status_ &= ~val;
    }
    u32 draws() const { return draws_; }
    bool is_fb_dirty() const { return fb_dirty_; }
    void clear_fb_dirty() { fb_dirty_ = false; }
    void mark_dirty() { fb_dirty_ = true; }
private:
    u32 status_, rb_rptr_, rb_wptr_, int_status_, int_en_, draws_;
    bool fb_dirty_;
    std::map<u32, u32> regs_;
};
