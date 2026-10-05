#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <nlohmann/json.hpp>

#include "fatcat/metadata_components.h"
#include "fatcat/native_project_source.h"
#include "fatcat/project_settings.h"
#include "fatcat/source_material_slots.h"
#include "fatcat/template_import.h"
#include "fatcat/template_assembly.h"
#include "fatcat/wipe_tower.h"
#include "supported_targets.h"
#include "build_info.h"

namespace py = pybind11;

namespace {

std::filesystem::path packaged_data_root(const char *relative_path) {
    const auto module = py::module_::import("fatcat_metadata");
    const auto module_file =
        module.attr("__file__").cast<std::string>();
    return std::filesystem::path(module_file).parent_path() /
        "fatcat_metadata_data" / relative_path;
}

std::string read_packaged_target(const std::string &slicer) {
    return fatcat::metadata_target_data(slicer, "", packaged_data_root(""));
}

std::string read_requested_target(const std::string &request_json) {
    const auto request = nlohmann::json::parse(request_json);
    const auto slicer = request.at("slicer_id").get<std::string>();
    return fatcat::metadata_target_data(slicer, request.value("application_version", ""),
                                       packaged_data_root(""));
}

std::string compose_builtin_from_package(const std::string &request_json) {
    try {
        return fatcat::compose_builtin_project_settings(
            request_json, packaged_data_root(""));
    } catch (const std::exception &error) {
        throw py::value_error(error.what());
    }
}

std::string compose_from_package(const std::optional<std::string> &project_json,
                                const std::string &request_json) {
    if (!project_json) return compose_builtin_from_package(request_json);
    try {
        if (nlohmann::json::parse(request_json).contains("project_source")) {
            throw fatcat::ProjectSettingsError(
                "project_source requires project_json=None or the one-argument built-in overload");
        }
        return fatcat::compose_project_settings_from_data(
            *project_json, request_json, packaged_data_root(""));
    } catch (const std::exception &error) {
        throw py::value_error(error.what());
    }
}

std::string dictionary_json(const py::dict &value) {
    return py::module_::import("json").attr("dumps")(
        value, py::arg("ensure_ascii") = false, py::arg("allow_nan") = false,
        py::arg("sort_keys") = true, py::arg("separators") = py::make_tuple(",", ":"))
        .cast<std::string>();
}

py::dict project_dictionary(const std::string &result_json) {
    const auto loads = py::module_::import("json").attr("loads");
    auto result = loads(result_json).cast<py::dict>();
    result["project_settings"] = loads(result.attr("pop")("project_settings_json"));
    return result;
}

std::array<std::string, 6> read_import_targets() {
    std::array<std::string, 6> targets;
    std::size_t index = 0;
    for (const auto &target : fatcat::detail::supported_targets()) {
        if (target.at("template_import").get<bool>()) {
            targets.at(index++) = read_packaged_target(
                target.at("slicer_id").get<std::string>());
        }
    }
    return targets;
}

std::optional<std::string_view> optional_view(
    const std::optional<std::string> &value) {
    if (value.has_value()) {
        return std::string_view(*value);
    }
    return std::nullopt;
}

}  // namespace

PYBIND11_MODULE(fatcat_metadata, module) {
    module.doc() =
        "C++17 core for Fat Cat 3MF project and slicer metadata components.";
    module.attr("__version__") = FATCAT_METADATA_VERSION;
    module.attr("__source_revision__") = fatcat::detail::source_revision;
    module.attr("__source_dirty__") = fatcat::detail::source_dirty < 0
        ? py::object(py::none()) : py::object(py::bool_(fatcat::detail::source_dirty != 0));
    module.attr("__fatcat_cpp_extension__") = true;
    module.attr("__fatcat_project_settings__") = true;

    module.def(
        "metadata_target",
        [](const std::string &slicer_id) {
            const auto target = nlohmann::json::parse(read_packaged_target(slicer_id));
            return target.at("target_contract").dump();
        },
        py::arg("slicer_id"),
        "Read the installed target identity for a supported slicer.");

    module.def(
        "metadata_resource_specs",
        [](const std::string &slicer_id) {
            const auto target = nlohmann::json::parse(read_packaged_target(slicer_id));
            return target.at("package_dialect").at("metadata_components").at("resources").dump();
        },
        py::arg("slicer_id"),
        "Read target-owned resource paths and relationships.");

    module.def(
        "assemble_project_template",
        [](const std::string &base_project_json,
           const std::optional<std::string> &registry_machine_json,
           const std::string &options_json) {
            try {
                return fatcat::assemble_project_template(
                    base_project_json, optional_view(registry_machine_json), options_json);
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("base_project_json"), py::arg("registry_machine_json"),
        py::arg("options_json"),
        "Assemble selected source project and machine templates in C++.");

    module.def(
        "assemble_machine_registry_template",
        [](const std::string &source_settings_json, const std::string &options_json) {
            return fatcat::assemble_machine_registry_template(
                source_settings_json, options_json);
        },
        py::arg("source_settings_json"), py::arg("options_json"),
        "Compose a machine registry metadata template from source facts.");

    module.def(
        "source_material_settings_parts",
        [](const std::string &request_json) {
            try {
                const auto target = read_requested_target(request_json);
                return fatcat::source_material_settings_parts(target);
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("request_json"),
        "Return the target-described project/model settings member paths.");

    module.def(
        "extract_source_material_slots",
        [](const std::string &project_json,
           const std::string &model_settings_xml,
           const std::string &request_json) {
            try {
                const auto target = read_requested_target(request_json);
                return fatcat::extract_source_material_slots(
                    project_json, model_settings_xml, target);
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("project_json"), py::arg("model_settings_xml"),
        py::arg("request_json"),
        "Extract canonical source slot records for an explicit slicer/version.");

    module.def(
        "read_project_layout",
        [](const std::string &project_json, const std::string &request_json) {
            return fatcat::read_project_layout(project_json, read_requested_target(request_json));
        },
        py::arg("project_json"), py::arg("request_json"),
        "Read numeric layout inputs from source project metadata.");

    module.def(
        "read_placement_warnings",
        [](const std::optional<std::string> &placement_json) {
            return fatcat::read_placement_warnings(optional_view(placement_json));
        },
        py::arg("placement_json"),
        "Read placement warning codes from an optional metadata part.");

    module.def(
        "read_source_metadata",
        [](const std::string &project_json, const std::string &model_settings_xml,
           const std::string &request_json,
           const std::optional<std::string> &source_model_xml,
           const std::optional<std::string> &slice_info_xml,
           const std::optional<std::string> &placement_json) {
            return fatcat::read_source_metadata(
                project_json, model_settings_xml, read_requested_target(request_json),
                optional_view(source_model_xml), optional_view(slice_info_xml),
                optional_view(placement_json));
        },
        py::arg("project_json"), py::arg("model_settings_xml"), py::arg("request_json"),
        py::arg("source_model_xml") = py::none(), py::arg("slice_info_xml") = py::none(),
        py::arg("placement_json") = py::none(),
        "Read source slots, project, object and plate metadata without parsing geometry.");

    module.def(
        "serialize_layer_config_ranges", &fatcat::serialize_layer_config_ranges,
        py::arg("data_json"), py::arg("fine_layer_height_mm"),
        "Serialize caller-computed layer ranges without computing geometry.");

    module.def(
        "read_model_object_metadata", &fatcat::read_model_object_metadata,
        py::arg("model_xml"),
        "Read metadata attached to source model objects.");

    module.def(
        "validate_template_hardware", &fatcat::validate_template_hardware,
        py::arg("template_json"), py::arg("expected_project_json"),
        py::arg("source_slicer"), py::arg("selected_slicer"),
        "Compare an imported source's hardware with the selected template.");

    module.def(
        "template_import_parts",
        [](const std::string &request_json) {
            try {
                return fatcat::template_import_parts(
                    read_requested_target(request_json));
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("request_json"),
        "Return the target-described text member paths for template import.");

    module.def(
        "resolve_template_build_plate",
        [](const std::string &request_json,
           const std::string &default_build_plate_uid,
           const std::optional<std::string> &model_plate_value,
           const std::optional<std::string> &sidecar_bed_value,
           const std::optional<std::string> &project_bed_value) {
            try {
                return fatcat::resolve_template_build_plate(
                    read_requested_target(request_json), default_build_plate_uid,
                    optional_view(model_plate_value), optional_view(sidecar_bed_value),
                    optional_view(project_bed_value));
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("request_json"), py::arg("default_build_plate_uid"),
        py::arg("model_plate_value"), py::arg("sidecar_bed_value"),
        py::arg("project_bed_value"),
        "Resolve an imported template's effective build plate from target bindings.");

    module.def(
        "detect_template_source",
        [](const std::optional<std::string> &source_model_xml,
           const std::optional<std::string> &slice_info_xml) {
            try {
                const auto targets = read_import_targets();
                return fatcat::detect_template_source(
                    optional_view(source_model_xml), optional_view(slice_info_xml),
                    targets[0], targets[1], targets[2], targets[3], targets[4], targets[5]);
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("source_model_xml"), py::arg("slice_info_xml"),
        "Identify a supported source slicer and its declared version.");

    module.def(
        "import_template_metadata",
        [](const std::string &project_settings_json,
           const std::optional<std::string> &source_model_xml,
           const std::optional<std::string> &slice_info_xml,
           const std::optional<std::string> &model_settings_xml,
           const std::optional<std::string> &plate_sidecar_json,
           const std::string &request_json, bool explicit_slicer_hint) {
            try {
                const auto selected = read_requested_target(request_json);
                const auto targets = read_import_targets();
                return fatcat::import_template_metadata(
                    project_settings_json, optional_view(source_model_xml),
                    optional_view(slice_info_xml), optional_view(model_settings_xml),
                    optional_view(plate_sidecar_json), selected, targets[0], targets[1], targets[2],
                    targets[3], targets[4], targets[5], explicit_slicer_hint);
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("project_settings_json"), py::arg("source_model_xml"),
        py::arg("slice_info_xml"), py::arg("model_settings_xml"),
        py::arg("plate_sidecar_json"), py::arg("request_json"),
        py::arg("explicit_slicer_hint"),
        "Interpret a supported user template without changing its settings.");

    module.def(
        "patch_wipe_tower",
        [](const std::string &project_json, const std::string &settings_json,
           const std::string &dialect_json) {
            try {
                return fatcat::patch_wipe_tower(project_json, settings_json,
                                                dialect_json);
            } catch (const fatcat::WipeTowerError &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("project_json"), py::arg("settings_json"),
        py::arg("dialect_json"),
        "Patch caller-computed wipe-tower settings in project JSON text.");

    module.def(
        "compose_project_settings",
        [](const std::string &request_json) {
            return compose_builtin_from_package(request_json);
        },
        py::arg("request_json"),
        "Compose a selected Fat Cat built-in native source without project JSON.");

    module.def(
        "compose_project_settings",
        &compose_from_package,
        py::arg("project_json"), py::arg("request_json"),
        "Compose project settings from explicit project JSON or a built-in source.");

    module.def(
        "compose_project_settings",
        [](const py::dict &request) {
            return project_dictionary(compose_builtin_from_package(dictionary_json(request)));
        },
        py::arg("request"),
        "Compose a built-in source from a dictionary; return project_settings as a dictionary.");

    module.def(
        "compose_project_settings",
        [](const std::optional<py::dict> &project, const py::dict &request) {
            const auto source = project
                ? std::optional<std::string>(dictionary_json(*project)) : std::nullopt;
            return project_dictionary(compose_from_package(source, dictionary_json(request)));
        },
        py::arg("project"), py::arg("request"),
        "Compose an explicit source dictionary or None through the same C++ composer.");

    module.def("list_targets", []() {
        try {
            return py::module_::import("json").attr("loads")(
                fatcat::metadata_target_catalog(packaged_data_root(""))).cast<py::dict>();
        } catch (const std::exception &error) {
            throw py::value_error(error.what());
        }
    }, "List installed slicer/version identities. 查询已安装的软件与版本。");

    module.def("list_project_options", [](const py::dict &request) {
        try {
            return py::module_::import("json").attr("loads")(
                fatcat::native_project_options(dictionary_json(request), packaged_data_root("")))
                .cast<py::dict>();
        } catch (const std::exception &error) {
            throw py::value_error(error.what());
        }
    }, py::arg("request"), "List exact native choices and unavailable reasons. 查询原生候选及不可用原因。");

    module.def("list_machines", [](const std::string &slicer_id, const std::string &version) {
        try {
            const auto target = nlohmann::json::parse(fatcat::metadata_target_data(
                slicer_id, version, packaged_data_root("")));
            return py::module_::import("json").attr("loads")(
                fatcat::native_project_source_catalog(slicer_id,
                    target.at("target_contract").at("application_version").get<std::string>(),
                    packaged_data_root("native_project_sources"))).cast<py::dict>();
        } catch (const std::exception &error) {
            throw py::value_error(error.what());
        }
    }, py::arg("slicer_id"), py::arg("application_version") = "",
       "List exact machine/nozzle identities. 查询机型与喷嘴标识。");

    module.def(
        "native_project_source_catalog",
        [](const std::string &slicer_id) {
            try {
                const auto target = nlohmann::json::parse(read_packaged_target(slicer_id));
                const auto version = target.at("target_contract")
                                         .at("application_version")
                                         .get<std::string>();
                return fatcat::native_project_source_catalog(
                    slicer_id, version, packaged_data_root("native_project_sources"));
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("slicer_id"),
        "Read the installed native machine identities and hardware facts.");

    module.def(
        "compose_model_metadata",
        [](const std::string &project_json, const std::string &request_json) {
            try {
                return fatcat::compose_model_metadata_from_data(
                    project_json, request_json, packaged_data_root(""));
            } catch (const fatcat::MetadataComponentsError &error) {
                throw py::value_error(error.what());
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("project_json"), py::arg("request_json"),
        "Generate metadata parts and package descriptions for the explicit slicer/version.");

    module.def(
        "compose_model_metadata",
        [](const py::dict &project, const py::dict &request) {
            try {
                const auto description = fatcat::compose_model_metadata_from_data(
                    dictionary_json(project), dictionary_json(request), packaged_data_root(""));
                return py::module_::import("json").attr("loads")(description).cast<py::dict>();
            } catch (const std::exception &error) {
                throw py::value_error(error.what());
            }
        },
        py::arg("project"), py::arg("request"),
        "Describe metadata from dictionaries using the same C++ composer.");
}
