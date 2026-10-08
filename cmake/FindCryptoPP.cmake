# Distro Crypto++: pkg-config name is libcryptopp (EPEL, SUSE) or
# libcrypto++ (Debian, Ubuntu); plain header/library search as fallback.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_CryptoPP QUIET libcryptopp)
  if(NOT PC_CryptoPP_FOUND)
    pkg_check_modules(PC_CryptoPP QUIET libcrypto++)
  endif()
endif()

find_path(CryptoPP_INCLUDE_DIR cryptopp/sha.h HINTS ${PC_CryptoPP_INCLUDE_DIRS})
find_library(CryptoPP_LIBRARY NAMES cryptopp crypto++ HINTS ${PC_CryptoPP_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CryptoPP
  REQUIRED_VARS CryptoPP_LIBRARY CryptoPP_INCLUDE_DIR
  VERSION_VAR PC_CryptoPP_VERSION)

if(CryptoPP_FOUND AND NOT TARGET CryptoPP::CryptoPP)
  add_library(CryptoPP::CryptoPP UNKNOWN IMPORTED)
  set_target_properties(CryptoPP::CryptoPP PROPERTIES
    IMPORTED_LOCATION "${CryptoPP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${CryptoPP_INCLUDE_DIR}")
endif()
mark_as_advanced(CryptoPP_INCLUDE_DIR CryptoPP_LIBRARY)
