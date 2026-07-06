# Determinism

`scenarios/ci/determinism.py` is the living example and source of truth for what StormWeaver guarantees here; this page summarizes it.

## The seed model

`seed=` on `Workload` (or `WorkloadParams` for a standalone `Worker`) seeds a per-worker RNG stream. The same seed is passed to every worker; the C++ side derives a distinct stream per worker by mixing the seed with the worker's name (FNV-1a), so the derivation is stable across platforms and runs.

## What replays and what doesn't

**Single-worker workloads replay byte-identically.** With `workers=1`, the sequence of SQL statements a worker sends for a fixed seed is fully reproducible - verified with 500-800+ statements per run, zero divergence.

**Multi-worker workloads are only per-worker *decision* deterministic, not sequence-identical.** With `workers >= 2`, replay diverges within the first ~25-50 statements per worker, and it's a real logic-level divergence, not log noise. Root cause: `action::find_random_table()` (`core/src/action/helper.cpp`) draws `rand.random_number(0, metaCtx.size() - 1)` against the *shared* `Metadata` object, and DDL actions check `metaCtx.size()` against `max_table_count` before consuming further RNG draws. `metaCtx.size()` changes concurrently as other workers create/drop tables, so the same RNG draw can pick a different table (or consume a different number of draws) depending on cross-worker timing. That timing is real wall-clock thread scheduling and is not reproducible. Each worker's own RNG stream is still deterministic in isolation - what's not deterministic is how its draws interact with concurrent mutations to shared metadata.

Other things worth knowing when relying on determinism:

* The workload is duration-cut, not count-cut: two runs of the same seed legitimately stop at slightly different statement counts (wall-clock jitter).
* The server's own per-backend PRNG (used by SQL like `ORDER BY random() LIMIT n`) is separate from StormWeaver's seeded RNG and must be seeded independently (e.g. `SELECT setseed(0.42)` on connect) if you need row-selection to replay too.
* `autovacuum` runs on wall-clock timing and can shift row placement between runs; disable it if that would leak into `random()`-based row picks.

## Known limitation: metadata divergence under concurrent DDL

StormWeaver's in-memory `Metadata` tracks what it believes the schema looks like, but concurrent DDL from multiple workers can make it diverge from the database's actual schema - this is a known limitation, not a bug to chase down per-scenario. A metadata rework is planned to close this gap. Until then:

* `Worker.validate_metadata()` can fail on legitimate concurrent runs; scenarios should log a warning rather than fail hard on this (see `scenarios/ci/basic.py`).
* `.tsan-suppressions` carries suppressions for known `metadata::Metadata::Reservation` races/deadlocks, parked for the same rework.
* Prefer `workers=1` (or accept only per-worker-decision determinism) for anything that needs reproducible results today.
