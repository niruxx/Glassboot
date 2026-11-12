#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace glassboot {

class BlockDevice;

// A file to create while formatting, in the root or in one sub-directory.
struct SeedFile {
    std::string dir;  // "" for the root directory, otherwise a single directory name
    std::string name; // ASCII file name
    std::vector<uint8_t> data;
};

struct ExFatOptions {
    uint32_t bytesPerSector = 512;
    uint64_t partitionOffsetBytes = 0; // where the partition starts on the device
    uint64_t partitionSectors = 0;     // 512-byte sectors
    std::string label;                 // up to 11 ASCII characters
    uint32_t serial = 0;
    std::vector<SeedFile> files;
};

// Format an exFAT file system per the Microsoft exFAT specification (2019 revision).
// `progress` receives 0..1 while the FAT region is being cleared.
void formatExFat(BlockDevice& dev, const ExFatOptions& opt, const std::function<void(double)>& progress = {});

} // namespace glassboot
