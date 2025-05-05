CMAKE_MINIMUM_REQUIRED(VERSION 3.30)
PROJECT(StormWeaver)

SET(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

SET( CMAKE_EXPORT_COMPILE_COMMANDS ON )
enable_testing()

option(WITH_ASAN "Build with address sanitizer")
option(WITH_UBSAN "Build with undefined behavior sanitizer")
option(WITH_TSAN "Build with thread sanitizer")
option(WITH_MSAN "Build with memory sanitizer")

if (WITH_ASAN)
	add_compile_options(-fsanitize=address)
	add_link_options(-fsanitize=address)
endif ()

if (WITH_UBSAN)
	add_compile_options(-fsanitize=undefined)
	add_link_options(-fsanitize=undefined)
endif ()

if (WITH_TSAN)
	add_compile_options(-fsanitize=thread)
	add_link_options(-fsanitize=thread)
endif ()

if (WITH_MSAN)
	add_compile_options(-fsanitize=memory)
	add_link_options(-fsanitize=memory)
endif ()

if (WITH_ASAN OR WITH_UBSAN OR WITH_TSAN OR WITH_MSAN)
	add_compile_options(-fno-omit-frame-pointer)
endif()

include(CheckCXXSymbolExists)
CHECK_CXX_SYMBOL_EXISTS(_LIBCPP_VERSION version LIBCPP)
if(LIBCPP)
	add_compile_options(-fexperimental-library)
endif()

find_package(Threads REQUIRED)
#find_package(libmysqlclient REQUIRED)
find_package(libpqxx REQUIRED)
find_package(Catch2 REQUIRED)
find_package(Boost REQUIRED)
find_package(reflectcpp REQUIRED)
find_package(Git REQUIRED)
find_package(Sol2 REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(magic_enum REQUIRED)

INCLUDE(PQSetupCompiler)

INCLUDE(PQCPack)
INCLUDE(PQFindRevision)

set("TEST_PG_DIR" "" CACHE STRING "Postgres installation directory used for testing")

if("${TEST_PG_DIR}" STREQUAL "")
    message("TEST_PG_DIR is empty, trying to autodetect")
    set(POSSIBLE_PG_VERS 17 16 15 14)
    foreach(ver ${POSSIBLE_PG_VERS})
        set(dir "/usr/lib/postgresql/${ver}")
        if(EXISTS "${dir}")
            message("Setting TEST_PG_DIR to ${dir}")
            set("TEST_PG_DIR" "${dir}")
            break()
        endif()
    endforeach()
endif()

if("${TEST_PG_DIR}" STREQUAL "")
	message(WARNING "TEST_PG_DIR is empty and couldn't be autodetected, skipping SQL tests")
endif()

INCLUDE(LuaModules)

add_subdirectory(libstormweaver)
add_subdirectory(stormweaver)
add_subdirectory(tests)
