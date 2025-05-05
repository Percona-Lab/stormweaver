if(NOT "${TEST_PG_DIR}" STREQUAL "")

    set(TEST_SCENARIOS
        background
        simple-pg
    )

    foreach(test ${TEST_SCENARIOS})
        add_test(NAME stormweaver-${test}
		COMMAND stormweaver "${CMAKE_SOURCE_DIR}/tests/${test}.lua" "-i" "${TEST_PG_DIR}"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endforeach()

endif()
