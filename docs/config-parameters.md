# Configuration parameters

StormWeaver reads a small TOML config file plus a handful of CLI flags. Scenario code (Python, not a config layer) decides how to combine them - `scenarios/ci/basic.py` uses `args.install_dir or config.pgroot`, giving the CLI flag precedence; your own scenarios can do whatever makes sense for them.

## TOML config file

Default path `config/stormweaver.toml`, overridable with `-c/--config`. Parsed by `stormweaver.Config` (`python/stormweaver/config.py`), which only reads a `[default]` table:

```toml
[default]
pgroot = ""                # PostgreSQL installation directory
datadir_root = "datadirs"  # root directory for generated PostgreSQL data directories
port_start = 15432         # first port a scenario should use
port_end = 15531           # last port a scenario should use
```

`Config.datadir(name)` returns `<datadir_root>/datadir_<name>`. Unknown keys are ignored; there is no environment-variable overlay or `--include` search path in the current config loader - a scenario is free to add its own argparse flags or env var reads.

## CLI arguments

`stormweaver <scenario> [options] `, defined in `python/stormweaver/cli.py`:

| Flag | Purpose | Default |
| --- | --- | --- |
| `scenario` (positional) | Path to the scenario `.py` file | required |
| `-c, --config` | Path to the TOML config file | `config/stormweaver.toml` |
| `-i, --install-dir` | PostgreSQL installation directory, passed to `args.install_dir` | `""` |
| `-v, --verbose` | Debug-level logging | off |
| `-q, --quiet` | Warning-level logging only | off |

`-v` and `-q` are mutually exclusive; the default log level is `INFO`.

## PostgreSQL defaults

`sw.Postgres` runs `initdb` with no extra options, then appends to `postgresql.conf`: the chosen `port`, and `unix_socket_directories` pointed at the data directory (to avoid needing `/run/postgresql`). Everything else is PostgreSQL's own `initdb` defaults - add whatever else you need with `pg.add_config({...})`:

```python
pg.add_config({
    "max_connections": "200",
    "shared_buffers": "256MB",
})
```

## Action configuration

Actions read their knobs from `sw.AllConfig` (`WorkloadParams.action_config`),
not from the TOML file - see [Writing scenarios](writing-scenarios.md) for how
a scenario builds one. The `transaction` action (see
[Transactions](transactions.md)) reads `AllConfig.transaction`
(`TransactionConfig`):

| Knob | Default | Meaning |
| --- | --- | --- |
| `transaction.min_sub_actions` | 2 | Minimum number of sub-actions run inside one transaction |
| `transaction.max_sub_actions` | 10 | Maximum number of sub-actions run inside one transaction |
| `transaction.commit_prob` | 95 | Percent chance the transaction ends with `COMMIT` instead of `ROLLBACK` |
| `transaction.rollback_to_savepoint_prob` | 10 | Percent chance, per successful sub-action in `savepoint` mode, of rewinding to a random earlier savepoint |
| `transaction.error_mode` | `"savepoint"` | `"savepoint"` rolls back to a per-sub-action savepoint on failure; `"abort"` rolls back the whole transaction on the first failure |
| `transaction.mysql_ddl_mode` | `"mirror"` | `"mirror"` keeps DDL in the pool and models MySQL's implicit commit truthfully; `"exclude"` filters DDL out so MySQL transactions stay DML-only |
| `transaction.isolation_weights.server_default` | 1 | Relative weight for leaving the isolation level at the server default |
| `transaction.isolation_weights.read_committed` | 1 | Relative weight for `READ COMMITTED` |
| `transaction.isolation_weights.repeatable_read` | 1 | Relative weight for `REPEATABLE READ` |
| `transaction.isolation_weights.serializable` | 1 | Relative weight for `SERIALIZABLE` |

The `transaction` action's own selection frequency is not a config field -
it's the registry weight, same as any other action: `registry.get("transaction").weight`
to tune it, `registry.remove("transaction")` to disable it.

## Build-time configuration

See [Building from source](building.md) for CMake presets (`debug`, `asan-ubsan`, `tsan`) and the Conan `cppstd=gnu23` profile requirement.
