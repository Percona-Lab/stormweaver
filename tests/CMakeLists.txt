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
    message(WARNING "TEST_PG_DIR is empty and couldn't be autodetected, skipping tests")
endif()

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
