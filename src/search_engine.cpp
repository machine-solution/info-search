#include "search_engine.hpp"

#include "query_parser.hpp"
#include "varint.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <zlib.h>

namespace wikisearch {
namespace {

constexpr char kLexiconMagic[] = "WLEX1";
constexpr char kDocsMagic[] = "WDOC1";

template <typename T>
T read_pod(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw std::runtime_error("truncated index file");
    return value;
}

std::string read_bytes(std::istream& input, std::size_t maximum = 128 * 1024 * 1024) {
    std::uint64_t size = 0;
    if (!read_varint(input, size) || size > maximum) {
        throw std::runtime_error("invalid string in index file");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    input.read(value.data(), static_cast<std::streamsize>(size));
    if (!input) throw std::runtime_error("truncated string in index file");
    return value;
}

std::string read_compressed_bytes(
    std::istream& input, std::size_t maximum = 128 * 1024 * 1024) {
    std::uint64_t original_size = 0;
    std::uint64_t compressed_size = 0;
    if (!read_varint(input, original_size) || !read_varint(input, compressed_size) ||
        original_size > maximum || compressed_size > maximum) {
        throw std::runtime_error("invalid compressed string in document store");
    }
    std::string compressed(static_cast<std::size_t>(compressed_size), '\0');
    input.read(compressed.data(), static_cast<std::streamsize>(compressed_size));
    if (!input) throw std::runtime_error("truncated compressed document text");
    std::string value(static_cast<std::size_t>(original_size), '\0');
    uLongf output_size = static_cast<uLongf>(original_size);
    const auto status = uncompress(
        reinterpret_cast<Bytef*>(value.data()), &output_size,
        reinterpret_cast<const Bytef*>(compressed.data()), static_cast<uLong>(compressed.size()));
    if (status != Z_OK || output_size != original_size) {
        throw std::runtime_error("cannot decompress document text");
    }
    return value;
}

void verify_magic(std::istream& input, std::string_view expected) {
    std::string actual(expected.size(), '\0');
    input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    if (actual != expected) throw std::runtime_error("invalid index file header");
}

class PostingsCursor {
public:
    PostingsCursor(const std::filesystem::path& path, std::uint64_t offset, std::uint32_t count)
        : input_(path, std::ios::binary), remaining_(count) {
        if (!input_) throw std::runtime_error("cannot open postings.bin");
        input_.seekg(static_cast<std::streamoff>(offset));
        if (!input_) throw std::runtime_error("cannot seek postings.bin");
        advance();
    }

    bool valid() const { return valid_; }
    std::uint32_t doc_id() const { return current_; }
    std::uint64_t cost() const { return remaining_ + (valid_ ? 1u : 0u); }

    void advance() {
        if (remaining_ == 0) {
            valid_ = false;
            return;
        }
        std::uint64_t delta = 0;
        if (!read_varint(input_, delta)) throw std::runtime_error("truncated postings list");
        const auto next = static_cast<std::uint64_t>(current_) + delta;
        if (next > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("posting document id overflow");
        }
        current_ = static_cast<std::uint32_t>(next);
        --remaining_;
        valid_ = true;
    }

    void advance_to(std::uint32_t target) {
        while (valid_ && current_ < target) advance();
    }

private:
    std::ifstream input_;
    std::uint32_t remaining_ = 0;
    std::uint32_t current_ = 0;
    bool valid_ = false;
};

class DocIterator {
public:
    virtual ~DocIterator() = default;
    virtual bool valid() const = 0;
    virtual std::uint32_t doc_id() const = 0;
    virtual void next() = 0;
    virtual void advance_to(std::uint32_t target) = 0;
    virtual std::uint64_t cost() const = 0;
};

class EmptyIterator final : public DocIterator {
public:
    bool valid() const override { return false; }
    std::uint32_t doc_id() const override { return 0; }
    void next() override {}
    void advance_to(std::uint32_t) override {}
    std::uint64_t cost() const override { return 0; }
};

class TermIterator final : public DocIterator {
public:
    TermIterator(
        const std::filesystem::path& path, std::uint64_t offset, std::uint32_t count)
        : cursor_(path, offset, count) {}

    bool valid() const override { return cursor_.valid(); }
    std::uint32_t doc_id() const override { return cursor_.doc_id(); }
    void next() override { cursor_.advance(); }
    void advance_to(std::uint32_t target) override { cursor_.advance_to(target); }
    std::uint64_t cost() const override { return cursor_.cost(); }

private:
    PostingsCursor cursor_;
};

class AllDocumentsIterator final : public DocIterator {
public:
    explicit AllDocumentsIterator(std::uint64_t document_count)
        : document_count_(document_count) {}

    bool valid() const override { return current_ < document_count_; }
    std::uint32_t doc_id() const override { return static_cast<std::uint32_t>(current_); }
    void next() override {
        if (valid()) ++current_;
    }
    void advance_to(std::uint32_t target) override {
        if (current_ < target) current_ = target;
    }
    std::uint64_t cost() const override {
        return current_ < document_count_ ? document_count_ - current_ : 0;
    }

private:
    std::uint64_t document_count_ = 0;
    std::uint64_t current_ = 0;
};

class AndIterator final : public DocIterator {
public:
    explicit AndIterator(std::vector<std::unique_ptr<DocIterator>> children)
        : children_(std::move(children)) {
        std::sort(children_.begin(), children_.end(), [](const auto& lhs, const auto& rhs) {
            return lhs->cost() < rhs->cost();
        });
        align();
    }

    bool valid() const override { return valid_; }
    std::uint32_t doc_id() const override { return current_; }

    void next() override {
        if (!valid_) return;
        children_.front()->next();
        align();
    }

    void advance_to(std::uint32_t target) override {
        if (!valid_ || current_ >= target) return;
        for (auto& child : children_) child->advance_to(target);
        align();
    }

    std::uint64_t cost() const override {
        if (children_.empty()) return 0;
        std::uint64_t result = std::numeric_limits<std::uint64_t>::max();
        for (const auto& child : children_) result = std::min(result, child->cost());
        return result;
    }

private:
    void align() {
        valid_ = false;
        if (children_.empty()) return;
        while (true) {
            std::uint32_t target = 0;
            for (const auto& child : children_) {
                if (!child->valid()) return;
                target = std::max(target, child->doc_id());
            }
            bool changed = false;
            for (auto& child : children_) {
                child->advance_to(target);
                if (!child->valid()) return;
                if (child->doc_id() > target) {
                    target = child->doc_id();
                    changed = true;
                }
            }
            if (!changed) {
                current_ = target;
                valid_ = true;
                return;
            }
        }
    }

    std::vector<std::unique_ptr<DocIterator>> children_;
    std::uint32_t current_ = 0;
    bool valid_ = false;
};

class OrIterator final : public DocIterator {
public:
    explicit OrIterator(std::vector<std::unique_ptr<DocIterator>> children)
        : children_(std::move(children)) {
        rebuild_heap(0);
    }

    bool valid() const override { return !heap_.empty(); }
    std::uint32_t doc_id() const override { return heap_.empty() ? 0 : heap_.top().doc_id; }

    void next() override {
        if (heap_.empty()) return;
        const auto current = heap_.top().doc_id;
        while (!heap_.empty() && heap_.top().doc_id == current) {
            const auto index = heap_.top().child_index;
            heap_.pop();
            children_[index]->next();
            if (children_[index]->valid()) {
                heap_.push({children_[index]->doc_id(), index});
            }
        }
    }

    void advance_to(std::uint32_t target) override {
        if (!heap_.empty() && heap_.top().doc_id >= target) return;
        rebuild_heap(target);
    }

    std::uint64_t cost() const override {
        std::uint64_t result = 0;
        for (const auto& child : children_) {
            const auto child_cost = child->cost();
            if (result > std::numeric_limits<std::uint64_t>::max() - child_cost) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            result += child_cost;
        }
        return result;
    }

private:
    struct HeapEntry {
        std::uint32_t doc_id;
        std::size_t child_index;
        bool operator>(const HeapEntry& other) const {
            if (doc_id != other.doc_id) return doc_id > other.doc_id;
            return child_index > other.child_index;
        }
    };

    void rebuild_heap(std::uint32_t target) {
        heap_ = {};
        for (std::size_t i = 0; i < children_.size(); ++i) {
            children_[i]->advance_to(target);
            if (children_[i]->valid()) heap_.push({children_[i]->doc_id(), i});
        }
    }

    std::vector<std::unique_ptr<DocIterator>> children_;
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<>> heap_;
};

class AndNotIterator final : public DocIterator {
public:
    AndNotIterator(
        std::unique_ptr<DocIterator> positive, std::unique_ptr<DocIterator> negative)
        : positive_(std::move(positive)), negative_(std::move(negative)) {
        synchronize();
    }

    bool valid() const override { return valid_; }
    std::uint32_t doc_id() const override { return current_; }

    void next() override {
        if (!valid_) return;
        positive_->next();
        synchronize();
    }

    void advance_to(std::uint32_t target) override {
        if (!valid_ || current_ >= target) return;
        positive_->advance_to(target);
        synchronize();
    }

    std::uint64_t cost() const override { return positive_->cost(); }

private:
    void synchronize() {
        valid_ = false;
        while (positive_->valid()) {
            const auto candidate = positive_->doc_id();
            negative_->advance_to(candidate);
            if (!negative_->valid() || negative_->doc_id() != candidate) {
                current_ = candidate;
                valid_ = true;
                return;
            }
            positive_->next();
        }
    }

    std::unique_ptr<DocIterator> positive_;
    std::unique_ptr<DocIterator> negative_;
    std::uint32_t current_ = 0;
    bool valid_ = false;
};

class NotIterator final : public DocIterator {
public:
    NotIterator(std::unique_ptr<DocIterator> excluded, std::uint64_t document_count)
        : excluded_(std::move(excluded)), document_count_(document_count) {
        synchronize();
    }

    bool valid() const override { return valid_; }
    std::uint32_t doc_id() const override { return static_cast<std::uint32_t>(current_); }

    void next() override {
        if (!valid_) return;
        ++current_;
        synchronize();
    }

    void advance_to(std::uint32_t target) override {
        if (!valid_ || current_ >= target) return;
        current_ = target;
        synchronize();
    }

    std::uint64_t cost() const override {
        return current_ < document_count_ ? document_count_ - current_ : 0;
    }

private:
    void synchronize() {
        valid_ = false;
        while (current_ < document_count_) {
            const auto candidate = static_cast<std::uint32_t>(current_);
            excluded_->advance_to(candidate);
            if (!excluded_->valid() || excluded_->doc_id() != candidate) {
                valid_ = true;
                return;
            }
            ++current_;
        }
    }

    std::unique_ptr<DocIterator> excluded_;
    std::uint64_t document_count_ = 0;
    std::uint64_t current_ = 0;
    bool valid_ = false;
};

}  // namespace

SearchEngine::SearchEngine(const std::filesystem::path& index_directory)
    : index_directory_(index_directory),
      documents_file_(index_directory_ / "docs.bin", std::ios::binary) {
    if (!documents_file_) throw std::runtime_error("cannot open docs.bin");
    if (!std::filesystem::is_regular_file(index_directory_ / "postings.bin")) {
        throw std::runtime_error("cannot open postings.bin");
    }
    verify_magic(documents_file_, kDocsMagic);
    document_store_version_ = read_pod<std::uint32_t>(documents_file_);
    if (document_store_version_ != 1 && document_store_version_ != 2) {
        throw std::runtime_error("unsupported document store version");
    }

    std::ifstream lexicon_file(index_directory_ / "lexicon.bin", std::ios::binary);
    if (!lexicon_file) throw std::runtime_error("cannot open lexicon.bin");
    verify_magic(lexicon_file, kLexiconMagic);
    const auto version = read_pod<std::uint32_t>(lexicon_file);
    if (version != 1) throw std::runtime_error("unsupported index version");
    document_count_ = read_pod<std::uint64_t>(lexicon_file);
    const auto term_count = read_pod<std::uint64_t>(lexicon_file);
    lexicon_.reserve(static_cast<std::size_t>(term_count));
    for (std::uint64_t i = 0; i < term_count; ++i) {
        auto term = read_bytes(lexicon_file, 256);
        LexiconEntry entry{};
        entry.offset = read_pod<std::uint64_t>(lexicon_file);
        entry.size = read_pod<std::uint64_t>(lexicon_file);
        entry.document_frequency = read_pod<std::uint32_t>(lexicon_file);
        lexicon_.push_back({std::move(term), entry});
    }

    std::ifstream offsets_file(index_directory_ / "docs.offsets", std::ios::binary);
    if (!offsets_file) throw std::runtime_error("cannot open docs.offsets");
    document_offsets_.resize(static_cast<std::size_t>(document_count_));
    offsets_file.read(reinterpret_cast<char*>(document_offsets_.data()),
                      static_cast<std::streamsize>(document_offsets_.size() * sizeof(std::uint64_t)));
    if (!offsets_file) throw std::runtime_error("truncated docs.offsets");
}

const SearchEngine::LexiconEntry* SearchEngine::find_term(std::string_view term) const {
    const auto found = std::lower_bound(
        lexicon_.begin(), lexicon_.end(), term,
        [](const LexiconRecord& record, std::string_view value) { return record.term < value; });
    if (found == lexicon_.end() || found->term != term) return nullptr;
    return &found->entry;
}

std::vector<std::uint32_t> SearchEngine::search_boolean(
    std::string_view expression, std::size_t limit) const {
    if (limit == 0) return {};
    const auto query = parse_boolean_query(expression);

    std::function<std::unique_ptr<DocIterator>(const QueryNode&)> compile;
    compile = [&](const QueryNode& node) -> std::unique_ptr<DocIterator> {
        switch (node.type) {
            case QueryNodeType::Term: {
                const auto* entry = find_term(node.term);
                if (entry == nullptr) return std::make_unique<EmptyIterator>();
                return std::make_unique<TermIterator>(
                    index_directory_ / "postings.bin", entry->offset,
                    entry->document_frequency);
            }
            case QueryNodeType::Or: {
                std::vector<std::unique_ptr<DocIterator>> children;
                children.reserve(node.children.size());
                for (const auto& child : node.children) children.push_back(compile(*child));
                return std::make_unique<OrIterator>(std::move(children));
            }
            case QueryNodeType::And: {
                std::vector<std::unique_ptr<DocIterator>> positive;
                std::vector<std::unique_ptr<DocIterator>> negative;
                for (const auto& child : node.children) {
                    if (child->type == QueryNodeType::Not) {
                        negative.push_back(compile(*child->children.front()));
                    } else {
                        positive.push_back(compile(*child));
                    }
                }

                std::unique_ptr<DocIterator> positive_iterator;
                if (positive.empty()) {
                    positive_iterator =
                        std::make_unique<AllDocumentsIterator>(document_count_);
                } else if (positive.size() == 1) {
                    positive_iterator = std::move(positive.front());
                } else {
                    positive_iterator = std::make_unique<AndIterator>(std::move(positive));
                }

                if (negative.empty()) return positive_iterator;
                std::unique_ptr<DocIterator> negative_iterator;
                if (negative.size() == 1) {
                    negative_iterator = std::move(negative.front());
                } else {
                    negative_iterator = std::make_unique<OrIterator>(std::move(negative));
                }
                return std::make_unique<AndNotIterator>(
                    std::move(positive_iterator), std::move(negative_iterator));
            }
            case QueryNodeType::Not: {
                const auto& child = *node.children.front();
                if (child.type == QueryNodeType::Not) {
                    return compile(*child.children.front());
                }
                return std::make_unique<NotIterator>(compile(child), document_count_);
            }
        }
        throw std::logic_error("unknown boolean query node");
    };

    auto iterator = compile(*query);
    std::vector<std::uint32_t> result;
    result.reserve(limit);
    while (iterator->valid() && result.size() < limit) {
        result.push_back(iterator->doc_id());
        iterator->next();
    }
    return result;
}

StoredDocument SearchEngine::document(std::uint32_t doc_id) const {
    if (doc_id >= document_offsets_.size()) throw std::out_of_range("invalid document id");
    documents_file_.clear();
    documents_file_.seekg(static_cast<std::streamoff>(document_offsets_[doc_id]));
    if (!documents_file_) throw std::runtime_error("cannot seek docs.bin");
    auto external_id = read_bytes(documents_file_);
    auto title = read_bytes(documents_file_);
    if (document_store_version_ == 1) {
        return {std::move(external_id), std::move(title), read_bytes(documents_file_), {}};
    }
    auto url = read_bytes(documents_file_);
    auto text = read_compressed_bytes(documents_file_);
    return {std::move(external_id), std::move(title), std::move(text), std::move(url)};
}

}  // namespace wikisearch

