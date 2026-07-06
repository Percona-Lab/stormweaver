set(LIBRARY_SOURCES
    action/action.cpp
    action/action_registry.cpp
    action/custom.cpp
    action/ddl.cpp
    action/dml.cpp
    action/helper.cpp
    checksum.cpp
    logging.cpp
    random.cpp
    metadata.cpp
    statistics.cpp
    workload.cpp
    schema_discovery.cpp
    metadata_populator.cpp
    sql_variant/generic.cpp
    sql_variant/postgresql.cpp
    sql_variant/sql_variant.cpp
)

add_library(stormweaver_core STATIC ${LIBRARY_SOURCES})
target_include_directories(stormweaver_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../include/")
target_compile_options(stormweaver_core PRIVATE
    -Wall -Wextra -Werror -Wmissing-declarations)
target_link_libraries(stormweaver_core PUBLIC
    Threads::Threads
    libpqxx::pqxx
    boost::boost
    reflectcpp::reflectcpp
    spdlog::spdlog
    cryptopp::cryptopp
)
