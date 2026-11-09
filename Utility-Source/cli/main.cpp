// glassboot-cli: scriptable installer, also used for automated testing.
//
//   glassboot-cli list [--all]
//   glassboot-cli install --device ID [--yes] [--payload DIR]
//   glassboot-cli install --image FILE --size 8G [--payload DIR]
//   glassboot-cli update  --device ID | --image FILE      (keeps the ISO files)
#include "BlockDevice.h"
#include "Installer.h"
#include "Payload.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace glassboot;

namespace {

std::atomic<bool> g_cancel{false};

void usage() {
    std::cerr << "usage:\n"
                 "  glassboot-cli list [--all]\n"
                 "  glassboot-cli install --device ID [--yes] [--payload DIR]\n"
                 "  glassboot-cli install --image FILE --size SIZE [--payload DIR]\n"
                 "  glassboot-cli update --device ID | --image FILE [--yes] [--payload DIR]\n"
                 "      refresh the boot files, keeping the ISO files on the drive\n"
                 "\n"
                 "SIZE accepts M/G suffixes (e.g. 8G). ID is shown by 'list'.\n";
}

uint64_t parseSize(const std::string& s) {
    size_t pos = 0;
    double v = std::stod(s, &pos);
    std::string suf = s.substr(pos);
    uint64_t mul = 1;
    if (suf == "K" || suf == "k") mul = 1ull << 10;
    else if (suf == "M" || suf == "m") mul = 1ull << 20;
    else if (suf == "G" || suf == "g") mul = 1ull << 30;
    else if (suf == "T" || suf == "t") mul = 1ull << 40;
    else if (!suf.empty()) throw Error("bad size suffix: " + suf);
    return uint64_t(v * double(mul));
}

std::string exeDir(const char* argv0) {
    std::string p = argv0;
    auto i = p.find_last_of("/\\");
    return i == std::string::npos ? "." : p.substr(0, i);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) { usage(); return 2; }
    std::signal(SIGINT, [](int) { g_cancel = true; });

    try {
        const std::string cmd = args[0];
        std::string device, image, payloadDir, size;
        bool yes = false, all = false;
        for (size_t i = 1; i < args.size(); ++i) {
            auto next = [&]() -> std::string {
                if (i + 1 >= args.size()) throw Error("missing value for " + args[i]);
                return args[++i];
            };
            if (args[i] == "--device") device = next();
            else if (args[i] == "--image") image = next();
            else if (args[i] == "--size") size = next();
            else if (args[i] == "--payload") payloadDir = next();
            else if (args[i] == "--yes") yes = true;
            else if (args[i] == "--all") all = true;
            else throw Error("unknown option " + args[i]);
        }

        if (cmd == "list") {
            // Compatible drives; with --all also the others, with the reason they're excluded.
            auto disks = listDisks();
            size_t hidden = 0;
            for (const auto& d : disks) {
                std::string why = incompatibility(d);
                if (why.empty()) {
                    ExistingInstall ex = probeInstall(d);
                    std::cout << "  " << d.displayName() << (ex.found ? "  [Glassboot " + ex.version + "]" : "") << "\n";
                }
                else if (all) std::cout << "  [not compatible: " << why << "] " << d.displayName() << "\n";
                else ++hidden;
            }
            if (disks.empty())
                std::cerr << "No disks visible. Run from an administrator (Windows) or root (Linux) shell.\n";
            else if (hidden)
                std::cerr << hidden << " incompatible disk(s) hidden; use --all to see why.\n";
            return 0;
        }
        if (cmd != "install" && cmd != "update") { usage(); return 2; }
        const bool isUpdate = cmd == "update";

        if (payloadDir.empty()) payloadDir = Payload::locate(exeDir(argv[0]));
        if (payloadDir.empty()) throw Error("payload not found; pass --payload DIR");
        std::cout << "Verifying payload in " << payloadDir << "...\n";
        Payload payload = Payload::load(payloadDir);
        std::cout << "Glassboot " << payload.version << "\n";

        std::unique_ptr<BlockDevice> dev;
        if (!image.empty()) {
            if (isUpdate) {
                dev = openImageFile(image, true);
            } else {
                if (size.empty()) throw Error("--image needs --size");
                dev = createImageFile(image, parseSize(size));
            }
        } else if (!device.empty()) {
            DiskInfo target;
            bool found = false;
            for (const auto& d : listDisks())
                if (d.id == device) { target = d; found = true; }
            if (!found) throw Error("no such disk: " + device);
            // The CLI accepts internal disks when named explicitly; everything else must match.
            std::string why = incompatibility(target, payload.espMb, /*allowNonUsb=*/true);
            if (!why.empty()) throw Error(device + " is not compatible: " + why);
            if (!target.usb && !target.removable)
                std::cout << "Warning: " << device << " is not a USB or removable drive.\n";
            std::cout << "Target: " << target.displayName() << "\n";
            ExistingInstall existing = probeInstall(target);
            if (existing.found) std::cout << "Currently: Glassboot " << existing.version << "\n";
            if (isUpdate && !existing.found) throw Error("no Glassboot installation on " + device + "; use install");
            if (!yes) {
                std::cout << (isUpdate ? "The boot files will be updated; ISO files are kept. Type YES to continue: "
                                       : "ALL DATA ON THIS DISK WILL BE ERASED. Type YES to continue: ")
                          << std::flush;
                std::string answer;
                std::getline(std::cin, answer);
                if (answer != "YES") { std::cout << "Aborted.\n"; return 1; }
            }
            dev = openDisk(target);
        } else {
            throw Error(cmd + " needs --device or --image");
        }

        // A live progress line only makes sense on a terminal; logs go to stdout either way.
        const bool tty = isatty(fileno(stderr));
        bool progressShown = false;
        int lastPct = -1;
        InstallCallbacks cb;
        cb.progress = [&](double f, const std::string& stepName) {
            int pct = int(f * 100);
            if (!tty || pct == lastPct) return;
            std::fprintf(stderr, "\r[%3d%%] %-50s", pct, stepName.c_str());
            std::fflush(stderr);
            lastPct = pct;
            progressShown = true;
        };
        cb.log = [&](const std::string& line) {
            if (progressShown) { std::fprintf(stderr, "\r%-60s\r", ""); progressShown = false; }
            std::cout << line << std::endl;
        };
        if (isUpdate) update(*dev, payload, cb, g_cancel);
        else install(*dev, payload, cb, g_cancel);
        if (progressShown) std::fprintf(stderr, "\n");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\nerror: " << e.what() << "\n";
        return 1;
    }
}
