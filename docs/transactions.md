# Transactions

The `transaction` action wraps several other actions in a single database
transaction, exercising commit/rollback interplay with the metadata catalog
and (on MySQL) the fact that DDL is not transactional.

## Overview

`transaction` is a built-in action, registered by default at weight `100`
alongside the other DDL/DML actions. Each run:

1. picks a random isolation level (see [Isolation weights](#isolation-weights)) and opens
   the transaction with the dialect's `BEGIN`/`SET TRANSACTION` statements,
2. picks a random count of sub-actions (`min_sub_actions`..`max_sub_actions`),
   drawing each one from the same pool used outside of transactions (minus
   `transaction` itself, to avoid nesting),
3. runs them one by one, honoring the configured [error mode](#error-modes),
4. finishes with `COMMIT` (probability `commit_prob`) or `ROLLBACK`.

Like any other action, it can be tuned or removed through the registry:

```python
registry = sw.default_action_registry()

# change how often it's picked relative to other actions
registry.get("transaction").weight = 300

# or drop it entirely
registry.remove("transaction")
```

There is no separate "frequency" config knob - the registry weight *is* the
frequency knob, exactly like for every other action.

## Metadata semantics

Sub-actions run against a buffered metadata context (`TxnBuffer`), not the
worker's shared catalog directly - the catalog must only ever reflect
durable state, never work that might still roll back. Deltas (table/index/
partition creation, drops, renames, ...) accumulate in the buffer as the
sub-actions execute.

* On `COMMIT` success, the whole buffer is published into the shared
  catalog in one shot.
* On `ROLLBACK` (chosen randomly, or forced by a sub-action failure in
  `abort` mode), the buffer is discarded - the catalog never saw it.

This is why the drift oracle (`validate_metadata` / checksum comparison)
needs no special-casing for transactions: whatever it can see in the
catalog is exactly what's durable in the database.

## Error modes

Controlled by `error_mode`:

* **`savepoint`** (default) - before each sub-action, the transaction
  issues `SAVEPOINT spN`. If the sub-action fails (a SQL error or an
  `ActionException`), the action rolls back to that savepoint and the
  buffered deltas for that sub-action are discarded - the transaction
  itself survives and the loop continues with the next sub-action.

  After a successful sub-action, there's an additional `rollback_to_savepoint_prob`
  percent chance of rewinding further: a random earlier savepoint is picked
  and the transaction rolls back to it (discarding every buffered delta
  since, and dropping the now-dead later savepoints), pstress-style. This
  exercises multi-step rollback within one still-open transaction.

* **`abort`** - the first sub-action failure rolls back the *entire*
  transaction (no savepoints are taken) and the action stops; nothing from
  that transaction is committed. This is the harsher, less realistic mode -
  useful for stressing rollback-of-everything paths without the
  savepoint machinery in between.

## Isolation weights

The isolation level for each transaction is a weighted random pick between
`server_default`, `read_committed`, `repeatable_read`, and `serializable`
(`isolation_weights.*`, all weights `1` by default: an even split with a
bias toward whatever the server defaults to). The dialect renders the
matching `BEGIN`/`SET TRANSACTION ISOLATION LEVEL` statements for the
connected server - weights are relative, not percentages, same as action
weights in the registry.

## MySQL divergence

PostgreSQL DDL is transactional: it rolls back like any other statement.
MySQL DDL is not - it implicitly commits whatever was open before it runs.
`mysql_ddl_mode` decides how the transaction action handles this on
MySQL-family servers (it's a no-op on PostgreSQL, where DDL is always
transactional):

* **`mirror`** (default) - DDL sub-actions stay in the pool, and the
  implicit commit is modeled truthfully: when a DDL sub-action actually
  sends a statement to the server (tracked via the connection's query
  counter, so an action that no-ops - e.g. it skipped because a limit was
  already hit - does *not* falsely end the transaction), the buffer
  publishes immediately, in-flight savepoints are dropped (they die with
  the implicit commit anyway), and every remaining sub-action for that
  transaction runs in autocommit instead. A DDL failure still counts as a
  failure, but the implicit commit already happened, so it does not roll
  back anything.

* **`exclude`** - DDL-typed actions are filtered out of the sub-action pool
  entirely, so a `transaction` only ever contains DML. This gives clean
  all-or-nothing transactional semantics at the cost of never exercising
  the DDL-inside-transaction interaction on MySQL.

## Python action rules

Because the transaction action owns `BEGIN`/`COMMIT`/`ROLLBACK`/`SAVEPOINT`
framing:

* Python actions (custom, registered via `@sw.action`/`register_python`)
  must never issue their own `BEGIN`, `COMMIT`, or `ROLLBACK` - doing so
  from inside a `transaction` sub-action would desynchronize the buffered
  metadata from the real connection state.
* If a Python action runs DDL, register it with `action_type="ddl"` so
  `mysql_ddl_mode="mirror"` can detect the implicit commit, and
  `mysql_ddl_mode="exclude"` can filter it out. See
  [Python actions](python-actions.md) for the registration signature.

## Config reference

See the `transaction.*` rows in [Config parameters](config-parameters.md#action-configuration)
for the full knob table.
