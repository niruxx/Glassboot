#pragma once
#include "ExFat.h"
#include "BlockDevice.h"
namespace glassboot {
// Check all required formatter tools before the installer writes anything.
void checkDataFilesystem(const std::string& type, uint64_t bytes);
// Linux: format a bounded loop view of the new partition and seed its files.
void formatDataFilesystem(BlockDevice& dev, const ExFatOptions& options, const std::string& type);
void shrinkNtfsPartition(BlockDevice& dev, uint64_t start, uint64_t oldSectors, uint64_t newSectors);
}
