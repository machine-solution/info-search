#pragma once

#include <string>
#include <string_view>

namespace wikisearch {

struct SourceDocument {
    std::string external_id;
    std::string title;
    std::string text;
    std::string url;
};

bool parse_document_json(std::string_view line, SourceDocument& document, std::string& error);

}  // namespace wikisearch

