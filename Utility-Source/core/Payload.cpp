#include "Payload.h"

#include "BlockDevice.h"
#include "Sha256.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace glassboot {
namespace {

struct FileEntry { uint64_t size = 0; std::string sha256; };

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

bool exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return bool(f);
}

std::string parent(const std::string& path) {
    auto p = path.find_last_of("/\\");
    return p == std::string::npos || p == 0 ? std::string() : path.substr(0, p);
}

// Hash a file in chunks; optionally keep its bytes.
std::string hashFile(const std::string& path, uint64_t& size, std::vector<uint8_t>* keep) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("payload file missing: " + path);
    Sha256 h;
    std::vector<char> buf(1 << 20);
    size = 0;
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        auto n = size_t(f.gcount());
        if (!n) break;
        h.update(buf.data(), n);
        if (keep) keep->insert(keep->end(), buf.begin(), buf.begin() + std::ptrdiff_t(n));
        size += n;
    }
    return Sha256::hex(h.finish());
}

} // namespace

Payload Payload::load(const std::string& dir) {
    Payload p;
    p.dir = dir;
    std::ifstream mf(join(dir, "manifest.txt"));
    if (!mf) throw Error("no Glassboot payload found in " + dir + " (manifest.txt missing)");

    std::map<std::string, FileEntry> files;
    std::string line;
    while (std::getline(mf, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "version") ls >> p.version;
        else if (key == "esp_mb") ls >> p.espMb;
        else if (key == "data_label") ls >> p.dataLabel;
        else if (key == "esp_label") ls >> p.espLabel;
        else if (key == "file") {
            std::string name;
            FileEntry e;
            ls >> name >> e.size >> e.sha256;
            files[name] = e;
        }
    }
    for (const char* need : {"mbr.bin", "core.img", "esp.img"})
        if (!files.count(need)) throw Error(std::string("manifest.txt does not list ") + need);
    if (p.espMb == 0 || p.dataLabel.empty() || p.espLabel.empty()) throw Error("manifest.txt is incomplete");

    auto verify = [&](const std::string& name, std::vector<uint8_t>* keep) {
        uint64_t size = 0;
        std::string got = hashFile(join(dir, name), size, keep);
        const auto& want = files[name];
        if (size != want.size || got != want.sha256)
            throw Error("payload file " + name + " is corrupt (checksum mismatch)");
        return size;
    };
    verify("mbr.bin", &p.mbr);
    verify("core.img", &p.core);
    p.espImagePath = join(dir, "esp.img");
    p.espImageSize = verify("esp.img", nullptr);
    if (files.count("esp4k.img")) {
        if (verify("esp4k.img", nullptr) != p.espImageSize) throw Error("esp4k.img size differs from esp.img");
        p.esp4kImagePath = join(dir, "esp4k.img");
    }

    if (p.mbr.size() != 512) throw Error("mbr.bin must be 512 bytes");
    if (p.core.size() < 1024 || p.core.size() > 2047 * 512) throw Error("core.img has an unexpected size");
    if (p.espImageSize != uint64_t(p.espMb) << 20) throw Error("esp.img size does not match esp_mb");
    return p;
}

std::string Payload::locate(const std::string& exeDir) {
    if (const char* env = std::getenv("GLASSBOOT_PAYLOAD"))
        if (exists(join(env, "manifest.txt"))) return env;
    std::vector<std::string> candidates = {
        join(exeDir, "payload"),
        join(parent(exeDir), "share/glassboot/payload"),
    };
    // Development tree: walk up looking for out/payload.
    for (std::string d = exeDir; !d.empty(); d = parent(d)) candidates.push_back(join(d, "out/payload"));
    for (const auto& c : candidates)
        if (exists(join(c, "manifest.txt"))) return c;
    return {};
}

} // namespace glassboot
