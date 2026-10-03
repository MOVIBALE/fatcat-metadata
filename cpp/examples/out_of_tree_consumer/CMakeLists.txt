cmake_minimum_required(VERSION 3.18)

project(fatcat_metadata_out_of_tree_consumer LANGUAGES CXX)

set(FATCAT_BUILD_PYTHON OFF CACHE BOOL "Do not build the Python binding" FORCE)
set(FATCAT_BUILD_TESTS OFF CACHE BOOL "Build only the consumer sample" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. fatcat-core)

add_executable(fatcat_metadata_out_of_tree_consumer main.cpp)
target_link_libraries(fatcat_metadata_out_of_tree_consumer PRIVATE fatcat_metadata_core)
target_compile_definitions(fatcat_metadata_out_of_tree_consumer PRIVATE
    FATCAT_FIXTURE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../.."
    FATCAT_PUBLIC_DATA_ROOT="${CMAKE_CURRENT_BINARY_DIR}/fatcat_metadata_data"
)

# Provider-side packaging; the consumer receives one public data root and
# never resolves the library's source-file layout itself.
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
