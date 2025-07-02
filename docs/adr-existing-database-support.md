# ADR: Existing Database Support for StormWeaver

## Status
**Proposed** - Ready for implementation

## Context

StormWeaver currently assumes it starts with an empty database and builds its metadata from scratch as it creates tables, indexes, and other database objects. This approach works well for stress testing scenarios that start from a clean state, but it limits StormWeaver's ability to test backup/restore scenarios and other operations that require working with existing databases.

### Current Limitation
- StormWeaver can only work with databases it creates from scratch
- No support for connecting to existing databases with pre-existing schema
- Cannot test backup/restore workflows that start with populated databases
- Limited ability to test database recovery scenarios

### Use Cases for Existing Database Support
1. **Backup/Restore Testing**: Create a database, populate it, backup, restore, and continue testing within the same StormWeaver run
2. **Database Recovery Testing**: Test recovery from various failure scenarios during a single test execution
3. **Schema Migration Testing**: Test schema changes on existing databases within a single test workflow

## Decision

We will implement support for initializing StormWeaver's metadata from existing databases by:

1. **Adding Schema Discovery**: Query PostgreSQL system catalogs to discover existing schema
2. **Implementing Metadata Population**: Convert discovered schema into StormWeaver's metadata structures
3. **Extending Worker API**: Add `discover_existing_schema()` method to Worker class for initialization
4. **Maintaining Backwards Compatibility**: Ensure all existing scenarios continue working unchanged
5. **Single-Run Scope**: Feature designed for use within a single StormWeaver execution only

## Architecture

### Core Components

#### 1. SchemaDiscovery Class
```cpp
class SchemaDiscovery {
public:
    explicit SchemaDiscovery(sql_variant::LoggedSQL* connection);
    
    std::vector<DiscoveredTable> discoverTables();
    std::vector<DiscoveredColumn> discoverColumns(const std::string& table_name);
    std::vector<DiscoveredIndex> discoverIndexes(const std::string& table_name);
    std::vector<DiscoveredConstraint> discoverConstraints(const std::string& table_name);
    std::vector<DiscoveredPartition> discoverPartitions(const std::string& table_name);
    
private:
    sql_variant::LoggedSQL* connection_;
    // SQL queries for system catalog introspection
};
```

#### 2. MetadataPopulator Class
```cpp
class MetadataPopulator {
public:
    explicit MetadataPopulator(metadata::Metadata& metadata);
    
    void populateFromExistingDatabase(SchemaDiscovery& discovery);
    
private:
    metadata::Metadata& metadata_;
    
    metadata::Table convertCompleteTable(SchemaDiscovery& discovery, const DiscoveredTable& table);
    metadata::Column convertColumn(const DiscoveredColumn& discovered);
    metadata::Index convertIndex(const DiscoveredIndex& discovered);
    metadata::ColumnType mapPostgreSQLType(const std::string& pg_type);
};
```

#### 3. Extended Worker API
```cpp
class Worker {
public:
    // Existing constructor unchanged
    Worker(std::string const &name, sql_connector_t const &sql_connector,
           WorkloadParams const &config, metadata_ptr metadata);
    
    // Existing methods unchanged
    void create_random_tables(std::size_t count);
    void generate_initial_data();
    
    // NEW: Discover and populate metadata from existing database
    void discover_existing_schema();
    
private:
    bool discoverAndPopulateSchema();
    // ... existing members
};
```

### PostgreSQL System Catalog Queries

#### Table Discovery
```sql
SELECT 
  c.relname as table_name,
  c.relkind as table_type,
  am.amname as access_method,
  ts.spcname as tablespace,
  c.relpartbound is not null as is_partition,
  pt.parttype as partition_type
FROM pg_class c
LEFT JOIN pg_am am ON c.relam = am.oid
LEFT JOIN pg_tablespace ts ON c.reltablespace = ts.oid
LEFT JOIN pg_partition pt ON c.oid = pt.partrelid
WHERE c.relkind IN ('r', 'p')
  AND c.relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'public')
ORDER BY c.relname;
```

#### Column Discovery
```sql
SELECT 
  a.attname as column_name,
  t.typname as data_type,
  a.attlen as length,
  a.atttypmod as type_modifier,
  a.attnotnull as not_null,
  a.attnum as ordinal_position,
  CASE WHEN a.attidentity = 'd' THEN true ELSE false END as is_serial,
  CASE WHEN a.attgenerated = 's' THEN 'stored'
       WHEN a.attgenerated = 'v' THEN 'virtual'
       ELSE 'not_generated' END as generated_type,
  pg_get_expr(ad.adbin, ad.adrelid) as default_value
FROM pg_attribute a
JOIN pg_type t ON a.atttypid = t.oid
LEFT JOIN pg_attrdef ad ON a.attrelid = ad.adrelid AND a.attnum = ad.adnum
WHERE a.attrelid = $1
  AND a.attnum > 0
  AND NOT a.attisdropped
ORDER BY a.attnum;
```

#### Index Discovery
```sql
SELECT 
  i.relname as index_name,
  ix.indisunique as is_unique,
  array_agg(a.attname ORDER BY ix.indkey) as column_names,
  array_agg(CASE WHEN o.option & 1 = 1 THEN 'desc' ELSE 'asc' END) as orderings
FROM pg_index ix
JOIN pg_class i ON ix.indexrelid = i.oid
JOIN pg_class t ON ix.indrelid = t.oid
JOIN pg_attribute a ON t.oid = a.attrelid AND a.attnum = ANY(ix.indkey)
LEFT JOIN unnest(ix.indoption) WITH ORDINALITY AS o(option, pos) ON true
WHERE ix.indrelid = $1
  AND NOT ix.indisprimary
GROUP BY i.relname, ix.indisunique
ORDER BY i.relname;
```

#### Primary Key Discovery
```sql
SELECT 
  a.attname as column_name,
  a.attnum as ordinal_position
FROM pg_constraint c
JOIN pg_index i ON c.conindid = i.indexrelid
JOIN pg_attribute a ON i.indrelid = a.attrelid AND a.attnum = ANY(i.indkey)
WHERE c.conrelid = $1
  AND c.contype = 'p'
ORDER BY a.attnum;
```

#### Partition Discovery
```sql
SELECT 
  c.relname as partition_name,
  pg_get_expr(c.relpartbound, c.oid) as partition_bound
FROM pg_class c
WHERE c.relispartition = true
  AND c.relnamespace = (SELECT oid FROM pg_namespace WHERE nspname = 'public')
  AND c.relname LIKE $1
ORDER BY c.relname;
```

### Type Mapping Strategy

PostgreSQL to StormWeaver type mapping:
- `int4`, `int8`, `serial`, `bigserial` → `metadata::ColumnType::INT`
- `char`, `bpchar` → `metadata::ColumnType::CHAR`
- `varchar` → `metadata::ColumnType::VARCHAR`
- `text` → `metadata::ColumnType::TEXT`
- `float4`, `float8`, `numeric` → `metadata::ColumnType::REAL`
- `bool` → `metadata::ColumnType::BOOL`
- `bytea` → `metadata::ColumnType::BYTEA`

### Configuration and Usage

#### Lua Interface (Only Configuration Method)
```lua
-- New: Existing database mode using Worker initialization
function main()
    local node = stormweaver.create_node(sql_params)
    
    -- Create a setup worker to discover existing schema
    local setup_worker = node:make_worker("schema_discovery")
    setup_worker:discover_existing_schema()  -- NEW method
    
    -- Continue with normal workload
    local workload = node:init_random_workload(params)
    workload:run()
end

-- Existing scenarios remain unchanged
function main()
    local node = stormweaver.create_node(sql_params)
    local workload = node:init_random_workload(params)
    workload:run()
end
```

### Error Handling and Fallback

```cpp
void Worker::discover_existing_schema() {
    try {
        SchemaDiscovery discovery(sql_conn.get());
        auto tables = discovery.discoverTables();
        
        if (tables.empty()) {
            spdlog::info("No existing tables found, metadata remains empty");
            return;
        }
        
        MetadataPopulator populator(*metadata);
        populator.populateFromExistingDatabase(discovery);
        
        spdlog::info("Successfully discovered {} tables from existing database", tables.size());
        
    } catch (const std::exception& e) {
        spdlog::warn("Failed to discover existing schema: {}", e.what());
        spdlog::info("Metadata remains empty, workers will create tables as normal");
    }
}
```

## Consequences

### Positive
1. **Backup/Restore Testing**: Enables comprehensive backup/restore workflow testing
2. **Database Recovery Testing**: Supports testing recovery from various failure scenarios
3. **Backwards Compatibility**: Zero breaking changes to existing scenarios
4. **Flexible Configuration**: Multiple ways to configure existing database mode
5. **Robust Error Handling**: Graceful fallback to empty database mode on failures
6. **Thread Safety**: Leverages existing metadata reservation system for safety

### Negative
1. **Implementation Complexity**: Adds significant complexity to the codebase
2. **PostgreSQL Dependency**: Queries are PostgreSQL-specific (consistent with current focus)
3. **Maintenance Overhead**: New SQL queries and type mappings to maintain
4. **Testing Complexity**: Requires comprehensive testing of schema discovery logic
5. **Single-Run Limitation**: Cannot persist schema information between StormWeaver runs

### Risks and Mitigations

#### Risk: Schema Discovery Failures
- **Mitigation**: Robust error handling with fallback to empty database mode
- **Monitoring**: Comprehensive logging of discovery process

#### Risk: Type Mapping Incompatibilities
- **Mitigation**: Conservative type mapping with explicit error handling
- **Testing**: Comprehensive test coverage for all supported PostgreSQL types

#### Risk: Thread Safety Issues
- **Mitigation**: Use existing metadata reservation system
- **Testing**: Extensive concurrency testing

#### Risk: Schema Discovery Overhead
- **Mitigation**: Discovery only happens when explicitly called via Worker method
- **Optimization**: Discovery is one-time operation during initialization phase

## Implementation Plan

### Phase 1: Core Infrastructure
1. Implement `SchemaDiscovery` class with PostgreSQL system catalog queries
2. Implement `MetadataPopulator` class for type conversion
3. Add `Node::initFromExistingDatabase()` method
4. Implement basic error handling and fallback

### Phase 2: Worker Integration
1. Add Lua interface for `worker:discover_existing_schema()`
2. Integrate with existing Worker initialization patterns
3. Ensure proper metadata sharing between discovery worker and workload workers

### Phase 3: Comprehensive Testing
1. Unit tests for schema discovery and population
2. Integration tests with various database schemas
3. Regression tests for backwards compatibility
4. Performance benchmarks

### Phase 4: Documentation and Examples
1. Update documentation with new capabilities
2. Create example scenarios for backup/restore testing
3. Add troubleshooting guide for common issues

## Testing Strategy

### Unit Tests
- `SchemaDiscovery` class with mock database connections
- `MetadataPopulator` class with various schema combinations
- Type mapping functions for all supported PostgreSQL types
- Error handling and fallback scenarios

### Integration Tests
- Real PostgreSQL database with various schema configurations
- Backup/restore workflow testing
- Concurrent access during schema discovery
- Performance impact measurement

### Regression Tests
- All existing scenarios continue working unchanged
- Metadata consistency between empty and existing database modes
- Thread safety under concurrent load

## Acceptance Criteria

1. **Functional Requirements**
   - [ ] Can discover existing tables, columns, indexes, and constraints
   - [ ] Correctly maps PostgreSQL types to StormWeaver types
   - [ ] Supports partitioned tables and range partitioning
   - [ ] Handles primary keys and unique constraints
   - [ ] Gracefully handles schema discovery failures

2. **Non-Functional Requirements**
   - [ ] Zero breaking changes to existing scenarios
   - [ ] Thread-safe metadata population
   - [ ] Comprehensive error handling and logging
   - [ ] Performance impact < 5% for existing workflows
   - [ ] Clear documentation and examples

3. **Quality Requirements**
   - [ ] 95%+ unit test coverage for new code
   - [ ] All existing tests continue passing
   - [ ] Performance benchmarks within acceptable limits
   - [ ] Security review of SQL queries completed

## Alternatives Considered

### Alternative 1: Node-based Discovery
Implement schema discovery as a Node method instead of Worker method.
- **Pros**: Centralized initialization logic
- **Cons**: Breaks existing initialization patterns, requires new API surface

### Alternative 2: Automatic Discovery
Automatically detect existing schema on first connection.
- **Pros**: No explicit configuration needed
- **Cons**: Unexpected behavior changes, potential performance impact

### Alternative 3: Configuration-based Discovery
Use command-line or configuration files to enable discovery.
- **Pros**: External configuration control
- **Cons**: Complicates testing, affects existing test suites

## Implementation Notes

### Thread Safety Considerations
- Schema discovery occurs before workers start, avoiding concurrent access
- Metadata population uses existing reservation system for thread safety
- Discovery process is atomic - either fully succeeds or falls back to empty mode

### Performance Considerations
- Schema discovery is one-time cost during initialization
- Discovered metadata is cached for duration of test run
- Fallback to empty mode has zero performance impact

### Monitoring and Observability
- Comprehensive logging of discovery process
- Metrics for discovery success/failure rates
- Performance metrics for discovery duration
- Schema complexity metrics (table count, column count, etc.)

## Future Enhancements

### Multi-Schema Support
Extend to support multiple PostgreSQL schemas beyond 'public'.

### Schema Validation
Compare discovered schema against expected schema for validation testing.

### Cross-Database Support
Extend schema discovery to support other database systems (MySQL, Oracle, etc.).

### Selective Schema Discovery
Allow filtering of specific tables or schema elements during discovery.

## References

- [PostgreSQL System Catalogs Documentation](https://www.postgresql.org/docs/current/catalogs.html)
- [StormWeaver Metadata System Documentation](libstormweaver/include/metadata.hpp)
- [StormWeaver Architecture Overview](CLAUDE.md)