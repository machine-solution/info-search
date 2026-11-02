#include "index_builder.hpp"
#include "search_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void usage(std::ostream& output) {
    output << "Wikisearch — compact corpus-independent C++ search engine\n\n"
           << "Build:\n"
           << "  wikisearch build --input FILE|- --index DIR [--limit N] [--batch-size N]\n\n"
           << "Search:\n"
           << "  wikisearch search --index DIR [--query EXPR] [--top N]\n"
           << "  EXPR supports AND, OR, NOT, parentheses, and implicit AND.\n\n"
           << "Batch JSONL:\n"
           << "  wikisearch batch --index DIR --input FILE|- --output FILE|- [--top N]\n";
}

std::string require_value(int& index, int argc, char** argv, std::string_view option) {
    if (++index >= argc) throw std::invalid_argument("missing value for " + std::string(option));
    return argv[index];
}

std::uint64_t parse_positive(std::string_view value, std::string_view name) {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(std::string(value), &consumed);
    if (consumed != value.size() || parsed == 0) {
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    }
    return parsed;
}

std::string snippet(std::string text, std::size_t length = 240) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    if (text.size() > length) {
        std::size_t boundary = length;
        while (boundary > 0 &&
               (static_cast<unsigned char>(text[boundary]) & 0xc0) == 0x80) {
            --boundary;
        }
        text = text.substr(0, boundary) + "…";
    }
    return text;
}

std::string result_url(const wikisearch::StoredDocument& document) {
    if (!document.url.empty()) return document.url;
    if (document.external_id.starts_with("http://") ||
        document.external_id.starts_with("https://")) {
        return document.external_id;
    }
    return {};
}

bool stdout_is_terminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

std::string linked_id(std::string_view external_id, std::string_view url) {
    const std::string label = "[" + std::string(external_id) + "]";
    if (url.empty() || !stdout_is_terminal()) return label;
    return "\033]8;;" + std::string(url) + "\033\\" + label + "\033]8;;\033\\";
}

int build_command(int argc, char** argv) {
    std::string input_path;
    wikisearch::BuildOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--input") input_path = require_value(i, argc, argv, argument);
        else if (argument == "--index") options.index_directory = require_value(i, argc, argv, argument);
        else if (argument == "--limit") options.document_limit = parse_positive(require_value(i, argc, argv, argument), argument);
        else if (argument == "--batch-size") options.batch_size = static_cast<std::size_t>(parse_positive(require_value(i, argc, argv, argument), argument));
        else throw std::invalid_argument("unknown build option: " + std::string(argument));
    }
    if (input_path.empty() || options.index_directory.empty()) {
        throw std::invalid_argument("build requires --input and --index");
    }
    if (input_path == "-") {
        wikisearch::build_index(std::cin, options);
    } else {
        std::ifstream input(input_path);
        if (!input) throw std::runtime_error("cannot open input " + input_path);
        wikisearch::build_index(input, options);
    }
    return 0;
}

void print_results(const wikisearch::SearchEngine& engine, std::string_view query,
                   std::size_t top) {
    const auto started = std::chrono::steady_clock::now();
    const auto results = engine.search_boolean(query, top);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "found " << results.size() << " result(s)\n";
    for (std::size_t rank = 0; rank < results.size(); ++rank) {
        const auto doc = engine.document(results[rank]);
        const auto url = result_url(doc);
        std::cout << '\n' << rank + 1 << ". " << linked_id(doc.external_id, url) << ' '
                  << doc.title << '\n';
        if (!url.empty()) std::cout << "   " << url << '\n';
        std::cout << "   " << snippet(doc.text) << '\n';
    }
    std::cout << "\nquery time: " << elapsed << " ms\n";
}

int search_command(int argc, char** argv) {
    std::filesystem::path index_directory;
    std::string query;
    std::size_t top = 10;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--index") index_directory = require_value(i, argc, argv, argument);
        else if (argument == "--query") query = require_value(i, argc, argv, argument);
        else if (argument == "--top") {
            top = static_cast<std::size_t>(parse_positive(require_value(i, argc, argv, argument), argument));
        } else throw std::invalid_argument("unknown search option: " + std::string(argument));
    }
    if (index_directory.empty()) throw std::invalid_argument("search requires --index");
    const wikisearch::SearchEngine engine(index_directory);
    std::cerr << "loaded " << engine.document_count() << " documents, " << engine.term_count()
              << " terms\n";
    if (!query.empty()) {
        print_results(engine, query, top);
        return 0;
    }
    std::cout << "Interactive Boolean search (AND, OR, NOT, parentheses). "
                 "Empty line exits.\n";
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, query) || query.empty()) break;
        print_results(engine, query, top);
    }
    return 0;
}

std::string json_escape(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (byte < 0x20) {
                    escaped += "\\u00";
                    escaped.push_back(hex[byte >> 4]);
                    escaped.push_back(hex[byte & 0x0f]);
                } else {
                    escaped.push_back(static_cast<char>(byte));
                }
        }
    }
    return escaped;
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    auto index = static_cast<std::size_t>(std::ceil(fraction * sorted.size()));
    if (index == 0) index = 1;
    return sorted[std::min(index - 1, sorted.size() - 1)];
}

int batch_command(int argc, char** argv) {
    std::filesystem::path index_directory;
    std::string input_path;
    std::string output_path;
    std::size_t top = 10;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--index") {
            index_directory = require_value(i, argc, argv, argument);
        } else if (argument == "--input") {
            input_path = require_value(i, argc, argv, argument);
        } else if (argument == "--output") {
            output_path = require_value(i, argc, argv, argument);
        } else if (argument == "--top") {
            top = static_cast<std::size_t>(
                parse_positive(require_value(i, argc, argv, argument), argument));
        } else {
            throw std::invalid_argument("unknown batch option: " + std::string(argument));
        }
    }
    if (index_directory.empty() || input_path.empty() || output_path.empty()) {
        throw std::invalid_argument("batch requires --index, --input, and --output");
    }

    std::ifstream input_file;
    std::istream* input = &std::cin;
    if (input_path != "-") {
        input_file.open(input_path);
        if (!input_file) throw std::runtime_error("cannot open input " + input_path);
        input = &input_file;
    }

    std::ofstream output_file;
    std::ostream* output = &std::cout;
    if (output_path != "-") {
        output_file.open(output_path, std::ios::trunc);
        if (!output_file) throw std::runtime_error("cannot create output " + output_path);
        output = &output_file;
    }

    const wikisearch::SearchEngine engine(index_directory);
    std::cerr << "loaded " << engine.document_count() << " documents, "
              << engine.term_count() << " terms\n";

    const auto batch_started = std::chrono::steady_clock::now();
    std::vector<double> search_times;
    std::size_t query_count = 0;
    std::size_t failed_count = 0;
    std::size_t returned_documents = 0;
    std::string line;
    std::size_t line_number = 0;
    *output << std::fixed << std::setprecision(6);

    while (std::getline(*input, line)) {
        ++line_number;
        const auto expression = trim(line);
        if (expression.empty() || expression.front() == '#') continue;
        ++query_count;
        const auto query_started = std::chrono::steady_clock::now();
        try {
            const auto search_started = std::chrono::steady_clock::now();
            const auto ids = engine.search_boolean(expression, top);
            const auto search_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - search_started).count();
            search_times.push_back(search_ms);
            returned_documents += ids.size();

            struct Result {
                std::uint32_t doc_id;
                wikisearch::StoredDocument document;
            };
            std::vector<Result> results;
            results.reserve(ids.size());
            for (const auto id : ids) results.push_back({id, engine.document(id)});
            const auto total_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - query_started).count();

            *output << "{\"type\":\"query\",\"line\":" << line_number
                    << ",\"query\":\"" << json_escape(expression)
                    << "\",\"ok\":true,\"returned\":" << results.size()
                    << ",\"search_ms\":" << search_ms
                    << ",\"total_ms\":" << total_ms << ",\"results\":[";
            for (std::size_t i = 0; i < results.size(); ++i) {
                if (i != 0) *output << ',';
                const auto& result = results[i];
                const auto url = result_url(result.document);
                *output << "{\"rank\":" << i + 1
                        << ",\"doc_id\":" << result.doc_id
                        << ",\"external_id\":\""
                        << json_escape(result.document.external_id)
                        << "\",\"title\":\"" << json_escape(result.document.title)
                        << "\",\"url\":\"" << json_escape(url)
                        << "\",\"snippet\":\""
                        << json_escape(snippet(result.document.text)) << "\"}";
            }
            *output << "]}\n";
        } catch (const std::exception& exception) {
            ++failed_count;
            const auto total_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - query_started).count();
            *output << "{\"type\":\"query\",\"line\":" << line_number
                    << ",\"query\":\"" << json_escape(expression)
                    << "\",\"ok\":false,\"total_ms\":" << total_ms
                    << ",\"error\":\"" << json_escape(exception.what()) << "\"}\n";
        }
    }

    std::sort(search_times.begin(), search_times.end());
    double sum = 0.0;
    for (const auto value : search_times) sum += value;
    const auto batch_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - batch_started).count();
    *output << "{\"type\":\"summary\",\"queries\":" << query_count
            << ",\"succeeded\":" << search_times.size()
            << ",\"failed\":" << failed_count
            << ",\"returned_documents\":" << returned_documents
            << ",\"mean_search_ms\":"
            << (search_times.empty() ? 0.0 : sum / search_times.size())
            << ",\"p50_search_ms\":" << percentile(search_times, 0.50)
            << ",\"p95_search_ms\":" << percentile(search_times, 0.95)
            << ",\"max_search_ms\":"
            << (search_times.empty() ? 0.0 : search_times.back())
            << ",\"batch_wall_ms\":" << batch_ms << "}\n";
    if (!*output) throw std::runtime_error("failed to write batch output");
    return failed_count == 0 ? 0 : 3;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }
        const std::string_view command = argv[1];
        if (command == "build") return build_command(argc, argv);
        if (command == "search") return search_command(argc, argv);
        if (command == "batch") return batch_command(argc, argv);
        if (command == "--help" || command == "help") {
            usage(std::cout);
            return 0;
        }
        throw std::invalid_argument("unknown command: " + std::string(command));
    } catch (const std::exception& exception) {
        std::cerr << "error: " << exception.what() << '\n';
        return 1;
    }
}

