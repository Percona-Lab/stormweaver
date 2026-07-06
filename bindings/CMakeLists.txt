nanobind_add_module(_stormweaver FREE_THREADED module.cpp)
target_link_libraries(_stormweaver PRIVATE stormweaver_core)
install(TARGETS _stormweaver LIBRARY DESTINATION stormweaver)

foreach(_nb_target nanobind-static nanobind-static-ft)
  if(TARGET ${_nb_target})
    target_compile_options(${_nb_target} PRIVATE -Wno-missing-declarations -Wno-error)
  endif()
endforeach()
