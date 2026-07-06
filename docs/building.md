# Building from source

## Python package (the normal path)

StormWeaver ships as a Python extension module built with `scikit-build-core` + `nanobind`, using Conan 2 to fetch C++ dependencies. Everything needed is driven through `uv`:

```bash
uv python install 3.14t
uv venv
uv pip install -e . --group dev
```

`uv pip install -e .` invokes CMake/Conan under the hood and rebuilds the extension whenever sources change. `task setup` runs the same steps plus `pre-commit install`. `task build` re-runs just the editable install.

### Conan profile

The first build needs a Conan profile:

```bash
conan profile detect --exist-ok
```

StormWeaver requires C++23, and gcc >= 15 defaults its C compiler to C23 (which breaks libpq's `typedef bool`). Both are handled by `conan/host.profile`, checked into the repo and wired in automatically by `cmake/conan_setup.cmake` - no manual profile editing needed, on any machine.

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
| `task setup` | uv python install + venv + editable install + pre-commit hooks |
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
