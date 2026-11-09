// Unit tests for the Qt-free core pieces that don't touch disks: PBKDF2/SHA-512 and the
// glassboot.cfg editor. Run: glassboot-core-test [PASSWORD SALT_HEX ITER] (prints a
// GRUB hash for cross-checking against other implementations).
#include "Config.h"
#include "Pbkdf2.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace glassboot;

static int failures = 0;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

static std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

static std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(uint8_t(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
    return v;
}

static void testCrypto() {
    Sha512 h;
    h.update("abc", 3);
    auto d = h.finish();
    CHECK(hex(std::vector<uint8_t>(d.begin(), d.end())) ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    std::vector<uint8_t> salt{'s', 'a', 'l', 't'};
    CHECK(hex(pbkdf2Sha512("password", salt, 1, 64)) ==
          "867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252"
          "c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce");
    std::string g = grubPasswordHash("test", 2, salt);
    CHECK(g.rfind("grub.pbkdf2.sha512.2.73616C74.", 0) == 0);
}

static void testConfig() {
    const std::string in =
        "# my settings\r\n"
        "timeout = 5\r\n"
        "view=tree\r\n"
        "alias \\Linux\\a.iso = A Linux\r\n"
        "hide /Old\r\n"
        "frobnicate = 1\r\n"
        "args /Linux/a.iso = quiet\r\n";
    Config c = Config::parse(in);
    CHECK(c.global("timeout") == "5");
    CHECK(c.global("view") == "tree");
    CHECK(c.value(Config::Kind::Alias, "/linux/A.ISO") == "A Linux");
    CHECK(c.hidden("/old"));
    CHECK(c.hiddenByParent("/Old/x.iso"));
    CHECK(!c.hiddenByParent("/Older/x.iso"));
    // Unchanged: identical output (apart from normalized spacing of settings).
    CHECK(Config::parse(c.serialize()).serialize() == c.serialize());
    CHECK(c.serialize().find("# my settings\r\nfrobnicate") == std::string::npos);
    CHECK(c.serialize().find("frobnicate = 1\r\n") != std::string::npos);

    c.setGlobal("timeout", "");                         // removed
    c.setGlobal("default", "Linux\\a.iso");             // added
    c.setValue(Config::Kind::Alias, "/Linux/a.iso", "Renamed");
    c.setHidden("/Old", false);
    c.setValue(Config::Kind::Persist, "/Linux/a.iso", "\\glassboot\\p.dat");
    std::string out = c.serialize();
    CHECK(out ==
          "# my settings\r\n"
          "view = tree\r\n"
          "alias /Linux/a.iso = Renamed\r\n"
          "frobnicate = 1\r\n"
          "args /Linux/a.iso = quiet\r\n"
          "\r\n"
          "default = /Linux/a.iso\r\n"
          "persist /Linux/a.iso = /glassboot/p.dat\r\n");
    if (failures) std::printf("---\n%s---\n", out.c_str());

    Config fresh;
    fresh.setGlobal("timeout", "3");
    CHECK(fresh.serialize().find("\ntimeout = 3\n") != std::string::npos);
    CHECK(Config().serialize().empty());
}

int main(int argc, char** argv) {
    if (argc == 4) {
        std::printf("%s\n", grubPasswordHash(argv[1], uint32_t(std::atoi(argv[3])), unhex(argv[2])).c_str());
        return 0;
    }
    testCrypto();
    testConfig();
    std::printf(failures ? "%d failure(s)\n" : "all tests passed\n", failures);
    return failures ? 1 : 0;
}
