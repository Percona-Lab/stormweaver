
set(LIBRARY_SOURCES
    action/action.cpp
    action/action_registry.cpp
    action/custom.cpp
    action/ddl.cpp
    action/dml.cpp
    process/postgres.cpp
    process/process.cpp
    random.cpp
    metadata.cpp
    workload.cpp
    scripting/luactx.cpp
    sql_variant/generic.cpp
    #sql_variant/mysql.cpp
    sql_variant/postgresql.cpp
    sql_variant/sql_variant.cpp
)

ADD_LIBRARY(libstormweaver STATIC ${LIBRARY_SOURCES})
TARGET_INCLUDE_DIRECTORIES(libstormweaver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include/")
TARGET_LINK_LIBRARIES(libstormweaver 
    #libmysqlclient::libmysqlclient
    libpqxx::pqxx
    boost::boost
    reflectcpp::reflectcpp
    spdlog::spdlog
    sol2::sol2
    toml-lua
    lfs-lua
)
SET_PROPERTY(TARGET libstormweaver PROPERTY CXX_STANDARD 23)
