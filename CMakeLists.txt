cmake_minimum_required(VERSION 3.20)

# Takes over the identity of the retired Aho-Corasick library: the package is
# `qattern`, so consumers keep find_package(qattern), qattern::qattern and
# #include "qattern.hpp" exactly as before.
project(qattern VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QATTERN_BUILD_TESTS "Build the encoding and dictionary self tests" ON)
option(QATTERN_BUILD_BENCH "Build the index comparison benchmark" ON)
option(QATTERN_BUILD_TOOLS "Build the dictionary generator" ON)

include(CMakePackageConfigHelpers)

find_package(Microsoft.GSL CONFIG REQUIRED)

add_library(qattern INTERFACE)
add_library(qattern::qattern ALIAS qattern)

target_include_directories(qattern INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(qattern INTERFACE cxx_std_20)
target_link_libraries(qattern INTERFACE Microsoft.GSL::GSL)

if(MSVC)
    add_compile_options(/W4 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

install(TARGETS qattern EXPORT qatternTargets)
install(DIRECTORY include/ DESTINATION include)
install(EXPORT qatternTargets
    NAMESPACE qattern::
    DESTINATION lib/cmake/qattern
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qatternConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/qatternConfig.cmake"
    INSTALL_DESTINATION lib/cmake/qattern
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/qatternConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/qatternConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/qatternConfigVersion.cmake"
    DESTINATION lib/cmake/qattern
)

set(QATTERN_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(QATTERN_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools")

if(QATTERN_BUILD_TESTS)
    enable_testing()

    # Pass corpus paths as absolute arguments: relative paths would silently
    # skip and show all-green when CWD is wrong.
    add_executable(encodings tests/encodings.cpp)
    target_link_libraries(encodings PRIVATE qattern)
    target_include_directories(encodings PRIVATE "${QATTERN_TOOLS_DIR}")
    add_test(NAME encodings COMMAND encodings
        "${QATTERN_DATA_DIR}/qchars.txt" "${QATTERN_DATA_DIR}/qchars_cn_utf8.txt")

    add_executable(dictionary tests/dictionary.cpp)
    target_link_libraries(dictionary PRIVATE qattern)
    target_include_directories(dictionary PRIVATE "${QATTERN_TOOLS_DIR}")
    add_test(NAME dictionary COMMAND dictionary
        "${QATTERN_DATA_DIR}/qchars.txt" "${QATTERN_DATA_DIR}/qchars_cn_utf8.txt")
endif()

if(QATTERN_BUILD_TOOLS)
    add_executable(gen_dict tools/gen_dict.cpp)
    target_link_libraries(gen_dict PRIVATE qattern)
endif()

if(QATTERN_BUILD_BENCH)
    add_executable(index_compare benchmark/index_compare.cpp)
    target_link_libraries(index_compare PRIVATE qattern)
    # 三方索引命中数必须相等，否则返回非零；顺带量一次耗时。
    add_test(NAME index_compare COMMAND index_compare
        "${QATTERN_DATA_DIR}/qchars.txt")
endif()
