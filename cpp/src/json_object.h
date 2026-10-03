#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fatcat::detail {

// The two composers use the same JSON contract and keep their own error type.
template <typename Error>
nlohmann::json parse_json_object(std::string_view text, std::string_view name) {
    using json = nlohmann::json;
    bool duplicate_key = false;
    std::string duplicate_name;
    std::vector<std::set<std::string>> object_keys;
    const json::parser_callback_t callback =
        [&](int, json::parse_event_t event, json &parsed) {
            if (event == json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == json::parse_event_t::object_end) {
                if (!object_keys.empty()) object_keys.pop_back();
            } else if (event == json::parse_event_t::key && !object_keys.empty()) {
                const auto key = parsed.get<std::string>();
                if (!object_keys.back().insert(key).second) {
                    duplicate_key = true;
                    duplicate_name = key;
                    return false;
                }
            }
            return true;
        };
    try {
        json parsed = json::parse(text, callback, true, false);
        if (duplicate_key) {
            throw Error("duplicate JSON field in " + std::string(name) + ": " +
                        duplicate_name);
        }
        if (!parsed.is_object()) {
            throw Error(std::string(name) + " must be a JSON object");
        }
        return parsed;
    } catch (const json::exception &error) {
        throw Error("invalid " + std::string(name) + " JSON: " + error.what());
    }
}

}  // namespace fatcat::detail
