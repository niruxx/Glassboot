#pragma once
#include "BlockDevice.h"
#include "Payload.h"

#include <atomic>
#include <functional>
#include <string>

namespace glassboot {

struct InstallCallbacks {
    // Overall progress 0..1 plus a short description of the current step.
    std::function<void(double fraction, const std::string& step)> progress;
    // Detailed log lines.
    std::function<void(const std::string& line)> log;
};

// Default size of the EFI system partition, used before a payload has been loaded.
constexpr uint32_t kDefaultEspMb = 64;

// Smallest disk Glassboot can be installed on.
uint64_t minimumDiskBytes(uint32_t espMb = kDefaultEspMb);

// Empty if Glassboot can be installed on `disk`, otherwise a short human-readable reason
// ("not a USB drive", "too small (needs at least 400 MB)", ...). Internal disks are only
// accepted when `allowNonUsb` is set; the system disk never is.
std::string incompatibility(const DiskInfo& disk, uint32_t espMb = kDefaultEspMb, bool allowNonUsb = false);

// An existing Glassboot installation found on a drive.
struct ExistingInstall {
    bool found = false;
    std::string version; // "0.1.x (no marker)" for drives made before markers existed
    uint64_t dataStart = 0, dataSectors = 0, espStart = 0, espSectors = 0;
    bool gpt = false;
};

// Look for Glassboot on `dev` (reads a few sectors; never writes).
ExistingInstall detectInstall(BlockDevice& dev);

// Same, opening the disk read-only. Never throws: returns found=false on any error.
ExistingInstall probeInstall(const DiskInfo& disk);

// Refresh the boot files of an existing Glassboot drive WITHOUT touching the data
// partition (the user's ISOs): EFI partition, BIOS boot code, marker. Keeps the
// partition table and disk signature. Throws Error if the drive isn't Glassboot or its
// layout doesn't match this payload (then a full install is needed).
void update(BlockDevice& dev, const Payload& payload, const InstallCallbacks& cb, const std::atomic<bool>& cancel);

// Writes a complete Glassboot stick to `dev`, destroying everything on it:
//   1. verifies the payload            5. writes GRUB core.img into the boot gap
//   2. clears old partition tables     6. writes the MBR (boot code + partitions)
//   3. writes the EFI system partition 7. reads everything back and compares
//   4. formats the exFAT data partition
// Throws Error on failure. Setting `cancel` aborts between steps (the stick is then
// left unusable until the installer is run again).
struct InstallOptions {
    bool gpt = false; // UEFI-only; MBR remains the default for Legacy BIOS.
    uint64_t reserveBytes = 0; // unallocated space after the ESP (before backup GPT)
};
void install(BlockDevice& dev, const Payload& payload, const InstallCallbacks& cb, const std::atomic<bool>& cancel,
             const InstallOptions& options = {});

} // namespace glassboot
