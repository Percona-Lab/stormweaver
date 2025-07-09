# Lua C++ API Reference

This is a reference of the Lua API implemented directly in C++.

!!! note

    Lua bindings are defined in `libstormweaver/src/scripting/luactx.cpp`

## Global functions

### sleep

```lua
sleep(milliseconds)
```

Sleeps for the specified time

### defaultActionRegistry

```lua
a = defaultActionRegistry()
```

Returns a modifiable reference to the default action registry

### initPostgresDatadir

Creates a new postgres data directory

```lua
pg = initPostgresDatadir('pg/install/dir', 'new/data/dir')
```

### initBasebackupFrom

Creates a new postgres data directory as a backup of a running postgres instance.

```lua
replica = initBasebackupFrom('pg/install/dir', 'new/data/dir', primaryNode) -- allows any number of additional parameters, added to the basebackup command line
```

### debug

Writes a debug message to the log

### info

Writes an info message to the log

### warning

Writes a warning message to the log

### getenv

Returns the value of an environment variable, or a default.

```lua
e = getenv("NAME", "default")
```

### setup_node_pg

Configures the connection parameters to a PostgreSQL installation.

```lua
return setup_node_pg({
  host = "localhost",
  port = 5432,
  user = "username",
  password = "",
  database = "stormweaver",
  on_connect = conn_callback,
})
```


## ActionFactory

Used to create a specific action, stored in an `ActionRegistry`

### weight

A property representing the chance/weight of an action

```lua
action.weight = action.weight + 13;
```

## ActionRegistry

Represents a set of actions executed by worker(s).

### remove

Removes the action with the name from the registry.

```lua
ar:remove("alter_table")
```

### insert

Inserts the specified ActionFactory to the registry.
This function requires a specific Factory, which can be retrieved from another Registry using the `get` function.

```lua
ar1:insert(ar2:get("truncate"))
```

### has

Tells if the registry has a factory with the specified name.

```lua
if ar:has("alter_table") then
  --- ...
end
```

### makeCustomSqlAction

Creates a new custom SQL action with a specific query and weight.

```lua
ar:makeCustomSqlAction("checkpoint", "CHECKPOINT;", 1)
```

### makeCustomTableSqlAction

Creates a new custom SQL action related to a table, with a specific weight.
The `{table}` expression in the SQL command is replaced with a randomly choosen table.
Can be specified multiple times, and the same table will be used each time.

```lua
ar:makeCustomTableSqlAction("truncate_table", "TRUNCATE {table};", 2)
```

### get

Returns a reference to the factory with the specified name.

```lua
ar1:insert(ar2:get("truncate"))
```

### use

Overwrites the contents of the registry using the specified other registry.
The two registries will be equal after executing this function.

```lua
ar1:use(ar2)
```

## Default Actions

The default action registry includes the following built-in actions available for randomized testing:

### Table Management Actions
- `create_normal_table` - Creates a regular table with random columns and with some initial data
- `create_partitioned_table` - Creates a partitioned table with random columns and with some initial data
- `drop_table` - Drops a randomly selected existing table
- `alter_table` - Performs random alterations to a randomly selected existing table
- `rename_table` - Renames a randomly selected table

### Index Management Actions  
- `create_index` - Creates an index on a randomly selected table
- `drop_index` - Drops a randomly selected existing index

### Partition Management Actions
- `create_partition` - Creates a partition for an existing randomly selected partitioned table
- `drop_partition` - Drops a randomly selected partition

### Data Manipulation Action
- `insert_some_data` - Inserts random data into a randomly selected existing table (multiple rows)
- `delete_some_data` - Deletes random rows from a randomly selected existing table
- `update_one_row` - Updates a random row in a a randomly selected existing table

### Example: Customizing Default Actions

```lua
-- This function modifies the actual default ActionRegistry
-- Every call after this modification will use the modified default settings
function customize_default_actions()
  local registry = defaultActionRegistry()
  
  -- Remove actions we don't want
  registry:remove("drop_table")
  registry:remove("drop_partition")
  
  -- Adjust weights for remaining actions
  registry:get("insert_some_data").weight = 20  -- Make inserts more likely
  registry:get("create_index").weight = 5       -- Make index creation less likely
  
  -- Add custom actions
  registry:makeCustomSqlAction("vacuum", "VACUUM;", 2)
  registry:makeCustomTableSqlAction("analyze_table", "ANALYZE {table};", 3)
end
```

## fs

A collection of filesystem operations.

### is_directory

Returns true if a directory with the specified name exists

```lua
if fs.is_directory('foo/bar') then
  -- ...
end
```

### copy_directory

Copies the directory recursively

```lua
fs.copy_directory('from', 'to')
```

### create_directory

Creates a new directory recursively

```lua
fs.create_directory('path/to/the/new/dir')
```

### delete_directory

Deletes the specified directory

```lua
fs.delete_directory('foo/bar')
```

## LoggedSQL

A class representing a database connection

### execute_query

Executes a SQL query and returns a QueryResult object containing the result set.
This function can execute any type of SQL statement and provides access to result data for queries that return results (like SELECT).

```lua
conn.execute_query("CREATE EXTENSION pg_tde;")
```

## QueryResult

A class representing the result of a SQL query execution.

### success

A boolean property indicating whether the query executed successfully.

```lua
if result.success then
  -- Query executed successfully
end
```
### query

A string property containing the original SQL query that was executed.

```lua
print("Executed query: " .. result.query)
```

### data

A QuerySpecificResult object containing the actual result data, only available when success is true.

```lua
if result.success then
  local resultSet = result.data
end
```

## QuerySpecificResult

A class representing the data returned from a successful SQL query.

### numRows

A property containing the number of rows returned by the query.

```lua
local rowCount = result.data.numRows
```

### numFields

A property containing the number of columns (fields) in the result set.

```lua
local columnCount = result.data.numFields
```

### nextRow

Advances to the next row in the result set. Returns true if there is a next row, false if at the end.
Must be called before accessing field data for each row.

```lua
while result.data:nextRow() do
  -- Process current row
end
```

## RowView

A class representing the current row in a result set, accessed through QuerySpecificResult.

### field

Returns the value of the specified field in the current row.
Can be accessed only by column index (1-indexed).

```lua
local value = result.data:field(1)  -- Get first column (1-indexed)
```

### numFields

Returns the number of fields (columns) in the current row.

```lua
local columnCount = result.data:numFields()
```

### Example: Processing Query Results

```lua
function process_table_data(worker)
  local conn = worker:sql_connection()
  local result = conn:execute_query("SELECT id, name, value FROM test_table")
  
  if result.success then
    info("Query returned " .. result.data.numRows .. " rows")
    
    while result.data:nextRow() do
      local id = result.data:field(1)      -- By index
      local name = result.data:field("name")  -- By column name
      local value = result.data:field(3)
      
      info("Row: id=" .. id .. ", name=" .. name .. ", value=" .. value)
    end
  else
    warning("Query failed: " .. result.query)
  end
end
```

## Node

A class representing a database server, created using the `setupPgNode` global function.

### init

Intended for node initialization, takes a callback that receives as a Worker as the single parameter.

```lua
function setup_fun(worker)
	worker:create_random_tables(5)
end
pgm.primaryNode:init(setup_fun)
```

### initRandomWorkload

Creates a new random workload setup with the given parameters.

It expects a WorkloadParams object:

```lua
params = WorkloadParams()
params.duration_in_seconds = 10
params.number_of_workers = 5
node:initRandomWorkload(params)
```

Supported parameters:

* `duration_in_seconds`: how long to run the workload (default: 60)
* `number_of_workers`: how many workers to use in the workload (default 5)
* `repeat_times`: how many times to repeat the entire workload (default: 10)
* `max_reconnect_attempts`: maximum number of reconnection attempts when a worker loses connection (default: 5)

### possibleActions

Returns a reference to the action registry associated with the node.
This is a copy made at the time of creating the node, and can be modified separately.
Will be copied into the workload when one is created using `initRandomWorkload`.

```lua
a = node:possibleActions()
```

### make_worker

```lua
worker = node:make_worker("initialization")
```

Creates a standalone Worker instance for the node with the specified log name.
Useful for database initialization or other non-workload operations.

## WorkloadParams

A class for configuring workload parameters. Can be created and configured before passing to `initRandomWorkload`.

### Constructor

```lua
params = WorkloadParams()
```

Creates a new WorkloadParams object with default values.

### Properties

```lua
params.duration_in_seconds = 30      -- How long each workload iteration runs
params.number_of_workers = 8         -- Number of concurrent worker threads
params.repeat_times = 5              -- Number of times to repeat the workload
params.max_reconnect_attempts = 3    -- Max reconnection attempts per worker
```

## Postgres

A class used to manage a Postgres installation/datadir.

An instance of this class can be created with the global functions `initPostgresDatadir` and `initBasebackupFrom`.

### start

Starts the server, returns true if succeeds.

```lua
pg1:start()                                           -- Normal start
pg1:start("/usr/bin/valgrind", {"--leak-check=full"}) -- Start under valgrind
```

The optional wrapper and wrapperArgs parameters allow running PostgreSQL through external tools like debuggers or profilers.

!!! note

    The wrapper parameter doesn't take `$PATH` into account, it requires a full filename

### stop

Stops the server
Has a parameter, the graceful wait period - after that, it executes kill9.


```lua
pg1:stop(10)
```

### restart

Restarts (stops and starts) the server, returns true if succeeds.
Has a parameter, the graceful wait period - after that, it executes kill9.

```lua
pg1:restart(10)                                            -- Normal restart with 10s grace period
pg1:restart(5, "/usr/bin/valgrind", {"--leak-check=full"}) -- Restart under valgrind
```

The optional wrapper and wrapperArgs parameters allow running PostgreSQL through external tools like debuggers or profilers on restart.

!!! note

    The wrapper parameter doesn't take `$PATH` into account, it requires a full filename

### kill9

Kills the server without waiting for it to stop.

```lua
pg1:kill9()
```

### basebackup

Performs a base backup of the database to the specified directory.

The first parameter is an array of strings that gets added to the PostgreSQL `pg_basebackup` command. The `-h`, `-p` options are automatically added based on the server configuration.

```lua
-- Basic backup
pg1:basebackup({"-D", "backup_dir", "-U", "stormweaver", "-c", "fast"})

-- Incremental backup
pg1:basebackup({"-D", "backup_dir", "-U", "stormweaver", "-c", "fast", "-i", "prev_backup/backup_manifest"})
```

### createdb

Creates a database with the specified name.

```lua
pg1:createdb("foo")
```

### dropdb

Drops the database with the specified name.

```lua
pg1:dropdb("foo")
```

### createuser

Creates a user.

The first parameter is the name of the user, and the second parameter is an array of strings that gets added to the postgres `createuser` command.

```lua
pg1:createuser("stormweaver", {"-s"})
```

### is_running

Returns true if the server is running.

```lua
if pg1:is_running() then
 -- ...
end
```

### serverPort

Returns the port the server is using

```lua
p = pg1:serverPort()
```

### dataDir

Returns the data directory path as a string

```lua
path = pg1:dataDir()
```

### is_ready

Returns true if the server is ready to accept connections.

```lua
if pg1:is_ready then
  -- ...
end
```

### wait_ready

Waits up to the specified time in seconds, or until the server is ready.

```lua
if not pg1:wait_ready(20) then
  -- ...
end
```

### add_config

Appends the specified settings to the postgres configuration.

As Postgres only uses the last entry if the same setting is specified multiple times, this can also be used to modify existing settings.

```lua

pg1:add_config({
  logging_collector = "on",
  log_directory = "'logs'",
  log_filename = "'server.log'",
  log_min_messages = "'info'",
})
```

### add_hba

Adds an entry to the `pg_hba.conf`.

```lua
pg1:add_hba("host", "replication", "repuser", "127.0.0.1/32", "trust")
```

## BackgroundThread

Allows scripts to run something in the background.

For a more detailed example, see the section in the Lua Examples page.

### run

Starts a background thread, using the specified logfile name and lua function.

### join

Waits until the background thread completes.

### send

Sends the specified string to the background thread.

### receive

Receives a single string from the background thread - blocks and waits if it didn't send anything yet.

### receiveIfAny

Receives a single string from the background thread, if it sent anything.

## CommQueue

A communication queue for inter-thread message passing. Available in background thread contexts for communication with the main thread.

### send

```lua
-- In background thread
commQueue:send("message to main thread")
```

Sends a string message to the main thread.

### receive

```lua
-- In background thread
local message = commQueue:receive()
```

Receives a string message from the main thread. Blocks until a message is available.

### receiveIfAny

```lua
-- In background thread
local message = commQueue:receiveIfAny()
if message then
  -- Process message
end
```

Receives a string message from the main thread if one is available, otherwise returns nil.

## BackgroundProcess

A class for managing external processes in the background.

### start

Starts a new background process with the specified command and arguments.

```lua
-- Static method to start a new process
local proc = BackgroundProcess.start("logname", "/usr/bin/command", "arg1", "arg2")
```

**Parameters:**
- `logname` (string): Name for the log file (logs will be written to `logs/{logname}.log`)
- `command` (string): Full path to the executable to run
- `...` (variadic): Command line arguments to pass to the process

**Returns:**
- A BackgroundProcess object that can be used to control the process

### waitUntilExits

Waits for the process to complete and returns the exit status.

```lua
local success = proc:waitUntilExits()
if success then
  info("Process completed successfully")
else
  warning("Process failed")
end
```

**Returns:**
- `true` if the process exited with status 0, `false` otherwise

### kill

Sends a signal to the process to terminate it.

```lua
proc:kill(9)  -- Send SIGKILL
```

**Parameters:**
- `signal` (number): The signal number to send (e.g., 9 for SIGKILL, 15 for SIGTERM)

### running

Checks if the process is currently running.

```lua
if proc:running() then
  info("Process is still running")
else
  info("Process has finished")
end
```

**Returns:**
- `true` if the process is running, `false` if it has finished

### Example: Running External Commands

```lua
function run_pg_dump()
  local proc = BackgroundProcess.start("pg_dump", "/usr/bin/pg_dump", "-h", "localhost", "-p", "5432", "mydb")
  
  if proc:waitUntilExits() then
    info("Database dump completed successfully")
  else
    warning("Database dump failed")
  end
end

function run_with_timeout()
  local proc = BackgroundProcess.start("long_task", "/usr/bin/sleep", "30")
  
  -- Wait for 10 seconds
  local start_time = os.time()
  while proc:running() and (os.time() - start_time) < 10 do
    sleep(1000)  -- Check every second
  end
  
  if proc:running() then
    warning("Process taking too long, killing it")
    proc:kill(15)  -- Send SIGTERM
    sleep(2000)    -- Give it time to clean up
    if proc:running() then
      proc:kill(9)  -- Force kill if still running
    end
  end
end
```

## RandomWorker

Represents a randomized worker.
It supports the same functions as the `Worker`, with the following additions:

### possibleActions

Returns a reference to the action registry associated with the specfic worker.
This can be used to modify the actions executed by a single Worker, without affecting other Workers.

```lua
workload:worker(3):possibleActions():remove("alter_table")
```


## Worker

A (non randomized) worker that can be used for initialization and other database related things.

### create_random_tables

Creates a specified number of tables according to the DDL configuration.

Currently the DDL configuration can't be modified.

```lua
worker:create_random_tables(5)
```

### discover_existing_schema

Discovers and populates the metadata system with the existing database schema.

This function scans the connected database and builds StormWeaver's internal metadata representation based on the actual database structure (tables, columns, indexes, etc.).

```lua
worker:discover_existing_schema()
```

### reset_metadata

Clears all metadata, resetting it to an empty state.

This function removes all stored table and schema information from StormWeaver's internal metadata system.

```lua
worker:reset_metadata()
```

### validate_metadata

Validates that the current metadata accurately reflects the actual database schema.

This function compares the current metadata against a fresh schema discovery from the database. If validation fails, it writes debug files to the `logs/` directory containing both the original and newly discovered metadata for comparison.

Returns `true` if metadata matches the database schema, `false` otherwise.

```lua
if worker:validate_metadata() then
  info("Metadata is consistent with database schema")
else
  warning("Metadata validation failed - check logs/ directory for debug files")
end
```

### sql_connection

Returns the SQL connection (LoggedSQL) of the worker, which can be used to execute SQL statements directly.

```lua
sql = worker:sql_connection()
```

### calculate_database_checksums

Calculates SHA-256 checksums for all metadata tables in the database and writes the results to a specified file.

This function iterates through all tables tracked in the metadata system, calculates a checksum of their contents using SHA-256 hashing, and outputs the results in CSV format. The checksums are deterministic and will produce the same result for identical table contents.

```lua
worker:calculate_database_checksums("checksums.csv")
```

**Parameters:**
- `filename` (string): Path to the output file where checksums will be written

**Output Format:**
The output file contains CSV data with the following columns:
- `table_name`: Name of the table
- `checksum`: SHA-256 hash of the table contents (64-character hex string)
- `row_count`: Number of rows in the table

**Example output:**
```csv
table_name,checksum,row_count
users,a1b2c3d4e5f6789012345678901234567890abcdef1234567890abcdef123456,1500
products,e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855,0
orders,f7a8b9c0d1e2f3456789012345678901234567890abcdef1234567890abcdef12,2300
```

**Note:** Empty tables will have the SHA-256 hash of an empty string (`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`).

**Error Handling:**
- Throws a Lua error if the file cannot be opened for writing
- Throws a Lua error if a table query fails
- Throws a Lua error if row count retrieval fails

## Workload

Represents a testrun, it is created by `Node` using `initRandomWorkload`.

### run

Starts the workload.

The workload is executed in the background, this functions returns immediately after starting.
This allows the script to modify the workload, or interact with the SQL servers while the workload is running.

```lua
workload:run()
```

### wait_completion

Waits until the currently running workload is completed.

```lua
workload:wait_completion()
```

### worker

Returns the Worker with the specified index.

Indexing starts with 1.

```lua
workload:worker(3)
```

### worker_count

Returns the number of workers.

```lua
workload:worker_count()
```

### reconnect_workers

Reconnects any workers that lots their connection because of a restart.
