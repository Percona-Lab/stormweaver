SET(SQLTEST_SOURCES
    main.cpp
    ddl.cpp
    schema_discovery.cpp
    metadata_populator.cpp
    worker_schema_discovery.cpp
)

add_executable(test-stormweaver-sql ${SQLTEST_SOURCES})
target_link_libraries(test-stormweaver-sql
    Catch2::Catch2 libstormweaver
)

add_test(NAME test-stormweaver-sql COMMAND sh -c "mkdir -p ${CMAKE_BINARY_DIR}/test-reports && $<TARGET_FILE:test-stormweaver-sql> ${TEST_PG_DIR} ${CMAKE_BINARY_DIR}/test/sql-data 25432 --reporter=JUnit >${CMAKE_BINARY_DIR}/test-reports/test-stormweaver-sql.xml")
