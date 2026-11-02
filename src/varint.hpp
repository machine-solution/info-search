#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>

namespace wikisearch {

inline void write_varint(std::ostream& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.put(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.put(static_cast<char>(value));
    if (!out) {
        throw std::runtime_error("failed to write varint");
    }
}

inline bool read_varint(std::istream& in, std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        const int byte = in.get();
        if (byte == std::char_traits<char>::eof()) {
            if (shift == 0) {
                return false;
            }
            throw std::runtime_error("truncated varint");
        }
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    throw std::runtime_error("varint is too long");
}

}  // namespace wikisearch

