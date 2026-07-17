# pstress -> StormWeaver gap analysis

Features present in pstress (the original inspiration) that StormWeaver currently lacks,
grouped by usefulness/priority. Snapshot as of the `newpython` branch with MySQL support
(commit `a442de0`).

## High priority -- core bug-finding value

- **Transactions + savepoints** -- pstress wraps statements in transactions
  (`--trx-prob-k`, `--trx-size`, `--commit-prob`, `--savepoint-prob-k`,
  `ROLLBACK TO SAVEPOINT`). StormWeaver runs pure autocommit; crash-recovery testing
  without in-flight/rolled-back transactions misses a whole bug class. Fits as a
  composite action plus worker-level transaction state.
- **SELECT workload** -- no built-in read action. pstress generates point lookups,
  range, `IN`, `BETWEEN`, `LIKE`, full scans, and partition-pruned selects. Reads are
  needed for MVCC/locking/index-corruption bugs. Cheap to add via the existing
  random-row machinery.
- **MySQL encryption/keyring testing** -- pstress's core Percona value:
  `ALTER TABLE/TABLESPACE/DATABASE ENCRYPTION`, master key rotation, Percona
  system-key rotation, `RELOAD KEYRING`, redo-log enable/disable, keyring
  file/Vault/KMIP setup. StormWeaver MySQL has zero encryption support (PG has pg_tde,
  but no key-rotation action either -- same shape of gap).
- **ALTER ALGORITHM/LOCK variants** -- pstress cycles INPLACE/COPY/INSTANT x
  NONE/SHARED/EXCLUSIVE with a compatibility matrix. Historically a rich MySQL bug
  source. Natural dialect-layer addition.
- **Crash-loop harness features** -- the pstress driver script provides: signal choice
  (SIGKILL/SIGTERM/SIGILL-core), trial saving (datadir + logs + binary for repro),
  error-log scanning with known-bug exclude patterns, core-file detection, datadir
  reinit on failed start. StormWeaver has kill/restart primitives but no unattended
  trial loop with triage. Scenario-level Python work, not core.
- **Richer column/DDL variety** -- generated (stored/virtual) column action (metadata
  scaffolding already exists), rename column/index, real MODIFY COLUMN variety
  (StormWeaver: numeric->VARCHAR(32) only), NOT NULL/defaults, FLOAT/DOUBLE, blob
  subtypes.

## Medium priority -- useful coverage

- **Temporary tables** -- `Table::Type::temporary` enum exists, no action. Needs
  per-worker (per-session) metadata visibility.
- **HASH/LIST/KEY partitioning + partition ops** -- StormWeaver is RANGE-only; pstress
  also does REORGANIZE, COALESCE, TRUNCATE PARTITION, drop/add LIST values. Discovery
  enums already recognize hash/list.
- **Tablespaces** -- general + undo tablespaces, ALTER TABLESPACE rename/encryption,
  DISCARD TABLESPACE, per-table assignment, KEY_BLOCK_SIZE/ROW_FORMAT/compression.
  Mostly MySQL-side; metadata field exists but no action touches it.
- **OPTIMIZE/ANALYZE/CHECK TABLE actions** -- the demo scenario has ad-hoc custom SQL
  for vacuum/reindex; pstress has these as weighted built-ins plus a pre-workload
  CHECK sweep (`--check-preload`) with failure counting. CHECK TABLE post-crash
  complements checksums.
- **Runtime server-variable fuzzing** -- `SET GLOBAL/SESSION` from a weighted option
  pool during load (`--mso`/`--sof`), plus random mysqld startup options per trial.
  Strong bug-finder; easy as a Python action + backend config hook.
  Implemented: `--var-fuzz {safe,semantics,disruptive}` + `--seed`, session/global/
  reload toggle actions, and seed-derived startup option rolls. See
  [variable fuzzing](variable-fuzzing.md).
- **FK variety** -- pstress randomizes ON UPDATE/ON DELETE
  (RESTRICT/CASCADE/SET NULL/NO ACTION); StormWeaver hardcodes ON DELETE CASCADE,
  single FK per table.
- **Grammar/template SQL** -- pstress grammar-file with `T1`/`T1_INT_1`-style
  placeholders including multi-table joins; StormWeaver's `make_custom_table_sql`
  supports only `{table}`. Extend substitution to columns + multiple tables.
- **SQL replay/reduction mode** -- pquery mode (execute SQL file sequential/shuffled)
  plus the replay_test.sh repro workflow. StormWeaver already logs every statement per
  connection -- replay tooling is the missing half of a repro story.
- **Statement-count budget** -- pstress `--queries-per-thread`; StormWeaver only has
  wall-clock duration. Trivial, helps determinism.

## Low priority -- probably out of scope / defer

- **PXC / Group Replication cluster orchestration** -- 3-node bootstrap, SST methods,
  wsrep option fuzzing, GCache key rotation. Big; only if cluster testing becomes a
  goal.
- **RocksDB / engine variety** -- engine option pool, per-engine feature gating.
  `changeAccessMethod` groundwork exists.
- **rr/gdb server wrapping** -- trivial to do in the Python backend when needed.
- **REPLACE + bulk multi-row insert action** -- minor DML variants.
- **Version/fork gating** -- pstress gates features by server version/fork;
  StormWeaver's `ServerInfo` predicates are wired but the PG side is stubbed (flavor
  hardcoded, version 0). More debt than feature; prerequisite for the encryption work
  above.
- **Metadata persistence between runs** -- pstress serializes step JSON; StormWeaver
  rebuilds via schema discovery -- a better design, no need to copy. But discovery gaps
  worth fixing: UNIQUE/CHECK constraints discovered then dropped, engine not stored
  back, MySQL tablespace always empty.

## Not gaps

StormWeaver already beats pstress on validation (row checksums, entropy verification,
discovery-based metadata compare -- pstress has none of these internally), statistics,
seed determinism, and scenario flexibility.
