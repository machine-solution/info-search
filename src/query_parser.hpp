#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace wikisearch {

enum class QueryNodeType { Term, And, Or, Not };

struct QueryNode {
    QueryNodeType type;
    std::string term;
    std::vector<std::unique_ptr<QueryNode>> children;
};

// Grammar, from strongest to weakest precedence:
//   primary := TERM | '(' expression ')'
//   unary   := NOT unary | primary
//   and     := unary (AND unary | unary)*       // adjacency means AND
//   or      := and (OR and)*
// Double-quoted text is always treated as literal terms, so "and" searches for
// the word instead of producing an operator.
std::unique_ptr<QueryNode> parse_boolean_query(std::string_view expression);

}  // namespace wikisearch
