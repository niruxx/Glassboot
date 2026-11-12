#pragma once
#include "Mbr.h"
#include "BlockDevice.h"

namespace glassboot {
// Layout uses 512-byte units internally; on-disk GPT uses the device's logical sectors.
Layout computeGptLayout(uint64_t diskBytes, uint32_t espMb, uint64_t reserveBytes = 0, uint32_t sectorSize = 512);
void writeGpt(BlockDevice& dev, const Layout& layout);
bool readGpt(BlockDevice& dev, Layout& layout);
} // namespace glassboot
