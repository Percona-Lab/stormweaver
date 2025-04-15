
SET(PSTRESS_SOURCES
    main.cpp
)

ADD_EXECUTABLE(stormweaver ${PSTRESS_SOURCES})
TARGET_LINK_LIBRARIES(stormweaver
    libstormweaver
)

INSTALL(TARGETS stormweaver DESTINATION bin)
