#include "Mbr.h"

#include "BlockDevice.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace glassboot {
namespace {

// Offsets from GRUB include/grub/i386/pc/boot.h and include/grub/offsets.h.
constexpr size_t kKernelSector = 0x5c; // u64: LBA of core.img's first sector
constexpr size_t kBootDrive = 0x64;    // u8: 0xff = use the drive the BIOS booted from
constexpr size_t kDriveCheck = 0x66;   // 2-byte jmp, NOPed for hard disks (as grub-bios-setup does)
constexpr size_t kBootCodeEnd = 0x1b8; // disk signature starts here
constexpr size_t kPartTable = 0x1be;
// struct grub_pc_bios_boot_blocklist { u64 start; u16 len; u16 segment; } at end of sector 0 of core.img
constexpr size_t kBlocklist = 512 - 12;

constexpr uint32_t kMiB = 2048; // sectors

void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (8 * i)); }

// Legacy CHS address (255 heads, 63 sectors/track), saturating at 1023/254/63.
void chs(uint8_t* p, uint32_t lba) {
    uint32_t c = lba / (255 * 63), h = (lba / 63) % 255, s = lba % 63 + 1;
    if (c > 1023) { c = 1023; h = 254; s = 63; }
    p[0] = uint8_t(h);
    p[1] = uint8_t((s & 0x3f) | ((c >> 2) & 0xc0));
    p[2] = uint8_t(c & 0xff);
}

void patchBootCode(uint8_t* s, const std::vector<uint8_t>& bootImg); // defined below

void partEntry(uint8_t* e, bool active, uint8_t type, uint32_t start, uint32_t count) {
    e[0] = active ? 0x80 : 0x00;
    chs(e + 1, start);
    e[4] = type;
    chs(e + 5, start + count - 1);
    put32(e + 8, start);
    put32(e + 12, count);
}

} // namespace

Layout Layout::compute(uint64_t diskBytes, uint32_t espMb) {
    Layout l;
    l.totalSectors = diskBytes / 512;
    // MBR can address at most 2^32 sectors (2 TiB). Larger sticks use the first 2 TiB.
    uint64_t usable = std::min<uint64_t>(l.totalSectors, 0xFFFFFFFFull);
    l.espSectors = espMb * kMiB;
    l.dataStart = kMiB;
    uint64_t minData = 256ull * kMiB; // 256 MiB
    if (usable < l.dataStart + minData + l.espSectors)
        throw Error("the disk is too small (at least " + std::to_string(espMb + 257) + " MiB needed)");
    uint64_t dataSectors = (usable - l.dataStart - l.espSectors) / kMiB * kMiB;
    l.dataSectors = uint32_t(dataSectors);
    l.espStart = l.dataStart + l.dataSectors;
    return l;
}

std::array<uint8_t, 512> buildMbr(const std::vector<uint8_t>& bootImg, const Layout& l, uint32_t diskSignature) {
    if (bootImg.size() != 512) throw Error("boot.img must be 512 bytes");
    std::array<uint8_t, 512> s{};
    patchBootCode(s.data(), bootImg);
    put32(&s[kBootCodeEnd], diskSignature);
    put16(&s[kBootCodeEnd + 4], 0);
    partEntry(&s[kPartTable], true, 0x07, l.dataStart, l.dataSectors);
    partEntry(&s[kPartTable + 16], false, 0xEF, l.espStart, l.espSectors);
    s[510] = 0x55;
    s[511] = 0xAA;
    return s;
}

namespace {
void patchBootCode(uint8_t* s, const std::vector<uint8_t>& bootImg) {
    if (bootImg.size() != 512) throw Error("boot.img must be 512 bytes");
    std::memcpy(s, bootImg.data(), kBootCodeEnd);
    put64(s + kKernelSector, 1);
    s[kBootDrive] = 0xff;
    s[kDriveCheck] = 0x90;
    s[kDriveCheck + 1] = 0x90;
}
uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
const char kMarkerMagic[] = "GLASSBOOT-MARKER\n";
} // namespace

std::array<uint8_t, 512> replaceBootCode(const std::vector<uint8_t>& bootImg, const uint8_t* existing) {
    std::array<uint8_t, 512> s{};
    std::memcpy(s.data(), existing, 512);
    patchBootCode(s.data(), bootImg);
    return s;
}

bool parseLayout(const uint8_t* s, uint64_t diskBytes, Layout& l) {
    if (s[510] != 0x55 || s[511] != 0xAA) return false;
    const uint8_t* p1 = s + kPartTable;
    const uint8_t* p2 = s + kPartTable + 16;
    if (p1[4] != 0x07 || p2[4] != 0xEF) return false;
    l = Layout{};
    l.totalSectors = diskBytes / 512;
    l.dataStart = get32(p1 + 8);
    l.dataSectors = get32(p1 + 12);
    l.espStart = get32(p2 + 8);
    l.espSectors = get32(p2 + 12);
    return l.dataStart == kMiB && l.espStart >= l.dataStart + l.dataSectors &&
           uint64_t(l.espStart) + l.espSectors <= l.totalSectors && l.espSectors >= kMiB;
}

std::array<uint8_t, 512> buildMarker(const std::string& version, uint32_t espMb) {
    std::array<uint8_t, 512> s{};
    std::string text = std::string(kMarkerMagic) + "version=" + version + "\nesp_mb=" + std::to_string(espMb) + "\n";
    if (text.size() > 500) throw Error("marker too long");
    std::memcpy(s.data(), text.data(), text.size());
    return s;
}

bool parseMarker(const uint8_t* s, std::string& version) {
    if (std::memcmp(s, kMarkerMagic, sizeof kMarkerMagic - 1) != 0) return false;
    std::string text(reinterpret_cast<const char*>(s), strnlen(reinterpret_cast<const char*>(s), 512));
    auto at = text.find("\nversion=");
    if (at == std::string::npos) return false;
    auto end = text.find('\n', at + 9);
    version = text.substr(at + 9, end == std::string::npos ? std::string::npos : end - at - 9);
    return true;
}

std::vector<uint8_t> prepareCoreImg(const std::vector<uint8_t>& core) {
    std::vector<uint8_t> img = core;
    img.resize((img.size() + 511) / 512 * 512, 0);
    size_t sectors = img.size() / 512;
    // LBA 1 .. kMarkerLba-1: the last sector of the gap holds the Glassboot marker.
    if (sectors < 2 || sectors > kMarkerLba - 1) throw Error("core.img does not fit in the boot gap");
    uint8_t* bl = &img[kBlocklist];
    put64(bl, 2);                      // rest of core.img starts at LBA 2
    put16(bl + 8, uint16_t(sectors - 1));
    // bl + 10: load segment, left as built by grub-mkimage (0x820).
    return img;
}

} // namespace glassboot
