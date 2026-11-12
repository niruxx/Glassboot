#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace glassboot {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A disk the installer could target.
struct DiskInfo {
    std::string id;           // Linux: /dev/sdX   Windows: \\.\PhysicalDriveN
    std::string vendor;
    std::string model;
    uint64_t sizeBytes = 0;
    uint32_t sectorSize = 512; // logical sector size
    bool usb = false;          // attached over USB
    bool removable = false;    // reports removable media
    bool system = false;       // holds the running OS: never offered as a target
    std::vector<std::string> mountPoints; // drive letters / mount paths of its volumes

    std::string displayName() const;
};

// Raw, sector-addressed writable target (a whole disk or an image file).
// Offsets and lengths passed to read()/write() must be multiples of 512.
class BlockDevice {
public:
    virtual ~BlockDevice() = default;
    virtual uint64_t sizeBytes() const = 0;
    virtual std::string backingPath() const { return {}; }
    virtual uint32_t logicalSectorSize() const { return 512; }
    virtual void write(uint64_t offset, const void* data, size_t len) = 0;
    virtual void read(uint64_t offset, void* data, size_t len) = 0;
    virtual void flush() = 0;
    // Called once after everything is written: flush and make the OS re-read the
    // partition table so the new volumes appear.
    virtual void finalize() { flush(); }

    // Convenience: write `len` zero bytes starting at `offset`.
    void zero(uint64_t offset, uint64_t len);
};

// Enumerate physical disks. System disks are included but flagged `system`.
std::vector<DiskInfo> listDisks();

// Open a physical disk for exclusive raw writing. Unmounts / locks / dismounts all of its
// volumes first. Throws Error on failure (e.g. a file on the stick is still open).
std::unique_ptr<BlockDevice> openDisk(const DiskInfo& disk);

// Open a physical disk read-only without unmounting anything (used to detect an
// existing Glassboot installation). write() throws.
std::unique_ptr<BlockDevice> openDiskReadOnly(const DiskInfo& disk);

// Open an existing disk-image file read-write (or read-only).
std::unique_ptr<BlockDevice> openImageFile(const std::string& path, bool writable);
// Retain byte-addressed installer I/O while aligning physical transfers to 4 KiB.
std::unique_ptr<BlockDevice> sectorView(std::unique_ptr<BlockDevice> device, uint32_t sectorSize);

// Create (or truncate) a regular file of `sizeBytes` and use it as a disk image.
std::unique_ptr<BlockDevice> createImageFile(const std::string& path, uint64_t sizeBytes);

// Buffer with 4 KiB alignment, as unbuffered raw disk I/O on Windows requires.
class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t size);
    ~AlignedBuffer();
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    uint8_t* data() { return p_; }
    const uint8_t* data() const { return p_; }
    size_t size() const { return size_; }
    void clear();

private:
    uint8_t* p_ = nullptr;
    size_t size_ = 0;
};

std::string formatSize(uint64_t bytes);

} // namespace glassboot
