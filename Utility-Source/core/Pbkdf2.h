#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace glassboot {

// Small self-contained SHA-512 (FIPS 180-4).
class Sha512 {
public:
    Sha512();
    void update(const void* data, size_t len);
    std::array<uint8_t, 64> finish();

private:
    void block(const uint8_t* p);
    uint64_t h_[8];
    uint8_t buf_[128];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

// PBKDF2-HMAC-SHA512 (RFC 8018).
std::vector<uint8_t> pbkdf2Sha512(const std::string& password, const std::vector<uint8_t>& salt,
                                  uint32_t iterations, size_t outLen);

// A GRUB password hash ("grub.pbkdf2.sha512.ITER.SALT.HASH"), the format grub-mkpasswd-pbkdf2
// prints and GRUB's password_pbkdf2 command (Glassboot's "password =" setting) accepts.
// A random 64-byte salt is used unless one is given (tests).
std::string grubPasswordHash(const std::string& password, uint32_t iterations = 10000,
                             const std::vector<uint8_t>& salt = {});

} // namespace glassboot
