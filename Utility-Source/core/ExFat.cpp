// Minimal exFAT formatter, written from the Microsoft exFAT specification:
// https://learn.microsoft.com/windows/win32/fileio/exfat-specification
//
// Section references below (§) are to that document.
#include "ExFat.h"

#include "BlockDevice.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>

namespace glassboot {
namespace {


void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }
uint64_t divUp(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

// Default cluster sizes used by Windows for exFAT.
uint32_t clusterBytesFor(uint64_t volumeBytes) {
    if (volumeBytes <= 256ull << 20) return 4096;
    if (volumeBytes <= 32ull << 30) return 32768;
    return 131072;
}

// ---------------------------------------------------------------- up-case table (§7.2)
// Maps ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic and fullwidth Latin. Characters
// not listed map to themselves, which the spec permits.
std::vector<uint16_t> upcaseMap() {
    std::vector<uint16_t> m(0x10000);
    for (uint32_t i = 0; i < 0x10000; ++i) m[i] = uint16_t(i);
    auto range = [&](uint32_t lo, uint32_t hi, int delta) {
        for (uint32_t c = lo; c <= hi; ++c) m[c] = uint16_t(int(c) + delta);
    };
    auto pairs = [&](uint32_t lo, uint32_t hi, uint32_t lowerParity) {
        for (uint32_t c = lo; c <= hi; ++c)
            if ((c & 1) == lowerParity) m[c] = uint16_t(c - 1);
    };
    range('a', 'z', -0x20);
    range(0xE0, 0xFE, -0x20);
    m[0xF7] = 0xF7; // division sign
    m[0xFF] = 0x178;
    pairs(0x100, 0x12F, 1);
    pairs(0x132, 0x137, 1);
    pairs(0x139, 0x148, 0);
    pairs(0x14A, 0x177, 1);
    pairs(0x179, 0x17E, 0);
    range(0x3B1, 0x3C1, -0x20);
    m[0x3C2] = 0x3A3; // final sigma
    range(0x3C3, 0x3CB, -0x20);
    range(0x430, 0x44F, -0x20);
    range(0x450, 0x45F, -0x50);
    range(0xFF41, 0xFF5A, -0x20);
    return m;
}

// §7.2.5: runs of identity mappings are compressed as 0xFFFF, <run length>.
std::vector<uint8_t> compressUpcase(const std::vector<uint16_t>& m) {
    std::vector<uint16_t> words;
    for (uint32_t i = 0; i < 0x10000;) {
        if (m[i] == i) {
            uint32_t j = i;
            while (j < 0x10000 && m[j] == j) ++j;
            uint32_t run = j - i;
            if (run == 1 && i != 0xFFFF) {
                words.push_back(uint16_t(i));
            } else {
                words.push_back(0xFFFF);
                words.push_back(uint16_t(run == 0x10000 ? 0xFFFF : run));
            }
            i = j;
        } else {
            words.push_back(m[i]);
            ++i;
        }
    }
    std::vector<uint8_t> bytes(words.size() * 2);
    for (size_t i = 0; i < words.size(); ++i) put16(&bytes[i * 2], words[i]);
    return bytes;
}

uint32_t tableChecksum(const std::vector<uint8_t>& t) { // §7.2.2
    uint32_t c = 0;
    for (uint8_t b : t) c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + b;
    return c;
}

uint32_t bootChecksum(const uint8_t* sectors, size_t bytes) { // §3.4
    uint32_t c = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (i == 106 || i == 107 || i == 112) continue; // VolumeFlags, PercentInUse
        c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + sectors[i];
    }
    return c;
}

uint16_t entrySetChecksum(const uint8_t* e, size_t bytes) { // §6.3.3
    uint16_t c = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (i == 2 || i == 3) continue;
        c = uint16_t(((c & 1) ? 0x8000 : 0) + (c >> 1) + e[i]);
    }
    return c;
}

uint16_t nameHash(const std::u16string& name, const std::vector<uint16_t>& up) { // §7.6.4
    uint16_t h = 0;
    for (char16_t ch : name) {
        uint16_t u = up[ch];
        for (uint8_t b : {uint8_t(u & 0xff), uint8_t(u >> 8)}) h = uint16_t(((h & 1) ? 0x8000 : 0) + (h >> 1) + b);
    }
    return h;
}

uint32_t nowTimestamp() { // §7.4.8, stored as UTC
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    return uint32_t(tm.tm_year + 1900 - 1980) << 25 | uint32_t(tm.tm_mon + 1) << 21 | uint32_t(tm.tm_mday) << 16 |
           uint32_t(tm.tm_hour) << 11 | uint32_t(tm.tm_min) << 5 | uint32_t(tm.tm_sec / 2);
}

std::u16string toU16(const std::string& s) {
    std::u16string r;
    for (unsigned char c : s) {
        if (c >= 0x80) throw Error("exFAT seed names must be ASCII");
        r += char16_t(c);
    }
    return r;
}

// File + Stream Extension + File Name entries for one file or directory (§7.4-7.7).
std::vector<uint8_t> entrySet(const std::string& name8, bool directory, uint32_t firstCluster, uint64_t length,
                              const std::vector<uint16_t>& up, uint32_t ts) {
    std::u16string name = toU16(name8);
    if (name.empty() || name.size() > 255) throw Error("bad exFAT file name");
    size_t nameEntries = divUp(name.size(), 15);
    std::vector<uint8_t> e(32 * (2 + nameEntries), 0);

    uint8_t* f = &e[0];
    f[0] = 0x85;
    f[1] = uint8_t(1 + nameEntries);
    put16(f + 4, directory ? 0x10 : 0x20);
    put32(f + 8, ts);
    put32(f + 12, ts);
    put32(f + 16, ts);
    f[22] = f[23] = f[24] = 0x80; // UTC offsets valid, +00:00

    uint8_t* s = &e[32];
    s[0] = 0xC0;
    s[1] = firstCluster ? 0x01 : 0x00; // AllocationPossible; clusters are chained in the FAT
    s[3] = uint8_t(name.size());
    put16(s + 4, nameHash(name, up));
    put64(s + 8, length);  // ValidDataLength
    put32(s + 20, firstCluster);
    put64(s + 24, length); // DataLength

    for (size_t i = 0; i < nameEntries; ++i) {
        uint8_t* n = &e[64 + 32 * i];
        n[0] = 0xC1;
        for (size_t j = 0; j < 15 && i * 15 + j < name.size(); ++j) put16(n + 2 + 2 * j, name[i * 15 + j]);
    }
    put16(f + 2, entrySetChecksum(e.data(), e.size()));
    return e;
}

} // namespace

void formatExFat(BlockDevice& dev, const ExFatOptions& opt, const std::function<void(double)>& progress) {
    const uint32_t kSector = opt.bytesPerSector;
    if ((kSector != 512 && kSector != 4096) || opt.partitionOffsetBytes % kSector || opt.partitionSectors % (kSector / 512))
        throw Error("unsupported or unaligned exFAT sector geometry");
    const uint64_t volSectors = opt.partitionSectors / (kSector / 512);
    const uint64_t volBytes = volSectors * kSector;
    if (volBytes < (64ull << 20)) throw Error("exFAT volume too small");
    if (opt.label.size() > 11) throw Error("exFAT label longer than 11 characters");

    // ------------------------------------------------------------ geometry (§3.1)
    const uint32_t clusterBytes = clusterBytesFor(volBytes);
    const uint32_t spc = clusterBytes / kSector;
    uint8_t spcShift = 0;
    while ((1u << spcShift) < spc) ++spcShift;
    const uint32_t align = std::max<uint32_t>(spc, (1 << 20) / kSector);

    const uint32_t fatOffset = (1 << 20) / kSector;
    const uint64_t maxClusters = (volSectors - fatOffset) / spc;
    const uint32_t fatLength = uint32_t(divUp((maxClusters + 2) * 4, kSector));
    const uint32_t heapOffset = uint32_t(divUp(uint64_t(fatOffset) + fatLength, align) * align);
    const uint64_t clusterCount64 = (volSectors - heapOffset) / spc;
    if (clusterCount64 < 16 || clusterCount64 > 0xFFFFFFF5ull) throw Error("unsupported exFAT volume size");
    const uint32_t clusterCount = uint32_t(clusterCount64);

    auto clusterOffset = [&](uint32_t c) {
        return opt.partitionOffsetBytes + (uint64_t(heapOffset) + uint64_t(c - 2) * spc) * kSector;
    };

    // ------------------------------------------------------------ allocate clusters
    std::vector<uint32_t> fat = {0xFFFFFFF8, 0xFFFFFFFF};
    auto allocate = [&](uint64_t bytes) -> uint32_t {
        if (bytes == 0) return 0;
        uint32_t n = uint32_t(divUp(bytes, clusterBytes));
        uint32_t first = uint32_t(fat.size());
        if (uint64_t(first) + n - 2 > clusterCount) throw Error("exFAT volume full");
        for (uint32_t i = 0; i < n; ++i) fat.push_back(i + 1 < n ? first + i + 1 : 0xFFFFFFFF);
        return first;
    };

    const auto up = upcaseMap();
    const auto upcase = compressUpcase(up);
    const uint64_t bitmapBytes = divUp(clusterCount, 8);
    const uint32_t bitmapFirst = allocate(bitmapBytes);
    const uint32_t upcaseFirst = allocate(upcase.size());
    const uint32_t rootFirst = allocate(clusterBytes);

    const uint32_t ts = nowTimestamp();
    std::map<std::string, std::vector<uint8_t>> dirEntries; // "" = root, else sub-directory
    std::map<std::string, uint32_t> dirCluster{{"", rootFirst}};
    std::vector<std::pair<uint32_t, const std::vector<uint8_t>*>> fileData;

    for (const auto& f : opt.files) {
        if (!f.dir.empty() && !dirCluster.count(f.dir)) {
            uint32_t c = allocate(clusterBytes);
            dirCluster[f.dir] = c;
            auto set = entrySet(f.dir, true, c, clusterBytes, up, ts);
            auto& root = dirEntries[""];
            root.insert(root.end(), set.begin(), set.end());
        }
        uint32_t c = allocate(f.data.size());
        if (c) fileData.emplace_back(c, &f.data);
        auto set = entrySet(f.name, false, c, f.data.size(), up, ts);
        auto& d = dirEntries[f.dir];
        d.insert(d.end(), set.begin(), set.end());
    }
    const uint32_t usedClusters = uint32_t(fat.size() - 2);

    // ------------------------------------------------------------ FAT region (§4)
    // Clear the whole FAT first so no stale chains survive, then write the used entries.
    const uint64_t fatStart = opt.partitionOffsetBytes + uint64_t(fatOffset) * kSector;
    const uint64_t fatBytes = uint64_t(heapOffset - fatOffset) * kSector;
    {
        const size_t chunk = 4 << 20;
        AlignedBuffer z(chunk);
        for (uint64_t done = 0; done < fatBytes;) {
            size_t n = size_t(std::min<uint64_t>(chunk, fatBytes - done));
            dev.write(fatStart + done, z.data(), n);
            done += n;
            if (progress) progress(double(done) / double(fatBytes));
        }
    }
    {
        AlignedBuffer b(size_t(divUp(fat.size() * 4, kSector) * kSector));
        for (size_t i = 0; i < fat.size(); ++i) put32(b.data() + i * 4, fat[i]);
        dev.write(fatStart, b.data(), b.size());
    }

    auto writeClusters = [&](uint32_t first, const uint8_t* data, size_t len) {
        AlignedBuffer b(size_t(divUp(std::max<size_t>(len, 1), clusterBytes) * clusterBytes));
        if (len) std::memcpy(b.data(), data, len);
        dev.write(clusterOffset(first), b.data(), b.size());
    };

    // ------------------------------------------------------------ allocation bitmap (§7.1)
    {
        std::vector<uint8_t> bm(size_t(bitmapBytes), 0);
        for (uint32_t i = 0; i < usedClusters; ++i) bm[i / 8] |= uint8_t(1u << (i % 8));
        writeClusters(bitmapFirst, bm.data(), bm.size());
    }
    writeClusters(upcaseFirst, upcase.data(), upcase.size());

    // ------------------------------------------------------------ directories
    {
        std::vector<uint8_t> root(96, 0);
        root[0] = 0x83; // volume label (§7.3)
        auto label = toU16(opt.label);
        root[1] = uint8_t(label.size());
        for (size_t i = 0; i < label.size(); ++i) put16(&root[2 + 2 * i], label[i]);
        root[32] = 0x81; // allocation bitmap (§7.1)
        put32(&root[32 + 20], bitmapFirst);
        put64(&root[32 + 24], bitmapBytes);
        root[64] = 0x82; // up-case table (§7.2)
        put32(&root[64 + 4], tableChecksum(upcase));
        put32(&root[64 + 20], upcaseFirst);
        put64(&root[64 + 24], upcase.size());
        auto& rest = dirEntries[""];
        root.insert(root.end(), rest.begin(), rest.end());
        if (root.size() > clusterBytes) throw Error("too many seed files for the root directory");
        writeClusters(rootFirst, root.data(), root.size());
    }
    for (const auto& [name, cluster] : dirCluster) {
        if (name.empty()) continue;
        const auto& entries = dirEntries[name];
        if (entries.size() > clusterBytes) throw Error("too many seed files in " + name);
        writeClusters(cluster, entries.data(), entries.size());
    }
    for (const auto& [cluster, data] : fileData) writeClusters(cluster, data->data(), data->size());

    // ------------------------------------------------------------ boot region (§3)
    // Written last: until it exists the partition is not recognised as exFAT.
    AlignedBuffer boot(12 * kSector);
    uint8_t* bs = boot.data();
    bs[0] = 0xEB; bs[1] = 0x76; bs[2] = 0x90;
    std::memcpy(bs + 3, "EXFAT   ", 8);
    put64(bs + 64, opt.partitionOffsetBytes / kSector);
    put64(bs + 72, volSectors);
    put32(bs + 80, fatOffset);
    put32(bs + 84, fatLength);
    put32(bs + 88, heapOffset);
    put32(bs + 92, clusterCount);
    put32(bs + 96, rootFirst);
    put32(bs + 100, opt.serial);
    put16(bs + 104, 0x0100); // revision 1.00
    put16(bs + 106, 0);      // VolumeFlags
    bs[108] = kSector == 4096 ? 12 : 9;
    bs[109] = spcShift;
    bs[110] = 1;             // one FAT
    bs[111] = 0x80;
    bs[112] = uint8_t(uint64_t(usedClusters) * 100 / clusterCount);
    std::memset(bs + 120, 0xF4, 390); // boot code: hlt (this volume is not bootable)
    bs[510] = 0x55; bs[511] = 0xAA;
    for (int s = 1; s <= 8; ++s) { // extended boot sectors
        bs[(s + 1) * kSector - 2] = 0x55;
        bs[(s + 1) * kSector - 1] = 0xAA;
    }
    // Sectors 9 (OEM parameters) and 10 (reserved) stay zero.
    uint32_t sum = bootChecksum(bs, 11 * kSector);
    for (uint32_t i = 0; i < kSector; i += 4) put32(bs + 11 * kSector + i, sum);

    dev.write(opt.partitionOffsetBytes + 12 * kSector, boot.data(), boot.size()); // backup
    dev.write(opt.partitionOffsetBytes, boot.data(), boot.size());                // main
}

} // namespace glassboot
