#include "DataFilesystem.h"
#include <filesystem>
#include <fstream>
#include <map>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#endif

namespace glassboot {
namespace {
const std::map<std::string, std::string> tools = {
    {"ntfs", "mkfs.ntfs"}, {"ext4", "mkfs.ext4"}, {"xfs", "mkfs.xfs"}, {"udf", "mkudffs"}
};
#ifndef _WIN32
std::string executable(const std::string& tool) {
    // Fixed system directories: never run a formatter from a user-writable PATH.
    for (const auto* dir : {"/usr/sbin/", "/usr/bin/", "/sbin/", "/bin/"}) {
        std::string path = dir + tool;
        if (access(path.c_str(), X_OK) == 0) return path;
    }
    throw Error("required tool not installed: " + tool);
}
std::string run(const std::vector<std::string>& args, bool confirm = false) {
    std::string command = executable(args.at(0));
    int pipefd[2];
    if (pipe(pipefd)) throw Error("cannot create tool output pipe");
    int input[2] = {-1, -1};
    if (confirm) {
        if (pipe(input) || write(input[1], "y\n", 2) != 2) {
            if (input[0] >= 0) close(input[0]);
            if (input[1] >= 0) close(input[1]);
            close(pipefd[0]); close(pipefd[1]); throw Error("cannot create confirmation pipe");
        }
        close(input[1]);
    }
    pid_t child = fork();
    if (child < 0) { if (confirm) close(input[0]); close(pipefd[0]); close(pipefd[1]); throw Error("cannot start " + command); }
    if (!child) {
        if (confirm) { dup2(input[0], STDIN_FILENO); close(input[0]); }
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[0]); close(pipefd[1]);
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(command.c_str(), argv.data()); _exit(127);
    }
    close(pipefd[1]);
    if (confirm) close(input[0]);
    std::string output; char buf[4096]; ssize_t count;
    while ((count = read(pipefd[0], buf, sizeof buf)) != 0) {
        if (count < 0) { if (errno == EINTR) continue; break; }
        if (output.size() < 65536) output.append(buf, size_t(count));
    }
    close(pipefd[0]); int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) throw Error("cannot wait for " + command);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) throw Error(command + " failed: " + output);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}
#endif
}
void checkDataFilesystem(const std::string& type, uint64_t bytes) {
    if (type == "exfat") return;
    auto tool = tools.find(type);
    if (tool == tools.end()) throw Error("unknown data filesystem: " + type);
#ifdef _WIN32
    (void)bytes;
    throw Error("NTFS/ext4/XFS/UDF formatting currently requires the Linux installer");
#else
    if (type == "xfs" && bytes < 300ull * 1024 * 1024) throw Error("XFS requires at least 300 MiB of data space");
    for (const auto& name : {tool->second, std::string("losetup"), std::string("mount"), std::string("umount")}) executable(name);
    if (type == "ntfs") executable("ntfs-3g");
#endif
}
void formatDataFilesystem(BlockDevice& dev, const ExFatOptions& options, const std::string& type) {
#ifdef _WIN32
    (void)dev; (void)options;
    throw Error("formatting " + type + " requires Linux");
#else
    checkDataFilesystem(type, options.partitionSectors * 512);
    if (dev.backingPath().empty()) throw Error("target has no backing path for Linux formatting");
    dev.flush();
    std::string loop = run({"losetup", "--find", "--show", "--sector-size", std::to_string(dev.logicalSectorSize()), "--offset", std::to_string(options.partitionOffsetBytes),
                            "--sizelimit", std::to_string(options.partitionSectors * 512), "--", dev.backingPath()});
    if (loop.rfind("/dev/loop", 0) != 0 || loop.find('\n') != std::string::npos) throw Error("unexpected loop device response");
    char temp[] = "/tmp/glassboot-format-XXXXXX";
    char* created = mkdtemp(temp);
    if (!created) { run({"losetup", "--detach", loop}); throw Error("cannot create mount directory"); }
    std::string dir = created; bool mounted = false;
    try {
        if (type == "ntfs") run({"mkfs.ntfs", "--force", "--quick", "--label", options.label, loop});
        else if (type == "ext4") run({"mkfs.ext4", "-F", "-q", "-L", options.label, loop});
        else if (type == "xfs") run({"mkfs.xfs", "-f", "-L", options.label, loop});
        else run({"mkudffs", "--media-type=hd", "--blocksize=" + std::to_string(dev.logicalSectorSize()), "--label=" + options.label, loop});
        run({"mount", "-t", type == "ntfs" ? "ntfs-3g" : type, "-o", "nodev,nosuid,noexec", loop, dir});
        mounted = true;
        for (const auto& file : options.files) {
            auto safe = [](const std::string& s) { return s.find('/') == std::string::npos && s.find('\\') == std::string::npos && s != "." && s != ".."; };
            if (!safe(file.dir) || !safe(file.name) || file.name.empty()) throw Error("invalid seed filename");
            auto folder = std::filesystem::path(dir) / file.dir;
            std::filesystem::create_directories(folder);
            std::ofstream out(folder / file.name, std::ios::binary);
            out.write(reinterpret_cast<const char*>(file.data.data()), std::streamsize(file.data.size()));
            out.close(); if (!out) throw Error("cannot seed data partition");
        }
        run({"umount", dir}); mounted = false;
        run({"losetup", "--detach", loop}); std::filesystem::remove(dir);
    } catch (...) {
        try { if (mounted) run({"umount", dir}); } catch (...) {}
        try { run({"losetup", "--detach", loop}); } catch (...) {}
        std::error_code ec; std::filesystem::remove(dir, ec); throw;
    }
#endif
}

void shrinkNtfsPartition(BlockDevice& dev, uint64_t start, uint64_t oldSectors, uint64_t newSectors) {
#ifdef _WIN32
    (void)dev; (void)start; (void)oldSectors; (void)newSectors;
    throw Error("NTFS shrinking currently requires Linux");
#else
    executable("ntfsresize");
    if (dev.backingPath().empty() || dev.logicalSectorSize() != 512 || !newSectors || newSectors >= oldSectors)
        throw Error("unsupported NTFS shrink target");
    dev.flush();
    auto loop = run({"losetup", "--find", "--show", "--offset", std::to_string(start * 512),
                     "--sizelimit", std::to_string(oldSectors * 512), "--", dev.backingPath()});
    try {
        // Keep every ntfsresize consistency/hibernation/bad-sector check enabled.
        // The dry run proves that data can be relocated before any real resize.
        run({"ntfsresize", "--no-progress-bar", "--no-action", "--size", std::to_string(newSectors * 512), loop});
        run({"ntfsresize", "--no-progress-bar", "--size", std::to_string(newSectors * 512), loop}, true);
        run({"losetup", "--detach", loop}); dev.flush();
    } catch (...) { try { run({"losetup", "--detach", loop}); } catch (...) {} throw; }
#endif
}
}
