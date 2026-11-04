cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME plume_adapter)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be
# added in ./vcpkg.json and then used in cmake with find_package. Feel free to
# remove or replace with other dependencies. Note that it should also be removed
# from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/plume_adapter_extension.cpp)

# use the extension's duckdb repo instead of plume's
set(DUCKDB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/duckdb)

add_library(plume_duckdb INTERFACE)
target_include_directories(
  plume_duckdb SYSTEM
  INTERFACE ${DUCKDB_DIR}/src/include
            ${DUCKDB_DIR}/extension/core_functions/include
)

# dependencies for plume's compile layer
find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(PLUME_INCLUDES OFF) # we add dependencies directly here, we also use the DuckDB submodule of the extension
set(PLUME_WITH_COMPILE_LAYER ON) # need parser and conversion functions of plume
set(PLUME_BUILD_FUNCTIONS OFF)
set(PLUME_BUILD_CLIENT OFF)
set(PLUME_BUILD_BENCHMARKS OFF)
set(PLUME_BUILD_UBENCH OFF)
set(PLUME_WITH_TESTS OFF)

add_subdirectory(third_party/plume EXCLUDE_FROM_ALL)

set_target_properties(plume PROPERTIES POSITION_INDEPENDENT_CODE ON)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# force CXX 17 for plume, else duckdb's CMakeLists.txt forces CXX 11
set_target_properties(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME} 
  PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
)

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto $<BUILD_LOCAL_INTERFACE:plume>) # need BUILD_LOCAL_INTERFACE so CMake doesn't complain
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto plume)

target_include_directories(${EXTENSION_NAME} PRIVATE ${DUCKDB_DIR}/extension/parquet/include)
target_include_directories(${LOADABLE_EXTENSION_NAME} PRIVATE ${DUCKDB_DIR}/extension/parquet/include)
target_link_libraries(${EXTENSION_NAME} core_functions_extension parquet_extension)
target_link_libraries(${LOADABLE_EXTENSION_NAME} core_functions_extension parquet_extension)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
