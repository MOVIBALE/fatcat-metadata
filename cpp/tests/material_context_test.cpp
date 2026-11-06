#include <iostream>
#include <stdexcept>

#include "../src/material_context.h"

namespace {
using json = nlohmann::json;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void check_contract_and_layouts() {
    const json snapshot = {
        {"array_rules", json::array({
            {{"keys", {"safe_temperature"}}, {"material_source_override", true},
             {"selection", "default_or_group"}, {"group_size", 1}},
            {{"keys", {"filament_machine_setting"}}, {"material_source_override", false},
             {"selection", "group"}, {"group_size", 1}}
        })},
        {"native_schema_defaults", {{"values", {{"safe_temperature", {"native default"}}}}}}
    };
    const auto original = snapshot;
    fatcat::detail::MaterialContext context(snapshot);
    require(context.is_material("safe_temperature"), "declared non-prefix field must be material");
    require(!context.is_material("filament_machine_setting"), "explicit target role must override legacy prefix");
    require(context.is_material("filament_custom_extension"), "unlisted legacy fields remain supported");
    context.record_profile({{"safe_temperature", {"a", "b"}}}, "PLA");
    context.record_profile(json::object(), "TPU");
    require(snapshot == original, "runtime profile knowledge must not mutate target data");

    json project = {{"filament_settings_id", {"PLA", "TPU"}},
                    {"safe_temperature", {"a", "b", "native default"}}};
    auto layout = context.layout(project, "safe_temperature", 2);
    require(layout.group(project.at("safe_temperature"), 0) == json({"a", "b"}), "native group retained");
    require(layout.group(project.at("safe_temperature"), 1) == json({"native default"}), "omitted preset field uses schema boundary");

    project["safe_temperature"] = {"edited a", "edited b"};
    layout = context.layout(project, "safe_temperature", 2);
    require(layout.group(project.at("safe_temperature"), 1) == json({"edited b"}), "edited uniform source wins over catalog shape");
    project["safe_temperature"] = {"shared"};
    layout = context.layout(project, "safe_temperature", 2, true);
    require(layout.group(project.at("safe_temperature"), 1) == json({"shared"}), "singleton broadcasts only when requested");

    project["filament_settings_id"] = {"unknown", "TPU"};
    project["safe_temperature"] = {"a", "b", "c"};
    bool rejected = false;
    try {
        context.layout(project, "safe_temperature", 2);
    } catch (const fatcat::ProjectSettingsError &error) {
        rejected = std::string(error.what()).find("safe_temperature") != std::string::npos;
    }
    require(rejected, "unknown uneven groups must fail with the affected field");
}
}  // namespace

int main() {
    try {
        check_contract_and_layouts();
        std::cout << "PASS material contracts, native/default/uniform/broadcast boundaries\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
