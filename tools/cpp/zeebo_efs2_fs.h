// zeebo_efs2_fs.h — Read-only parser for the Zeebo 0:EFS2APPS partition.
//
// The APPS-side EFS2 filesystem (the BREW `fs:/` tree) lives in the NAND dump
// `nand/1.1.2.bin` starting at partition offset 0x3220000 (block 0x191). This
// header models, from BYTES PROVEN in the dump (never instruction counts):
//
//   * Dirent records — each begins with the marker 0x69:
//       0x69 [inode:u32][reclen:u8][type:u8][parent_ref:u32][pad:0x00]
//            [name: reclen-5 bytes]
//     `parent_ref` encodes filiation as (parent_inode << 8) | tag
//     (tag = 0x64 or 0x00). Confirmed against dirents for `.qxt`, `.qxm`,
//     `reksio.mod`, `274755`, etc. (69,634 records recovered from 1.1.2.bin).
//
//   * Data clusters — file payload lives in 512-byte clusters addressed as
//       offset_nand = 0x3220000 + cluster_id * 512.
//
//   * Indirect blocks — a cluster holding a table of u32 cluster pointers that
//     chains a file's data clusters (e.g. block @0x3b1d400 lists clusters
//     0x6d11, 0x6d1d, 0x6d59, ...). Terminator/hole = 0xFFFFFFFF.
//
// Pure host code: no Unicorn dependency; reads the raw dump directly. The DMA
// (NandController/DMOVModel) read path is validated separately by
// zeebo_efs2apps.cpp.
//
// Build: g++ -std=c++23 -O2 -c ... (header-only, include from a .cpp)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <limits>
#include <unordered_map>

namespace efs2 {

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Partition geometry (proven in nand/1.1.2.bin).
static constexpr u64 EFS2APPS_OFF   = 0x3220000ULL; // block 0x191
static constexpr u32 CLUSTER_SIZE   = 512;
static constexpr u8  DIRENT_MARKER  = 0x69;
static constexpr u32 CLUSTER_NONE   = 0xFFFFFFFFu;

struct Dirent {
    u64 file_off;     // absolute offset of the 0x69 marker in the dump
    u32 inode;
    u8  reclen;
    u8  type;
    u32 parent_ref;   // (parent_inode << 8) | tag
    std::string name;

    u32 parent_inode() const { return parent_ref >> 8; }
    u8  parent_tag()   const { return parent_ref & 0xFF; }
};

class Efs2Filesystem {
public:
    // Open the NAND dump and cache the EFS2APPS partition region [off, end).
    bool open(const std::string& nand_path, u64 part_off = EFS2APPS_OFF) {
        // A failed reopen must not expose the previous NAND/index.
        buf_.clear();
        dirents_.clear();
        by_key_.clear();
        by_inode_.clear();
        part_len_ = 0;
        part_off_ = part_off;
        std::ifstream f(nand_path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        const std::streamoff end = f.tellg();
        if (end < 0) return false;
        const u64 sz = static_cast<u64>(end);
        if (part_off_ >= sz) return false;
        part_len_ = sz - part_off_;
        if (part_len_ > static_cast<u64>(std::numeric_limits<size_t>::max())) {
            part_len_ = 0;
            return false;
        }
        buf_.resize(static_cast<size_t>(part_len_));
        f.seekg(static_cast<std::streamoff>(part_off_), std::ios::beg);
        f.read(reinterpret_cast<char*>(buf_.data()), static_cast<std::streamsize>(part_len_));
        if (static_cast<u64>(f.gcount()) != part_len_) {
            buf_.clear();
            part_len_ = 0;
            return false;
        }
        return true;
    }

    u64 partition_offset() const { return part_off_; }
    u64 partition_size()   const { return part_len_; }
    const std::vector<u8>& data() const { return buf_; }

    // Scan the whole partition for well-formed dirent records. Builds an
    // O(1) index keyed by (parent_inode, name) and by inode.
    size_t scan_dirents() {
        dirents_.clear(); by_key_.clear(); by_inode_.clear();
        const u8* d = buf_.data();
        u64 L = part_len_;
        for (u64 i = 0; i + 12 <= L; ) {
            if (d[i] != DIRENT_MARKER) { i++; continue; }
            u8 reclen = d[i + 5];
            if (reclen < 6 || reclen > 200) { i++; continue; }
            // name_len = reclen - 5: o reclen conta apenas [type(1) + parent_ref(4)]
            // como cabeçalho de nome, NÃO o byte de pad em i+11. O pad é um byte de
            // alinhamento separado (validado abaixo == 0x00), fora da contagem do
            // reclen. É a fonte de confusão clássica ao reimplementar este parser.
            u32 name_len = (u32)reclen - 5;
            if (i + 12 + name_len > L) { i++; continue; }
            if (d[i + 11] != 0x00) { i++; continue; } // pad
            const u8* nm = d + i + 12;
            bool printable = true;
            for (u32 k = 0; k < name_len; k++)
                if (nm[k] < 32 || nm[k] >= 127) { printable = false; break; }
            if (!printable) { i++; continue; }

            Dirent e;
            e.file_off   = part_off_ + i;
            e.inode      = rd32(d + i + 1);
            e.reclen     = reclen;
            e.type       = d[i + 6];
            e.parent_ref = rd32(d + i + 7);
            e.name.assign((const char*)nm, name_len);
            size_t idx = dirents_.size();
            by_key_[key(e.parent_inode(), e.name)] = idx;
            by_inode_.emplace(e.inode, idx);
            dirents_.push_back(std::move(e));
            i += 12 + name_len;
        }
        return dirents_.size();
    }

    const std::vector<Dirent>& dirents() const { return dirents_; }

    // O(1) lookup by (parent_inode, name).
    const Dirent* find(u32 parent_inode, const std::string& name) const {
        auto it = by_key_.find(key(parent_inode, name));
        return it == by_key_.end() ? nullptr : &dirents_[it->second];
    }
    // First dirent matching a bare filename (any parent).
    const Dirent* find_by_name(const std::string& name) const {
        for (auto& e : dirents_) if (e.name == name) return &e;
        return nullptr;
    }
    const Dirent* find_by_inode(u32 inode) const {
        auto it = by_inode_.find(inode);
        return it == by_inode_.end() ? nullptr : &dirents_[it->second];
    }

    // Read one 512-byte data cluster by its EFS2 cluster id.
    // offset_nand = part_off + cluster_id * 512.
    bool read_cluster(u32 cluster_id, u8 out[CLUSTER_SIZE]) const {
        u64 off = (u64)cluster_id * CLUSTER_SIZE;
        if (off + CLUSTER_SIZE > part_len_) return false;
        std::memcpy(out, buf_.data() + off, CLUSTER_SIZE);
        return true;
    }

    // Read an indirect block (a cluster full of u32 cluster pointers) located
    // at an ABSOLUTE dump offset. Returns the pointer list, stopping at the
    // first 0xFFFFFFFF terminator when stop_at_terminator is set.
    std::vector<u32> read_indirect_block_at(u64 abs_off,
                                            bool stop_at_terminator = true) const {
        std::vector<u32> ptrs;
        if (abs_off < part_off_) return ptrs;
        u64 rel = abs_off - part_off_;
        if (rel + CLUSTER_SIZE > part_len_) return ptrs;
        const u8* p = buf_.data() + rel;
        for (u32 k = 0; k < CLUSTER_SIZE / 4; k++) {
            u32 c = rd32(p + k * 4);
            if (stop_at_terminator && c == CLUSTER_NONE) break;
            ptrs.push_back(c);
        }
        return ptrs;
    }

    // Gather file payload by following an indirect block's cluster chain.
    // `nbytes` caps the output (files are not necessarily cluster-aligned).
    std::vector<u8> read_data_from_indirect(u64 indirect_abs_off,
                                            u64 nbytes = 0) const {
        std::vector<u8> out;
        auto clusters = read_indirect_block_at(indirect_abs_off);
        u8 c[CLUSTER_SIZE];
        for (u32 cid : clusters) {
            if (cid == CLUSTER_NONE) break;
            if (!read_cluster(cid, c)) break;
            out.insert(out.end(), c, c + CLUSTER_SIZE);
            if (nbytes && out.size() >= nbytes) break;
        }
        if (nbytes && out.size() > nbytes) out.resize(nbytes);
        return out;
    }

    static u32 checksum32(const std::vector<u8>& v) {
        u32 s = 0x811c9dc5u; // FNV-1a
        for (u8 b : v) { s ^= b; s *= 0x01000193u; }
        return s;
    }

private:
    static u32 rd32(const u8* p) {
        return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    }
    static std::string key(u32 parent_inode, const std::string& name) {
        std::string k; k.reserve(name.size() + 5);
        k.push_back((char)(parent_inode & 0xFF));
        k.push_back((char)((parent_inode >> 8) & 0xFF));
        k.push_back((char)((parent_inode >> 16) & 0xFF));
        k.push_back((char)((parent_inode >> 24) & 0xFF));
        k.push_back(':'); k += name; return k;
    }

    u64 part_off_ = EFS2APPS_OFF;
    u64 part_len_ = 0;
    std::vector<u8> buf_;
    std::vector<Dirent> dirents_;
    std::unordered_map<std::string, size_t> by_key_;
    std::unordered_multimap<u32, size_t>    by_inode_;
};

} // namespace efs2
