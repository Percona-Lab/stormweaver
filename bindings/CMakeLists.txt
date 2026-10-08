nanobind_add_module(_stormweaver FREE_THREADED module.cpp)
target_link_libraries(_stormweaver PRIVATE stormweaver_core)
target_compile_definitions(_stormweaver PRIVATE STORMWEAVER_VERSION="${STORMWEAVER_VERSION}")

# Empty: wheel layout (scikit-build-core installs relative to the wheel root).
# Absolute site-packages dir: plain CMake install, what the distro packages do.
set(STORMWEAVER_PYTHON_INSTALL_DIR "" CACHE PATH "site-packages directory to install the stormweaver package into")
if(STORMWEAVER_PYTHON_INSTALL_DIR)
  if(NOT IS_ABSOLUTE "${STORMWEAVER_PYTHON_INSTALL_DIR}")
    message(FATAL_ERROR "STORMWEAVER_PYTHON_INSTALL_DIR must be absolute")
  endif()
  install(TARGETS _stormweaver LIBRARY DESTINATION "${STORMWEAVER_PYTHON_INSTALL_DIR}/stormweaver")
  # non-.py package data must be added to the pattern list or it ships in the wheel only
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/python/stormweaver"
          DESTINATION "${STORMWEAVER_PYTHON_INSTALL_DIR}"
          FILES_MATCHING PATTERN "*.py" PATTERN "py.typed"
          PATTERN "__pycache__" EXCLUDE)
  install(CODE "execute_process(
      COMMAND \"${Python_EXECUTABLE}\" \"${CMAKE_SOURCE_DIR}/packaging/write_dist_info.py\"
              \"\$ENV{DESTDIR}${STORMWEAVER_PYTHON_INSTALL_DIR}\" \"${CMAKE_SOURCE_DIR}\"
      COMMAND_ERROR_IS_FATAL ANY)")
else()
  install(TARGETS _stormweaver LIBRARY DESTINATION stormweaver)
endif()

foreach(_nb_target nanobind-static nanobind-static-ft)
  if(TARGET ${_nb_target})
    target_compile_options(${_nb_target} PRIVATE -Wno-missing-declarations -Wno-error)
  endif()
endforeach()
