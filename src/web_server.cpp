#include "web_server.hpp"

#include "query_parser.hpp"
#include "search_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(_WIN32)

namespace wikisearch {

int serve_web(const WebServerOptions&) {
    throw std::runtime_error("the local web server is currently supported on POSIX systems");
}

}  // namespace wikisearch

#else

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wikisearch {
namespace {

constexpr std::size_t kMaximumRequestBytes = 64 * 1024;
constexpr std::size_t kMaximumQueryBytes = 4096;
constexpr std::size_t kMaximumResults = 100;

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t listening_socket = -1;

void request_stop(int) {
    stop_requested = 1;
    if (listening_socket >= 0) {
        close(static_cast<int>(listening_socket));
        listening_socket = -1;
    }
}

class Socket {
public:
    explicit Socket(int descriptor = -1) : descriptor_(descriptor) {}
    ~Socket() {
        if (descriptor_ >= 0) close(descriptor_);
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }

    int get() const { return descriptor_; }

private:
    int descriptor_;
};

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

std::string snippet(std::string text, std::size_t length = 280) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    std::replace(text.begin(), text.end(), '\r', ' ');
    if (text.size() <= length) return text;
    std::size_t boundary = length;
    while (boundary > 0 &&
           (static_cast<unsigned char>(text[boundary]) & 0xc0) == 0x80) {
        --boundary;
    }
    return text.substr(0, boundary) + "…";
}

std::string result_url(const StoredDocument& document) {
    if (!document.url.empty()) return document.url;
    if (document.external_id.starts_with("http://") ||
        document.external_id.starts_with("https://")) {
        return document.external_id;
    }
    return {};
}

std::string query_tree_json(const QueryNode& node) {
    std::ostringstream output;
    output << '{';
    switch (node.type) {
        case QueryNodeType::Term:
            output << "\"type\":\"TERM\",\"term\":\""
                   << json_escape(node.term) << "\",\"children\":[]";
            break;
        case QueryNodeType::And:
        case QueryNodeType::Or:
        case QueryNodeType::Not: {
            const char* type = node.type == QueryNodeType::And
                ? "AND" : node.type == QueryNodeType::Or ? "OR" : "NOT";
            output << "\"type\":\"" << type << "\",\"children\":[";
            for (std::size_t i = 0; i < node.children.size(); ++i) {
                if (i != 0) output << ',';
                output << query_tree_json(*node.children[i]);
            }
            output << ']';
            break;
        }
    }
    output << '}';
    return output.str();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open web asset " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void send_all(int socket, std::string_view data) {
    while (!data.empty()) {
        const auto sent = send(socket, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (sent == 0) return;
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

void send_response(int socket, int status, std::string_view reason,
                   std::string_view content_type, std::string_view body,
                   bool cache_assets = false) {
    std::ostringstream headers;
    headers << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
            << "Content-Type: " << content_type << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "X-Frame-Options: DENY\r\n"
            << "Content-Security-Policy: default-src 'self'; "
               "script-src 'self'; style-src 'self'; connect-src 'self'; "
               "img-src 'self' data:\r\n"
            << "Cache-Control: " << (cache_assets ? "public, max-age=300" : "no-store")
            << "\r\n\r\n";
    send_all(socket, headers.str());
    send_all(socket, body);
}

void send_json_error(int socket, int status, std::string_view reason,
                     std::string_view message) {
    const std::string body = "{\"ok\":false,\"error\":\"" +
                             json_escape(message) + "\"}";
    send_response(socket, status, reason, "application/json; charset=utf-8", body);
}

int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return 10 + character - 'a';
    if (character >= 'A' && character <= 'F') return 10 + character - 'A';
    return -1;
}

std::string url_decode(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            decoded.push_back(' ');
        } else if (value[i] == '%') {
            if (i + 2 >= value.size()) throw std::invalid_argument("invalid URL encoding");
            const int high = hex_value(value[i + 1]);
            const int low = hex_value(value[i + 2]);
            if (high < 0 || low < 0) throw std::invalid_argument("invalid URL encoding");
            decoded.push_back(static_cast<char>((high << 4) | low));
            i += 2;
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

std::map<std::string, std::string> parse_parameters(std::string_view query) {
    std::map<std::string, std::string> parameters;
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto item = query.substr(0, ampersand);
        const auto equals = item.find('=');
        const auto key = url_decode(item.substr(0, equals));
        const auto value = equals == std::string_view::npos
            ? std::string{} : url_decode(item.substr(equals + 1));
        parameters[std::move(key)] = value;
        if (ampersand == std::string_view::npos) break;
        query.remove_prefix(ampersand + 1);
    }
    return parameters;
}

std::size_t parse_limit(const std::map<std::string, std::string>& parameters) {
    const auto found = parameters.find("limit");
    if (found == parameters.end() || found->second.empty()) return 10;
    std::size_t consumed = 0;
    const auto limit = std::stoull(found->second, &consumed);
    if (consumed != found->second.size() || limit == 0 || limit > kMaximumResults) {
        throw std::invalid_argument("limit must be between 1 and 100");
    }
    return static_cast<std::size_t>(limit);
}

std::string search_json(const SearchEngine& engine, std::string_view expression,
                        std::size_t limit) {
    const auto total_started = std::chrono::steady_clock::now();
    const auto tree = parse_boolean_query(expression);
    const auto search_started = std::chrono::steady_clock::now();
    const auto ids = engine.search_boolean(*tree, limit);
    const auto search_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - search_started).count();

    struct Result {
        std::uint32_t doc_id;
        StoredDocument document;
    };
    std::vector<Result> results;
    results.reserve(ids.size());
    for (const auto id : ids) results.push_back({id, engine.document(id)});
    const auto total_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - total_started).count();

    std::ostringstream output;
    output << std::fixed;
    output.precision(6);
    output << "{\"ok\":true,\"query\":\"" << json_escape(expression)
           << "\",\"limit\":" << limit
           << ",\"returned\":" << results.size()
           << ",\"search_ms\":" << search_ms
           << ",\"total_ms\":" << total_ms
           << ",\"query_tree\":" << query_tree_json(*tree)
           << ",\"results\":[";
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (i != 0) output << ',';
        const auto& result = results[i];
        output << "{\"rank\":" << i + 1
               << ",\"doc_id\":" << result.doc_id
               << ",\"external_id\":\"" << json_escape(result.document.external_id)
               << "\",\"title\":\"" << json_escape(result.document.title)
               << "\",\"url\":\"" << json_escape(result_url(result.document))
               << "\",\"snippet\":\"" << json_escape(snippet(result.document.text))
               << "\"}";
    }
    output << "]}";
    return output.str();
}

std::string read_request(int socket) {
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        const auto received = recv(socket, buffer, sizeof(buffer), 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot read HTTP request");
        }
        if (received == 0) break;
        request.append(buffer, static_cast<std::size_t>(received));
        if (request.size() > kMaximumRequestBytes) {
            throw std::invalid_argument("HTTP request is too large");
        }
    }
    return request;
}

void handle_connection(int socket, const SearchEngine& engine,
                       const std::filesystem::path& web_root) {
    try {
        const auto request = read_request(socket);
        const auto line_end = request.find("\r\n");
        if (line_end == std::string::npos) {
            send_json_error(socket, 400, "Bad Request", "invalid HTTP request");
            return;
        }
        const auto first_line = std::string_view(request).substr(0, line_end);
        const auto first_space = first_line.find(' ');
        const auto second_space = first_space == std::string_view::npos
            ? std::string_view::npos : first_line.find(' ', first_space + 1);
        if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
            send_json_error(socket, 400, "Bad Request", "invalid HTTP request line");
            return;
        }
        if (first_line.substr(0, first_space) != "GET") {
            send_json_error(socket, 405, "Method Not Allowed", "only GET is supported");
            return;
        }
        const auto target = first_line.substr(first_space + 1, second_space - first_space - 1);
        const auto question = target.find('?');
        const auto path = target.substr(0, question);
        const auto query_string = question == std::string_view::npos
            ? std::string_view{} : target.substr(question + 1);

        if (path == "/" || path == "/index.html") {
            const auto body = read_file(web_root / "index.html");
            send_response(socket, 200, "OK", "text/html; charset=utf-8", body);
        } else if (path == "/app.css") {
            const auto body = read_file(web_root / "app.css");
            send_response(socket, 200, "OK", "text/css; charset=utf-8", body);
        } else if (path == "/app.js") {
            const auto body = read_file(web_root / "app.js");
            send_response(socket, 200, "OK", "text/javascript; charset=utf-8", body);
        } else if (path == "/api/meta") {
            const auto body = "{\"ok\":true,\"documents\":" +
                std::to_string(engine.document_count()) + ",\"terms\":" +
                std::to_string(engine.term_count()) + "}";
            send_response(socket, 200, "OK", "application/json; charset=utf-8", body);
        } else if (path == "/api/search") {
            const auto parameters = parse_parameters(query_string);
            const auto query = parameters.find("query");
            if (query == parameters.end() || query->second.empty()) {
                throw std::invalid_argument("query must not be empty");
            }
            if (query->second.size() > kMaximumQueryBytes) {
                throw std::invalid_argument("query is too long");
            }
            const auto body = search_json(engine, query->second, parse_limit(parameters));
            send_response(socket, 200, "OK", "application/json; charset=utf-8", body);
        } else if (path == "/favicon.ico") {
            send_response(socket, 204, "No Content", "image/x-icon", {});
        } else {
            send_json_error(socket, 404, "Not Found", "route not found");
        }
    } catch (const std::invalid_argument& error) {
        send_json_error(socket, 400, "Bad Request", error.what());
    } catch (const std::exception& error) {
        send_json_error(socket, 500, "Internal Server Error", error.what());
    }
}

}  // namespace

int serve_web(const WebServerOptions& options) {
    for (const auto* asset : {"index.html", "app.css", "app.js"}) {
        if (!std::filesystem::is_regular_file(options.web_root / asset)) {
            throw std::runtime_error("missing web asset " + (options.web_root / asset).string());
        }
    }

    const auto load_started = std::chrono::steady_clock::now();
    const SearchEngine engine(options.index_directory);
    const auto load_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - load_started).count();

    Socket server(socket(AF_INET, SOCK_STREAM, 0));
    if (server.get() < 0) throw std::runtime_error("cannot create HTTP socket");
    int reuse = 1;
    if (setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        throw std::runtime_error("cannot configure HTTP socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("cannot bind 127.0.0.1:" + std::to_string(options.port) +
                                 ": " + std::strerror(errno));
    }
    if (listen(server.get(), 32) < 0) throw std::runtime_error("cannot listen on HTTP socket");

    stop_requested = 0;
    listening_socket = server.get();
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    std::cerr << "loaded " << engine.document_count() << " documents, "
              << engine.term_count() << " terms in " << load_ms << " ms\n"
              << "web UI: http://127.0.0.1:" << options.port << "\n"
              << "press Ctrl+C to stop\n";

    while (!stop_requested) {
        const int client_descriptor = accept(server.get(), nullptr, nullptr);
        if (client_descriptor < 0) {
            if (stop_requested) break;
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot accept HTTP connection");
        }
        Socket client(client_descriptor);
        handle_connection(client.get(), engine, options.web_root);
    }
    listening_socket = -1;
    return 0;
}

}  // namespace wikisearch

#endif
