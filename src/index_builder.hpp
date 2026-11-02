#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <istream>

namespace wikisearch {

struct BuildOptions {
    std::filesystem::path index_directory;
    std::uint64_t document_limit = 1'000'000;
    std::size_t batch_size = 25'000;
};

void build_index(std::istream& input, const BuildOptions& options);

}  // namespace wikisearch

