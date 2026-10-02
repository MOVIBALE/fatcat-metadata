cmake_minimum_required(VERSION 3.18)

project(fatcat_metadata_out_of_tree_consumer LANGUAGES CXX)

set(FATCAT_BUILD_PYTHON OFF CACHE BOOL "Do not build the Python binding" FORCE)
set(FATCAT_BUILD_TESTS OFF CACHE BOOL "Build only the consumer sample" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. fatcat-core)

add_executable(fatcat_metadata_out_of_tree_consumer main.cpp)
target_link_libraries(fatcat_metadata_out_of_tree_consumer PRIVATE fatcat_metadata_core)
target_compile_definitions(fatcat_metadata_out_of_tree_consumer PRIVATE
    FATCAT_FIXTURE_ROOT="${CMAKE_CURRENT_LIST_DIR}/../../.."
)
