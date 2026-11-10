#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace glassboot {

// Glassboot disk layout (all values in 512-byte sectors).
//
//   LBA 0                MBR: GRUB boot.img + partition table
//   LBA 1 ..             GRUB core.img (inside the 1 MiB gap)
//   p1  dataStart        exFAT data partition (ISOs), type 0x07, active
//   p2  espStart         FAT16 EFI System Partition, type 0xEF
struct Layout {
    uint64_t totalSectors = 0;
    uint64_t dataStart = 0, dataSectors = 0;
    uint64_t espStart = 0, espSectors = 0;

    static Layout compute(uint64_t diskBytes, uint32_t espMb, uint64_t reserveBytes = 0); // throws Error
};

// Build sector 0: boot code from GRUB's boot.img (patched to load core.img from LBA 1),
// a random disk signature and the two-entry partition table.
std::array<uint8_t, 512> buildMbr(const std::vector<uint8_t>& bootImg, const Layout& l, uint32_t diskSignature);

// Replace only the boot code of an existing sector 0, keeping its disk signature and
// partition table (used by Update).
std::array<uint8_t, 512> replaceBootCode(const std::vector<uint8_t>& bootImg, const uint8_t* existingSector0);

// Read the partition layout back from sector 0. Returns false if it isn't a Glassboot
// layout (p1 type 0x07 at LBA 2048, p2 type 0xEF after it).
bool parseLayout(const uint8_t* sector0, uint64_t diskBytes, Layout& out);

// Marker sector at the end of the boot gap identifying a Glassboot drive and its version.
constexpr uint64_t kMarkerLba = 2047;
std::array<uint8_t, 512> buildMarker(const std::string& version, uint32_t espMb);
bool parseMarker(const uint8_t* sector, std::string& version);

// Patch core.img's first-sector blocklist so diskboot loads the rest from LBA 2 onward.
// Returns the image padded to a whole number of sectors.
std::vector<uint8_t> prepareCoreImg(const std::vector<uint8_t>& core);

} // namespace glassboot
