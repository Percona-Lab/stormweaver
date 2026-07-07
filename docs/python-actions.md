# Python actions

Custom workload actions are plain Python callables, registered into an `ActionRegistry` with the `@sw.action` decorator:

```python
import stormweaver as sw

registry = sw.default_action_registry()

@sw.action(registry, "my_custom_action", weight=10)
def my_action(metadata, rand, connection):
    table = rand.random_number(0, metadata.size() - 1)
    connection.execute(f"SELECT 1")
```

`weight` determines the action's chance of being picked (see [Randomized testing concepts](randomized-testing-concepts.md)).

## The `fn` contract

`fn(metadata, rand, connection)`:

* `metadata` - the shared `Metadata` for the workload
* `rand` - the worker's seeded `Random` (`ps_random`) instance
* `connection` - the worker's `LoggedSQL` connection

From `stormweaver/actions.py`:

> The objects passed to the action are borrowed references owned by the worker, valid only during the workload run - do not stash them. Statistics accessors become valid only after the workload completes (post-join).

Concretely:

* Don't keep a reference to `metadata`, `rand`, or `connection` past the call - they're bound to the worker's C++-side lifetime, not Python's.
* Don't read `WorkerStatistics` while a workload is still running; call `workload.run()` to completion (it blocks until all workers join) before reading `workload.worker_statistics()` or `print_report()`.
* Exceptions raised in `fn` are caught on the C++ side and reported as an `ActionException` named `"python_action"` - they don't crash the worker thread.
* Worker threads are plain `std::thread`s, not attached to the Python interpreter; the C++ side acquires the GIL for the duration of each call to `fn`, so ordinary Python code is safe to write. `connection.execute()` runs with the GIL held (no release guard), so concurrent Python actions across workers serialize on both the GIL and the query round-trip - this is fine since PostgreSQL work is the bottleneck, not Python.

## Registering vs configuring

`registry.register_python(name, weight, fn, action_type)` is what `@sw.action` calls under the hood. Use `registry.has(name)` / `registry.remove(name)` to trim built-in actions (e.g. dropping partition actions for a simpler schema), and `registry.get(name)` to inspect/tweak weight after registration.

`action_type` (`"ddl"`, `"dml"`, or `"other"`, default `"other"`) tags what kind of SQL the action runs. It's not just bookkeeping: the [`transaction`](transactions.md) action uses it to decide which sub-actions to pick, and on MySQL, to detect an implicit commit caused by DDL. If your custom action runs `CREATE`/`DROP`/`ALTER` (or anything else non-transactional on MySQL), register it with `action_type="ddl"` - otherwise `mysql_ddl_mode="mirror"` won't know to treat it as a potential implicit-commit point, and `mysql_ddl_mode="exclude"` won't filter it out of transaction pools.

```python
@sw.action(registry, "my_ddl_action", weight=10, action_type="ddl")
def my_ddl_action(metadata, rand, connection):
    connection.execute("ALTER TABLE ...")
```

## Rules for actions run inside a transaction

Custom actions can be picked as sub-actions of the built-in `transaction`
action (see [Transactions](transactions.md)) the same way as any other
registered action. Because `transaction` owns the transaction's framing:

* **Never issue your own `BEGIN`, `COMMIT`, `ROLLBACK`, or `SAVEPOINT`** from
  inside an action's `fn`. `transaction` already manages these around and
  between sub-actions; a stray one from Python would desynchronize the
  buffered metadata from the connection's real transaction state.
* Tag any action that runs DDL with `action_type="ddl"` as described above,
  so MySQL's implicit-commit handling works correctly.
