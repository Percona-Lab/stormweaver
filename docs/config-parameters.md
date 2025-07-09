# Configuration Parameters

StormWeaver supports configuration through multiple layers with a clear precedence order: Environment Variables > Command Line Arguments > TOML Configuration File > Hardcoded Defaults.

!!! Note

    Configuration parameters are parsed by Lua scenario scripts.
    The C++ runner only passes the command-line parameters to the scenario, and provides an API for accessing all configuration options.
  
    Custom (external) scenario files might work differently.

## TOML Configuration File

The main configuration is stored in `config/stormweaver.toml`. You can specify a different config file using the `-c/--config` command-line option.

### Default Configuration Section

```toml
[default]
pgroot = ""             # PostgreSQL installation directory path
datadir_root = "datadirs"   # Root directory for PostgreSQL data directories
port_start = 15432      # Starting port number for PostgreSQL instances
port_end = 15531        # Ending port number for PostgreSQL instances
```

### Configuration Parameters

#### pgroot
- **Type**: String
- **Default**: `""` (empty string)
- **Purpose**: Path to the PostgreSQL installation directory
- **Override**: Can be overridden by `--install_dir` command-line option or `PGROOT` environment variable
- **Example**: `pgroot = "/usr/lib/postgresql/16"`

#### datadir_root
- **Type**: String  
- **Default**: `"datadirs"`
- **Purpose**: Root directory where PostgreSQL data directories will be created
- **Example**: `datadir_root = "/tmp/stormweaver_data"`

#### port_start
- **Type**: Integer
- **Default**: `15432`
- **Purpose**: Starting port number for PostgreSQL instances (avoids conflicts with system PostgreSQL)
- **Example**: `port_start = 20000`

#### port_end
- **Type**: Integer
- **Default**: `15531`
- **Purpose**: Ending port number for PostgreSQL instances (defines available port range)
- **Example**: `port_end = 20099`

## Command-Line Arguments

StormWeaver accepts the following command-line arguments:

### Required Arguments

#### scenario
```bash
stormweaver scenarios/basic.lua
```
- **Purpose**: Path to the Lua scenario file to execute
- **Type**: Positional argument (first argument)

### Optional Arguments

#### -c, --config
```bash
stormweaver -c /path/to/config.toml scenarios/basic.lua
stormweaver --config custom-config.toml scenarios/basic.lua
```
- **Purpose**: Specify configuration file path
- **Default**: `"config/stormweaver.toml"`
- **Type**: String

#### -i, --install_dir
```bash
stormweaver -i /usr/lib/postgresql/16 scenarios/basic.lua
stormweaver --install_dir /opt/postgres scenarios/basic.lua
```
- **Purpose**: PostgreSQL installation directory (overrides config file and environment variable)
- **Default**: `""` (uses config file value)
- **Type**: String

#### --include
```bash
stormweaver --include /path/to/scripts --include /another/path scenarios/basic.lua
```
- **Purpose**: Add directories to Lua package search path
- **Default**: `""` (empty)
- **Type**: String (can be specified multiple times)

### Example Usage
```bash
# Basic usage
stormweaver scenarios/basic.lua

# With custom config and PostgreSQL installation
stormweaver -c myconfig.toml -i /usr/lib/postgresql/16 scenarios/basic.lua

# With additional Lua script directories
stormweaver --include /my/scripts scenarios/basic.lua
```

## Environment Variables

### PGROOT
```bash
export PGROOT="/usr/lib/postgresql/16"
stormweaver scenarios/basic.lua
```
- **Purpose**: Override PostgreSQL installation directory
- **Precedence**: Takes precedence over TOML config but not command-line `--install_dir`
- **Type**: String

### Generic Environment Access
Lua scripts can access any environment variable using the `getenv()` function:
```lua
local value = getenv("MY_VARIABLE", "default_value")
```

## Build-Time Configuration

### CMake Variables

#### Sanitizer Options
```bash
cmake -DWITH_ASAN=ON -DWITH_UBSAN=ON ..
```
- **WITH_ASAN**: Enable Address Sanitizer (default: OFF)
- **WITH_UBSAN**: Enable Undefined Behavior Sanitizer (default: OFF)
- **WITH_TSAN**: Enable Thread Sanitizer (default: OFF)
- **WITH_MSAN**: Enable Memory Sanitizer (default: OFF)

#### Test Configuration
```bash
cmake -DTEST_PG_DIR=/usr/lib/postgresql/16 ..
```
- **TEST_PG_DIR**: PostgreSQL installation directory for testing
- **Default**: Auto-detected from common locations or empty
- **Purpose**: Required for SQL integration tests

#### Compiler Options
```bash
cmake -DSTRICT_FLAGS=OFF -DSTRICT_CPU=ON ..
```
- **STRICT_FLAGS**: Enable strict compiler warnings (default: ON)
- **STRICT_CPU**: Optimize for current CPU architecture (default: OFF)

### Conan Build Options
```bash
conan build . -o '&:asan=True' -o '&:ubsan=True'
```
- **asan**: Address sanitizer (default: False)
- **ubsan**: Undefined behavior sanitizer (default: False)
- **tsan**: Thread sanitizer (default: False)
- **msan**: Memory sanitizer (default: False)

## Default PostgreSQL Settings

When StormWeaver creates PostgreSQL instances, it applies these default settings:

```ini
listen_addresses = '*'
logging_collector = on
log_statement = ALL
log_directory = 'logs'
log_filename = 'server.log'
log_min_messages = info
unix_socket_directories = sock
```

These can be modified in scenarios using the `add_config()` method:

```lua
pg:add_config({
  max_connections = "200",
  shared_buffers = "256MB"
})
```

## Lua Package Path

StormWeaver automatically extends the Lua package path with:
- `scripts/` - Helper functions and utilities
- `scripts_3p/` - Third-party Lua modules
- Current working directory
- Any directories specified via `--include`

This allows scenarios to use `require()` to load modules from these locations.