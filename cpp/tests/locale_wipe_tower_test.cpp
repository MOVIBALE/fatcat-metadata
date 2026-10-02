#include "fatcat/wipe_tower.h"

#include <clocale>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail_check(const char *expression, const char *file,
                             int line) {
    std::ostringstream message;
    message << file << ':' << line << ": CHECK failed: " << expression;
    throw TestFailure(message.str());
}

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            fail_check(#expression, __FILE__, __LINE__); \
        } \
    } while (false)

const char *const kDialect = R"json({
  "enabled_key":"enable_prime_tower",
  "x_key":"wipe_tower_x",
  "y_key":"wipe_tower_y",
  "width_key":"prime_tower_width",
  "rotation_key":"wipe_tower_rotation_angle",
  "process_difference_key":"different_settings_to_system"
})json";

void run_locale_case(const std::string &expected_locale_kind) {
    const char *active_locale = std::setlocale(LC_NUMERIC, "");
    CHECK(active_locale != nullptr);
    const std::string before(active_locale);
    const std::string decimal_point = std::localeconv()->decimal_point;
    if (expected_locale_kind == "comma") {
        CHECK(decimal_point == ",");
    } else {
        CHECK(decimal_point == ".");
    }

    const auto output = json::parse(fatcat::patch_wipe_tower(
        R"json({
          "wipe_tower_x":["10.5","+20.000"],
          "wipe_tower_y":["30.25","40"],
          "different_settings_to_system":["layer_height"]
        })json",
        R"json({"positions":[{"plate_index":0,"x_mm":12.5,"y_mm":-2}]})json",
        kDialect));

    CHECK(output.at("wipe_tower_x") == json({"12.5", "+20.000"}));
    CHECK(output.at("wipe_tower_y") == json({"-2", "40"}));
    CHECK(output.at("different_settings_to_system") ==
          json({"layer_height;wipe_tower_x;wipe_tower_y"}));

    const char *after_locale = std::setlocale(LC_NUMERIC, nullptr);
    CHECK(after_locale != nullptr);
    CHECK(before == after_locale);
}

}  // namespace

int main(int argc, char **argv) {
    try {
        CHECK(argc == 2);
        run_locale_case(argv[1]);
        std::cout << "fatcat locale wipe tower test passed\n";
        return 0;
    } catch (const TestFailure &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
