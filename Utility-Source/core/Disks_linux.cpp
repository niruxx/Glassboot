// Linux disk enumeration and raw access. Uses only sysfs and /proc: no external tools.
#include "BlockDevice.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace glassboot {
namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t start = s.find_first_not_of(' ');
    return start == std::string::npos ? "" : s.substr(start);
}

std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') out.emplace_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// /proc/mounts escapes spaces etc. as \040.
std::string unescapeMount(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            r += char(std::stoi(s.substr(i + 1, 3), nullptr, 8));
            i += 3;
        } else {
            r += s[i];
        }
    }
    return r;
}

struct Mount { std::string dev, dir; };

std::vector<Mount> mounts() {
    std::vector<Mount> out;
    std::ifstream f("/proc/self/mounts");
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        Mount m;
        ls >> m.dev >> m.dir;
        m.dev = unescapeMount(m.dev);
        m.dir = unescapeMount(m.dir);
        out.push_back(m);
    }
    return out;
}

std::vector<std::string> swaps() {
    std::vector<std::string> out;
    std::ifstream f("/proc/swaps");
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        std::string dev;
        ls >> dev;
        out.push_back(dev);
    }
    return out;
}

// Names of the disk itself and all its partitions (sda, sda1, sda2, ...).
std::vector<std::string> diskAndPartitions(const std::string& name) {
    std::vector<std::string> out{name};
    for (auto& e : listDir("/sys/block/" + name))
        if (e.rfind(name, 0) == 0 && !readFile("/sys/block/" + name + "/" + e + "/partition").empty())
            out.push_back(e);
    return out;
}

bool hasHolders(const std::string& sysPath) { return !listDir(sysPath + "/holders").empty(); }

class LinuxDisk : public BlockDevice {
public:
    LinuxDisk(const std::string& path, uint64_t size, bool writable = true)
        : path_(path), size_(size), writable_(writable) {
        fd_ = ::open(path.c_str(), writable ? (O_RDWR | O_EXCL | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC));
        if (fd_ < 0)
            throw Error("cannot open " + path + (writable ? " exclusively: " : ": ") + std::strerror(errno) +
                        (errno == EACCES ? " (run as root)" : " (is it still in use?)"));
    }
    ~LinuxDisk() override { if (fd_ >= 0) ::close(fd_); }
    uint64_t sizeBytes() const override { return size_; }

    void write(uint64_t off, const void* data, size_t len) override {
        check(off, len);
        if (!writable_) throw Error(path_ + " is open read-only");
        auto p = static_cast<const uint8_t*>(data);
        while (len) {
            ssize_t n = ::pwrite(fd_, p, len, off_t(off));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw Error("write to " + path_ + " failed: " + std::strerror(errno));
            p += n; off += uint64_t(n); len -= size_t(n);
        }
    }
    void read(uint64_t off, void* data, size_t len) override {
        check(off, len);
        auto p = static_cast<uint8_t*>(data);
        while (len) {
            ssize_t n = ::pread(fd_, p, len, off_t(off));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw Error("read from " + path_ + " failed: " + std::strerror(errno));
            p += n; off += uint64_t(n); len -= size_t(n);
        }
    }
    void flush() override {
        if (!writable_) return;
        if (::fsync(fd_) != 0) throw Error("flush of " + path_ + " failed: " + std::strerror(errno));
    }
    void finalize() override {
        if (!writable_) return;
        flush();
        ::ioctl(fd_, BLKFLSBUF, 0);
        // The kernel may still be probing the old partitions; retry for a few seconds.
        for (int i = 0; i < 10; ++i) {
            if (::ioctl(fd_, BLKRRPART, 0) == 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        // Not fatal: the stick works, the OS just shows it after re-plugging.
    }

private:
    void check(uint64_t off, size_t len) const {
        if (off % 512 || len % 512 || off + len > size_) throw Error("unaligned or out-of-range I/O");
    }
    std::string path_;
    uint64_t size_;
    bool writable_;
    int fd_ = -1;
};

} // namespace

std::vector<DiskInfo> listDisks() {
    std::vector<DiskInfo> out;
    const auto mnts = mounts();
    const auto swp = swaps();

    for (const auto& name : listDir("/sys/block")) {
        const std::string sys = "/sys/block/" + name;
        // Only real devices (skips loop, ram, zram, dm-*, md*).
        struct stat st{};
        if (::stat((sys + "/device").c_str(), &st) != 0) continue;
        if (name.rfind("sr", 0) == 0) continue; // optical drives

        DiskInfo d;
        d.id = "/dev/" + name;
        d.sizeBytes = std::stoull("0" + readFile(sys + "/size")) * 512ULL;
        if (d.sizeBytes == 0) continue; // empty card reader slot
        auto lbs = readFile(sys + "/queue/logical_block_size");
        d.sectorSize = lbs.empty() ? 512 : uint32_t(std::stoul(lbs));
        d.vendor = readFile(sys + "/device/vendor");
        d.model = readFile(sys + "/device/model");
        d.removable = readFile(sys + "/removable") == "1";
        char real[4096];
        ssize_t n = ::readlink(sys.c_str(), real, sizeof real - 1);
        if (n > 0) { real[n] = 0; d.usb = std::strstr(real, "/usb") != nullptr; }

        for (const auto& part : diskAndPartitions(name)) {
            const std::string dev = "/dev/" + part;
            const std::string psys = part == name ? sys : sys + "/" + part;
            for (const auto& m : mnts) {
                if (m.dev != dev) continue;
                d.mountPoints.push_back(m.dir);
                if (m.dir == "/" || m.dir == "/boot" || m.dir == "/boot/efi" || m.dir == "/usr" ||
                    m.dir == "/home" || m.dir == "/var")
                    d.system = true;
            }
            if (std::find(swp.begin(), swp.end(), dev) != swp.end()) d.system = true;
            // Used by LVM / RAID / LUKS: treat as system, we can't safely tell what it is.
            if (hasHolders(psys)) d.system = true;
        }
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<BlockDevice> openDisk(const DiskInfo& disk) {
    if (disk.system) throw Error(disk.id + " holds the running system; refusing");
    if (disk.sectorSize != 512) throw Error("only disks with 512-byte logical sectors are supported");

    // Unmount everything on the disk (deepest mount points first).
    const std::string name = disk.id.substr(disk.id.rfind('/') + 1);
    auto parts = diskAndPartitions(name);
    std::vector<std::string> targets;
    for (const auto& m : mounts())
        for (const auto& p : parts)
            if (m.dev == "/dev/" + p) targets.push_back(m.dir);
    std::sort(targets.begin(), targets.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    for (const auto& t : targets) {
        if (::umount2(t.c_str(), 0) != 0 && errno != EINVAL)
            throw Error("cannot unmount " + t + ": " + std::strerror(errno) +
                        ". Close any programs using the USB drive and try again.");
    }
    return std::make_unique<LinuxDisk>(disk.id, disk.sizeBytes);
}

std::unique_ptr<BlockDevice> openDiskReadOnly(const DiskInfo& disk) {
    return std::make_unique<LinuxDisk>(disk.id, disk.sizeBytes, false);
}

} // namespace glassboot
