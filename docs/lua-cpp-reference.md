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
pg = initPostgresDatadir('pg/intsall/dir', 'new/data/dir')
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

Returns a the value of an environment variable, or a default.

```lua
e = getenv("NAME", "default")
```

### setup_node_pg

Configures the connection parameters to a postgres installation.

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
if ar:has("alter_table) then
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
ar1::use(ar2)
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
fs.delete_directory('from', 'to')
```

### delete_directory

Deletes the specified directory

```lua
fs.delete_directory('foo/bar')
```

## LoggedSQL

A class representing a database connection

### execute_query

```lua
conn.execute_query("CREATE EXTENSION pg_tde;")
```

A simple funtion that execute a single SQL query.
Result sets are not yet implemented in lua.

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

Currently supports the following parameters:

* `run_seconds`: how long to run the workload
* `worker_count`: how many workers to use in the workload

```lua
node:initRandomWorkload({ run_seconds = 10, worker_count = 5 })
```

### possibleActions

Returns a reference to the action registry associated with the node.
This is a copy made at the time of creating the node, and can be modified separately.
Will be copied into the workload when one is created using `initRandomWorkload`.

```lua
a = node:possibleActions()
```



## Postgres

A class used to manage a Postgres installation/datadir.

An instance of this class can be created with the global functions `initPostgresDatadir` and `initBasebackupFrom`.

### start

Starts the server, returns true if succeeds.

```lua
pg1:start()
```

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
pg1:restart(10)
```

### kill9

Kills the server without waiting for it to stop.

```lua
pg1:kill9()
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
pg1:createuser("stormweaver", {"-s"}
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

### generate_initial_data

Generates some rows in all currently existing tables.

```lua
worker:generate_initial_data()
```

### sql_connection

Returns the SQL connection (LoggedSQL) of the worker, which can be used to execute SQL statements directly.

```lua
sql = worker:sql_connection()
```

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
workload:wait_completin()
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