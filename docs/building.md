# Building from source

## Prerequisites

The C++ libraries StormWeaver compiles in-tree are git submodules under `third_party/`:

```bash
git clone --recursive https://github.com/Percona-Lab/stormweaver.git
# or, in an existing checkout
git submodule update --init --recursive
```

Two client libraries come from your system and are picked through their config tools:

| Library | Found through | Override |
| --- | --- | --- |
| libpq | `pg_config` on `PATH` (or `pkg-config libpq`) | `-DPG_CONFIG=/path/to/pg_config` |
| MySQL client (MariaDB Connector/C or libmysqlclient) | `mariadb_config` / `mysql_config` on `PATH` | `-DMYSQL_CONFIG=/path/to/mariadb_config` |

Crypto++ also comes from the system (`libcrypto++-dev`, `cryptopp-devel`, `libcryptopp-devel`).

Testing a libpq change from a PostgreSQL work tree is just pointing at that tree's install:

```bash
CMAKE_ARGS="-DPG_CONFIG=$HOME/pginst/bin/pg_config" uv pip install -e .
```

## Python package (the normal path)

```bash
uv python install 3.14t
uv venv
uv pip install -e . --group dev
```

`uv pip install -e .` drives CMake through `scikit-build-core` and rebuilds the extension whenever sources change. `task setup` runs the same steps plus the submodule init and `pre-commit install`. Extra CMake options go through `CMAKE_ARGS`.

## Plain CMake install (what the distro packages do)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPython_EXECUTABLE=/opt/percona-python3.14t/bin/python3.14t \
  -DSTORMWEAVER_PYTHON_INSTALL_DIR=/opt/percona-python3.14t/lib/python3.14t/site-packages
cmake --build build
cmake --install build
```

This installs the extension, the Python package and a `dist-info` into that site-packages, no pip involved.

## Pure C++ builds (no Python)

For core development (no Python bindings), use the CMake presets in `CMakePresets.json` directly:

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

or via Taskfile: `task cpp:build` / `task cpp:test` (set `PRESET=asan-ubsan` or `PRESET=tsan` to switch presets).

Available presets: `debug`, `asan-ubsan` (address + undefined behavior sanitizers), `tsan` (thread sanitizer). These build with `WITH_PYTHON=OFF`, so `bindings/module.cpp` is not part of this build - use the `uv pip install -e .` path to build the Python module, optionally with `CMAKE_ARGS="-DWITH_ASAN=ON -DWITH_UBSAN=ON"` for a sanitized extension build.

## Taskfile targets

| Task | What it does |
| --- | --- |
| `task setup` | submodule init + uv python install + venv + editable install + pre-commit hooks |
| `task build` | Rebuild the extension + package |
| `task cpp:build` | Pure C++ build (`PRESET=debug\|asan-ubsan\|tsan`) |
| `task cpp:test` | ctest for the pure C++ build |
| `task test:py` | Python unit tests (`pytest tests/unit`) |
| `task test:scenario:basic` | Runs `scenarios/ci/basic.py` end to end (needs `PG_DIR`) |
| `task test` | cpp:test + test:py + test:scenario:basic |
| `task fmt` | Autofix formatting (clang-format, ruff format + fix) |
| `task fmt:check` | Check-only formatting |
| `task tidy` | clang-tidy over `core/src` |
| `task lint` | fmt:check + mypy |
| `task clean` | Remove `build/` and `dist/` |

Scenario tasks need `PG_DIR` — a PostgreSQL installation containing `bin/initdb`. Pass it per invocation (`task test:scenario:basic PG_DIR=/usr/lib/postgresql/17`) or set it once in a gitignored `.env` file at the repo root (`PG_DIR=/usr/lib/postgresql/17`).

## Quality gates

`task fmt`, `task lint`, `task tidy`, and `task test` are also what CI runs: a lint job, a debug/asan-ubsan/tsan matrix, the basic scenario, the asan scenario, and the determinism check (`scenarios/ci/determinism.py`). Pre-commit hooks run a subset of the same checks locally on commit.
