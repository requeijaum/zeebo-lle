// atitc_decode.h — Host-side decoder for AMD ATC / ATITC compressed textures
// (QW9). Independent clean-room implementation.
//
// PROVENANCE / LICENSE:
//   - Enums & block sizes: Khronos registry AMD_compressed_ATC_texture (public
//     OpenGL ES extension #40). Public spec, freely referenceable.
//   - Block bit-layout & interpolation math: independently reimplemented from the
//     public technical paper "A Method for Load-Time Conversion of DXTC Assets to
//     ATC" (Ratelis & Bergman, Guild Software, 2012), which documents the ATC
//     block format from Chainfire's PUBLIC XDA-Developers documentation. That
//     paper explicitly states "No reverse engineering or NDA documentation was
//     utilized." No third-party decoder SOURCE was copied; only the publicly
//     documented algorithm/behavior was reimplemented here from scratch.
//   - Alpha sub-blocks: public S3TC DXT3 (4-bit explicit) / DXT5 (3-bit
//     interpolated) alpha layouts.
//
// Expected vectors for the tests are derived by a SEPARATE oracle (atc_oracle.py),
// not by this code, so the tests are not tautological against the implementation.
//
// Canonical host output = tightly-packed RGBA8, row-major, y-down. Integer math
// only (documented truncating division) so results are exactly reproducible.
#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

namespace zeebo::gpu {

// Khronos public enums (AMD_compressed_ATC_texture). INTERPOLATED = 0x87EE.
constexpr uint32_t kATITC_RGB              = 0x8C92; // 8 bytes / 4x4 block
constexpr uint32_t kATITC_RGBA_EXPLICIT    = 0x8C93; // 16 bytes / block
constexpr uint32_t kATITC_RGBA_INTERP      = 0x87EE; // 16 bytes / block

inline int atitc_block_bytes(uint32_t fmt){
    if(fmt==kATITC_RGB) return 8;
    if(fmt==kATITC_RGBA_EXPLICIT || fmt==kATITC_RGBA_INTERP) return 16;
    return 0; // unknown format
}

// Required imageSize for (w,h) given a valid fmt. 0 if fmt invalid.
inline size_t atitc_image_size(uint32_t fmt,int w,int h){
    int bb=atitc_block_bytes(fmt);
    if(bb==0 || w<=0 || h<=0) return 0;
    size_t bw=size_t((w+3)/4), bh=size_t((h+3)/4);
    return bw*bh*size_t(bb);
}

namespace atitc_detail {

inline uint8_t e5(uint32_t v){ return uint8_t((v<<3)|(v>>2)); }
inline uint8_t e6(uint32_t v){ return uint8_t((v<<2)|(v>>4)); }

struct RGB { uint8_t r,g,b; };

// color0 word: X RRRRR GGGGG BBBBB (555 + method in MSB, per Guild paper).
inline void unpack_c0(uint16_t w,int& method,RGB& c){
    method=(w>>15)&1;
    c.r=e5((w>>10)&0x1F); c.g=e5((w>>5)&0x1F); c.b=e5(w&0x1F);
}
// color1 word: RRRRR GGGGGG BBBBB (565).
inline RGB unpack_c1(uint16_t w){
    return RGB{ e5((w>>11)&0x1F), e6((w>>5)&0x3F), e5(w&0x1F) };
}
inline uint8_t mix(int a,int wa,int b,int wb,int denom){ return uint8_t((wa*a+wb*b)/denom); }

inline RGB lerp(int method,int t,const RGB& c0,const RGB& c1){
    if(method==0){
        switch(t){
            case 0: return c0;
            case 1: return RGB{ mix(c0.r,2,c1.r,1,3), mix(c0.g,2,c1.g,1,3), mix(c0.b,2,c1.b,1,3) };
            case 2: return RGB{ mix(c0.r,1,c1.r,2,3), mix(c0.g,1,c1.g,2,3), mix(c0.b,1,c1.b,2,3) };
            default:return c1;
        }
    } else {
        switch(t){
            case 0: return RGB{0,0,0};
            case 1: { auto sub=[](int a,int b){ int r=a-(b/4); return uint8_t(r<0?0:r); };
                      return RGB{ sub(c0.r,c1.r), sub(c0.g,c1.g), sub(c0.b,c1.b) }; }
            case 2: return c0;
            default:return c1;
        }
    }
}

// Decode one 8-byte color sub-block into 16 texels (row-major, LSB=leftmost).
inline void decode_colors(const uint8_t* cb,RGB out[16]){
    uint16_t w0=uint16_t(cb[0]|(cb[1]<<8));
    uint16_t w1=uint16_t(cb[2]|(cb[3]<<8));
    int method; RGB c0; unpack_c0(w0,method,c0);
    RGB c1=unpack_c1(w1);
    uint32_t idx=uint32_t(cb[4])|(uint32_t(cb[5])<<8)|(uint32_t(cb[6])<<16)|(uint32_t(cb[7])<<24);
    for(int i=0;i<16;i++) out[i]=lerp(method,(idx>>(2*i))&3,c0,c1);
}

// DXT3 explicit alpha: 4 bits/texel, low nibble first.
inline void decode_alpha_explicit(const uint8_t* ab,uint8_t out[16]){
    for(int i=0;i<8;i++){ out[i*2]=uint8_t((ab[i]&0x0F)*17); out[i*2+1]=uint8_t(((ab[i]>>4)&0x0F)*17); }
}
// DXT5 interpolated alpha: a0,a1 + 16*3-bit indices.
inline void decode_alpha_interp(const uint8_t* ab,uint8_t out[16]){
    int a0=ab[0],a1=ab[1]; uint8_t pal[8];
    pal[0]=uint8_t(a0); pal[1]=uint8_t(a1);
    if(a0>a1){ for(int j=0;j<6;j++) pal[2+j]=uint8_t(((6-j)*a0+(1+j)*a1)/7); }
    else { for(int j=0;j<4;j++) pal[2+j]=uint8_t(((4-j)*a0+(1+j)*a1)/5); pal[6]=0; pal[7]=255; }
    uint64_t bits=0; for(int k=0;k<6;k++) bits|=uint64_t(ab[2+k])<<(8*k);
    for(int i=0;i<16;i++) out[i]=pal[(bits>>(3*i))&7];
}

} // namespace atitc_detail

// Decode an ATC/ATITC image to tightly-packed RGBA8 (row-major, w*h*4 bytes).
// Returns false WITHOUT touching `out` on any malformed input (invalid fmt,
// non-positive dims, null data, or imageSize < required). No fallback/no partial.
inline bool decode_atitc(uint32_t fmt,int w,int h,const uint8_t* data,size_t data_size,
                         std::vector<uint8_t>& out){
    using namespace atitc_detail;
    const int bb=atitc_block_bytes(fmt);
    if(bb==0) return false;
    if(w<=0 || h<=0) return false;
    if(data==nullptr) return false;
    const size_t need=atitc_image_size(fmt,w,h);
    if(need==0 || data_size<need) return false;

    std::vector<uint8_t> pixels(size_t(w)*size_t(h)*4);
    const int bw=(w+3)/4, bh=(h+3)/4;
    size_t off=0;
    for(int by=0;by<bh;by++){
        for(int bx=0;bx<bw;bx++){
            const uint8_t* blk=data+off; off+=size_t(bb);
            RGB colors[16]; uint8_t alphas[16];
            if(fmt==kATITC_RGB){
                decode_colors(blk,colors);
                for(int i=0;i<16;i++) alphas[i]=255;
            } else if(fmt==kATITC_RGBA_EXPLICIT){
                decode_alpha_explicit(blk,alphas);
                decode_colors(blk+8,colors);
            } else { // kATITC_RGBA_INTERP
                decode_alpha_interp(blk,alphas);
                decode_colors(blk+8,colors);
            }
            for(int ty=0;ty<4;ty++){
                int py=by*4+ty; if(py>=h) continue;
                for(int tx=0;tx<4;tx++){
                    int px=bx*4+tx; if(px>=w) continue;
                    const RGB& c=colors[ty*4+tx];
                    size_t o=(size_t(py)*size_t(w)+size_t(px))*4;
                    pixels[o]=c.r; pixels[o+1]=c.g; pixels[o+2]=c.b; pixels[o+3]=alphas[ty*4+tx];
                }
            }
        }
    }
    out.swap(pixels);
    return true;
}

} // namespace zeebo::gpu
