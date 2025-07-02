# Getting started

StormWeaver is a concurrent database testing tool, inspired by [PStress](https://github.com/Percona-QA/pstress).

It has two goals:

* Provide a framework for writing highly concurrent, somewhat randomized tests to stress the database server and uncover synchronization/locking/memory management issues
* Provide a framework for writing complex test scenarios in a user-friendly format (TODO)

Currently it works with PostgreSQL and is used for testing `pg_tde`.

## Installation

For now, there are no prebuilt packages. StormWeaver has to be built from source.

See [Building from source](building.md) for details.

## A first scenario

StormWeaver stores test scenarios in the `scenarios` folder, written in Lua.
There is a scenario called `basic` with lots of comments intended as a first example.
To run it, execute the following command:

```bash
cd stormweaver
bin/stormweaver scenarios/basic.lua [-c config/stormweaver.toml] [-i /path/to/the/pg/folder/] [scenario specific arguments...]
```

This script will:

1. Set up a new data directory in `datadirs/datadir_pr/...`
2. Start PostgreSQL with this data directory
3. Unless the `WITHOUT_TDE` environment variable is defined, configure `pg_tde`
4. Start the server instance
5. Create normal or encrypted tables based on the above setting
6. Run a 20-second workload on the server, repeating 10 times.
   Each time it randomly restarts the server or sends a `kill -9` signal.

## What's next?

More details about the use of StormWeaver can be found in the following sections:

1. A quick high-level overview of how the project is constructed from a user's perspective
2. The configuration options supported by the default scripting framework
3. What functions are available in the scenario scripts
