#!/usr/bin/env python3
# atc_oracle.py — INDEPENDENT model/oracle for ATITC (ATC) decode, used ONLY to
# derive pinned test vectors for atitc_smoke.cpp. This is NOT production code and
# is deliberately kept separate from the C++ decoder.
#
# PROVENANCE (clean-room):
#   - Enums / block sizes: Khronos registry AMD_compressed_ATC_texture (public):
#       ATC_RGB_AMD                      = 0x8C92  (8 bytes/block, 4x4)
#       ATC_RGBA_EXPLICIT_ALPHA_AMD     = 0x8C93  (16 bytes/block)
#       ATC_RGBA_INTERPOLATED_ALPHA_AMD = 0x87EE  (16 bytes/block)
#   - Block bit-layout / interpolation math: Guild Software, Inc. (Ratelis &
#     Bergman, 2012) "A Method for Load-Time Conversion of DXTC Assets to ATC",
#     which documents the ATC block format from Chainfire's PUBLIC XDA-Developers
#     documentation (no reverse engineering, no NDA). License: publicly published
#     technical paper; algorithm/behavior transferred, not third-party source code.
#   - Alpha sub-blocks: DXT3 (4-bit explicit) and DXT5 (3-bit interpolated) alpha,
#     which are the public S3TC layouts.
#
# CANONICAL host conventions chosen (documented so C++ matches exactly):
#   - 5->8 bit expand: (v<<3)|(v>>2) ; 6->8 bit expand: (v<<2)|(v>>4)
#   - color interpolation on 8-bit channels, integer TRUNCATING division:
#       method0: 00=c0, 01=(2*c0+c1)/3, 10=(c0+2*c1)/3, 11=c1
#       method1: 00=black, 01=max(0,c0-c1//4), 10=c0, 11=c1
#   - texel bit order: row-major, LSB = leftmost texel (standard S3TC).

import struct

ATC_RGB   = 0x8C92
ATC_EXPL  = 0x8C93
ATC_INTP  = 0x87EE

def e5(v): return (v << 3) | (v >> 2)
def e6(v): return (v << 2) | (v >> 4)

def unpack_color0(w):   # XRRRRRGG GGGBBBBB (555 + method in MSB)
    method = (w >> 15) & 1
    r = (w >> 10) & 0x1F
    g = (w >> 5) & 0x1F
    b = w & 0x1F
    return method, (e5(r), e5(g), e5(b))

def unpack_color1(w):   # RRRRRGGG GGGBBBBB (565)
    r = (w >> 11) & 0x1F
    g = (w >> 5) & 0x3F
    b = w & 0x1F
    return (e5(r), e6(g), e5(b))

def lerp_rgb(method, t, c0, c1):
    if method == 0:
        if t == 0: return c0
        if t == 1: return tuple((2*c0[i]+c1[i])//3 for i in range(3))
        if t == 2: return tuple((c0[i]+2*c1[i])//3 for i in range(3))
        return c1
    else:
        if t == 0: return (0, 0, 0)
        if t == 1: return tuple(max(0, c0[i] - (c1[i]//4)) for i in range(3))
        if t == 2: return c0
        return c1

def decode_color_block(cb):
    # cb: 8 bytes. Returns list of 16 (r,g,b) in row-major texel order.
    w0 = cb[0] | (cb[1] << 8)
    w1 = cb[2] | (cb[3] << 8)
    method, c0 = unpack_color0(w0)
    c1 = unpack_color1(w1)
    idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | (cb[7] << 24)
    out = []
    for i in range(16):
        t = (idx >> (2*i)) & 3
        out.append(lerp_rgb(method, t, c0, c1))
    return out

def decode_explicit_alpha(ab):
    # ab: 8 bytes, 4 bits/texel (DXT3). texel i nibble; low nibble first.
    out = []
    for byte in ab:
        lo = byte & 0x0F
        hi = (byte >> 4) & 0x0F
        out.append(lo * 17)
        out.append(hi * 17)
    return out  # 16 alphas

def decode_interp_alpha(ab):
    # ab: 8 bytes. a0,a1 then 16*3-bit indices (DXT5).
    a0, a1 = ab[0], ab[1]
    bits = 0
    for k in range(6):
        bits |= ab[2+k] << (8*k)
    if a0 > a1:
        pal = [a0, a1] + [((6-j)*a0 + (1+j)*a1)//7 for j in range(6)]
    else:
        pal = [a0, a1] + [((4-j)*a0 + (1+j)*a1)//5 for j in range(4)] + [0, 255]
    out = []
    for i in range(16):
        out.append(pal[(bits >> (3*i)) & 7])
    return out

def decode_texture(fmt, w, h, data):
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    bpb = 8 if fmt == ATC_RGB else 16
    rgba = bytearray(w * h * 4)
    off = 0
    for by in range(bh):
        for bx in range(bw):
            block = data[off:off+bpb]
            off += bpb
            if fmt == ATC_RGB:
                colors = decode_color_block(block)
                alphas = [255]*16
            elif fmt == ATC_EXPL:
                alphas = decode_explicit_alpha(block[0:8])
                colors = decode_color_block(block[8:16])
            else:
                alphas = decode_interp_alpha(block[0:8])
                colors = decode_color_block(block[8:16])
            for ty in range(4):
                for tx in range(4):
                    px = bx*4 + tx
                    py = by*4 + ty
                    if px >= w or py >= h:
                        continue
                    ti = ty*4 + tx
                    r, g, b = colors[ti]
                    a = alphas[ti]
                    o = (py*w + px)*4
                    rgba[o], rgba[o+1], rgba[o+2], rgba[o+3] = r, g, b, a
    return bytes(rgba)

def fnv1a(data):
    h = 0x811c9dc5
    for byte in data:
        h ^= byte
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h

def hexlit(b):
    return ",".join("0x%02x" % x for x in b)

if __name__ == "__main__":
    # ---- Build deterministic test blocks (hand-chosen color words + index bytes) ----
    def color_block(w0, w1, idx32):
        return bytes([w0 & 0xFF, (w0>>8)&0xFF, w1 & 0xFF, (w1>>8)&0xFF,
                      idx32 & 0xFF, (idx32>>8)&0xFF, (idx32>>16)&0xFF, (idx32>>24)&0xFF])

    # V1: RGB method0. color0 555 with MSB=0. Pick red-ish c0, blue-ish c1.
    #   c0 word: method0, R=31,G=0,B=0 -> 0RRRRRGGGGGBBBBB = 0 11111 00000 00000
    w0_m0 = (0<<15)|(31<<10)|(0<<5)|0     # 0x7C00
    #   c1 565: R=0,G=0,B=31 -> 00000 000000 11111
    w1_565_blue = (0<<11)|(0<<5)|31       # 0x001F
    v1 = color_block(w0_m0, w1_565_blue, 0b11100100_11100100_11100100_11100100)
    # index pattern per byte 0b11100100 = pixels t0=0,t1=1,t2=2,t3=3

    # V2: RGB method1. c0 MSB=1.
    w0_m1 = (1<<15)|(20<<10)|(20<<5)|20    # method1 grayish
    w1_565 = (10<<11)|(20<<5)|10
    v2 = color_block(w0_m1, w1_565, 0b00011011_00011011_00011011_00011011)

    # V3: explicit alpha (DXT3) + color method0
    alpha_expl = bytes([0x0F,0xF0, 0x0F,0xF0, 0x0F,0xF0, 0x0F,0xF0])  # alt 255/0
    v3 = alpha_expl + color_block(w0_m0, w1_565_blue, 0)

    # V4: interpolated alpha (DXT5) + color method0. a0>a1 branch.
    alpha_intp = bytes([0xFF, 0x00, 0x00,0x00,0x00,0x00,0x00,0x00])  # all idx0 -> a0=255
    v4 = alpha_intp + color_block(w0_m0, w1_565_blue, 0x55555555)  # all t=1

    # V5: non-4x4 (6x6) RGB method0, 4 blocks, crop.
    b = color_block(w0_m0, w1_565_blue, 0)  # all t=0 -> c0 (red)
    v5data = b*4

    vectors = [
        ("RGB_M0", ATC_RGB, 4, 4, v1),
        ("RGB_M1", ATC_RGB, 4, 4, v2),
        ("EXPL",   ATC_EXPL, 4, 4, v3),
        ("INTP",   ATC_INTP, 4, 4, v4),
        ("CROP66", ATC_RGB, 6, 6, v5data),
    ]

    print("// AUTO-GENERATED by atc_oracle.py — pinned vectors (provenance in .py header).")
    for name, fmt, w, h, data in vectors:
        rgba = decode_texture(fmt, w, h, data)
        print(f"// {name} fmt=0x{fmt:04X} {w}x{h} blockbytes={len(data)} rgba_fnv1a=0x{fnv1a(rgba):08x}")
        print(f"//   in[{len(data)}]  = {{{hexlit(data)}}}")
        # print first 4 texels expected rgba
        first = rgba[:16]
        print(f"//   rgba[0..3] = {{{hexlit(first)}}}")
    # Emit a C++-usable dump for the corner texels used by the test.
    import json
    dump = {}
    for name, fmt, w, h, data in vectors:
        rgba = decode_texture(fmt, w, h, data)
        dump[name] = {"fmt": fmt, "w": w, "h": h,
                      "in": list(data), "rgba": list(rgba),
                      "fnv": fnv1a(rgba)}
    with open("/tmp/atc_vectors.json", "w") as f:
        json.dump(dump, f)
    print("// wrote /tmp/atc_vectors.json")
