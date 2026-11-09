#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace glassboot {

// The boot payload produced by scripts/build-payload.sh (see manifest.txt there).
struct Payload {
    std::string dir;
    std::string version;
    uint32_t espMb = 0;
    std::string dataLabel;
    std::string espLabel;
    std::vector<uint8_t> mbr;  // GRUB boot.img
    std::vector<uint8_t> core; // GRUB core.img
    std::string espImagePath;  // streamed from disk, not held in memory
    uint64_t espImageSize = 0;

    // Load and verify every file against manifest.txt. Throws Error.
    static Payload load(const std::string& dir);

    // Search the usual places: $GLASSBOOT_PAYLOAD, <exeDir>/payload,
    // <exeDir>/../share/glassboot/payload, and any parent's out/payload (dev builds).
    static std::string locate(const std::string& exeDir);
};

} // namespace glassboot
