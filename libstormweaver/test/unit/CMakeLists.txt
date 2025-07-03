SET(UNITTEST_SOURCES
    main.cpp
    metadata_test.cpp
    statistics_test.cpp
)

add_executable(test-stormweaver-unit ${UNITTEST_SOURCES})
target_link_libraries(test-stormweaver-unit
    Catch2::Catch2 libstormweaver
)

add_test(NAME test-stormweaver-unit COMMAND sh -c "mkdir -p ${CMAKE_BINARY_DIR}/test-reports && $<TARGET_FILE:test-stormweaver-unit> --reporter=JUnit >${CMAKE_BINARY_DIR}/test-reports/test-stormweaver-unit.xml")
