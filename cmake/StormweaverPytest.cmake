# registers a python test file as a ctest test run through the project venv.
# skipped when no venv exists (source checkout without an editable install).
find_program(SW_VENV_PYTHON python
             PATHS ${CMAKE_SOURCE_DIR}/.venv/bin
             NO_DEFAULT_PATH)

function(stormweaver_add_pytest file)
  if(NOT SW_VENV_PYTHON)
    return()
  endif()
  get_filename_component(_name ${file} NAME_WE)
  add_test(NAME pytest.${_name}
           COMMAND ${SW_VENV_PYTHON} -m pytest -q --no-header
                   ${CMAKE_SOURCE_DIR}/${file}
           WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  # feed pytest's install discovery from the same cache vars the C++ tests use,
  # so the demos run zero-config under ctest instead of skipping.
  set(_env "")
  if(NOT "${TEST_PG_DIR}" STREQUAL "")
    list(APPEND _env "STORMWEAVER_PG_DIR=${TEST_PG_DIR}")
  endif()
  if(DEFINED TEST_MYSQL_DIR AND NOT "${TEST_MYSQL_DIR}" STREQUAL "")
    list(APPEND _env "STORMWEAVER_MYSQL_DIR=${TEST_MYSQL_DIR}")
  endif()
  if(NOT "${TEST_PG_LD_LIBRARY_PATH}" STREQUAL "")
    list(APPEND _env "LD_LIBRARY_PATH=${TEST_PG_LD_LIBRARY_PATH}:$ENV{LD_LIBRARY_PATH}")
  endif()
  # one test per demo file, so a "skipped" summary means the whole file skipped
  # (missing install) rather than a partial run - report it as ctest SKIP.
  set_tests_properties(pytest.${_name} PROPERTIES
      TIMEOUT 600
      ENVIRONMENT "${_env}"
      SKIP_REGULAR_EXPRESSION "skipped")
endfunction()
