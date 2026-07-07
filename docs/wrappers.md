# Server wrappers

StormWeaver can run the database servers it manages under a wrapper tool.
This is mainly for crash debugging: record short sessions under
[rr](https://rr-project.org/) and replay the ones that crashed.

## Presets

    stormweaver scenarios/ci/basic.py -i $PG_DIR --wrapper rr
    stormweaver scenarios/ci/basic.py -i $PG_DIR --wrapper valgrind

Extra tool arguments and trace retention:

    --wrapper rr --wrapper-arg=--chaos   # rr chaos mode
    --keep-traces                        # keep traces of clean sessions too

rr traces are written to `logs/<run>/rr/<node>-s<NN>`, one per server
session. Traces of sessions that shut down cleanly are deleted unless
`--keep-traces` is given; traces of crashed sessions are always kept.
Valgrind logs go to `logs/<run>/valgrind-<node>-s<NN>-<pid>.log` and are
always kept. Readiness and shutdown timeouts are scaled automatically
(2x under rr, 20x under valgrind).

## Generic wrapper

Any tool that works as a command prefix:

    --wrapper-cmd "strace -f"

`--wrapper-arg` is rejected with `--wrapper-cmd`; put tool arguments
inside the quoted command instead, as above.

For programmatic use, pass a wrapper object to a context directly:

```python
from stormweaver.wrappers import ExecPrefixWrapper

with scenario.single_pg(opts, wrapper=ExecPrefixWrapper(["numactl", "-N", "0"])) as ctx:
    ...
```

Custom wrappers subclass `ServerWrapper`; override `wrap_command` for
prefix-style tools, or `spawn` to change how the process is launched
entirely (e.g. a tmux window running the server inside gdb).

## Run outcomes

Every server session end is recorded in `logs/<run>/outcome`:

    node=primary session=3 result=crashed exit=SIGSEGV
    scenario result=failed

`result` is `clean` (requested stop, exit 0), `killed` (scenario called
kill), or `crashed` (anything else). To find crashed runs in a batch:

    grep -l crashed logs/*/outcome
