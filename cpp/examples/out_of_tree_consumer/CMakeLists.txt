cmake_minimum_required(VERSION 3.18)

project(fatcat_metadata_out_of_tree_consumer LANGUAGES CXX)

option(FATCAT_CONSUME_INSTALLED "Consume an installed SDK through find_package" OFF)
if(FATCAT_CONSUME_INSTALLED)
    find_package(FatCatMetadata 0.1.0 CONFIG REQUIRED)
    set(consumer_target FatCatMetadata::Core)
    set(consumer_data_root ${FatCatMetadata_DATA_DIR})
else()
    set(FATCAT_BUILD_PYTHON OFF CACHE BOOL "Do not build the Python binding" FORCE)
    set(FATCAT_BUILD_TESTS OFF CACHE BOOL "Build only the consumer sample" FORCE)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. fatcat-core)
    set(consumer_target fatcat_metadata_core)
    set(consumer_data_root ${CMAKE_CURRENT_BINARY_DIR}/fatcat_metadata_data)
endif()

add_executable(fatcat_metadata_out_of_tree_consumer main.cpp)
if(FATCAT_CONSUME_INSTALLED)
    enable_testing()
    add_test(NAME installed_sdk_consumer COMMAND fatcat_metadata_out_of_tree_consumer)
endif()
target_link_libraries(fatcat_metadata_out_of_tree_consumer PRIVATE ${consumer_target})
target_compile_definitions(fatcat_metadata_out_of_tree_consumer PRIVATE
    FATCAT_EXAMPLE_REQUEST="${CMAKE_CURRENT_LIST_DIR}/request.json"
    FATCAT_PUBLIC_DATA_ROOT="${consumer_data_root}"
)

# Provider-side packaging; the consumer receives one public data root and
# never resolves the library's source-file layout itself.
if(NOT FATCAT_CONSUME_INSTALLED)
    file(COPY ${CMAKE_CURRENT_LIST_DIR}/../../../compatibility/current-src/translations
        DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fatcat_metadata_data)
    file(COPY ${CMAKE_CURRENT_LIST_DIR}/../../../compatibility/current-src/native-project-sources/
        DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fatcat_metadata_data/native_project_sources)
    file(COPY ${CMAKE_CURRENT_LIST_DIR}/../../../licenses
        DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fatcat_metadata_data)
    file(COPY ${CMAKE_CURRENT_LIST_DIR}/../../../LICENSE
        ${CMAKE_CURRENT_LIST_DIR}/../../../NOTICE.md
        ${CMAKE_CURRENT_LIST_DIR}/../../../COPYRIGHT.md
        DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/fatcat_metadata_data)
endif()
