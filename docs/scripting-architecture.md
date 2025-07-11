# Scripting architecture

StormWeaver relies on scripts written in Lua for test scenarios and utility code for the scenarios.

The C++ code is just a framework that provides a helper architecture and executes a main Lua script.
If that script does nothing, the program just exits after startup.

There are also helper functions/classes written in Lua, using the C++ code.
The goal of these is to simplify the development of test scenarios, while also allowing easy modification without rebuilding the main executable.

## Folder structure

Lua scripts are stored in three directories:

* `scenarios` is intended for specific test scenarios
* `scripts` is intended for helper functions/classes usable by multiple scenarios
* `scripts_3p/` contains third-party Lua libraries using git submodules

### Module Search Path

The following directories are automatically added to the Lua package search path:

* `scripts/` - Helper functions and utilities (relative to executable and working directory)
* `scripts_3p/` - Third-party Lua modules (relative to executable and working directory)
* Current working directory

## Third-party libraries

StormWeaver automatically loads several third-party Lua libraries and makes them available to scripts and scenarios:

### Automatically Available Libraries
* **toml** - TOML parsing library, automatically injected into the global namespace (no require needed)

### Manually Loaded Libraries
* **[argparse](https://github.com/mpeterv/argparse)** - Command-line argument parsing, available via `require("argparse")`
* **[inspect.lua](https://github.com/kikito/inspect.lua)** - Object inspection and pretty-printing, available via `require("inspect")`
* **lfs** - Lua File System library for advanced file operations, available via `require("lfs")`

### Usage Examples
```lua
-- TOML is automatically available globally
local config_data = toml.parse(config_text)

-- Other libraries require explicit loading
local lfs = require("lfs")
local inspect = require("inspect")
local argparse = require("argparse")

-- Access files from the automatic search paths
require("common")     -- Loads scripts/common.lua
require("PgManager")  -- Loads scripts/PgManager.lua
```

## C++ binding

StormWeaver includes several classes/functions implemented in C++, which are usable in Lua scripts.
The complete list of these functions is available in the [Lua C++ reference](lua-cpp-reference.md).

## Scenario configuration and entry point

### Script Entry Point
Every scenario script must define a `main` function that serves as the entry point:

```lua
require("common")

function main(argv)
  -- argv contains all command-line arguments passed to the script
  -- argv[1] is the script filename
  -- argv[2], argv[3], etc. are additional arguments
  
  local args = argparser:parse(argv)  -- Parse using argparse
  local conffile = parse_config(args) -- loads stormweaver.toml based on the --config argument
  -- Your scenario logic here
end
```

The `main` function receives command-line arguments and is called automatically when the script is executed.

### Argument Parsing
The `argparse` library is included, and the C++ runner forwards all arguments to the script, including the scenario filename.

The Lua helpers already create a default parser using the global variable `argparser`, which includes handling the common arguments, so scenarios only have to set up additional scenario-specific arguments.

### Example: Adding Custom Arguments
```lua
require("common")

-- Add scenario-specific arguments (can be done at global scope)
argparser:option("-t --timeout", "Test timeout in seconds", 30)
argparser:flag("--skip-cleanup", "Skip cleanup after test")

function main(argv)
  local args = argparser:parse(argv)
  
  local timeout = args.timeout
  local skip_cleanup = args.skip_cleanup
  
  info("Running with timeout: " .. timeout .. " seconds")
  if skip_cleanup then
    info("Cleanup will be skipped")
  end
  
  -- Rest of scenario logic
end
```

## Multi-threaded structure

StormWeaver is a multithreaded, concurrent test framework, while Lua is a single-threaded scripting language.
To make the two work together, it uses multiple Lua instances:

* There's one "main" instance used for the scenario startup and setup
* There's one instance per worker thread when running workloads
* And any of the above threads also might start background workers, resulting in additional Lua threads
 
The main thread and background workers are Lua-first:
there's a Lua function (`main` in case of the main thread, user specified for background workers) that gets executed, and when this function ends, the thread also exits.

The worker threads are however C++-first:
the random workload is generated by a C++ runner, and it only executes Lua functions when it is necessary.

### Limitations

To ensure that all threads have access to the same functions, the scenario file is loaded for all Lua states.

This results in the following limitations:

* the scenario script, and included other scripts get processed multiple times
* anything that is executed directly by the script will be executed by all threads
* while global variables will be available in all threads, their values will be different and won't be synced

In line with this, it is recommended that script/scenario files:

* shouldn't rely on mutable global variables
* shouldn't execute code directly in the source file, instead everything should go into the `main` function or other callable functions
* should define functions and constants at global scope, but defer execution to the `main` function

