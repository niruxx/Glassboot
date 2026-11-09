#include "BlockDevice.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace glassboot {

std::string formatSize(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1000.0; ++u; }
    char buf[32];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    return buf;
}

std::string DiskInfo::displayName() const {
    std::string name = vendor;
    if (!model.empty()) name += (name.empty() ? "" : " ") + model;
    if (name.empty()) name = "Unknown disk";
    std::string s = name + " (" + formatSize(sizeBytes) + ") " + id;
    if (!mountPoints.empty()) {
        s += " [";
        for (size_t i = 0; i < mountPoints.size(); ++i) s += (i ? ", " : "") + mountPoints[i];
        s += "]";
    }
    return s;
}

AlignedBuffer::AlignedBuffer(size_t size) : size_(size) {
#ifdef _WIN32
    p_ = static_cast<uint8_t*>(_aligned_malloc(size ? size : 1, 4096));
#else
    void* p = nullptr;
    if (posix_memalign(&p, 4096, size ? size : 1) != 0) p = nullptr;
    p_ = static_cast<uint8_t*>(p);
#endif
    if (!p_) throw std::bad_alloc();
    clear();
}

AlignedBuffer::~AlignedBuffer() {
#ifdef _WIN32
    _aligned_free(p_);
#else
    std::free(p_);
#endif
}

void AlignedBuffer::clear() { std::memset(p_, 0, size_); }

void BlockDevice::zero(uint64_t offset, uint64_t len) {
    static constexpr size_t chunk = 1 << 20;
    AlignedBuffer z(chunk);
    while (len) {
        size_t n = size_t(std::min<uint64_t>(len, chunk));
        write(offset, z.data(), n);
        offset += n;
        len -= n;
    }
}

// ---------------------------------------------------------------- image files
namespace {

class ImageFile : public BlockDevice {
public:
    // create: truncate/create the file at `size`. Otherwise open it as it is.
    ImageFile(const std::string& path, uint64_t size, bool create, bool writable)
        : size_(size), writable_(writable) {
#ifdef _WIN32
        int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(size_t(wn > 0 ? wn : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wn);
        h_ = CreateFileW(wpath.c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0), writable ? 0 : FILE_SHARE_READ,
                         nullptr, create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) throw Error("cannot open image file " + path);
        LARGE_INTEGER li;
        if (create) {
            li.QuadPart = LONGLONG(size);
            if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h_))
                throw Error("cannot size image file " + path);
        } else {
            if (!GetFileSizeEx(h_, &li)) throw Error("cannot size image file " + path);
            size_ = uint64_t(li.QuadPart);
        }
#else
        int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | (create ? O_CREAT | O_TRUNC : 0);
        fd_ = ::open(path.c_str(), flags, 0644);
        if (fd_ < 0) throw Error("cannot open image file " + path);
        if (create) {
            if (::ftruncate(fd_, off_t(size)) != 0) throw Error("cannot size image file " + path);
        } else {
            off_t end = ::lseek(fd_, 0, SEEK_END);
            if (end < 0) throw Error("cannot size image file " + path);
            size_ = uint64_t(end);
        }
#endif
    }
    ~ImageFile() override {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    uint64_t sizeBytes() const override { return size_; }

    void write(uint64_t off, const void* data, size_t len) override {
        check(off, len);
        if (!writable_) throw Error("image opened read-only");
#ifdef _WIN32
        seek(off);
        DWORD done = 0;
        if (!WriteFile(h_, data, DWORD(len), &done, nullptr) || done != len) throw Error("image write failed");
#else
        auto p = static_cast<const uint8_t*>(data);
        while (len) {
            ssize_t n = ::pwrite(fd_, p, len, off_t(off));
            if (n <= 0) throw Error("image write failed");
            p += n; off += uint64_t(n); len -= size_t(n);
        }
#endif
    }
    void read(uint64_t off, void* data, size_t len) override {
        check(off, len);
#ifdef _WIN32
        seek(off);
        DWORD done = 0;
        if (!ReadFile(h_, data, DWORD(len), &done, nullptr) || done != len) throw Error("image read failed");
#else
        auto p = static_cast<uint8_t*>(data);
        while (len) {
            ssize_t n = ::pread(fd_, p, len, off_t(off));
            if (n <= 0) throw Error("image read failed");
            p += n; off += uint64_t(n); len -= size_t(n);
        }
#endif
    }
    void flush() override {
#ifdef _WIN32
        FlushFileBuffers(h_);
#else
        ::fsync(fd_);
#endif
    }

private:
    void check(uint64_t off, size_t len) const {
        if (off % 512 || len % 512 || off + len > size_) throw Error("unaligned or out-of-range I/O");
    }
#ifdef _WIN32
    void seek(uint64_t off) {
        LARGE_INTEGER li;
        li.QuadPart = LONGLONG(off);
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) throw Error("seek failed");
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    uint64_t size_;
    bool writable_;
};

} // namespace

std::unique_ptr<BlockDevice> createImageFile(const std::string& path, uint64_t sizeBytes) {
    if (sizeBytes % (1 << 20)) throw Error("image size must be a multiple of 1 MiB");
    return std::make_unique<ImageFile>(path, sizeBytes, true, true);
}

std::unique_ptr<BlockDevice> openImageFile(const std::string& path, bool writable) {
    return std::make_unique<ImageFile>(path, 0, false, writable);
}

} // namespace glassboot
