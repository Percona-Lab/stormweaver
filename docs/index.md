# Getting started

StormWeaver is a concurrent database testing tool, inspired by [PStress](https://github.com/Percona-QA/pstress).

It has two goals:

* Provide a framework for writing highly concurrent somewhat randomized tests, to stress the database server, and to try to uncover syncrhonization/locking/memory management/... issues
* Provide a framework for writing complex test scenarios in a user-friendly format (TODO)

Currently it works with PostgreSQL, and used for testing `pg_tde`.

## Installation

For a simple start, execute:

```
TODO
```

This will:

1. Clone the StormWeaver repository in a folder called `stormweaver`
2. Try to download the prebuilt binaries from github, if available, or report a failure.

Currently prebuilt binaries exists for Ubuntu 22.04 and Ubuntu 24.04.

Or alternatively it is possible to build StormWeaver from sources manually, for that see [Building from source](building.md).

## A first scenario

StormWeaver stores test scenarios in the `scenarios` folder, written in lua.
There is a scenario called `example` with lots of comments intended as a first example.
To run it, execute the following command:

```bash
cd stormweaver
# PGROOT is only needed when testing a specific PostgreSQL installation,
# otherwise uses binaries available in $PATH
export PGROOT="/path/to/the/pg/folder/"
bin/stormweaver scenarios/example.lua
```

This script will:

1. Setup a new data directory in `datadirs/datadir_pr/...`
2. Start up PostgreSQL with this data directory
3. Unless the `WITHOUT_TDE` environment varialbe is defined, it configures `pg_tde`
4. Starts up the server instance
5. Creates normal or encrypted tables based on the above setting
6. Runs a 20 second workload on the server repeating, repeating 10 times.
   Each time it randomly restarts the server, or sends a `kill -9` signal.

## What's next?

More details about the use of StormWeaver can be found in the following sections:

1. About a quick high level overview of how the project is constructed from a users perspective
2. About the configuration options supported by the default scripting framework
3. About what functions are avaialble in the scenario scripts
