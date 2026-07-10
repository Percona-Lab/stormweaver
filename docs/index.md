# Getting started

StormWeaver is a concurrent database testing tool, inspired by [PStress](https://github.com/Percona-QA/pstress).

It has two goals:

* Randomized, highly concurrent stress testing to uncover synchronization/locking/memory management issues in the database server
* Stable, deterministic product tests you can run in CI (a pytest plugin, see [Stable tests](stable-tests.md))

The core engine (metadata, actions, workers, SQL) is C++23, driven from Python 3.14t (free-threaded) scenarios through nanobind bindings. PostgreSQL is supported today, MySQL support is planned next.

## Installation

Installing builds the C++ extension, so the first time on a machine you need a [Conan 2](https://docs.conan.io/2/installation.html) profile: `conan profile detect` - see [Conan profile](building.md#conan-profile). Then:

```bash
uv python install 3.14t
uv venv
uv pip install -e . --group dev
```

Or simply `task setup`. See [Building from source](building.md) for details, the pure C++ (no Python) build, and sanitizer presets.

## A first scenario

Scenarios are Python files with a `main(args)` function. `scenarios/ci/basic.py` is a small, commented example: it starts PostgreSQL, creates some tables, runs a randomized workload, and validates the metadata. Run it with:

```bash
uv run stormweaver scenarios/ci/basic.py -i /path/to/postgres/install
```

This will:

1. Load `config/stormweaver.toml` (or the file passed via `-c`) and create a fresh data directory under `datadirs/`
2. Start PostgreSQL and create a test database
3. Create a handful of random tables
4. Run a 4-worker, 30-second randomized workload, twice
5. Write per-worker statistics as CSV files into the run's log directory
6. Validate the in-memory metadata against the live schema (see the known limitation in [Determinism](determinism.md))

## What's next?

* [Writing scenarios](writing-scenarios.md) - scenario anatomy and the `sw` API
* [Python actions](python-actions.md) - registering custom actions
* [Stable tests](stable-tests.md) - the pytest plugin for deterministic CI tests
* [Determinism](determinism.md) - what replays and what doesn't
* [Randomized testing concepts](randomized-testing-concepts.md) - Workload/Worker/Action/ActionRegistry
* [Config parameters](config-parameters.md) - the TOML config file and CLI flags
