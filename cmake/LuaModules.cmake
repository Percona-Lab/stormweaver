
INSTALL(FILES _3p-lua/inspect/inspect.lua DESTINATION scripts_3p)
INSTALL(FILES _3p-lua/argparse/src/argparse.lua DESTINATION scripts_3p)

SET(TOML_LUA_SOURCES
_3p-lua/toml/src/toml.cpp
_3p-lua/toml/src/decoding/decoding.cpp
_3p-lua/toml/src/encoding/encoding.cpp
_3p-lua/toml/src/DataTypes/DateAndTime/dateAndTime.cpp
_3p-lua/toml/src/DataTypes/TOMLInt/TOMLInt.cpp
_3p-lua/toml/src/utilities/utilities.cpp
)

ADD_LIBRARY(toml-lua ${TOML_LUA_SOURCES})
TARGET_INCLUDE_DIRECTORIES(toml-lua PUBLIC "${CMAKE_SOURCE_DIR}/_3p-lua/toml/src/")
TARGET_LINK_LIBRARIES(toml-lua
    sol2::sol2
    tomlplusplus::tomlplusplus
    magic_enum::magic_enum
)

TARGET_COMPILE_OPTIONS(toml-lua PRIVATE "-Wno-missing-declarations")

set(LUA_FILESYSTEM_SOURCES
_3p-lua/luafilesystem/src/lfs.c
)

ADD_LIBRARY(lfs-lua ${LUA_FILESYSTEM_SOURCES})
TARGET_INCLUDE_DIRECTORIES(lfs-lua PUBLIC "${CMAKE_SOURCE_DIR}/_3p-lua/luafilesystem/src/")
TARGET_LINK_LIBRARIES(lfs-lua
    sol2::sol2
)
