# Extensibility Workflow

This guide covers advanced development workflows for extending StormWeaver, focusing on the action registry system and Lua C++ API extensions.

## Action Registry System

The action registry is at the core of StormWeaver's randomized testing framework.
It manages a collection of linearly weighted actions that are randomly selected and executed by worker threads.

During the random selection each action is added to a pool multiple times based on its weight (1 = only once, 10 - 10 entries, and so on), and then one entry in the pool is selected randomly.

### Understanding the Action Architecture

#### Core Components

**Action Interface**
```cpp
class Action {
public:
  virtual void execute(metadata::Metadata &metaCtx, ps_random &rand,
                       sql_variant::LoggedSQL *connection) const = 0;
};
```

All actions must implement this interface with these key principles:
- **Stateless**: Actions should be stateless to enable retry logic
- **Thread-safe**: Multiple workers execute actions concurrently
- **Metadata-aware**: Actions interact with shared database schema metadata

Actions are expected to throw a `RuntimeException` (preferably `ActionException`) on failure.

**ActionFactory Structure**
```cpp
struct ActionFactory {
  std::string name;        // Unique identifier
  action_build_t builder;  // Factory function
  std::size_t weight;      // Probability weight
};
```

**ActionRegistry Operations**
- `insert()`: Add new actions
- `remove()`: Remove actions by name
- `get()`: Retrieve action factory for modification
- `lookupByWeightOffset()`: Random weighted selection

### Creating Custom Actions

#### 1. Simple Custom SQL Actions

The easiest way to add custom actions is through Lua scripts:

```lua
function setup_custom_actions()
  local registry = defaultActionRegistry()
  
  -- Simple SQL action (no parameters)
  registry:makeCustomSqlAction("vacuum_db", "VACUUM;", 10)
  
  -- Table-based SQL action (uses {table} placeholder)
  registry:makeCustomTableSqlAction("analyze_table", "ANALYZE {table};", 5)
  
  -- Custom pg_tde actions
  registry:makeCustomSqlAction("tde_set_key", 
    "SELECT pg_tde_set_server_key('test_key','reg_file','false');", 15)
end
```

#### 2. Complex Custom Actions in C++

For more complex logic, create custom action classes:

**Step 1: Define Action Class**
```cpp
// File: libstormweaver/include/action/my_action.hpp
#pragma once
#include "action/action.hpp"

namespace action {

struct CustomConfig {
    std::string prefix;
};

class MyCustomAction : public Action {
public:
  
  MyCustomAction(CustomConfig const &config);
  
  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;
               
private:
  CustomConfig config_;
};

} // namespace action
```

!!! Note

    The above example showcases a completely new action type in a new source file.
    When appropriate, edit existing files, such as ddl.hpp/cpp or dml.hpp/cpp.
    When editing existing files, it is usually better to extend the existing configuration struct instead of defining a new struct.

**Step 2: Implement Action Logic**
```cpp
// File: libstormweaver/src/action/my_action.cpp
#include "action/my_action.hpp"
#include "action/helper.hpp"

namespace action {

MyCustomAction::MyCustomAction(CustomConfig const &config) : config_(config) {}

void MyCustomAction::execute(metadata::Metadata &metaCtx, ps_random &rand,
                             sql_variant::LoggedSQL *connection) const {
  // 1. Find a suitable table
  auto table = find_random_table(metaCtx, rand);
  if (!table) {
    throw ActionException("No tables available");
  }
  
  // 2. Generate dynamic SQL
  std::string sql = fmt::format("SELECT COUNT(*) FROM {} WHERE id LIKE '{}%'", 
                               table->name, config_.prefix);
  
  // 3. Execute query and handle results
  auto result = connection->executeQuery(sql);
  if (!result.success) {
    throw ActionException("Query failed: " + result.query);
  }
}

} // namespace action
```

**Step 3: Add Configuration to AllConfig**

When creating custom actions with their own configuration, you must add the configuration to the `AllConfig` structure:

```cpp
// File: libstormweaver/include/action/all.hpp
// Add your custom config to the AllConfig struct:

struct AllConfig {
  DdlConfig ddl;
  DmlConfig dml;
  CustomConfig custom;        // Add this line for your custom configuration
  // ... other existing configs
};
```

The `AllConfig` structure is used to pass all configuration parameters to action factories. Without adding your custom configuration here, your action factory won't be able to access the configuration parameters it needs.

**Step 4: Register Action**
```cpp
// File: libstormweaver/src/action/action_registry.cpp
#include "action/my_action.hpp"

// Declare a factory in the anonymous namespace:
ActionFactory myCustomAction{
    "my_custom_action",
    [](AllConfig const &config) {
      return std::make_unique<MyCustomAction>(config.custom);
    },
    25  // Weight
};

// Add it to the default registry in initializeDefaultRegistry
ar.insert(myCustomAction);
```

### Working with Composite Actions

Composite actions allow you to combine multiple operations.

For examples and possibilities, see `action/composite.hpp` and `action/action_registry.cpp`.

### Runtime Action Management

Modify action registry during scenario execution:

```lua
function customize_workload(node)
  local registry = node:possibleActions()
  
  -- Remove problematic actions
  registry:remove("drop_table")
  registry:remove("drop_partition")
  
  -- Increase data manipulation frequency
  registry:get("insert_some_data").weight = 200
  registry:get("update_one_row").weight = 150
  
  -- Add scenario-specific actions
  registry:makeCustomTableSqlAction("reindex_table", 
    "REINDEX TABLE {table};", 20)
  
  -- Add checkpoint action
  registry:makeCustomSqlAction("checkpoint", "CHECKPOINT;", 5)
end
```

## Extending the Lua C++ API

### Understanding the Binding System

StormWeaver uses Sol2 to bind C++ classes and functions to Lua. All bindings are defined in `libstormweaver/src/scripting/luactx.cpp`.

### Adding New Global Functions

**Step 1: Implement C++ Function**
```cpp
// In luactx.cpp or separate file
std::string get_database_version(sql_variant::LoggedSQL* conn) {
  auto result = conn->executeQuery("SELECT version()");
  if (result.success && result.data.nextRow()) {
    return result.data.field(1);
  }
  return "unknown";
}
```

**Step 2: Bind to Lua**
```cpp
// In LuaContext::LuaContext() constructor
luaState["get_database_version"] = &get_database_version;
```

**Step 3: Use in Lua Scripts**
```lua
function check_database_version(worker)
  local conn = worker:sql_connection()
  local version = get_database_version(conn)
  info("Database version: " .. version)
end
```

### Adding New Classes

**Step 1: Define C++ Class**
```cpp
// File: libstormweaver/include/utils/database_info.hpp
class DatabaseInfo {
public:
  DatabaseInfo(sql_variant::LoggedSQL* conn);
  
  std::string getVersion() const;
  int getConnectionCount() const;
  std::vector<std::string> getTableNames() const;
  
private:
  sql_variant::LoggedSQL* connection_;
};
```

**Step 2: Implement Class Methods**
```cpp
// File: libstormweaver/src/utils/database_info.cpp
DatabaseInfo::DatabaseInfo(sql_variant::LoggedSQL* conn) : connection_(conn) {}

std::string DatabaseInfo::getVersion() const {
  auto result = connection_->executeQuery("SELECT version()");
  // Implementation...
}

std::vector<std::string> DatabaseInfo::getTableNames() const {
  auto result = connection_->executeQuery(
    "SELECT tablename FROM pg_tables WHERE schemaname = 'public'");
  std::vector<std::string> tables;
  while (result.data.nextRow()) {
    tables.push_back(result.data.field(1));
  }
  return tables;
}
```

**Step 3: Bind Class to Lua**
```cpp
// In LuaContext::LuaContext() constructor
auto db_info_usertype = luaState.new_usertype<DatabaseInfo>(
    "DatabaseInfo", 
    sol::constructors<DatabaseInfo(sql_variant::LoggedSQL*)>());

db_info_usertype["getVersion"] = &DatabaseInfo::getVersion;
db_info_usertype["getConnectionCount"] = &DatabaseInfo::getConnectionCount;
db_info_usertype["getTableNames"] = &DatabaseInfo::getTableNames;
```

**Step 4: Use in Lua Scripts**
```lua
function analyze_database(worker)
  local conn = worker:sql_connection()
  local dbinfo = DatabaseInfo(conn)
  
  info("Database version: " .. dbinfo:getVersion())
  info("Connection count: " .. dbinfo:getConnectionCount())
  
  local tables = dbinfo:getTableNames()
  for i, table_name in ipairs(tables) do
    info("Table " .. i .. ": " .. table_name)
  end
end
```

### Extending Existing Classes

Add new methods to existing classes:

```cpp
// Add to Worker bindings
worker_usertype["getTableCount"] = [](Worker& self) -> int {
  return self.metadata().tableCount();
};

worker_usertype["createSpecialTable"] = [](Worker& self, std::string const& name) {
  // Custom table creation logic
};
```

### Advanced API Patterns

#### Callback Support
```cpp
// Support Lua callbacks in C++ code
// Note: Lua callbacks implemented this way are not thread-safe,
// only use them for immediate use
// For more advanced multithreaded use, see the LuaCallback class
auto lua_callback = luaState.get<sol::function>("my_callback");
if (lua_callback.valid()) {
  sol::protected_function_result result = lua_callback(arg1, arg2);
  if (!result.valid()) {
    sol::error err = result;
    spdlog::error("Lua callback failed: {}", err.what());
  }
}
```

#### Property Access
```cpp
// Add properties to existing classes
postgres_usertype["data_directory"] = sol::property(
  &process::Postgres::getDataDirectory,
  &process::Postgres::setDataDirectory
);
```

#### Error Handling
```cpp
// Proper exception handling for Lua bindings
luaState["risky_function"] = [](int value) -> int {
  try {
    if (value < 0) {
      throw std::invalid_argument("Value must be non-negative");
    }
    return value * 2;
  } catch (std::exception const& e) {
    throw sol::error(e.what());
  }
};
```

## Best Practices

### Action Development
1. **Thread Safety**: Use proper locking when accessing shared resources
2. **Error Handling**: Use `ActionException` for action-specific errors
3. **Metadata Consistency**: Always use the reservation pattern for metadata updates
4. **Testing**: Create unit and integration tests for custom actions

### API Extension
1. **Memory Management**: Use smart pointers and RAII principles
2. **Exception Safety**: Wrap C++ exceptions for Lua consumption
3. **Documentation**: Update lua-cpp-reference.md for new API additions
4. **Backward Compatibility**: Consider the impact on existing scripts

### Debugging
1. **Logging**: Use the existing spdlog infrastructure for consistent logging across C++ and Lua
2. **Error Messages**: Provide clear, actionable error messages
3. **Stack Traces**: Sol2 provides good error reporting for the Lua/C++ boundary
4. **Testing**: Use sanitizers (ASAN, TSAN) during development
