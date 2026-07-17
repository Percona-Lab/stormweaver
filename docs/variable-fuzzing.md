# Variable fuzzing

Randomizes server configuration variables during a workload: session-level
`SET`, MySQL `SET GLOBAL`, PostgreSQL `ALTER SYSTEM` + `pg_reload_conf()`, and
seed-derived startup option rolls baked into the config file before the
server ever starts.

## Quick start

Add `--var-fuzz safe --seed 42` to any scenario built on `scenario.single_pg`
or `scenario.single_mysql` (`basic.py` already defaults to `--var-fuzz safe`;
the flag is spelled out below for illustration, and `--seed` is what makes
the startup rolls reproducible):

```
uv run stormweaver scenarios/ci/basic.py -i "$STORMWEAVER_PG_DIR" \
    --var-fuzz safe --seed 42
```

`--seed` is optional - without it (or with `--seed 0`) startup rolls and
worker RNG streams are drawn from real entropy and are not reproducible
between runs.

`single_pg`/`single_mysql` roll a startup variable set before `start()` and
call `ctx.use_variables()` for you, so nothing else changes in the scenario
script. What to look for:

* one `STARTUP_VARIABLE name=... value=...` event line per rolled startup
  option, emitted before the server starts (`stormweaver.events` format, see
  [Writing scenarios](writing-scenarios.md#unified-logging)).
* `SET ...` / `SET GLOBAL ...` / `ALTER SYSTEM SET ...` + `SELECT
  pg_reload_conf()` statements interleaved in the per-connection SQL logs
  (`logs/sql-conn-*.log`), same as any other action.
* `set_session_variable`, `set_global_variable`, `reload_global_variable`
  showing up in the per-worker action stats once their weight is non-zero.

`--var-fuzz off` (the default when a scenario does not override it) disables
all of this; `scenarios/ci/basic.py` and `scenarios/ci/basic_mysql.py` set
the default to `safe`.

## Mechanisms

| mechanism | what it does | built-in action | servers |
|---|---|---|---|
| session | `SET x = v` (PG) / `SET SESSION x = v` (MySQL), scoped to the current connection | `set_session_variable` | both |
| global | `SET GLOBAL x = v`, visible to new connections process-wide | `set_global_variable` | MySQL only |
| reload | `ALTER SYSTEM SET x = v` then `SELECT pg_reload_conf()` - PostgreSQL's sighup-class parameters | `reload_global_variable` | PostgreSQL only |
| startup | baked into the config file before the server starts, needs a restart to change | none (rolled once via `roll_startup`) | both |

The three toggle actions are registered by `default_action_registry()` at
weight 0 - dormant until a scenario supplies a variable pool and raises their
weight (`ctx.use_variables()` / `Workload.action_config.variables`, see
below). A variable only takes part in a given mechanism if its spec lists
that mechanism; `set_global_variable` and `reload_global_variable` simply
no-op (return success, no SQL sent) when nothing eligible is configured for
the connected server flavor.

## Tiers and presets

Each `VariableSpec` carries a `Tier`:

* **safe** - session-tuning and sighup-class knobs (`work_mem`,
  `checkpoint_timeout`, `enable_seqscan`, ...). Changing them should not
  change query results or crash-recovery behavior, only performance and
  plan shape.
* **semantics** - changes that can legitimately change what a statement
  returns or whether it errors (`default_transaction_isolation`,
  `statement_timeout`, `plan_cache_mode`, MySQL `sql_mode`, ...). Enabling
  this tier means workloads should expect extra SQL errors that are not
  bugs - a statement that times out or gets rejected under
  `read_only`-adjacent settings is the fuzzing working as intended, not a
  regression.
* **disruptive** - changes that can affect durability or crash-safety
  (`fsync`, `full_page_writes`, `synchronous_commit`, MySQL
  `innodb_flush_log_at_trx_commit`, `sync_binlog`, `read_only`, ...).
  Enabling this tier means a crash-recovery checksum/validation step can
  legitimately fail (data genuinely lost because durability was turned off)
  - that is an expected side effect of the tier, not proof of a bug, though
  it is worth confirming the loss lines up with the toggled setting.

`variables.preset(flavor, max_tier=Tier.safe)` returns the curated
`PG_VARIABLES`/`MYSQL_VARIABLES` catalog filtered to `tier <= max_tier`.
`--var-fuzz` maps directly to `max_tier` (`off` skips fuzzing entirely).

## Programmatic API

```python
from stormweaver import variables

pool = variables.preset("postgres", max_tier=variables.Tier.semantics)
```

`VariablePool` is composed with plain methods, each returning a new pool:

* `pool.filter(flavor=, tier=, max_tier=, mechanism=)` - narrow down.
* `pool.extend([...])` - add custom `VariableSpec`s.
* `pool.remove(name)` - drop one variable by name (raises if absent).
* `pool_a + pool_b` - concatenate (duplicate `(flavor, name)` pairs raise).

Build custom specs the same way the built-in catalog does:

```python
from stormweaver.variables import VariableSpec, Choices, IntRange, Bool, Tier

my_spec = VariableSpec(
    name="my_setting",
    flavor="postgres",
    generator=Choices(("'1MB'", "'64MB'")),
    mechanisms=frozenset({"session", "startup"}),
    tier=Tier.safe,
    weight=10,
)
pool = pool.extend([my_spec])
```

Attach a pool to a scenario context:

```python
with scenario.single_pg(opts) as ctx:
    ctx.use_variables(pool, session_weight=5, global_weight=5, reload_weight=5)
    ctx.workload.run()
```

The weight arguments set the registry weight for the three toggle actions,
they default to 5 each. That is deliberately low next to the ~7000 combined
weight of the default DDL/DML action set, so toggles are rare unless you ask
for more - raise `session_weight`/`global_weight`/`reload_weight` (or set
`registry.get("set_session_variable").weight = ...` directly) for a run that
should hammer on variable changes specifically.

`single_pg`/`single_mysql` call `ctx.use_variables()` for you whenever
`opts.var_fuzz != "off"`; call it yourself only when building a context by
hand or when overriding the pool the CLI flag would have picked.

### Startup rolls

Startup-only values are not part of the runtime action pool - they are
rolled once, up front, and written into the config file:

```python
pool = variables.preset("postgres", max_tier=variables.Tier.safe)
rolled = pool.roll_startup(variables.startup_rng(opts.seed))
# rolled: {"shared_buffers": "256MB", "wal_buffers": "1MB", ...}
```

`variables.startup_rng(seed, run=0)` derives a dedicated RNG stream from the
workload seed (same FNV-1a derivation as per-worker streams, see
[Determinism](determinism.md)); `seed=0` falls back to real entropy, same
convention as workers. Pass `rolled` through `extra_config=` on
`single_pg`/`single_mysql`, or call `node.add_config(rolled)` and restart if
you need to re-roll mid-scenario - re-rolling is an explicit verb a scenario
calls, not something that happens automatically on restart.

## Caveats

**`ALTER SYSTEM` persists.** Every `reload_global_variable` toggle that
lands writes to `postgresql.auto.conf`, which survives a normal restart.
`Postgres.reset_auto_conf()` deletes that file, but only do it while the
server is stopped - it does not touch the running postmaster.

**Cross-worker interleaving is not reproducible.** Each worker's own
decision to toggle a variable (and which value it picks) is seed-
deterministic, same as any other action. But a `global`/`reload` toggle from
one worker is visible to every other connection at a wall-clock-dependent
moment, so the effect on other workers' statement outcomes is not
reproducible run to run - the same caveat that already applies to
shared-catalog DDL timing under concurrent workers, see
[Determinism](determinism.md#what-replays-and-what-doesnt).
`scenarios/ci/determinism.py` sidesteps this by enabling only
`set_session_variable` (session mechanism, safe tier) over its comparison
run: safe-tier session values do not change statement outcomes, so the
recorded SQL sequence stays comparable.

**One process, multiple nodes, same seed: identical startup rolls.**
`startup_rng(seed, run)` takes a `run` parameter meant to differentiate
multiple nodes rolled in the same process (e.g. primary + replica), but
nothing in the scenario helpers passes a non-zero `run` yet - two
`single_pg`/`single_mysql` contexts opened with the same seed in one process
currently roll the same startup values. Pass `run` explicitly if you need
them to differ.

**Startup rolls are not version- or build-gated beyond `min_version`/
`max_version`.** Those gates check the server's reported version, not what
the specific binary was built with. `wal_compression`'s `lz4`/`zstd` choices
are gated to `>= 15` but still assume the binary was built with those
codecs; a codec-less build will fail to start with those values rolled. If
you are testing an exotic build, filter the pool (`pool.filter(...)` /
`pool.remove(...)`) before rolling rather than relying on the version gate
alone.

## Adding catalog entries

Each catalog entry is one `_pg(...)` (postgres) or `_my(...)` (mysql) call
in `python/stormweaver/variables.py`:

```python
_pg(
    name,
    generator,        # Choices(...) | IntRange(min, max, step=, suffix=) | Bool()
    mechanisms,        # subset of {"session", "global", "reload", "startup"}
    tier=Tier.safe,    # safe | semantics | disruptive
    weight=10,         # relative pick weight within its mechanism
    min_version=0,
    max_version=0,
)
```

* `mechanisms` for `_my(...)` cannot include `"reload"` (PostgreSQL-only) and
  `_pg(...)` cannot include `"global"` (MySQL-only) - the mechanism filter in
  `variable_detail::eligible` also checks the connected server flavor, but
  putting an impossible mechanism on a spec is just dead weight.
* **Quoting**: PostgreSQL string/size values carry their SQL quotes directly
  in the `Choices` tuple (e.g. `"'64MB'"`, `"'read committed'"`) because the
  runtime path sends them straight into a `SET`/`ALTER SYSTEM` statement.
  `render_value(..., startup=True)` strips a matching pair of leading/
  trailing quotes automatically, since `add_config`'s config-file writer
  quotes its own values (`k = 'v'`) - do not pre-strip quotes yourself for
  the startup path. MySQL values are quoted only where the SQL grammar
  needs it (e.g. `'index_merge=off'` for `optimizer_switch`, bare `ON`/`OFF`
  elsewhere).
* **Version gates**: PostgreSQL uses `server_version_num` style integers
  (`150000` for 15.0, `140002` for 14.2). MySQL uses `X*10000 + Y*100 + Z`
  (`80030` for 8.0.30). Leave `0` (the default) for no gate on that side.
* Put the new entry in the `# safe` / `# semantics` / `# disruptive` block
  matching its tier - the catalogs are grouped that way for readability, not
  enforced by code.
