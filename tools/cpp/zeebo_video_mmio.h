// zeebo_video_mmio.h — modelos de MDDI e Adreno 130 (extraidos de
// zeebo_lle_main.cpp sem alteracao de comportamento, para que os testes
// exercitem o codigo real do runtime em vez de uma copia).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#ifndef ZEEBO_VIDEO_MMIO_TYPES
#define ZEEBO_VIDEO_MMIO_TYPES
using u32 = uint32_t;
#endif
using u16 = uint16_t;

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

// ---- MDDI scanout (guest-faithful) --------------------------------------
//
// Contrato de evidencia (observado, nao copiado de fonte de terceiros):
// o guest escreve em PRI_PTR (0xaa600008) o ponteiro para uma lista ligada de
// `mddi_llentry`. O motor MDDI consome a lista via DMA da memoria do guest. Um
// pacote VIDEO_STREAM em formato RGB565 carrega um span xy e um ponteiro de
// dados de pixels RGB565 que sao copiados para a superficie de scanout.
enum : u32 {
    MDDI_VIDEO_STREAM = 16,      // packet type (VIDEO_STREAM)
    MDDI_FMT_RGB565   = 0x5565,  // pixel data format
};

// Layout na memoria do guest: 8 words de 32 bits por entrada.
struct mddi_llentry {
    u32 type;      // +0   packet type; VIDEO_STREAM=16
    u32 format;    // +4   pixel format; RGB565=0x5565
    u32 x, y;      // +8   +12  destino (canto superior esquerdo)
    u32 w, h;      // +16  +20  dimensoes do span em pixels
    u32 data_ptr;  // +24  ponteiro do guest para os pixels RGB565
    u32 next;      // +28  proxima entrada (0 termina a lista)
};

// Superficie de scanout 640x480 RGB565. Sem UI de host: apenas o buffer.
struct MddiScanoutSink {
    static constexpr u32 W = 640, H = 480;
    std::vector<u16> px = std::vector<u16>(W * H, 0);
    u32 regions_drawn = 0;
    u16 at(u32 x, u32 y) const { return (x < W && y < H) ? px[y * W + x] : 0; }
    void blit(u32 x0, u32 y0, u32 w, u32 h, const std::function<u16(u32)>& src) {
        for (u32 j = 0; j < h; ++j)
            for (u32 i = 0; i < w; ++i) {
                u32 x = x0 + i, y = y0 + j;
                if (x < W && y < H) px[y * W + x] = src(j * w + i);
            }
        regions_drawn++;
    }
};

// ---- Unified Hardware Models ----

// 1. Virtual MDDI Engine
class UnifiedMDDI {
public:
    UnifiedMDDI() : status_(0x21), version_(0x00000102), pri_ptr_(0), frame_count_(0),
                    sink_(nullptr) {}
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
            scanout(val);
        }
    }
    u32 frame_count() const { return frame_count_; }

    // Liga um leitor da memoria do guest (word de 32 bits alinhada) e um sink.
    // Nao ha leitura global do host: toda memoria vem por este callback.
    void attach_scanout(std::function<u32(u32)> reader, MddiScanoutSink* sink) {
        reader_ = std::move(reader);
        sink_ = sink;
    }
private:
    // Consome a lista ligada de mddi_llentry a partir do ponteiro do guest.
    void scanout(u32 head) {
        if (!head || !reader_ || !sink_) return; // PRI_PTR=0 nao toca o sink
        u32 at = head;
        for (int guard = 0; at && guard < 4096; ++guard) {
            mddi_llentry e{};
            e.type     = reader_(at + 0);
            e.format   = reader_(at + 4);
            e.x        = reader_(at + 8);
            e.y        = reader_(at + 12);
            e.w        = reader_(at + 16);
            e.h        = reader_(at + 20);
            e.data_ptr = reader_(at + 24);
            e.next     = reader_(at + 28);
            // So pacotes VIDEO_STREAM RGB565 alcancam o sink; o resto e ignorado.
            if (e.type == MDDI_VIDEO_STREAM && e.format == MDDI_FMT_RGB565 && e.data_ptr) {
                u32 base = e.data_ptr;
                sink_->blit(e.x, e.y, e.w, e.h, [this, base](u32 idx) -> u16 {
                    u32 a = base + idx * 2;
                    u32 word = reader_(a & ~3u);
                    return (a & 2u) ? (u16)(word >> 16) : (u16)(word & 0xffff);
                });
            }
            at = e.next;
        }
    }

    u32 status_, version_, pri_ptr_, frame_count_;
    std::map<u32, u32> regs_;
    std::function<u32(u32)> reader_;
    MddiScanoutSink* sink_;
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
