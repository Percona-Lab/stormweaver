set(SQLTEST_SOURCES main.cpp ddl.cpp schema_discovery.cpp metadata_populator.cpp worker_schema_discovery.cpp checksum.cpp)
add_executable(test-stormweaver-sql ${SQLTEST_SOURCES})
target_link_libraries(test-stormweaver-sql Catch2::Catch2 stormweaver_core)
if(NOT "${TEST_PG_DIR}" STREQUAL "")
  add_test(NAME test-stormweaver-sql
           COMMAND test-stormweaver-sql "${TEST_PG_DIR}" "${CMAKE_BINARY_DIR}/sql-data" 25432)
  set_property(TEST test-stormweaver-sql PROPERTY ENVIRONMENT
      "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/.tsan-suppressions")
  if(NOT "${TEST_PG_LD_LIBRARY_PATH}" STREQUAL "")
    set_property(TEST test-stormweaver-sql APPEND PROPERTY ENVIRONMENT
        "LD_LIBRARY_PATH=${TEST_PG_LD_LIBRARY_PATH}:$ENV{LD_LIBRARY_PATH}")
  endif()
endif()
