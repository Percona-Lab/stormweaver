cmake_minimum_required(VERSION 3.30)
project(StormWeaver LANGUAGES CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# ninja+clang module scanning injects -fmodule-mapper/-fdeps-format flags into
# every compile command, which clang-tidy can't parse. we don't use modules.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
enable_testing()

option(WITH_ASAN "Build with address sanitizer")
option(WITH_UBSAN "Build with undefined behavior sanitizer")
option(WITH_TSAN "Build with thread sanitizer")
option(WITH_PYTHON "Build the python extension module" ON)

if(WITH_ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()
if(WITH_UBSAN)
  add_compile_options(-fsanitize=undefined)
  add_link_options(-fsanitize=undefined)
endif()
if(WITH_TSAN)
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
endif()
if(WITH_ASAN OR WITH_UBSAN OR WITH_TSAN)
  add_compile_options(-fno-omit-frame-pointer)
endif()

include(CheckCXXSymbolExists)
check_cxx_symbol_exists(_LIBCPP_VERSION version LIBCPP)
if(LIBCPP)
  add_compile_options(-fexperimental-library)
endif()

find_package(Threads REQUIRED)
find_package(libpqxx REQUIRED)
find_package(cryptopp REQUIRED)
find_package(Catch2 REQUIRED)
find_package(Boost REQUIRED)
find_package(reflectcpp REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(magic_enum REQUIRED)

set(TEST_PG_DIR "" CACHE STRING "Postgres installation directory used for testing")
if("${TEST_PG_DIR}" STREQUAL "")
  foreach(dir $ENV{HOME}/.local/share/stormweaver-pg18 /usr/lib/postgresql18 /usr/lib/postgresql17 /usr/lib/postgresql/18 /usr/lib/postgresql/17 /usr/lib/postgresql/16)
    if(EXISTS "${dir}/bin/initdb")
      set(TEST_PG_DIR "${dir}" CACHE STRING "Postgres installation directory used for testing" FORCE)
      break()
    endif()
  endforeach()
endif()
if("${TEST_PG_DIR}" STREQUAL "")
  message(WARNING "TEST_PG_DIR empty and not autodetected; SQL test will not be registered")
endif()

# workaround for envs with an incomplete postgres install (missing liburing/libnuma
# system packages) - extra dir prepended to LD_LIBRARY_PATH when running the sql test.
set(TEST_PG_LD_LIBRARY_PATH "" CACHE STRING "Extra LD_LIBRARY_PATH needed to run the test postgres binaries")

add_subdirectory(core)

if(WITH_PYTHON)
  find_package(Python 3.14 REQUIRED COMPONENTS Interpreter Development.Module)
  execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
  find_package(nanobind CONFIG REQUIRED)
  add_subdirectory(bindings)
endif()
