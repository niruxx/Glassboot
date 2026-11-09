#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace glassboot {

// Small self-contained SHA-256 (FIPS 180-4), used to verify the payload.
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> finish();
    static std::string hex(const std::array<uint8_t, 32>& digest);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

} // namespace glassboot
