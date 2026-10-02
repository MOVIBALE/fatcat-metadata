#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fatcat::detail {

// Keep raw order, duplicates and whitespace for the tower's existing index.
inline std::vector<std::string> split_difference_tokens(std::string_view entry) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < entry.size()) {
        const auto end = entry.find(';', start);
        const auto token = entry.substr(start, end - start);
        if (!token.empty()) result.emplace_back(token);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

inline std::set<std::string> difference_tokens(std::string_view entry) {
    std::set<std::string> result;
    for (const auto &token : split_difference_tokens(entry)) {
        const auto first = token.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            result.insert(token.substr(first, token.find_last_not_of(" \t\r\n") - first + 1));
        }
    }
    return result;
}

template <typename Tokens>
std::string render_difference_tokens(const Tokens &tokens) {
    std::string result;
    for (const auto &token : tokens) {
        if (!result.empty()) result += ';';
        result += token;
    }
    return result;
}

}  // namespace fatcat::detail
