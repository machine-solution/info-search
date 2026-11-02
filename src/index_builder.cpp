#include "index_builder.hpp"

#include "jsonl.hpp"
#include "tokenizer.hpp"
#include "varint.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <zlib.h>

namespace wikisearch {
namespace {

constexpr std::uint32_t kIndexFormatVersion = 1;
constexpr std::uint32_t kDocumentFormatVersion = 2;
constexpr char kSegmentMagic[] = "WSEG1";
constexpr char kLexiconMagic[] = "WLEX1";
constexpr char kDocsMagic[] = "WDOC1";

template <typename T>
void write_pod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) throw std::runtime_error("failed to write index file");
}

void write_bytes(std::ostream& output, const std::string& value) {
    write_varint(output, value.size());
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) throw std::runtime_error("failed to write index file");
}

void write_compressed_bytes(std::ostream& output, const std::string& value) {
    const auto bound = compressBound(static_cast<uLong>(value.size()));
    std::string compressed(static_cast<std::size_t>(bound), '\0');
    uLongf compressed_size = bound;
    const auto status = compress2(
        reinterpret_cast<Bytef*>(compressed.data()), &compressed_size,
        reinterpret_cast<const Bytef*>(value.data()), static_cast<uLong>(value.size()),
        Z_BEST_SPEED);
    if (status != Z_OK) throw std::runtime_error("failed to compress document text");
    compressed.resize(static_cast<std::size_t>(compressed_size));
    write_varint(output, value.size());
    write_varint(output, compressed.size());
    output.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));
    if (!output) throw std::runtime_error("failed to write compressed document text");
}

struct SegmentRecord {
    std::string term;
    std::vector<std::uint32_t> doc_ids;
};

class SegmentReader {
public:
    explicit SegmentReader(const std::filesystem::path& path) : input_(path, std::ios::binary) {
        if (!input_) throw std::runtime_error("cannot open segment " + path.string());
        char magic[sizeof(kSegmentMagic) - 1]{};
        input_.read(magic, sizeof(magic));
        if (std::string_view(magic, sizeof(magic)) != std::string_view(kSegmentMagic)) {
            throw std::runtime_error("bad segment header in " + path.string());
        }
        advance();
    }

    bool valid() const { return valid_; }
    const SegmentRecord& record() const { return record_; }

    void advance() {
        std::uint64_t term_size = 0;
        if (!read_varint(input_, term_size)) {
            valid_ = false;
            return;
        }
        if (term_size > 256) throw std::runtime_error("invalid term length in segment");
        record_.term.assign(term_size, '\0');
        input_.read(record_.term.data(), static_cast<std::streamsize>(term_size));
        std::uint64_t count = 0;
        if (!read_varint(input_, count)) throw std::runtime_error("truncated segment");
        record_.doc_ids.clear();
        record_.doc_ids.reserve(static_cast<std::size_t>(count));
        std::uint64_t previous = 0;
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t delta = 0;
            if (!read_varint(input_, delta)) throw std::runtime_error("truncated segment postings");
            previous += delta;
            record_.doc_ids.push_back(static_cast<std::uint32_t>(previous));
        }
        valid_ = true;
    }

private:
    std::ifstream input_;
    SegmentRecord record_;
    bool valid_ = false;
};

void flush_segment(
    std::unordered_map<std::string, std::vector<std::uint32_t>>& postings,
    const std::filesystem::path& path) {
    std::vector<std::string> terms;
    terms.reserve(postings.size());
    for (const auto& [term, _] : postings) terms.push_back(term);
    std::sort(terms.begin(), terms.end());

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create segment " + path.string());
    output.write(kSegmentMagic, sizeof(kSegmentMagic) - 1);
    for (const auto& term : terms) {
        auto& ids = postings.at(term);
        write_bytes(output, term);
        write_varint(output, ids.size());
        std::uint32_t previous = 0;
        for (const auto id : ids) {
            write_varint(output, static_cast<std::uint64_t>(id - previous));
            previous = id;
        }
    }
}

struct HeapItem {
    std::string term;
    std::size_t reader_index;
    bool operator>(const HeapItem& other) const {
        if (term != other.term) return term > other.term;
        return reader_index > other.reader_index;
    }
};

std::uint64_t merge_segments(
    const std::vector<std::filesystem::path>& segment_paths,
    const std::filesystem::path& index_directory,
    std::uint64_t document_count) {
    std::vector<std::unique_ptr<SegmentReader>> readers;
    readers.reserve(segment_paths.size());
    std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<>> heap;
    for (std::size_t i = 0; i < segment_paths.size(); ++i) {
        readers.push_back(std::make_unique<SegmentReader>(segment_paths[i]));
        if (readers.back()->valid()) heap.push({readers.back()->record().term, i});
    }

    std::ofstream postings(index_directory / "postings.bin", std::ios::binary | std::ios::trunc);
    std::ofstream lexicon(index_directory / "lexicon.bin", std::ios::binary | std::ios::trunc);
    if (!postings || !lexicon) throw std::runtime_error("cannot create final index files");
    lexicon.write(kLexiconMagic, sizeof(kLexiconMagic) - 1);
    write_pod(lexicon, kIndexFormatVersion);
    write_pod(lexicon, document_count);
    const auto term_count_position = lexicon.tellp();
    std::uint64_t term_count = 0;
    write_pod(lexicon, term_count);

    while (!heap.empty()) {
        const std::string term = heap.top().term;
        std::vector<std::uint32_t> merged;
        while (!heap.empty() && heap.top().term == term) {
            const auto reader_index = heap.top().reader_index;
            heap.pop();
            const auto& ids = readers[reader_index]->record().doc_ids;
            merged.insert(merged.end(), ids.begin(), ids.end());
            readers[reader_index]->advance();
            if (readers[reader_index]->valid()) {
                heap.push({readers[reader_index]->record().term, reader_index});
            }
        }
        std::sort(merged.begin(), merged.end());
        merged.erase(std::unique(merged.begin(), merged.end()), merged.end());

        const auto offset = static_cast<std::uint64_t>(postings.tellp());
        std::uint32_t previous = 0;
        for (const auto id : merged) {
            write_varint(postings, static_cast<std::uint64_t>(id - previous));
            previous = id;
        }
        const auto end = static_cast<std::uint64_t>(postings.tellp());
        write_bytes(lexicon, term);
        write_pod(lexicon, offset);
        write_pod(lexicon, end - offset);
        const auto df = static_cast<std::uint32_t>(merged.size());
        write_pod(lexicon, df);
        ++term_count;
    }

    lexicon.seekp(term_count_position);
    write_pod(lexicon, term_count);
    return term_count;
}

}  // namespace

void build_index(std::istream& input, const BuildOptions& options) {
    if (options.batch_size == 0 || options.document_limit == 0) {
        throw std::invalid_argument("limit and batch size must be positive");
    }
    std::filesystem::create_directories(options.index_directory);
    const auto temp_directory = options.index_directory / "segments.tmp";
    std::filesystem::remove_all(temp_directory);
    std::filesystem::create_directories(temp_directory);

    std::ofstream docs(options.index_directory / "docs.bin", std::ios::binary | std::ios::trunc);
    std::ofstream offsets(options.index_directory / "docs.offsets", std::ios::binary | std::ios::trunc);
    if (!docs || !offsets) throw std::runtime_error("cannot create document store");
    docs.write(kDocsMagic, sizeof(kDocsMagic) - 1);
    write_pod(docs, kDocumentFormatVersion);

    std::unordered_map<std::string, std::vector<std::uint32_t>> postings;
    std::vector<std::filesystem::path> segment_paths;
    std::string line;
    std::uint64_t line_number = 0;
    std::uint64_t document_count = 0;
    std::uint64_t token_count = 0;
    std::uint64_t term_document_pairs = 0;
    const auto started = std::chrono::steady_clock::now();

    auto flush = [&] {
        if (postings.empty()) return;
        const auto path = temp_directory / ("segment-" + std::to_string(segment_paths.size()) + ".bin");
        flush_segment(postings, path);
        segment_paths.push_back(path);
        postings.clear();
        postings.rehash(0);
        std::cerr << "built segment " << segment_paths.size() << " at " << document_count
                  << " documents\n";
    };

    while (document_count < options.document_limit && std::getline(input, line)) {
        ++line_number;
        SourceDocument document;
        std::string error;
        if (!parse_document_json(line, document, error)) {
            throw std::runtime_error("JSONL line " + std::to_string(line_number) + ": " + error);
        }
        if (document_count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("document count exceeds uint32 capacity");
        }
        const auto doc_id = static_cast<std::uint32_t>(document_count);
        const auto doc_offset = static_cast<std::uint64_t>(docs.tellp());
        write_pod(offsets, doc_offset);
        write_bytes(docs, document.external_id);
        write_bytes(docs, document.title);
        write_bytes(docs, document.url);
        write_compressed_bytes(docs, document.text);

        auto title_tokens = tokenize(document.title);
        auto text_tokens = tokenize(document.text);
        token_count += title_tokens.size() + text_tokens.size();
        std::unordered_set<std::string> unique_terms;
        for (auto& term : title_tokens) unique_terms.insert(std::move(term));
        for (auto& term : text_tokens) unique_terms.insert(std::move(term));
        term_document_pairs += unique_terms.size();
        for (auto& term : unique_terms) postings[std::move(term)].push_back(doc_id);

        ++document_count;
        if (document_count % options.batch_size == 0) flush();
    }
    flush();
    docs.close();
    offsets.close();
    if (document_count == 0) throw std::runtime_error("input contained no documents");

    std::cerr << "merging " << segment_paths.size() << " segments\n";
    const auto term_count = merge_segments(segment_paths, options.index_directory, document_count);
    std::filesystem::remove_all(temp_directory);

    std::ofstream metadata(options.index_directory / "metadata.txt", std::ios::trunc);
    metadata << "format_version=" << kIndexFormatVersion << '\n';
    metadata << "document_format_version=" << kDocumentFormatVersion << '\n';
    metadata << "document_text_compression=zlib-level-1\n";
    metadata << "documents=" << document_count << '\n';
    metadata << "terms=" << term_count << '\n';
    metadata << "tokens=" << token_count << '\n';
    metadata << "term_document_pairs=" << term_document_pairs << '\n';
    metadata << "requested_limit=" << options.document_limit << '\n';
    metadata << "tokenizer=utf8-ru-en-lowercase-v1\n";
    metadata << "postings=delta-varint-v1\n";
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    metadata << "build_seconds=" << elapsed << '\n';
    std::cerr << "index ready: " << document_count << " documents in " << elapsed << " s\n";
}

}  // namespace wikisearch

