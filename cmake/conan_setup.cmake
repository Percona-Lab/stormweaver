# wraps the vendored conan_provider.cmake without editing it: appends our repo
# profile last in the host profile list, so it wins over default/auto-cmake
# (conan composes profiles left to right, later overrides earlier).
set(CONAN_HOST_PROFILE "default;auto-cmake;${CMAKE_CURRENT_LIST_DIR}/../conan/host.profile" CACHE STRING "Conan host profile")
include(${CMAKE_CURRENT_LIST_DIR}/conan_provider.cmake)
