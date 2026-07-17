set(LIBRARY_SOURCES
    action/action.cpp
    action/action_registry.cpp
    action/custom.cpp
    action/ddl.cpp
    action/dml.cpp
    action/helper.cpp
    action/transaction.cpp
    action/variable.cpp
    querygen/generator.cpp
    querygen/render.cpp
    checksum.cpp
    logging.cpp
    random.cpp
    metadata/table.cpp
    statistics.cpp
    workload.cpp
    schema_discovery/pg.cpp
    schema_discovery/mysql.cpp
    schema_discovery/factory.cpp
    metadata_populator.cpp
    sql_variant/generic.cpp
    sql_variant/postgresql.cpp
    sql_variant/mysql.cpp
    sql_variant/sql_variant.cpp
    sql_dialect/pg.cpp
    sql_dialect/mysql.cpp
    sql_dialect/dialect.cpp
)

add_library(stormweaver_core STATIC ${LIBRARY_SOURCES})
target_include_directories(stormweaver_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include/")
target_compile_options(stormweaver_core PRIVATE
    -Wall -Wextra -Werror -Wmissing-declarations)
target_link_libraries(stormweaver_core PUBLIC
    Threads::Threads
    libpqxx::pqxx
    libmysqlclient::libmysqlclient
    boost::boost
    reflectcpp::reflectcpp
    spdlog::spdlog
    cryptopp::cryptopp
)
