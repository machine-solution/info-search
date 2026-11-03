#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace wikisearch {

struct QueryNode;

struct StoredDocument {
    std::string external_id;
    std::string title;
    std::string text;
    std::string url;
};

class SearchEngine {
public:
    explicit SearchEngine(const std::filesystem::path& index_directory);
    std::vector<std::uint32_t> search_boolean(
        std::string_view expression, std::size_t limit) const;
    std::vector<std::uint32_t> search_boolean(
        const QueryNode& query, std::size_t limit) const;
    StoredDocument document(std::uint32_t doc_id) const;
    std::uint64_t document_count() const { return document_count_; }
    std::size_t term_count() const { return lexicon_.size(); }

private:
    struct LexiconEntry {
        std::uint64_t offset;
        std::uint64_t size;
        std::uint32_t document_frequency;
    };
    struct LexiconRecord {
        std::string term;
        LexiconEntry entry;
    };

    const LexiconEntry* find_term(std::string_view term) const;

    std::filesystem::path index_directory_;
    std::vector<LexiconRecord> lexicon_;
    std::vector<std::uint64_t> document_offsets_;
    mutable std::ifstream documents_file_;
    std::uint64_t document_count_ = 0;
    std::uint32_t document_store_version_ = 0;
};

}  // namespace wikisearch

