## StormWeaver

StormWeaver is a concurrent database testing tool inspired by [PStress](https://github.com/Percona-QA/pstress). It has two jobs: randomized concurrent stress testing to shake out synchronization/locking/memory bugs in the server, and stable, deterministic product tests you can run in CI. The core is C++23 (metadata/actions/workers/SQL), driven from free-threaded Python 3.14t scenarios via nanobind. PostgreSQL and MySQL (community, Percona Server, PXC) are supported.

### Quickstart

Installing builds the C++ extension; libpq and a MySQL client library must be installed (details: [building docs](docs/building.md#prerequisites)). Then:

```bash
uv python install 3.14t
uv venv
uv pip install -e . --group dev

uv run stormweaver scenarios/ci/basic.py -i /path/to/postgres/install
```

### Dev setup

```bash
task setup   # uv python install + venv + editable install + pre-commit hooks
task test    # C++ tests, python unit tests, and the basic scenario
```

See `task --list` for all targets (fmt, lint, tidy, cpp:build, ...).

### Docs

Full documentation, including scenario writing, python actions, stable tests, and determinism guarantees, is at https://percona-lab.github.io/stormweaver/ (or browse `docs/`).
