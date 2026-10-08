# In-tree C++ dependencies. Everything here is built static and never
# installed; libpq, the MySQL client and cryptopp come from the system
# (see Find*.cmake). Pins live in .gitmodules.
cmake_path(SET STORMWEAVER_THIRD_PARTY NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../third_party")
if(NOT EXISTS "${STORMWEAVER_THIRD_PARTY}/fmt/CMakeLists.txt")
  message(FATAL_ERROR "third_party submodules missing, run: git submodule update --init --recursive")
endif()

# intentionally project-wide: everything in-tree is static
set(BUILD_SHARED_LIBS OFF)

set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
add_subdirectory("${STORMWEAVER_THIRD_PARTY}/fmt" EXCLUDE_FROM_ALL)

set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
add_subdirectory("${STORMWEAVER_THIRD_PARTY}/spdlog" EXCLUDE_FROM_ALL)

# libpqxx skips its own find_package(PostgreSQL) when the parent already
# found it; hand it the pg_config pick so no other libpq can sneak in.
find_package(Libpq REQUIRED)
set(PostgreSQL_FOUND ON)
set(PostgreSQL_INCLUDE_DIRS "${Libpq_INCLUDE_DIR}")
if(Libpq_SERVER_INCLUDE_DIR)
  list(APPEND PostgreSQL_INCLUDE_DIRS "${Libpq_SERVER_INCLUDE_DIR}")
endif()
set(PostgreSQL_LIBRARIES "${Libpq_LIBRARY}")
set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
add_subdirectory("${STORMWEAVER_THIRD_PARTY}/libpqxx" EXCLUDE_FROM_ALL)
unset(PostgreSQL_FOUND)
unset(PostgreSQL_INCLUDE_DIRS)
unset(PostgreSQL_LIBRARIES)

# header-only use (enum reflection only), so its CMake (needs 3.23) is skipped
add_library(reflectcpp INTERFACE)
target_include_directories(reflectcpp INTERFACE "${STORMWEAVER_THIRD_PARTY}/reflect-cpp/include")
add_library(reflectcpp::reflectcpp ALIAS reflectcpp)

set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${STORMWEAVER_THIRD_PARTY}/Catch2" EXCLUDE_FROM_ALL)

add_subdirectory("${STORMWEAVER_THIRD_PARTY}/small_vector" EXCLUDE_FROM_ALL)

find_package(MySQLClient REQUIRED)
find_package(CryptoPP REQUIRED)

# stormweaver_core builds with -Werror and so may its consumers: every
# third_party include dir is a SYSTEM include for everything that links it.
foreach(dep fmt spdlog pqxx reflectcpp Catch2 small_vector)
  set_property(TARGET ${dep} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
    $<TARGET_PROPERTY:${dep},INTERFACE_INCLUDE_DIRECTORIES>)
endforeach()
