// Windows disk enumeration and raw access via \\.\PhysicalDriveN (requires administrator).
#include "BlockDevice.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <map>
#include <thread>

namespace glassboot {
namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t i = s.find_first_not_of(' ');
    return i == std::string::npos ? "" : s.substr(i);
}

std::string lastErrorText() {
    DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string s = msg ? trim(narrow(msg)) : "error " + std::to_string(e);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == '.')) s.pop_back();
    return s;
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(Handle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
    Handle& operator=(Handle&& o) noexcept { std::swap(h, o.h); return *this; }
    Handle(const Handle&) = delete;
    ~Handle() { if (valid()) CloseHandle(h); }
    bool valid() const { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

// Disk numbers that a volume (\\?\Volume{...} without trailing slash, or \\.\C:) spans.
std::vector<DWORD> volumeDisks(const std::wstring& volumePath) {
    std::vector<DWORD> out;
    Handle v(CreateFileW(volumePath.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!v.valid()) return out;
    alignas(8) uint8_t buf[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)];
    DWORD got = 0;
    if (DeviceIoControl(v.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof buf, &got, nullptr)) {
        auto* ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf);
        for (DWORD i = 0; i < ext->NumberOfDiskExtents; ++i) out.push_back(ext->Extents[i].DiskNumber);
    }
    return out;
}

struct Volume {
    std::wstring guidPath; // \\?\Volume{...}\ (with trailing slash)
    std::vector<std::string> mountPoints;
};

// All volumes on the system, keyed by the disks they live on.
std::multimap<DWORD, Volume> volumesByDisk() {
    std::multimap<DWORD, Volume> out;
    wchar_t name[MAX_PATH];
    HANDLE f = FindFirstVolumeW(name, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        Volume v;
        v.guidPath = name;
        wchar_t paths[1024];
        DWORD len = 0;
        if (GetVolumePathNamesForVolumeNameW(name, paths, 1024, &len))
            for (wchar_t* p = paths; *p; p += wcslen(p) + 1) v.mountPoints.push_back(narrow(p));
        std::wstring noSlash = v.guidPath;
        if (!noSlash.empty() && noSlash.back() == L'\\') noSlash.pop_back();
        for (DWORD d : volumeDisks(noSlash)) out.emplace(d, v);
    } while (FindNextVolumeW(f, name, MAX_PATH));
    FindVolumeClose(f);
    return out;
}

std::wstring physicalDrivePath(DWORD n) { return L"\\\\.\\PhysicalDrive" + std::to_wstring(n); }

class WindowsDisk : public BlockDevice {
public:
    WindowsDisk(DWORD number, uint64_t size, std::vector<Handle> lockedVolumes, bool writable = true)
        : number_(number), size_(size), writable_(writable), volumes_(std::move(lockedVolumes)) {
        disk_ = Handle(CreateFileW(physicalDrivePath(number).c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   writable ? FILE_FLAG_WRITE_THROUGH : 0, nullptr));
        if (!disk_.valid()) throw Error("cannot open disk " + std::to_string(number) + ": " + lastErrorText());
        DWORD got = 0;
        // Allow I/O to the whole disk, including areas outside any partition.
        DeviceIoControl(disk_.h, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &got, nullptr);
    }
    uint64_t sizeBytes() const override { return size_; }

    void write(uint64_t off, const void* data, size_t len) override {
        check(off, len);
        if (!writable_) throw Error("disk is open read-only");
        seek(off);
        DWORD done = 0;
        if (!WriteFile(disk_.h, data, DWORD(len), &done, nullptr) || done != len)
            throw Error("write to disk failed at offset " + std::to_string(off) + ": " + lastErrorText());
    }
    void read(uint64_t off, void* data, size_t len) override {
        check(off, len);
        seek(off);
        DWORD done = 0;
        if (!ReadFile(disk_.h, data, DWORD(len), &done, nullptr) || done != len)
            throw Error("read from disk failed: " + lastErrorText());
    }
    void flush() override { if (writable_) FlushFileBuffers(disk_.h); }
    void finalize() override {
        if (!writable_) return;
        flush();
        DWORD got = 0;
        DeviceIoControl(disk_.h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &got, nullptr);
        // Releasing the old volume locks lets Windows mount the new partitions.
        volumes_.clear();
    }

private:
    void check(uint64_t off, size_t len) const {
        if (off % 512 || len % 512 || off + len > size_) throw Error("unaligned or out-of-range I/O");
        if (len > 0x7FFFFFFF) throw Error("I/O too large");
    }
    void seek(uint64_t off) {
        LARGE_INTEGER li;
        li.QuadPart = LONGLONG(off);
        if (!SetFilePointerEx(disk_.h, li, nullptr, FILE_BEGIN)) throw Error("seek failed: " + lastErrorText());
    }
    DWORD number_;
    uint64_t size_;
    bool writable_;
    std::vector<Handle> volumes_;
    Handle disk_;
};

} // namespace

std::vector<DiskInfo> listDisks() {
    std::vector<DiskInfo> out;

    // Disks holding the Windows directory are never offered.
    std::vector<DWORD> systemDisks;
    wchar_t winDir[MAX_PATH];
    if (GetWindowsDirectoryW(winDir, MAX_PATH) >= 2) {
        std::wstring drive = L"\\\\.\\" + std::wstring(winDir, 2); // \\.\C:
        systemDisks = volumeDisks(drive);
    }
    auto vols = volumesByDisk();

    for (DWORD n = 0; n < 64; ++n) {
        Handle h(CreateFileW(physicalDrivePath(n).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr));
        if (!h.valid()) continue;

        DiskInfo d;
        d.id = narrow(physicalDrivePath(n));

        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        alignas(8) uint8_t buf[1024] = {};
        DWORD got = 0;
        if (DeviceIoControl(h.h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, nullptr)) {
            auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
            auto str = [&](DWORD off) {
                return off && off < got ? trim(reinterpret_cast<const char*>(buf + off)) : std::string();
            };
            d.vendor = str(desc->VendorIdOffset);
            d.model = str(desc->ProductIdOffset);
            d.removable = desc->RemovableMedia;
            d.usb = desc->BusType == BusTypeUsb;
        }

        GET_LENGTH_INFORMATION len{};
        if (!DeviceIoControl(h.h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof len, &got, nullptr))
            continue; // no media
        d.sizeBytes = uint64_t(len.Length.QuadPart);
        if (d.sizeBytes == 0) continue;

        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h.h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof geo, &got, nullptr))
            d.sectorSize = geo.Geometry.BytesPerSector;

        d.system = std::find(systemDisks.begin(), systemDisks.end(), n) != systemDisks.end();
        auto range = vols.equal_range(n);
        for (auto it = range.first; it != range.second; ++it) {
            for (auto& mp : it->second.mountPoints) d.mountPoints.push_back(mp);
            // A volume spanning several disks (dynamic disk / Storage Spaces): don't touch.
            std::wstring noSlash = it->second.guidPath;
            noSlash.pop_back();
            if (volumeDisks(noSlash).size() > 1) d.system = true;
        }
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<BlockDevice> openDisk(const DiskInfo& disk) {
    if (disk.system) throw Error(disk.id + " holds the running system; refusing");
    if (disk.sectorSize != 512 && disk.sectorSize != 4096) throw Error("unsupported logical sector size");
    const std::string prefix = "\\\\.\\PhysicalDrive";
    if (disk.id.rfind(prefix, 0) != 0) throw Error("bad disk id " + disk.id);
    DWORD number = DWORD(std::stoul(disk.id.substr(prefix.size())));

    // Lock and dismount every volume on the disk; keep the handles open until done so
    // Windows can't remount them underneath us.
    std::vector<Handle> locked;
    auto vols = volumesByDisk();
    auto range = vols.equal_range(number);
    for (auto it = range.first; it != range.second; ++it) {
        std::wstring path = it->second.guidPath;
        path.pop_back(); // trailing backslash would open the root directory instead
        Handle v(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, 0, nullptr));
        if (!v.valid()) throw Error("cannot open volume " + narrow(path) + ": " + lastErrorText());
        DWORD got = 0;
        bool ok = false;
        for (int i = 0; i < 20 && !ok; ++i) {
            ok = DeviceIoControl(v.h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr);
            if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::string where = it->second.mountPoints.empty() ? narrow(path) : it->second.mountPoints.front();
        if (!ok)
            throw Error("cannot lock " + where +
                        ". Close Explorer windows and programs using the USB drive and try again.");
        if (!DeviceIoControl(v.h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &got, nullptr))
            throw Error("cannot dismount " + where + ": " + lastErrorText());
        locked.push_back(std::move(v));
    }
    return sectorView(std::make_unique<WindowsDisk>(number, disk.sizeBytes, std::move(locked)), disk.sectorSize);
}

std::unique_ptr<BlockDevice> openDiskReadOnly(const DiskInfo& disk) {
    const std::string prefix = "\\\\.\\PhysicalDrive";
    if (disk.id.rfind(prefix, 0) != 0) throw Error("bad disk id " + disk.id);
    DWORD number = DWORD(std::stoul(disk.id.substr(prefix.size())));
    return sectorView(std::make_unique<WindowsDisk>(number, disk.sizeBytes, std::vector<Handle>{}, false), disk.sectorSize);
}

} // namespace glassboot
