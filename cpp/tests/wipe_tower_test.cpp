#include "fatcat/wipe_tower.h"

#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;
using fatcat::WipeTowerError;

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
  "enabled_key": "enable_prime_tower",
  "x_key": "wipe_tower_x",
  "y_key": "wipe_tower_y",
  "width_key": "prime_tower_width",
  "rotation_key": "wipe_tower_rotation_angle",
  "process_difference_key": "different_settings_to_system"
})json";

const char *const kProject = R"json({
  "wipe_tower_x": ["10", "20.000"],
  "wipe_tower_y": ["30", "40"],
  "filament_colour": ["#FFFFFF", "#000000"],
  "extension": {"nested": [true, 4]},
  "different_settings_to_system": ["layer_height", "temperature", "machine"]
})json";

std::string patch(const std::string &project, const std::string &settings,
                  const std::string &dialect = kDialect) {
    return fatcat::patch_wipe_tower(project, settings, dialect);
}

void expect_error(const std::string &project, const std::string &settings,
                  const std::string &dialect = kDialect) {
    bool raised = false;
    try {
        (void)patch(project, settings, dialect);
    } catch (const WipeTowerError &) {
        raised = true;
    }
    CHECK(raised);
}

void test_accepts_rust_float_sign_and_preserves_unmodified_text() {
    const auto project = R"json({
      "wipe_tower_x":["10","+20.000"],"wipe_tower_y":["30","40"],
      "different_settings_to_system":["layer_height"]
    })json";
    const auto output = json::parse(patch(
        project,
        R"json({"positions":[{"plate_index":0,"x_mm":12.5,"y_mm":-2}]})json"));
    CHECK(output.at("wipe_tower_x") == json({"12.5", "+20.000"}));
    CHECK(output.at("wipe_tower_y") == json({"-2", "40"}));
}

void test_formats_numbers_like_rust_display() {
    const auto project = R"json({
      "wipe_tower_x":["1"],"wipe_tower_y":["2"],
      "different_settings_to_system":["existing"]
    })json";

    const auto check_width = [&](const std::string &input,
                                 const std::string &expected) {
        const auto output = json::parse(patch(
            project, "{\"width_mm\":" + input + "}"));
        CHECK(output.at("prime_tower_width") == expected);
    };
    const auto check_rotation = [&](const std::string &input,
                                    const std::string &expected) {
        const auto output = json::parse(patch(
            project, "{\"rotation_deg\":" + input + "}"));
        CHECK(output.at("wipe_tower_rotation_angle") == expected);
    };
    check_width("35", "35");
    check_width("12.5", "12.5");
    check_width("108.61849", "108.61849");
    check_width("0.0001", "0.0001");
    check_width("0.00001", "0.00001");
    check_width("100000", "100000");
    check_width("1000000", "1000000");
    check_width("1.2345678901234567", "1.2345678901234567");
    check_width("1.0000000000000002", "1.0000000000000002");
    check_width("9007199254740991", "9007199254740991");
    check_width("9007199254740992", "9007199254740992");
    check_rotation("-0.0", "-0");
    check_rotation("-0", "-0");
    check_rotation("-0e0", "-0");

    const auto min_subnormal =
        std::string("0.") + std::string(323, '0') + "5";
    const auto max_finite = std::string("17976931348623157") +
                            std::string(292, '0');
    check_width("5e-324", min_subnormal);
    check_width("1.7976931348623157e308", max_finite);
}

void test_preserves_negative_zero_coordinates() {
    const auto project = R"json({
      "wipe_tower_x":["1"],"wipe_tower_y":["2"],
      "different_settings_to_system":["existing"]
    })json";
    const auto output = json::parse(patch(
        project,
        R"json({"positions":[{"plate_index":0,"x_mm":-0,"y_mm":-0e0}]})json"));
    CHECK(output.at("wipe_tower_x") == json({"-0"}));
    CHECK(output.at("wipe_tower_y") == json({"-0"}));
}

void test_updates_one_plate_and_is_idempotent() {
    const auto output = json::parse(patch(
        kProject,
        R"json({"enabled":true,"width_mm":35,"rotation_deg":90,
                  "positions":[{"plate_index":1,"x_mm":12.5,"y_mm":-2}]})json"));
    CHECK(output.at("wipe_tower_x") == json({"10", "12.5"}));
    CHECK(output.at("wipe_tower_y") == json({"30", "-2"}));
    CHECK(output.at("wipe_tower_x").at(0) == "10");
    CHECK(output.at("enable_prime_tower") == "1");
    CHECK(output.at("prime_tower_width") == "35");
    CHECK(output.at("wipe_tower_rotation_angle") == "90");
    CHECK(output.at("filament_colour") == json({"#FFFFFF", "#000000"}));
    CHECK(output.at("extension") == json({{"nested", {true, 4}}}));
    CHECK(output.at("different_settings_to_system").at(1) == "temperature");
    CHECK(output.at("different_settings_to_system").at(2) == "machine");
    CHECK(json::parse(patch(
               output.dump(),
               R"json({"enabled":true,"width_mm":35,"rotation_deg":90,
                         "positions":[{"plate_index":1,"x_mm":12.5,"y_mm":-2}]})json")) ==
           output);
}

void test_empty_override_and_disable() {
    CHECK(json::parse(patch(kProject, R"json({})json")) == json::parse(kProject));
    const auto output = json::parse(patch(kProject, R"json({"enabled":false})json"));
    CHECK(output.at("enable_prime_tower") == "0");
    CHECK(output.at("wipe_tower_x") == json({"10", "20.000"}));
}

void test_initializes_only_plate_zero() {
    const auto empty = R"json({"different_settings_to_system":[""]})json";
    const auto output = json::parse(patch(empty,
        R"json({"positions":[{"plate_index":0,"x_mm":15,"y_mm":20}]})json"));
    CHECK(output.at("wipe_tower_x") == json({"15"}));
    CHECK(output.at("wipe_tower_y") == json({"20"}));
    expect_error(empty, R"json({"positions":[{"plate_index":1,"x_mm":15,"y_mm":20}]})json");

    const auto explicit_empty = R"json({
      "wipe_tower_x":[],"wipe_tower_y":[],"different_settings_to_system":[""]
    })json";
    const auto explicit_output = json::parse(patch(
        explicit_empty,
        R"json({"positions":[{"plate_index":0,"x_mm":15,"y_mm":20}]})json"));
    CHECK(explicit_output.at("wipe_tower_x") == json({"15"}));
    CHECK(explicit_output.at("wipe_tower_y") == json({"20"}));
}

void test_updates_multiple_plates_and_preserves_tail_values() {
    const auto project = R"json({
      "wipe_tower_x":["10","20","30"],"wipe_tower_y":["40","50","60"],
      "different_settings_to_system":["existing","tail",42,{"keep":true}],
      "unknown":{"nested":[1,"keep"]}
    })json";
    const auto output = json::parse(patch(
        project,
        R"json({"positions":[
          {"plate_index":0,"x_mm":1.25,"y_mm":2.5},
          {"plate_index":2,"x_mm":3.75,"y_mm":4.5}
        ]})json"));
    CHECK(output.at("wipe_tower_x") == json({"1.25", "20", "3.75"}));
    CHECK(output.at("wipe_tower_y") == json({"2.5", "50", "4.5"}));
    CHECK(output.at("different_settings_to_system").at(1) == "tail");
    CHECK(output.at("different_settings_to_system").at(2) == 42);
    CHECK(output.at("different_settings_to_system").at(3) == json({{"keep", true}}));
    CHECK(output.at("unknown") == json({{"nested", {1, "keep"}}}));
}

void test_null_scalars_and_difference_segments_follow_existing_policy() {
    CHECK(json::parse(patch(
        kProject,
        R"json({"enabled":null,"width_mm":null,"rotation_deg":null})json")) ==
          json::parse(kProject));

    const auto project = R"json({
      "different_settings_to_system":[";existing;;existing;"],
      "wipe_tower_x":["1"],"wipe_tower_y":["2"]
    })json";
    const auto output = json::parse(patch(project, R"json({"width_mm":4})json"));
    CHECK(output.at("different_settings_to_system").at(0) ==
          "existing;existing;prime_tower_width");
}

void test_uses_custom_six_field_mapping() {
    const auto dialect = R"json({
      "enabled_key":"tower_enabled","x_key":"tower_x","y_key":"tower_y",
      "width_key":"tower_width","rotation_key":"tower_rotation",
      "process_difference_key":"process_overrides"
    })json";
    const auto project = R"json({"tower_x":["1"],"tower_y":["2"],
      "process_overrides":["existing"],"other":{"keep":"me"}})json";
    const auto output = json::parse(patch(project, R"json({"enabled":true,"width_mm":4})json", dialect));
    CHECK(output.at("tower_enabled") == "1");
    CHECK(output.at("tower_width") == "4");
    CHECK(output.find("enable_prime_tower") == output.end());
    CHECK(output.at("process_overrides").at(0).get<std::string>() == "existing;tower_enabled;tower_width");
    CHECK(output.at("other") == json({{"keep", "me"}}));
}

void test_rejects_invalid_shape_and_values() {
    expect_error("[]", R"json({})json");
    expect_error("{}", R"json({"unknown":true})json");
    expect_error(kProject, R"json({"width_mm":0})json");
    expect_error(kProject, R"json({"width_mm":-1})json");
    expect_error(kProject, R"json({"rotation_deg":1e309})json");
    expect_error(kProject, R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2},{"plate_index":0,"x_mm":3,"y_mm":4}]})json");
    expect_error(kProject, R"json({"positions":[{"plate_index":2,"x_mm":1,"y_mm":2}]})json");
    expect_error(kProject, R"json({"positions":[{"plate_index":-1,"x_mm":1,"y_mm":2}]})json");
    expect_error(kProject, R"json({"positions":[{"plate_index":1.5,"x_mm":1,"y_mm":2}]})json");
    expect_error(kProject, R"json({"positions":[{"plate_index":true,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":["1"],"wipe_tower_y":["2","3"],"different_settings_to_system":[""]})json",
                 R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":["not-a-number"],"wipe_tower_y":["2"],"different_settings_to_system":[""]})json",
                 R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":[" 1"],"wipe_tower_y":["2"],"different_settings_to_system":[""]})json",
                 R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":["nan"],"wipe_tower_y":["2"],"different_settings_to_system":[""]})json",
                 R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":["0x1p2"],"wipe_tower_y":["2"],"different_settings_to_system":[""]})json",
                 R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":[1],"wipe_tower_y":["2"],"different_settings_to_system":[""]})json",
                 R"json({"positions":[{"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(R"json({"wipe_tower_x":["1"],"wipe_tower_y":["2"],"different_settings_to_system":[]})json",
                 R"json({"enabled":true})json");
}

void test_rejects_bad_dialect_and_unknown_settings() {
    const auto duplicate = R"json({
      "enabled_key":"same","x_key":"same","y_key":"y","width_key":"w",
      "rotation_key":"r","process_difference_key":"p"})json";
    expect_error(kProject, R"json({})json", duplicate);
    const auto unknown = R"json({
      "enabled_key":"e","x_key":"x","y_key":"y","width_key":"w",
      "rotation_key":"r","process_difference_key":"p","extra":"no"})json";
    expect_error(kProject, R"json({})json", unknown);
    expect_error(kProject, R"json({"enabled":true,"extra":1})json");
    expect_error(kProject, R"json({"positions":null})json");
    expect_error(kProject, "{/*comment*/\"enabled\":true}");
    expect_error(kProject, R"json({"width_mm":35,"width_mm":36})json");
    expect_error(kProject,
                 R"json({"positions":[{"plate_index":0,"plate_index":0,"x_mm":1,"y_mm":2}]})json");
    expect_error(kProject, R"json({"positions":[{"plate_index":-0,"x_mm":1,"y_mm":2}]})json");
    expect_error(kProject,
                 R"json({"positions":[{"plate_\u0069ndex":-0,"x_mm":1,"y_mm":2}]})json");
    expect_error(kProject, R"json({})json",
                 R"json({"enabled_key":"e","enabled_key":"e2","x_key":"x","y_key":"y","width_key":"w","rotation_key":"r","process_difference_key":"p"})json");
}

}  // namespace

int main() {
    try {
        test_accepts_rust_float_sign_and_preserves_unmodified_text();
        test_formats_numbers_like_rust_display();
        test_preserves_negative_zero_coordinates();
        test_updates_one_plate_and_is_idempotent();
        test_empty_override_and_disable();
        test_initializes_only_plate_zero();
        test_updates_multiple_plates_and_preserves_tail_values();
        test_null_scalars_and_difference_segments_follow_existing_policy();
        test_uses_custom_six_field_mapping();
        test_rejects_invalid_shape_and_values();
        test_rejects_bad_dialect_and_unknown_settings();
        std::cout << "fatcat wipe tower C++ tests passed\n";
        return 0;
    } catch (const TestFailure &error) {
        std::cerr << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "unexpected test exception: " << error.what() << '\n';
        return 1;
    }
}
