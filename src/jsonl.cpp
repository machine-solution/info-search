#include "jsonl.hpp"

#include <cctype>
#include <cstdint>

namespace wikisearch {
namespace {

void append_utf8(std::string& output, std::uint32_t cp) {
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

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

void skip_ws(std::string_view input, std::size_t& pos) {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) {
        ++pos;
    }
}

bool parse_string(std::string_view input, std::size_t& pos, std::string& output) {
    skip_ws(input, pos);
    if (pos >= input.size() || input[pos++] != '"') return false;
    output.clear();
    while (pos < input.size()) {
        const char c = input[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            output.push_back(c);
            continue;
        }
        if (pos >= input.size()) return false;
        const char escaped = input[pos++];
        switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > input.size()) return false;
                std::uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    const int value = hex_value(input[pos++]);
                    if (value < 0) return false;
                    cp = (cp << 4) | static_cast<std::uint32_t>(value);
                }
                if (cp >= 0xd800 && cp <= 0xdbff && pos + 6 <= input.size() &&
                    input[pos] == '\\' && input[pos + 1] == 'u') {
                    pos += 2;
                    std::uint32_t low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const int value = hex_value(input[pos++]);
                        if (value < 0) return false;
                        low = (low << 4) | static_cast<std::uint32_t>(value);
                    }
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else {
                        return false;
                    }
                }
                append_utf8(output, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

bool skip_value(std::string_view input, std::size_t& pos) {
    skip_ws(input, pos);
    if (pos >= input.size()) return false;
    if (input[pos] == '"') {
        std::string ignored;
        return parse_string(input, pos, ignored);
    }
    int depth = 0;
    bool in_string = false;
    for (; pos < input.size(); ++pos) {
        const char c = input[pos];
        if (in_string) {
            if (c == '\\') ++pos;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') {
            if (depth == 0) return true;
            --depth;
        } else if (c == ',' && depth == 0) {
            return true;
        }
    }
    return true;
}

}  // namespace

bool parse_document_json(std::string_view line, SourceDocument& document, std::string& error) {
    document = {};
    std::size_t pos = 0;
    skip_ws(line, pos);
    if (pos >= line.size() || line[pos++] != '{') {
        error = "expected JSON object";
        return false;
    }
    while (true) {
        skip_ws(line, pos);
        if (pos >= line.size()) {
            error = "unterminated JSON object";
            return false;
        }
        if (line[pos] == '}') break;
        std::string key;
        if (!parse_string(line, pos, key)) {
            error = "invalid object key";
            return false;
        }
        skip_ws(line, pos);
        if (pos >= line.size() || line[pos++] != ':') {
            error = "expected colon";
            return false;
        }
        std::string* destination = nullptr;
        if (key == "docid" || key == "id") destination = &document.external_id;
        else if (key == "title") destination = &document.title;
        else if (key == "text") destination = &document.text;
        else if (key == "url") destination = &document.url;

        if (destination != nullptr) {
            if (!parse_string(line, pos, *destination)) {
                error = "field '" + key + "' is not a valid string";
                return false;
            }
        } else if (!skip_value(line, pos)) {
            error = "invalid value";
            return false;
        }
        skip_ws(line, pos);
        if (pos < line.size() && line[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < line.size() && line[pos] == '}') break;
        error = "expected comma or closing brace";
        return false;
    }
    if (document.external_id.empty() || document.title.empty() || document.text.empty()) {
        error = "id/docid, title and text must be non-empty";
        return false;
    }
    return true;
}

}  // namespace wikisearch

