
SET(UNITTEST_SOURCES
    main.cpp
    metadata_test.cpp
)

add_executable(stormweaver-unit ${UNITTEST_SOURCES})
target_link_libraries(stormweaver-unit
    Catch2::Catch2 libstormweaver
)

add_test(NAME stormweaver-unit COMMAND sh -c "mkdir -p ${CMAKE_BINARY_DIR}/test-reports && $<TARGET_FILE:stormweaver-unit> --reporter=JUnit >${CMAKE_BINARY_DIR}/test-reports/stormweaver-unit.xml")