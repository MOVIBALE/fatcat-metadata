#include "native_preset_compatibility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <locale>
#include <regex>
#include <sstream>
#include <string_view>

#include "fatcat/project_settings.h"

namespace fatcat::detail {
namespace {
using json = nlohmann::json;

[[noreturn]] void invalid_condition(const std::string &reason) {
    throw ProjectSettingsError("native compatible_printers_condition: " + reason);
}

double number(const json &value) {
    if (value.is_number()) return value.get<double>();
    if (!value.is_string()) invalid_condition("expected a numeric machine option");
    std::istringstream stream(value.get<std::string>());
    stream.imbue(std::locale::classic());
    double result;
    if (!(stream >> result) || !stream.eof() || !std::isfinite(result)) {
        invalid_condition("invalid numeric machine option");
    }
    return result;
}

bool boolean(const json &value) {
    if (value.is_boolean()) return value.get<bool>();
    invalid_condition("expected a boolean expression");
}

std::string text(const json &value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (!value.is_number()) invalid_condition("expected a scalar operand");
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << value.get<double>();
    return stream.str();
}

json typed_option(const json &value, const std::string &key, const json &fields) {
    const auto field = fields.find(key);
    if (field == fields.end() || !field->contains("definitions")) {
        invalid_condition("no native type for machine option '" + key + "'");
    }
    std::string type;
    for (const auto &definition : field->at("definitions")) {
        const auto candidate = definition.value("native_type", std::string());
        if (!type.empty() && candidate != type) {
            invalid_condition("ambiguous native type for machine option '" + key + "'");
        }
        type = candidate;
    }
    if (type == "coBool" || type == "coBools") {
        if (value.is_boolean()) return value;
        if (value == "1" || value == 1) return true;
        if (value == "0" || value == 0) return false;
        invalid_condition("invalid boolean machine option '" + key + "'");
    }
    if (type == "coInt" || type == "coInts" || type == "coFloat" || type == "coFloats" ||
        type == "coFloatOrPercent" || type == "coFloatsOrPercents") {
        // Relative percentages require a native ratio/dependency context. Do
        // not guess their effective value when deciding preset compatibility.
        return number(value);
    }
    if (type == "coPercent" || type == "coPercents") {
        auto serialized = text(value);
        if (!serialized.empty() && serialized.back() == '%') serialized.pop_back();
        return number(serialized);
    }
    if (type == "coString" || type == "coStrings" || type == "coEnum" ||
        type == "coPoint" || type == "coPoints") return text(value);
    // Native enum arrays expose integer codes, unlike scalar enum labels.
    // The field inventory has no code map, so they cannot authorize a preset.
    invalid_condition("unsupported native type for machine option '" + key + "'");
}

// The native preset condition language, independent of G-code expansion.
// Unsupported syntax or missing variables cannot authorize a preset. The
// installed catalogue is exercised against every supported machine in CI.
class Condition {
public:
    Condition(std::string_view source, const json &machine, const std::string &name,
              const json &fields)
        : source_(source), machine_(machine), name_(name), fields_(fields) {}

    bool evaluate() {
        const auto result = expression();
        whitespace();
        if (position_ != source_.size()) invalid_condition("unsupported syntax near " +
            std::string(source_.substr(position_, 32)));
        return boolean(result);
    }

private:
    std::string_view source_;
    const json &machine_;
    const std::string &name_;
    const json &fields_;
    std::size_t position_ = 0;
    unsigned depth_ = 0;

    void whitespace() {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_]))) ++position_;
    }

    bool take(std::string_view token) {
        whitespace();
        if (source_.substr(position_, token.size()) != token) return false;
        const auto end = position_ + token.size();
        if (std::isalpha(static_cast<unsigned char>(token.back())) && end < source_.size() &&
            (std::isalnum(static_cast<unsigned char>(source_[end])) || source_[end] == '_')) return false;
        position_ = end;
        return true;
    }

    void require(std::string_view token) {
        if (!take(token)) invalid_condition("expected '" + std::string(token) + "'");
    }

    json expression() {
        auto result = conjunction();
        while (take("or") || take("||")) {
            const auto right = conjunction();
            const bool left_value = boolean(result), right_value = boolean(right);
            result = left_value || right_value;
        }
        return result;
    }

    json conjunction() {
        auto result = comparison();
        while (take("and") || take("&&")) {
            const auto right = comparison();
            const bool left_value = boolean(result), right_value = boolean(right);
            result = left_value && right_value;
        }
        return result;
    }

    json comparison() {
        const auto left = atom();
        for (const auto op : {"==", "!=", "<=", ">=", "=~", "!~", "<", ">"}) {
            if (!take(op)) continue;
            const auto right = atom();
            const std::string_view operation(op);
            if (operation == "=~" || operation == "!~") {
                if (!left.is_string() || !right.is_string()) invalid_condition("regex operands must be strings");
                try {
                    const bool matches = std::regex_match(left.get<std::string>(), std::regex(right.get<std::string>()));
                    return operation == "=~" ? matches : !matches;
                } catch (const std::regex_error &) {
                    invalid_condition("invalid regular expression");
                }
            }
            if (left.is_boolean() && right.is_boolean()) {
                if (operation == "==") return boolean(left) == boolean(right);
                if (operation == "!=") return boolean(left) != boolean(right);
                invalid_condition("booleans only support equality comparisons");
            }
            bool equal, lower, greater;
            if (left.is_number() && right.is_number()) {
                const double a = number(left), b = number(right);
                equal = std::abs(a - b) < 1e-8;
                lower = a < b;
                greater = a > b;
            } else if (left.is_string() || right.is_string()) {
                const auto a = text(left), b = text(right);
                equal = a == b;
                lower = a < b;
                greater = a > b;
            } else {
                invalid_condition("cannot compare operand types");
            }
            if (operation == "==") return equal;
            if (operation == "!=") return !equal;
            if (operation == "<=") return !greater;
            if (operation == ">=") return !lower;
            return operation == "<" ? lower : greater;
        }
        return left;
    }

    json atom() {
        if (++depth_ > 64) invalid_condition("expression nesting is too deep");
        const auto result = value();
        --depth_;
        return result;
    }

    json value() {
        if (take("not") || take("!")) return !boolean(atom());
        if (take("(")) {
            const auto result = expression();
            require(")");
            return result;
        }
        whitespace();
        if (position_ == source_.size()) invalid_condition("missing operand");
        const char first = source_[position_];
        if (first == '"' || first == '/') {
            const auto start = position_++;
            bool escaped = false;
            while (position_ < source_.size()) {
                const char ch = source_[position_++];
                if (ch == first && !escaped) {
                    const auto literal = source_.substr(start, position_ - start);
                    if (first == '/') return std::string(literal.substr(1, literal.size() - 2));
                    try { return json::parse(literal); }
                    catch (const json::exception &) { invalid_condition("invalid string literal"); }
                }
                escaped = ch == '\\' && !escaped;
            }
            invalid_condition("unterminated literal");
        }
        const auto start = position_;
        if (std::isdigit(static_cast<unsigned char>(first)) || first == '-' || first == '.') {
            while (position_ < source_.size() &&
                   (std::isdigit(static_cast<unsigned char>(source_[position_])) ||
                    source_[position_] == '.' || source_[position_] == '-' ||
                    source_[position_] == '+' || source_[position_] == 'e' || source_[position_] == 'E')) ++position_;
            return number(std::string(source_.substr(start, position_ - start)));
        }
        while (position_ < source_.size() &&
               (std::isalnum(static_cast<unsigned char>(source_[position_])) || source_[position_] == '_')) ++position_;
        const std::string key(source_.substr(start, position_ - start));
        if (key.empty()) invalid_condition("unexpected operand");
        if (key == "true") return true;
        if (key == "false") return false;
        if (key == "printer_preset") return name_;
        if (key == "num_extruders") return machine_.value("nozzle_diameter", json::array()).size();
        const auto found = machine_.find(key);
        if (found == machine_.end()) invalid_condition("unknown machine option '" + key + "'");
        if (take("[")) {
            const double index = number(atom());
            require("]");
            if (!found->is_array() || index < 0 || index >= found->size() || index != std::floor(index)) {
                invalid_condition("invalid index for machine option '" + key + "'");
            }
            return typed_option(found->at(static_cast<std::size_t>(index)), key, fields_);
        }
        if (found->is_array()) {
            if (found->empty()) invalid_condition("empty machine option '" + key + "'");
            return typed_option(found->front(), key, fields_);
        }
        return typed_option(*found, key, fields_);
    }
};
}  // namespace

bool preset_compatible_with_printer(const json &profile, const json &machine,
                                    const std::string &machine_name, const json &field_definitions) {
    const auto printers = profile.find("compatible_printers");
    if (printers != profile.end()) {
        if (!printers->is_array() || !std::all_of(printers->begin(), printers->end(),
                [](const json &name) { return name.is_string(); })) {
            throw ProjectSettingsError("native compatible_printers must be an array of names");
        }
        if (!printers->empty()) return std::find(printers->begin(), printers->end(), machine_name) != printers->end();
    }
    const auto condition = profile.find("compatible_printers_condition");
    if (condition == profile.end()) return true;
    if (!condition->is_string()) invalid_condition("must be text");
    const auto &text = condition->get_ref<const std::string &>();
    return text.empty() || Condition(text, machine, machine_name, field_definitions).evaluate();
}

}  // namespace fatcat::detail
