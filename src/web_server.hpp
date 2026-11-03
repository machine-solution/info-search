#pragma once

#include <cstdint>
#include <filesystem>

namespace wikisearch {

struct WebServerOptions {
    std::filesystem::path index_directory;
    std::filesystem::path web_root = "web";
    std::uint16_t port = 8080;
};

int serve_web(const WebServerOptions& options);

}  // namespace wikisearch
