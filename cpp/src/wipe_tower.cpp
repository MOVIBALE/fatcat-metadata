#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "fatcat/wipe_tower.h"
#include "prusa_project.h"
#include "difference_index.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <locale.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fatcat {
namespace {

using json = nlohmann::json;

struct Dialect {
    std::string enabled_key;
    std::string x_key;
    std::string y_key;
    std::string width_key;
    std::string rotation_key;
    std::string process_difference_key;
    bool record_process_differences = true;
};

struct Position {
    std::size_t plate_index;
    double x_mm;
    double y_mm;
    bool x_negative_zero = false;
    bool y_negative_zero = false;
};

struct Settings {
    std::optional<bool> enabled;
    std::optional<double> width_mm;
    std::optional<double> rotation_deg;
    bool rotation_negative_zero = false;
    std::vector<Position> positions;
};

struct NegativeZeroPosition {
    bool x = false;
    bool y = false;
};

struct NegativeZeroSettings {
    bool rotation = false;
    std::vector<NegativeZeroPosition> positions;
};

[[noreturn]] void invalid(std::string message) {
    throw WipeTowerError(std::move(message));
}

bool is_known(const std::string &key,
              std::initializer_list<std::string_view> allowed) {
    for (const auto candidate : allowed) {
        if (key == candidate) {
            return true;
        }
    }
    return false;
}

json parse_json_text(std::string_view text, bool reject_duplicate_keys) {
    bool duplicate_key = false;
    std::string duplicate_name;
    std::vector<std::set<std::string>> object_keys;
    const json::parser_callback_t callback =
        [&](int, json::parse_event_t event, json &parsed) {
            if (!reject_duplicate_keys) {
                return true;
            }
            if (event == json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == json::parse_event_t::object_end) {
                if (!object_keys.empty()) {
                    object_keys.pop_back();
                }
            } else if (event == json::parse_event_t::key &&
                       !object_keys.empty()) {
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
        auto parsed = reject_duplicate_keys
                          ? json::parse(text, callback, true, false)
                          : json::parse(text);
        if (duplicate_key) {
            invalid("duplicate JSON field: " + duplicate_name);
        }
        return parsed;
    } catch (const json::parse_error &error) {
        throw WipeTowerError(std::string("invalid JSON: ") + error.what());
    } catch (const json::exception &error) {
        throw WipeTowerError(std::string("invalid JSON: ") + error.what());
    }
}

void skip_json_whitespace(std::string_view text, std::size_t &cursor) {
    while (cursor < text.size() &&
           std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
}

bool parse_json_string_token(std::string_view text, std::size_t &cursor,
                             std::string &decoded) {
    if (cursor >= text.size() || text[cursor] != '"') {
        return false;
    }
    const auto start = cursor;
    ++cursor;
    bool escaped = false;
    while (cursor < text.size()) {
        const char character = text[cursor++];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            try {
                decoded = json::parse(
                              std::string(text.substr(start, cursor - start)))
                              .get<std::string>();
            } catch (const json::exception &) {
                return false;
            }
            return true;
        }
    }
    return false;
}

bool skip_json_value(std::string_view text, std::size_t &cursor) {
    skip_json_whitespace(text, cursor);
    if (cursor >= text.size()) {
        return false;
    }
    if (text[cursor] == '"') {
        std::string ignored;
        return parse_json_string_token(text, cursor, ignored);
    }
    if (text[cursor] == '[') {
        ++cursor;
        skip_json_whitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == ']') {
            ++cursor;
            return true;
        }
        while (cursor < text.size()) {
            if (!skip_json_value(text, cursor)) {
                return false;
            }
            skip_json_whitespace(text, cursor);
            if (cursor < text.size() && text[cursor] == ']') {
                ++cursor;
                return true;
            }
            if (cursor >= text.size() || text[cursor] != ',') {
                return false;
            }
            ++cursor;
        }
        return false;
    }
    if (text[cursor] == '{') {
        ++cursor;
        skip_json_whitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == '}') {
            ++cursor;
            return true;
        }
        while (cursor < text.size()) {
            std::string ignored;
            if (!parse_json_string_token(text, cursor, ignored)) {
                return false;
            }
            skip_json_whitespace(text, cursor);
            if (cursor >= text.size() || text[cursor++] != ':') {
                return false;
            }
            if (!skip_json_value(text, cursor)) {
                return false;
            }
            skip_json_whitespace(text, cursor);
            if (cursor < text.size() && text[cursor] == '}') {
                ++cursor;
                return true;
            }
            if (cursor >= text.size() || text[cursor] != ',') {
                return false;
            }
            ++cursor;
            skip_json_whitespace(text, cursor);
        }
        return false;
    }

    const auto start = cursor;
    while (cursor < text.size() && text[cursor] != ',' &&
           text[cursor] != ']' && text[cursor] != '}' &&
           !std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    return cursor > start;
}

bool is_negative_zero_number(std::string_view text, std::size_t cursor) {
    if (cursor >= text.size() || text[cursor] != '-') {
        return false;
    }
    ++cursor;
    if (cursor >= text.size() || text[cursor++] != '0') {
        return false;
    }
    if (cursor < text.size() && text[cursor] == '.') {
        ++cursor;
        while (cursor < text.size() && text[cursor] >= '0' &&
               text[cursor] <= '9') {
            if (text[cursor] != '0') {
                return false;
            }
            ++cursor;
        }
    }
    if (cursor < text.size() &&
        (text[cursor] == 'e' || text[cursor] == 'E')) {
        ++cursor;
        if (cursor < text.size() &&
            (text[cursor] == '+' || text[cursor] == '-')) {
            ++cursor;
        }
        const auto exponent_start = cursor;
        while (cursor < text.size() && text[cursor] >= '0' &&
               text[cursor] <= '9') {
            if (text[cursor] != '0') {
                return false;
            }
            ++cursor;
        }
        if (cursor == exponent_start) {
            return false;
        }
    }
    return cursor == text.size() || text[cursor] == ',' || text[cursor] == '}' ||
           text[cursor] == ']' ||
           std::isspace(static_cast<unsigned char>(text[cursor]));
}

bool scan_json_value_for_negative_zero_plate_index(std::string_view text,
                                                   std::size_t &cursor) {
    skip_json_whitespace(text, cursor);
    if (cursor >= text.size()) {
        return false;
    }
    if (text[cursor] == '"') {
        std::string ignored;
        return parse_json_string_token(text, cursor, ignored);
    }
    if (text[cursor] == '[') {
        ++cursor;
        skip_json_whitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == ']') {
            ++cursor;
            return false;
        }
        while (cursor < text.size()) {
            if (scan_json_value_for_negative_zero_plate_index(text, cursor)) {
                return true;
            }
            skip_json_whitespace(text, cursor);
            if (cursor >= text.size() || text[cursor] == ']') {
                if (cursor < text.size()) {
                    ++cursor;
                }
                return false;
            }
            if (text[cursor] != ',') {
                return false;
            }
            ++cursor;
        }
        return false;
    }
    if (text[cursor] == '{') {
        ++cursor;
        skip_json_whitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == '}') {
            ++cursor;
            return false;
        }
        while (cursor < text.size()) {
            std::string key;
            if (!parse_json_string_token(text, cursor, key)) {
                return false;
            }
            skip_json_whitespace(text, cursor);
            if (cursor >= text.size() || text[cursor++] != ':') {
                return false;
            }
            skip_json_whitespace(text, cursor);
            if (key == "plate_index" &&
                is_negative_zero_number(text, cursor)) {
                return true;
            }
            if (scan_json_value_for_negative_zero_plate_index(text, cursor)) {
                return true;
            }
            skip_json_whitespace(text, cursor);
            if (cursor >= text.size() || text[cursor] == '}') {
                if (cursor < text.size()) {
                    ++cursor;
                }
                return false;
            }
            if (text[cursor] != ',') {
                return false;
            }
            ++cursor;
            skip_json_whitespace(text, cursor);
        }
        return false;
    }

    while (cursor < text.size() && text[cursor] != ',' &&
           text[cursor] != ']' && text[cursor] != '}' &&
           !std::isspace(static_cast<unsigned char>(text[cursor]))) {
        ++cursor;
    }
    return false;
}

bool has_negative_zero_plate_index(std::string_view text) {
    std::size_t cursor = 0;
    return scan_json_value_for_negative_zero_plate_index(text, cursor);
}

NegativeZeroSettings collect_negative_zero_settings(std::string_view text) {
    NegativeZeroSettings flags;
    std::size_t cursor = 0;
    skip_json_whitespace(text, cursor);
    if (cursor >= text.size() || text[cursor] != '{') {
        return flags;
    }
    ++cursor;
    skip_json_whitespace(text, cursor);
    if (cursor < text.size() && text[cursor] == '}') {
        return flags;
    }

    while (cursor < text.size()) {
        std::string key;
        if (!parse_json_string_token(text, cursor, key)) {
            return flags;
        }
        skip_json_whitespace(text, cursor);
        if (cursor >= text.size() || text[cursor++] != ':') {
            return flags;
        }
        skip_json_whitespace(text, cursor);

        if (key == "rotation_deg") {
            flags.rotation = is_negative_zero_number(text, cursor);
            if (!skip_json_value(text, cursor)) {
                return flags;
            }
        } else if (key == "positions" && cursor < text.size() &&
                   text[cursor] == '[') {
            ++cursor;
            skip_json_whitespace(text, cursor);
            if (cursor < text.size() && text[cursor] == ']') {
                ++cursor;
            } else {
                while (cursor < text.size()) {
                    NegativeZeroPosition position_flags;
                    skip_json_whitespace(text, cursor);
                    if (cursor < text.size() && text[cursor] == '{') {
                        ++cursor;
                        skip_json_whitespace(text, cursor);
                        if (cursor < text.size() && text[cursor] == '}') {
                            ++cursor;
                        } else {
                            while (cursor < text.size()) {
                                std::string position_key;
                                if (!parse_json_string_token(text, cursor,
                                                             position_key)) {
                                    return flags;
                                }
                                skip_json_whitespace(text, cursor);
                                if (cursor >= text.size() ||
                                    text[cursor++] != ':') {
                                    return flags;
                                }
                                skip_json_whitespace(text, cursor);
                                if (position_key == "x_mm") {
                                    position_flags.x =
                                        is_negative_zero_number(text, cursor);
                                } else if (position_key == "y_mm") {
                                    position_flags.y =
                                        is_negative_zero_number(text, cursor);
                                }
                                if (!skip_json_value(text, cursor)) {
                                    return flags;
                                }
                                skip_json_whitespace(text, cursor);
                                if (cursor < text.size() &&
                                    text[cursor] == '}') {
                                    ++cursor;
                                    break;
                                }
                                if (cursor >= text.size() ||
                                    text[cursor++] != ',') {
                                    return flags;
                                }
                                skip_json_whitespace(text, cursor);
                            }
                        }
                    } else if (!skip_json_value(text, cursor)) {
                        return flags;
                    }
                    flags.positions.push_back(position_flags);
                    skip_json_whitespace(text, cursor);
                    if (cursor < text.size() && text[cursor] == ']') {
                        ++cursor;
                        break;
                    }
                    if (cursor >= text.size() || text[cursor++] != ',') {
                        return flags;
                    }
                    skip_json_whitespace(text, cursor);
                }
            }
        } else if (!skip_json_value(text, cursor)) {
            return flags;
        }

        skip_json_whitespace(text, cursor);
        if (cursor < text.size() && text[cursor] == '}') {
            break;
        }
        if (cursor >= text.size() || text[cursor++] != ',') {
            return flags;
        }
        skip_json_whitespace(text, cursor);
    }
    return flags;
}

std::string required_text(const json &value, std::string_view field) {
    if (!value.is_string()) {
        invalid("wipe tower dialect field " + std::string(field) +
                " must be text");
    }
    const auto text = value.get<std::string>();
    if (text.empty()) {
        invalid("wipe tower dialect requires six distinct nonempty field names");
    }
    return text;
}

Dialect parse_dialect(const json &value) {
    if (!value.is_object()) {
        invalid("wipe tower dialect must be an object");
    }
    for (const auto &[key, ignored] : value.items()) {
        if (!is_known(key, {"enabled_key", "x_key", "y_key", "width_key",
                            "rotation_key", "process_difference_key",
                            "record_process_differences"})) {
            invalid("unknown wipe tower dialect field: " + key);
        }
    }
    const auto get = [&](std::string_view key) {
        const auto it = value.find(key);
        if (it == value.end()) {
            invalid("missing wipe tower dialect field: " + std::string(key));
        }
        return required_text(*it, key);
    };
    Dialect dialect{
        get("enabled_key"),
        get("x_key"),
        get("y_key"),
        get("width_key"),
        get("rotation_key"),
        get("process_difference_key"),
    };
    const auto record_differences = value.find("record_process_differences");
    if (record_differences != value.end()) {
        if (!record_differences->is_boolean()) {
            invalid("wipe tower dialect record_process_differences must be boolean");
        }
        dialect.record_process_differences = record_differences->get<bool>();
    }
    const std::set<std::string> keys{
        dialect.enabled_key,
        dialect.x_key,
        dialect.y_key,
        dialect.width_key,
        dialect.rotation_key,
        dialect.process_difference_key,
    };
    if (keys.size() != 6) {
        invalid("wipe tower dialect requires six distinct nonempty field names");
    }
    return dialect;
}

double finite_number(const json &value, std::string_view field) {
    if (!value.is_number()) {
        invalid("wipe tower " + std::string(field) + " must be numeric");
    }
    double number = 0.0;
    try {
        number = value.get<double>();
    } catch (const json::exception &) {
        invalid("wipe tower " + std::string(field) + " must be finite");
    }
    if (!std::isfinite(number)) {
        invalid("wipe tower " + std::string(field) + " must be finite");
    }
    return number;
}

Position parse_position(const json &value) {
    if (!value.is_object()) {
        invalid("wipe tower position must be an object");
    }
    for (const auto &[key, ignored] : value.items()) {
        if (!is_known(key, {"plate_index", "x_mm", "y_mm"})) {
            invalid("unknown wipe tower position field: " + key);
        }
    }
    const auto index_it = value.find("plate_index");
    const auto x_it = value.find("x_mm");
    const auto y_it = value.find("y_mm");
    if (index_it == value.end() || x_it == value.end() || y_it == value.end()) {
        invalid("wipe tower position requires plate_index, x_mm and y_mm");
    }
    if (!index_it->is_number_integer() && !index_it->is_number_unsigned()) {
        invalid("wipe tower plate_index must be a nonnegative integer");
    }
    std::uint64_t index = 0;
    try {
        if (index_it->is_number_unsigned()) {
            index = index_it->get<std::uint64_t>();
        } else {
            const auto signed_index = index_it->get<std::int64_t>();
            if (signed_index < 0) {
                invalid("wipe tower plate_index must be a nonnegative integer");
            }
            index = static_cast<std::uint64_t>(signed_index);
        }
    } catch (const json::exception &) {
        invalid("wipe tower plate_index must be a nonnegative integer");
    }
    if (index > std::numeric_limits<std::size_t>::max()) {
        invalid("wipe tower plate_index is too large");
    }
    return Position{
        static_cast<std::size_t>(index),
        finite_number(*x_it, "x_mm"),
        finite_number(*y_it, "y_mm"),
    };
}

Settings parse_settings(const json &value,
                        const NegativeZeroSettings &negative_zero_flags) {
    if (!value.is_object()) {
        invalid("wipe tower settings must be an object");
    }
    for (const auto &[key, ignored] : value.items()) {
        if (!is_known(key, {"enabled", "width_mm", "rotation_deg", "positions"})) {
            invalid("unknown wipe tower settings field: " + key);
        }
    }
    Settings settings;
    if (const auto it = value.find("enabled"); it != value.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            invalid("wipe tower enabled must be boolean");
        }
        settings.enabled = it->get<bool>();
    }
    if (const auto it = value.find("width_mm"); it != value.end() && !it->is_null()) {
        settings.width_mm = finite_number(*it, "width_mm");
        if (*settings.width_mm <= 0.0) {
            invalid("wipe tower width_mm must be finite and positive");
        }
    }
    if (const auto it = value.find("rotation_deg"); it != value.end() && !it->is_null()) {
        settings.rotation_deg = finite_number(*it, "rotation_deg");
        settings.rotation_negative_zero = negative_zero_flags.rotation;
    }
    if (const auto it = value.find("positions"); it != value.end()) {
        if (!it->is_array()) {
            invalid("wipe tower positions must be an array");
        }
        settings.positions.reserve(it->size());
        std::size_t position_index = 0;
        for (const auto &position : *it) {
            auto parsed_position = parse_position(position);
            if (position_index < negative_zero_flags.positions.size()) {
                parsed_position.x_negative_zero =
                    negative_zero_flags.positions[position_index].x;
                parsed_position.y_negative_zero =
                    negative_zero_flags.positions[position_index].y;
            }
            settings.positions.push_back(parsed_position);
            ++position_index;
        }
    }
    return settings;
}

std::string format_number(double value, bool preserve_negative_zero = false) {
    if (preserve_negative_zero && value == 0.0) {
        return "-0";
    }
    char buffer[128]{};
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        invalid("wipe tower number could not be formatted");
    }
    const std::string shortest(buffer, result.ptr);
    const auto exponent_marker = shortest.find_first_of("eE");
    if (exponent_marker == std::string::npos) {
        return shortest;
    }

    const auto mantissa_end = exponent_marker;
    const auto exponent_start = exponent_marker + 1;
    if (exponent_start >= shortest.size()) {
        invalid("wipe tower number could not be formatted");
    }

    int exponent_sign = 1;
    std::size_t exponent_index = exponent_start;
    if (shortest[exponent_index] == '+' || shortest[exponent_index] == '-') {
        if (shortest[exponent_index] == '-') {
            exponent_sign = -1;
        }
        ++exponent_index;
    }
    if (exponent_index >= shortest.size()) {
        invalid("wipe tower number could not be formatted");
    }
    int exponent = 0;
    for (; exponent_index < shortest.size(); ++exponent_index) {
        const auto character = shortest[exponent_index];
        if (character < '0' || character > '9') {
            invalid("wipe tower number could not be formatted");
        }
        exponent = exponent * 10 + (character - '0');
    }
    exponent *= exponent_sign;

    const bool negative = shortest.front() == '-';
    const std::size_t mantissa_start = negative ? 1 : 0;
    const auto decimal_point = shortest.find('.', mantissa_start);
    const auto integer_digits = decimal_point == std::string::npos
                                    ? mantissa_end - mantissa_start
                                    : decimal_point - mantissa_start;
    std::string digits;
    digits.reserve(mantissa_end - mantissa_start);
    for (std::size_t index = mantissa_start; index < mantissa_end; ++index) {
        if (shortest[index] != '.') {
            digits.push_back(shortest[index]);
        }
    }
    if (digits.empty()) {
        invalid("wipe tower number could not be formatted");
    }

    const auto decimal_position = static_cast<long long>(integer_digits) +
                                  static_cast<long long>(exponent);
    std::string formatted;
    if (negative) {
        formatted.push_back('-');
    }
    if (decimal_position <= 0) {
        formatted += "0.";
        formatted.append(static_cast<std::size_t>(-decimal_position), '0');
        formatted += digits;
    } else if (decimal_position >= static_cast<long long>(digits.size())) {
        formatted += digits;
        formatted.append(
            static_cast<std::size_t>(decimal_position) - digits.size(), '0');
    } else {
        const auto split = static_cast<std::size_t>(decimal_position);
        formatted.append(digits, 0, split);
        formatted.push_back('.');
        formatted.append(digits, split, std::string::npos);
    }
    return formatted;
}

bool is_decimal_float_literal(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (text[index] == '+' || text[index] == '-') {
        ++index;
    }

    bool has_digit = false;
    while (index < text.size() && text[index] >= '0' &&
           text[index] <= '9') {
        has_digit = true;
        ++index;
    }
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && text[index] >= '0' &&
               text[index] <= '9') {
            has_digit = true;
            ++index;
        }
    }
    if (!has_digit) {
        return false;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() &&
            (text[index] == '+' || text[index] == '-')) {
            ++index;
        }
        const auto exponent_start = index;
        while (index < text.size() && text[index] >= '0' &&
               text[index] <= '9') {
            ++index;
        }
        if (index == exponent_start) {
            return false;
        }
    }
    return index == text.size();
}

bool parse_finite_decimal(std::string_view text, double &number) {
    if (!is_decimal_float_literal(text)) {
        return false;
    }
    const std::string input(text);
    char *end = nullptr;
    errno = 0;
#if defined(_WIN32)
    const _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (c_locale == nullptr) {
        return false;
    }
    number = _strtod_l(input.c_str(), &end, c_locale);
    _free_locale(c_locale);
#else
    const locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    if (c_locale == nullptr) {
        return false;
    }
    number = strtod_l(input.c_str(), &end, c_locale);
    freelocale(c_locale);
#endif
    if (end != input.c_str() + input.size()) {
        return false;
    }
    // Rust's f64::from_str accepts finite underflow (for example 1e-400,
    // which becomes zero), but rejects overflow to infinity.
    return std::isfinite(number);
}

std::vector<json> coordinate_values(const json &project, const std::string &key) {
    const auto it = project.find(key);
    if (it == project.end()) {
        return {};
    }
    if (!it->is_array()) {
        invalid(key + " must be an array of finite numeric strings");
    }
    std::vector<json> values;
    values.reserve(it->size());
    for (const auto &value : *it) {
        if (!value.is_string()) {
            invalid(key + " must be an array of finite numeric strings");
        }
        const auto text = value.get<std::string>();
        const std::string_view numeric_text = text;
        double number = 0.0;
        if (!parse_finite_decimal(numeric_text, number)) {
            invalid(key + " must be an array of finite numeric strings");
        }
        values.push_back(value);
    }
    return values;
}

void update_difference_list(json &result, const std::string &key,
                            const std::set<std::string> &changed) {
    if (changed.empty()) {
        return;
    }
    const auto it = result.find(key);
    if (it == result.end() || !it->is_array()) {
        invalid("wipe tower requires the existing process difference-list entry");
    }
    if (it->empty() || !it->at(0).is_string()) {
        invalid(it->empty()
                    ? "wipe tower process difference-list entry is missing"
                    : "wipe tower process difference-list entry must be text");
    }
    auto keys = detail::split_difference_tokens(it->at(0).get_ref<const std::string &>());
    for (const auto &key_name : changed) {
        if (std::find(keys.begin(), keys.end(), key_name) == keys.end()) {
            keys.push_back(key_name);
        }
    }
    it->at(0) = detail::render_difference_tokens(keys);
}

}  // namespace

namespace detail {

bool parse_finite_decimal_string(std::string_view text, double &number) {
    return parse_finite_decimal(text, number);
}

}  // namespace detail

std::string patch_wipe_tower(std::string_view project_json,
                             std::string_view settings_json,
                             std::string_view dialect_json) {
    const auto project = parse_json_text(project_json, false);
    const auto settings_value = parse_json_text(settings_json, true);
    const auto dialect_value = parse_json_text(dialect_json, true);
    if (detail::prusa::is_target(dialect_value)) {
        try {
            return detail::prusa::patch_tower(project, settings_value).dump();
        } catch (const std::exception &error) {
            invalid(error.what());
        }
    }
    if (!project.is_object()) {
        invalid("project settings must be an object");
    }
    if (has_negative_zero_plate_index(settings_json)) {
        invalid("wipe tower plate_index must be a nonnegative integer");
    }
    const auto dialect = parse_dialect(dialect_value);
    const auto negative_zero_flags = collect_negative_zero_settings(settings_json);
    const auto settings = parse_settings(settings_value, negative_zero_flags);
    json result = project;
    std::set<std::string> changed;
    if (settings.enabled.has_value()) {
        result[dialect.enabled_key] = *settings.enabled ? "1" : "0";
        changed.insert(dialect.enabled_key);
    }
    if (settings.width_mm.has_value()) {
        result[dialect.width_key] = format_number(*settings.width_mm);
        changed.insert(dialect.width_key);
    }
    if (settings.rotation_deg.has_value()) {
        result[dialect.rotation_key] =
            format_number(*settings.rotation_deg, settings.rotation_negative_zero);
        changed.insert(dialect.rotation_key);
    }
    if (!settings.positions.empty()) {
        auto x_values = coordinate_values(result, dialect.x_key);
        auto y_values = coordinate_values(result, dialect.y_key);
        if (x_values.size() != y_values.size()) {
            invalid("wipe tower x/y plate counts differ");
        }
        std::set<std::size_t> plates;
        for (const auto &position : settings.positions) {
            if (!plates.insert(position.plate_index).second) {
                invalid("wipe tower plate_index occurs more than once");
            }
            if (x_values.empty() && position.plate_index == 0) {
                x_values.emplace_back("");
                y_values.emplace_back("");
            }
            if (position.plate_index >= x_values.size()) {
                invalid("wipe tower plate_index " +
                        std::to_string(position.plate_index) +
                        " has no existing coordinate slot");
            }
            x_values[position.plate_index] =
                format_number(position.x_mm, position.x_negative_zero);
            y_values[position.plate_index] =
                format_number(position.y_mm, position.y_negative_zero);
        }
        result[dialect.x_key] = x_values;
        result[dialect.y_key] = y_values;
        changed.insert(dialect.x_key);
        changed.insert(dialect.y_key);
    }
    if (dialect.record_process_differences) {
        update_difference_list(result, dialect.process_difference_key, changed);
    }
    return result.dump();
}

}  // namespace fatcat
