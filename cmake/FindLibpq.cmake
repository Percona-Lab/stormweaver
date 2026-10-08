# libpq of one PostgreSQL installation, picked through its pg_config.
# -DPG_CONFIG=/path/to/pg_config selects a custom build (a libpq work tree,
# a Percona install, ...); default is pg_config on PATH.
set(PG_CONFIG "" CACHE FILEPATH "pg_config of the PostgreSQL installation to build against")
if(NOT PG_CONFIG)
  find_program(PG_CONFIG_DEFAULT pg_config)
  if(PG_CONFIG_DEFAULT)
    set(PG_CONFIG "${PG_CONFIG_DEFAULT}" CACHE FILEPATH "pg_config of the PostgreSQL installation to build against" FORCE)
  endif()
endif()
mark_as_advanced(PG_CONFIG_DEFAULT)
if(PG_CONFIG AND NOT EXISTS "${PG_CONFIG}")
  message(FATAL_ERROR "PG_CONFIG=${PG_CONFIG} does not exist")
endif()
if(PG_CONFIG)
  # exactly this installation, never a system libpq
  set(_libpq_search NO_DEFAULT_PATH)
  execute_process(COMMAND "${PG_CONFIG}" --includedir
    OUTPUT_VARIABLE Libpq_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${PG_CONFIG}" --includedir-server
    OUTPUT_VARIABLE Libpq_SERVER_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${PG_CONFIG}" --libdir
    OUTPUT_VARIABLE Libpq_LIBRARY_DIR OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${PG_CONFIG}" --version
    OUTPUT_VARIABLE Libpq_VERSION_STRING OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  # "PostgreSQL 18.0" -> "18.0", same shape as the pkg-config value
  string(REGEX MATCH "[0-9]+(\\.[0-9]+)*" Libpq_VERSION_STRING "${Libpq_VERSION_STRING}")
  # client-only devel package (Debian libpq-dev): the reported dir is missing
  if(NOT IS_DIRECTORY "${Libpq_SERVER_INCLUDE_DIR}")
    set(Libpq_SERVER_INCLUDE_DIR "")
  endif()
else()
  # no pg_config (SUSE ships it in the server-devel package): libpq.pc is enough
  # for the client library, the server include dir stays empty. Its libdir is
  # only a hint: SUSE points it at an empty dir while libpq.so sits in /usr/lib64
  set(_libpq_search "")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(PC_Libpq QUIET libpq)
  endif()
  if(NOT PC_Libpq_FOUND)
    message(FATAL_ERROR "neither pg_config nor pkg-config libpq found, pass -DPG_CONFIG=/path/to/pg_config")
  endif()
  set(Libpq_INCLUDE_DIR "${PC_Libpq_INCLUDEDIR}")
  set(Libpq_SERVER_INCLUDE_DIR "")
  set(Libpq_LIBRARY_DIR "${PC_Libpq_LIBDIR}")
  set(Libpq_VERSION_STRING "${PC_Libpq_VERSION}")
endif()

# cached results belong to one pg_config, drop them when it changes
if(NOT "${_libpq_pg_config_used}" STREQUAL "${PG_CONFIG}")
  unset(Libpq_HEADER_DIR CACHE)
  unset(Libpq_LIBRARY CACHE)
endif()
set(_libpq_pg_config_used "${PG_CONFIG}" CACHE INTERNAL "")

find_path(Libpq_HEADER_DIR libpq-fe.h HINTS "${Libpq_INCLUDE_DIR}" ${_libpq_search})
find_library(Libpq_LIBRARY NAMES pq HINTS "${Libpq_LIBRARY_DIR}" ${_libpq_search})
if(Libpq_HEADER_DIR)
  set(Libpq_INCLUDE_DIR "${Libpq_HEADER_DIR}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libpq
  REQUIRED_VARS Libpq_LIBRARY Libpq_HEADER_DIR
  VERSION_VAR Libpq_VERSION_STRING)

if(Libpq_FOUND AND NOT TARGET Libpq::pq)
  add_library(Libpq::pq UNKNOWN IMPORTED)
  set_target_properties(Libpq::pq PROPERTIES
    IMPORTED_LOCATION "${Libpq_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Libpq_HEADER_DIR}")
endif()
mark_as_advanced(Libpq_HEADER_DIR Libpq_LIBRARY)
unset(_libpq_search)
