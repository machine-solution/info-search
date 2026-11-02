#include "query_parser.hpp"

#include "tokenizer.hpp"

#include <cctype>
#include <deque>
#include <stdexcept>
#include <utility>

namespace wikisearch {
namespace {

enum class TokenType { Term, And, Or, Not, LeftParen, RightParen, End };

struct Token {
    TokenType type = TokenType::End;
    std::string text;
    std::size_t position = 0;
};

bool ascii_equal_ignore_case(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto left = static_cast<unsigned char>(lhs[i]);
        const auto right = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    Token next() {
        if (!pending_.empty()) {
            auto token = std::move(pending_.front());
            pending_.pop_front();
            return token;
        }

        while (offset_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[offset_]))) {
            ++offset_;
        }
        if (offset_ == input_.size()) return {TokenType::End, {}, offset_};

        const auto position = offset_;
        if (input_[offset_] == '(') {
            ++offset_;
            return {TokenType::LeftParen, "(", position};
        }
        if (input_[offset_] == ')') {
            ++offset_;
            return {TokenType::RightParen, ")", position};
        }
        if (input_[offset_] == '"') {
            ++offset_;
            std::string literal;
            bool closed = false;
            while (offset_ < input_.size()) {
                const char current = input_[offset_++];
                if (current == '"') {
                    closed = true;
                    break;
                }
                if (current == '\\' && offset_ < input_.size()) {
                    literal.push_back(input_[offset_++]);
                } else {
                    literal.push_back(current);
                }
            }
            if (!closed) {
                throw std::invalid_argument(
                    "unterminated quoted text at byte " + std::to_string(position));
            }
            enqueue_terms(literal, position);
            if (pending_.empty()) {
                throw std::invalid_argument(
                    "quoted text contains no searchable terms at byte " +
                    std::to_string(position));
            }
            return next();
        }

        while (offset_ < input_.size() &&
               !std::isspace(static_cast<unsigned char>(input_[offset_])) &&
               input_[offset_] != '(' && input_[offset_] != ')') {
            ++offset_;
        }
        const auto raw = input_.substr(position, offset_ - position);
        if (ascii_equal_ignore_case(raw, "AND")) return {TokenType::And, {}, position};
        if (ascii_equal_ignore_case(raw, "OR")) return {TokenType::Or, {}, position};
        if (ascii_equal_ignore_case(raw, "NOT")) return {TokenType::Not, {}, position};

        enqueue_terms(raw, position);
        if (pending_.empty()) return next();
        return next();
    }

private:
    void enqueue_terms(std::string_view text, std::size_t position) {
        for (auto& term : tokenize(text)) {
            pending_.push_back({TokenType::Term, std::move(term), position});
        }
    }

    std::string_view input_;
    std::size_t offset_ = 0;
    std::deque<Token> pending_;
};

std::unique_ptr<QueryNode> make_node(
    QueryNodeType type, std::vector<std::unique_ptr<QueryNode>> children) {
    auto node = std::make_unique<QueryNode>();
    node->type = type;
    node->children = std::move(children);
    return node;
}

class Parser {
public:
    explicit Parser(std::string_view expression) : lexer_(expression), current_(lexer_.next()) {}

    std::unique_ptr<QueryNode> parse() {
        if (current_.type == TokenType::End) {
            throw std::invalid_argument("boolean query is empty");
        }
        auto result = parse_or();
        if (current_.type != TokenType::End) {
            throw std::invalid_argument(
                "unexpected token at byte " + std::to_string(current_.position));
        }
        return result;
    }

private:
    static bool starts_unary(TokenType type) {
        return type == TokenType::Term || type == TokenType::Not ||
               type == TokenType::LeftParen;
    }

    void consume() { current_ = lexer_.next(); }

    std::unique_ptr<QueryNode> parse_or() {
        std::vector<std::unique_ptr<QueryNode>> children;
        children.push_back(parse_and());
        while (current_.type == TokenType::Or) {
            const auto position = current_.position;
            consume();
            if (!starts_unary(current_.type)) {
                throw std::invalid_argument(
                    "OR has no right operand at byte " + std::to_string(position));
            }
            children.push_back(parse_and());
        }
        if (children.size() == 1) return std::move(children.front());
        return make_node(QueryNodeType::Or, std::move(children));
    }

    std::unique_ptr<QueryNode> parse_and() {
        std::vector<std::unique_ptr<QueryNode>> children;
        children.push_back(parse_unary());
        while (true) {
            if (current_.type == TokenType::And) {
                const auto position = current_.position;
                consume();
                if (!starts_unary(current_.type)) {
                    throw std::invalid_argument(
                        "AND has no right operand at byte " + std::to_string(position));
                }
                children.push_back(parse_unary());
            } else if (starts_unary(current_.type)) {
                // Plain adjacent terms use the search-engine default: conjunction.
                children.push_back(parse_unary());
            } else {
                break;
            }
        }
        if (children.size() == 1) return std::move(children.front());
        return make_node(QueryNodeType::And, std::move(children));
    }

    std::unique_ptr<QueryNode> parse_unary() {
        if (current_.type == TokenType::Not) {
            const auto position = current_.position;
            consume();
            if (!starts_unary(current_.type)) {
                throw std::invalid_argument(
                    "NOT has no operand at byte " + std::to_string(position));
            }
            std::vector<std::unique_ptr<QueryNode>> children;
            children.push_back(parse_unary());
            return make_node(QueryNodeType::Not, std::move(children));
        }
        return parse_primary();
    }

    std::unique_ptr<QueryNode> parse_primary() {
        if (current_.type == TokenType::Term) {
            auto node = std::make_unique<QueryNode>();
            node->type = QueryNodeType::Term;
            node->term = std::move(current_.text);
            consume();
            return node;
        }
        if (current_.type == TokenType::LeftParen) {
            const auto position = current_.position;
            consume();
            if (current_.type == TokenType::RightParen) {
                throw std::invalid_argument(
                    "empty parentheses at byte " + std::to_string(position));
            }
            auto node = parse_or();
            if (current_.type != TokenType::RightParen) {
                throw std::invalid_argument(
                    "missing ')' for '(' at byte " + std::to_string(position));
            }
            consume();
            return node;
        }
        throw std::invalid_argument(
            "expected term, NOT, or '(' at byte " + std::to_string(current_.position));
    }

    Lexer lexer_;
    Token current_;
};

}  // namespace

std::unique_ptr<QueryNode> parse_boolean_query(std::string_view expression) {
    return Parser(expression).parse();
}

}  // namespace wikisearch
