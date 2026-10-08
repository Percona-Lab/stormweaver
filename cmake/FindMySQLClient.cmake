# MySQL client library through mariadb_config / mysql_config.
# -DMYSQL_CONFIG=/path/to/mariadb_config selects one; default: first of the
# two found on PATH. MariaDB Connector/C and libmysqlclient both work.
set(MYSQL_CONFIG "" CACHE FILEPATH "mariadb_config or mysql_config of the client library to build against")
if(NOT MYSQL_CONFIG)
  find_program(MYSQL_CONFIG_DEFAULT NAMES mariadb_config mysql_config)
  if(MYSQL_CONFIG_DEFAULT)
    set(MYSQL_CONFIG "${MYSQL_CONFIG_DEFAULT}" CACHE FILEPATH "mariadb_config or mysql_config of the client library to build against" FORCE)
  endif()
endif()
mark_as_advanced(MYSQL_CONFIG_DEFAULT)
if(MYSQL_CONFIG AND NOT EXISTS "${MYSQL_CONFIG}")
  message(FATAL_ERROR "MYSQL_CONFIG=${MYSQL_CONFIG} does not exist")
endif()

# Without a config tool the plain search below still runs, so
# -DMySQLClient_ROOT=/prefix (CMP0074) works for a hand-built client library.
set(_mysql_include_flags "")
set(_mysql_lib_flags "")
if(MYSQL_CONFIG)
  execute_process(COMMAND "${MYSQL_CONFIG}" --include
    OUTPUT_VARIABLE _mysql_include_flags OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${MYSQL_CONFIG}" --libs
    OUTPUT_VARIABLE _mysql_lib_flags OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${MYSQL_CONFIG}" --version
    OUTPUT_VARIABLE MySQLClient_VERSION_STRING OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
endif()

# --include: "-I/usr/include/mysql" (possibly several); --libs: "-L/dir -lmariadb -lz ..."
separate_arguments(_mysql_include_flags UNIX_COMMAND "${_mysql_include_flags}")
separate_arguments(_mysql_lib_flags UNIX_COMMAND "${_mysql_lib_flags}")
set(_mysql_include_dirs "")
foreach(_flag IN LISTS _mysql_include_flags)
  if(_flag MATCHES "^-I(.+)$")
    list(APPEND _mysql_include_dirs "${CMAKE_MATCH_1}")
  endif()
endforeach()
set(_mysql_lib_dirs "")
set(_mysql_lib_names "")
foreach(_flag IN LISTS _mysql_lib_flags)
  if(_flag MATCHES "^-L(.+)$")
    list(APPEND _mysql_lib_dirs "${CMAKE_MATCH_1}")
  elseif(_flag MATCHES "^-l(mariadb|mysqlclient|perconaserverclient)$")
    list(APPEND _mysql_lib_names "${CMAKE_MATCH_1}")
  endif()
endforeach()

# cached results belong to one config tool, drop them when it changes
if(NOT "${_mysql_config_used}" STREQUAL "${MYSQL_CONFIG}")
  unset(MySQLClient_INCLUDE_DIR CACHE)
  unset(MySQLClient_LIBRARY CACHE)
endif()
set(_mysql_config_used "${MYSQL_CONFIG}" CACHE INTERNAL "")

find_path(MySQLClient_INCLUDE_DIR mysql.h HINTS ${_mysql_include_dirs} PATH_SUFFIXES mysql mariadb)
find_library(MySQLClient_LIBRARY NAMES ${_mysql_lib_names} mariadb mysqlclient HINTS ${_mysql_lib_dirs} PATH_SUFFIXES mariadb mysql)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MySQLClient
  REQUIRED_VARS MySQLClient_LIBRARY MySQLClient_INCLUDE_DIR
  VERSION_VAR MySQLClient_VERSION_STRING)

if(MySQLClient_FOUND AND NOT TARGET MySQLClient::MySQLClient)
  add_library(MySQLClient::MySQLClient UNKNOWN IMPORTED)
  set_target_properties(MySQLClient::MySQLClient PROPERTIES
    IMPORTED_LOCATION "${MySQLClient_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${MySQLClient_INCLUDE_DIR}")
endif()
mark_as_advanced(MySQLClient_INCLUDE_DIR MySQLClient_LIBRARY)
unset(_mysql_include_flags)
unset(_mysql_lib_flags)
unset(_mysql_include_dirs)
unset(_mysql_lib_dirs)
unset(_mysql_lib_names)
