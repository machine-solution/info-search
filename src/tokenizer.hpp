#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wikisearch {

std::vector<std::string> tokenize(std::string_view text);

}  // namespace wikisearch

