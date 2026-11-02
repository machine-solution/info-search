#include "tokenizer.hpp"

#include <cstdint>

namespace wikisearch {
namespace {

bool decode_utf8(std::string_view input, std::size_t& offset, char32_t& cp) {
    if (offset >= input.size()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(input[offset++]);
    if (lead < 0x80) {
        cp = lead;
        return true;
    }
    int continuation = 0;
    if ((lead & 0xe0) == 0xc0) {
        cp = lead & 0x1f;
        continuation = 1;
    } else if ((lead & 0xf0) == 0xe0) {
        cp = lead & 0x0f;
        continuation = 2;
    } else if ((lead & 0xf8) == 0xf0) {
        cp = lead & 0x07;
        continuation = 3;
    } else {
        cp = 0xfffd;
        return true;
    }
    for (int i = 0; i < continuation; ++i) {
        if (offset >= input.size()) {
            cp = 0xfffd;
            return true;
        }
        const auto byte = static_cast<unsigned char>(input[offset]);
        if ((byte & 0xc0) != 0x80) {
            cp = 0xfffd;
            return true;
        }
        ++offset;
        cp = (cp << 6) | (byte & 0x3f);
    }
    return true;
}

void append_utf8(std::string& output, char32_t cp) {
    if (cp <= 0x7f) {
        output.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

bool is_token_char(char32_t cp) {
    return (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') ||
           (cp >= U'0' && cp <= U'9') || (cp >= 0x0410 && cp <= 0x044f) ||
           cp == 0x0401 || cp == 0x0451;
}

char32_t lowercase(char32_t cp) {
    if (cp >= U'A' && cp <= U'Z') {
        return cp + (U'a' - U'A');
    }
    if (cp >= 0x0410 && cp <= 0x042f) {
        return cp + 0x20;
    }
    if (cp == 0x0401) {
        return 0x0451;
    }
    return cp;
}

}  // namespace

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> result;
    std::string token;
    std::size_t offset = 0;
    char32_t cp = 0;
    while (decode_utf8(text, offset, cp)) {
        if (is_token_char(cp)) {
            append_utf8(token, lowercase(cp));
            if (token.size() > 96) {
                token.clear();
                while (offset < text.size()) {
                    const auto saved = offset;
                    decode_utf8(text, offset, cp);
                    if (!is_token_char(cp)) {
                        break;
                    }
                    if (offset == saved) {
                        break;
                    }
                }
            }
        } else if (!token.empty()) {
            result.push_back(std::move(token));
            token.clear();
        }
    }
    if (!token.empty()) {
        result.push_back(std::move(token));
    }
    return result;
}

}  // namespace wikisearch

