#pragma once
#include "Mbr.h"
#include "BlockDevice.h"

namespace glassboot {
// 512-byte logical sectors; GPT installs use UEFI, without BIOS boot code.
Layout computeGptLayout(uint64_t diskBytes, uint32_t espMb, uint64_t reserveBytes = 0);
void writeGpt(BlockDevice& dev, const Layout& layout);
bool readGpt(BlockDevice& dev, Layout& layout);
} // namespace glassboot
