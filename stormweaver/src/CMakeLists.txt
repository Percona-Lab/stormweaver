
SET(PSTRESS_SOURCES
    main.cpp
)

ADD_EXECUTABLE(stormweaver ${PSTRESS_SOURCES})
TARGET_LINK_LIBRARIES(stormweaver
    libstormweaver
    sol2::sol2
    nlohmann_json::nlohmann_json
    toml-lua
)

INSTALL(TARGETS stormweaver DESTINATION bin)
