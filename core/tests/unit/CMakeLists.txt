set(UNITTEST_SOURCES main.cpp statistics_test.cpp random_test.cpp logging_test.cpp catalog_test.cpp table_test.cpp catalog_stress_test.cpp dialect_test.cpp context_test.cpp error_class_test.cpp querygen_render_test.cpp querygen_generator_test.cpp stmt_classify_test.cpp logged_sql_test.cpp)
add_executable(test-stormweaver-unit ${UNITTEST_SOURCES})
target_link_libraries(test-stormweaver-unit Catch2::Catch2 stormweaver_core)
add_test(NAME test-stormweaver-unit COMMAND test-stormweaver-unit)
set_property(TEST test-stormweaver-unit PROPERTY ENVIRONMENT
    "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/.tsan-suppressions")
