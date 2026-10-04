cmake_minimum_required(VERSION 3.18)

project(fatcat_3mf_metadata_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FATCAT_SUPPORTED_TARGETS_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/../compatibility/current-src/translations/supported-targets.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${FATCAT_SUPPORTED_TARGETS_FILE})
file(READ ${FATCAT_SUPPORTED_TARGETS_FILE} FATCAT_SUPPORTED_TARGETS_JSON)
configure_file(cmake/supported_targets.h.in generated/supported_targets.h @ONLY)

option(FATCAT_BUILD_PYTHON "Build the Python extension" ON)
option(FATCAT_BUILD_TESTS "Build the standalone C++ tests" ON)
option(FATCAT_INSTALL_CPP "Install the standalone C++ SDK and its public data" OFF)
option(FATCAT_TEST_NON_C_LOCALE
    "Register the comma-decimal locale test (requires de_DE.UTF-8)" OFF)

# Prefer an installed package for local development, but keep the C++ subproject
# self-contained for wheel builds on machines without Homebrew packages.
find_package(nlohmann_json 3.12.0 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    # Scope dependency installation to this SDK without changing the host cache.
    set(JSON_Install ${FATCAT_INSTALL_CPP})
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# TinyXML2 is the single lightweight XML serializer used for fixed slicer
# metadata parts. Version 10.0.0 is zlib-licensed and pinned for the three
# CI platforms; a system package is preferred for local builds.
set(FATCAT_BUNDLED_TINYXML2 OFF)
find_package(tinyxml2 10.0 CONFIG QUIET)
if(NOT tinyxml2_FOUND)
    set(FATCAT_BUNDLED_TINYXML2 ON)
    include(FetchContent)
    FetchContent_Declare(
        tinyxml2
        URL https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz
        URL_HASH SHA256=3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # Populate only the two tinyxml2 source files.  Adding its upstream
    # subdirectory would also install its static library and CMake package into
    # our wheel, while the core only needs a private build target.
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(tinyxml2)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
    add_library(fatcat_tinyxml2 STATIC
        ${tinyxml2_SOURCE_DIR}/tinyxml2.cpp
    )
    # The same static object is linked into the Python shared module on Linux.
    # Keep the fallback target position-independent for wheel builds.
    set_target_properties(fatcat_tinyxml2 PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(fatcat_tinyxml2 PUBLIC
        $<BUILD_INTERFACE:${tinyxml2_SOURCE_DIR}>
    )
    add_library(tinyxml2::tinyxml2 ALIAS fatcat_tinyxml2)
endif()

add_library(fatcat_metadata_core STATIC
    src/metadata_components.cpp
    src/project_settings.cpp
    src/native_project_source.cpp
    src/source_project_settings.cpp
    src/source_material_slots.cpp
    src/template_import.cpp
    src/template_assembly.cpp
    src/wipe_tower.cpp
)
set_target_properties(fatcat_metadata_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(fatcat_metadata_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(fatcat_metadata_core PUBLIC nlohmann_json::nlohmann_json
    tinyxml2::tinyxml2)
target_compile_features(fatcat_metadata_core PUBLIC cxx_std_17)

if(FATCAT_INSTALL_CPP)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set_target_properties(fatcat_metadata_core PROPERTIES EXPORT_NAME Core)
    install(TARGETS fatcat_metadata_core EXPORT FatCatMetadataTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(FATCAT_BUNDLED_TINYXML2)
        set_target_properties(fatcat_tinyxml2 PROPERTIES EXPORT_NAME PrivateTinyXML2)
        install(TARGETS fatcat_tinyxml2 EXPORT FatCatMetadataTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    install(DIRECTORY include/fatcat DESTINATION include)
    set(FATCAT_DATA_INSTALL_DIR ${CMAKE_INSTALL_DATADIR}/fatcat-metadata)
    install(DIRECTORY ../compatibility/current-src/translations
        DESTINATION ${FATCAT_DATA_INSTALL_DIR})
    install(DIRECTORY ../compatibility/current-src/native-project-sources/
        DESTINATION ${FATCAT_DATA_INSTALL_DIR}/native_project_sources)
    install(DIRECTORY ../licenses/third-party
        DESTINATION ${FATCAT_DATA_INSTALL_DIR}/licenses)
    install(FILES ../LICENSE ../NOTICE.md ../COPYRIGHT.md
        DESTINATION ${FATCAT_DATA_INSTALL_DIR})
    configure_package_config_file(cmake/FatCatMetadataConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/FatCatMetadataConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FatCatMetadata
        PATH_VARS FATCAT_DATA_INSTALL_DIR)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/FatCatMetadataConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(EXPORT FatCatMetadataTargets NAMESPACE FatCatMetadata::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FatCatMetadata)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/FatCatMetadataConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FatCatMetadataConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FatCatMetadata)
endif()

if(FATCAT_BUILD_TESTS)
    enable_testing()
    add_executable(fatcat_wipe_tower_test
        tests/wipe_tower_test.cpp
    )
    target_link_libraries(fatcat_wipe_tower_test PRIVATE fatcat_metadata_core)
    add_test(NAME fatcat_wipe_tower_test COMMAND fatcat_wipe_tower_test)

    add_executable(fatcat_project_settings_test
        tests/project_settings_test.cpp
    )
    target_link_libraries(fatcat_project_settings_test PRIVATE fatcat_metadata_core)
    target_compile_definitions(fatcat_project_settings_test PRIVATE
        FATCAT_TEST_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    add_test(NAME fatcat_project_settings_test COMMAND fatcat_project_settings_test)

    add_executable(fatcat_compact_project_settings_test
        tests/compact_project_settings_test.cpp
    )
    target_link_libraries(fatcat_compact_project_settings_test PRIVATE fatcat_metadata_core)
    target_compile_definitions(fatcat_compact_project_settings_test PRIVATE
        FATCAT_TEST_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    add_test(NAME fatcat_compact_project_settings_test COMMAND fatcat_compact_project_settings_test)

    add_executable(fatcat_native_material_settings_test tests/native_material_settings_test.cpp)
    target_link_libraries(fatcat_native_material_settings_test PRIVATE fatcat_metadata_core)
    target_compile_definitions(fatcat_native_material_settings_test PRIVATE
        FATCAT_TEST_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    add_test(NAME fatcat_native_material_settings_test COMMAND fatcat_native_material_settings_test)

    add_executable(fatcat_metadata_components_test
        tests/metadata_components_test.cpp
    )
    target_link_libraries(fatcat_metadata_components_test PRIVATE fatcat_metadata_core)
    target_compile_definitions(fatcat_metadata_components_test PRIVATE
        FATCAT_TEST_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    add_test(NAME fatcat_metadata_components_test COMMAND fatcat_metadata_components_test)

    add_executable(fatcat_locale_test
        tests/locale_wipe_tower_test.cpp
    )
    target_link_libraries(fatcat_locale_test PRIVATE fatcat_metadata_core)
    add_test(NAME fatcat_locale_test_c COMMAND fatcat_locale_test dot)
    set_tests_properties(fatcat_locale_test_c PROPERTIES
        ENVIRONMENT "LC_ALL=C;LC_NUMERIC=C"
    )
    if(FATCAT_TEST_NON_C_LOCALE)
        add_test(NAME fatcat_locale_test_non_c COMMAND fatcat_locale_test comma)
        set_tests_properties(fatcat_locale_test_non_c PROPERTIES
            ENVIRONMENT "LC_ALL=de_DE.UTF-8;LC_NUMERIC=de_DE.UTF-8"
        )
    endif()
endif()

if(FATCAT_BUILD_PYTHON)
    find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 CONFIG REQUIRED)

    pybind11_add_module(fatcat_metadata MODULE
        bindings/module.cpp
    )
    target_link_libraries(fatcat_metadata PRIVATE fatcat_metadata_core)
    target_include_directories(fatcat_metadata PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_features(fatcat_metadata PRIVATE cxx_std_17)
    target_compile_definitions(fatcat_metadata PRIVATE
        FATCAT_METADATA_VERSION="${PROJECT_VERSION}"
    )

    # Install the extension at the wheel root for `import fatcat_metadata`.
    install(TARGETS fatcat_metadata
        LIBRARY DESTINATION .
        RUNTIME DESTINATION .
        ARCHIVE DESTINATION .
    )
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../python/fatcat_metadata_neroued.py
        DESTINATION .
    )
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../python/fatcat_metadata.pyi
        DESTINATION fatcat_metadata-stubs RENAME __init__.pyi)
    install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../compatibility/current-src/translations/canonical.json
        ${FATCAT_SUPPORTED_TARGETS_FILE}
        DESTINATION fatcat_metadata_data/translations
    )
    install(DIRECTORY
        ${CMAKE_CURRENT_SOURCE_DIR}/../compatibility/current-src/translations/targets
        DESTINATION fatcat_metadata_data/translations
        FILES_MATCHING PATTERN "*.json"
    )
    install(DIRECTORY
        ${CMAKE_CURRENT_SOURCE_DIR}/../compatibility/current-src/native-project-sources/
        DESTINATION fatcat_metadata_data/native_project_sources
    )
    install(DIRECTORY
        ${CMAKE_CURRENT_SOURCE_DIR}/../licenses/third-party/
        DESTINATION fatcat_metadata_data/licenses/third-party
    )
endif()
