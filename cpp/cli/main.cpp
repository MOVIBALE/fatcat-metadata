#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fatcat/native_project_source.h"
#include "build_info.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

const char *help = R"(FatCat Metadata: native choices and 3MF metadata composition

Usage:
  fatcat catalog [--slicer ID] [--application-version VERSION]
  fatcat choices --slicer ID --application-version VERSION --machine UID --nozzle UID
                 [--plate UID]
  fatcat compose-settings --request REQUEST.json [--project SOURCE.json]
  fatcat compose-metadata --request MODEL.json --project SETTINGS.json
  fatcat --version

All commands accept --data-root DIR and --output FILE. Otherwise JSON is written
to stdout. SETTINGS.json may be plain project settings or a compose-settings
result. FatCat supplies metadata; your generator and writer create the 3MF.
Exit codes: 0 success; 2 invalid arguments, input, or file access.
)";

std::string read_file(const std::string &name) {
    std::ifstream stream(fs::u8path(name), std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read file: " + name);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

fs::path executable_path(const std::string &arg0) {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0 && size < buffer.size()) return fs::path(buffer.substr(0, size));
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) return fs::canonical(buffer.data());
#elif defined(__linux__)
    return fs::read_symlink("/proc/self/exe");
#endif
    return fs::absolute(fs::u8path(arg0));
}

using Options = std::map<std::string, std::string>;

std::string required(const Options &options, const std::string &key) {
    const auto found = options.find(key);
    if (found == options.end()) throw std::runtime_error("missing " + key + "; run fatcat --help");
    return found->second;
}

std::string optional(const Options &options, const std::string &key) {
    const auto found = options.find(key);
    return found == options.end() ? "" : found->second;
}

Options parse_options(const std::vector<std::string> &args, const std::string &command) {
    std::set<std::string> allowed = {"--data-root", "--output"};
    if (command == "catalog" || command == "choices") {
        allowed.insert({"--slicer", "--application-version"});
        if (command == "choices") allowed.insert({"--machine", "--nozzle", "--plate"});
    } else if (command == "compose-settings" || command == "compose-metadata") {
        allowed.insert({"--request", "--project"});
    } else {
        throw std::runtime_error("unknown command: " + command + "; run fatcat --help");
    }
    Options options;
    for (std::size_t i = 2; i < args.size(); i += 2) {
        const auto &key = args[i];
        if (!allowed.count(key)) throw std::runtime_error("unsupported option for " + command + ": " + key);
        if (i + 1 == args.size() || args[i + 1].empty() || args[i + 1].rfind("--", 0) == 0) {
            throw std::runtime_error("missing value for " + key);
        }
        if (!options.emplace(key, args[i + 1]).second) throw std::runtime_error("duplicate option: " + key);
    }
    return options;
}

json project_settings(const std::string &name) {
    auto project = json::parse(read_file(name));
    if (!project.is_object()) throw std::runtime_error("project file must contain a JSON object");
    if (project.contains("project_settings")) return project.at("project_settings");
    if (project.contains("project_settings_json")) {
        return json::parse(project.at("project_settings_json").get<std::string>());
    }
    return project;
}

std::string execute(const std::string &command, const Options &options, const fs::path &root) {
    if (command == "catalog") {
        const auto slicer = optional(options, "--slicer");
        if (slicer.empty()) {
            if (options.count("--application-version")) throw std::runtime_error("--application-version requires --slicer");
            return fatcat::metadata_target_catalog(root);
        }
        const auto target = json::parse(fatcat::metadata_target_data(
            slicer, optional(options, "--application-version"), root));
        return fatcat::native_project_source_catalog(slicer,
            target.at("target_contract").at("application_version").get<std::string>(),
            root / "native_project_sources");
    }
    if (command == "choices") {
        json request = {{"slicer_id", required(options, "--slicer")},
            {"application_version", required(options, "--application-version")},
            {"machine_uid", required(options, "--machine")},
            {"nozzle_uid", required(options, "--nozzle")}};
        if (options.count("--plate")) request["build_plate_uid"] = required(options, "--plate");
        return fatcat::native_project_options(request.dump(), root);
    }
    const auto request = read_file(required(options, "--request"));
    if (command == "compose-settings") {
        auto result = json::parse(options.count("--project")
            ? fatcat::compose_project_settings_from_data(
                project_settings(required(options, "--project")).dump(), request, root)
            : fatcat::compose_builtin_project_settings(request, root));
        result["project_settings"] = json::parse(result.at("project_settings_json").get<std::string>());
        result.erase("project_settings_json");
        return result.dump();
    }
    return fatcat::compose_model_metadata_from_data(
        project_settings(required(options, "--project")).dump(), request, root);
}

int run(const std::vector<std::string> &args) {
    try {
        if (args.size() == 1 || (args.size() == 2 && (args[1] == "--help" || args[1] == "-h"))) {
            std::cout << help;
            return 0;
        }
        if (args.size() == 2 && args[1] == "--version") {
            std::cout << json{{"version", FATCAT_METADATA_VERSION},
                {"source_revision", fatcat::detail::source_revision},
                {"source_dirty", fatcat::detail::source_dirty < 0
                    ? json(nullptr) : json(fatcat::detail::source_dirty != 0)}}.dump(2) << '\n';
            return 0;
        }
        const auto options = parse_options(args, args.at(1));
        const auto root = options.count("--data-root") ? fs::u8path(required(options, "--data-root"))
            : executable_path(args[0]).parent_path().parent_path() / "share" / "fatcat-metadata";
        const auto result = json::parse(execute(args[1], options, root)).dump(2) + "\n";
        if (options.count("--output")) {
            std::ofstream stream(fs::u8path(required(options, "--output")), std::ios::binary);
            if (!stream) throw std::runtime_error("cannot write output file");
            stream << result;
            if (!stream) throw std::runtime_error("output file write failed");
        } else {
            std::cout << result;
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "fatcat: " << error.what() << '\n';
        return 2;
    }
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(fs::path(argv[i]).u8string());
    return run(args);
}
#else
int main(int argc, char **argv) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
