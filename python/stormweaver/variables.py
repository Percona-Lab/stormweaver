"""Server variable fuzzing: declarative specs, pools, startup rolls.

Runtime toggling (session/global/reload) executes in C++; this module
only describes the variables and converts them to the bound config.
Startup-only variables are rolled here and applied via add_config.
"""

import random
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from enum import IntEnum
from typing import Any

import stormweaver._stormweaver as _sw


class Tier(IntEnum):
    safe = 0
    semantics = 1
    disruptive = 2


MECHANISMS = frozenset({"session", "global", "reload", "startup"})
RUNTIME_MECHANISMS = frozenset({"session", "global", "reload"})


@dataclass(frozen=True)
class Choices:
    values: tuple[str, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "values", tuple(self.values))
        if not self.values:
            raise ValueError("Choices needs at least one value")


@dataclass(frozen=True)
class IntRange:
    min: int
    max: int
    step: int = 1
    suffix: str = ""

    def __post_init__(self) -> None:
        if self.min > self.max:
            raise ValueError("IntRange min > max")
        if self.min < 0:
            raise ValueError("IntRange must be non-negative")
        if self.step <= 0:
            raise ValueError("IntRange step must be positive")


@dataclass(frozen=True)
class Bool:
    pass


Generator = Choices | IntRange | Bool


@dataclass(frozen=True)
class VariableSpec:
    name: str
    flavor: str  # "postgres" | "mysql"
    generator: Generator
    mechanisms: frozenset[str]
    tier: Tier = Tier.safe
    weight: int = 10
    min_version: int = 0
    max_version: int = 0

    def __post_init__(self) -> None:
        if self.flavor not in ("postgres", "mysql"):
            raise ValueError(f"unknown flavor: {self.flavor}")
        object.__setattr__(self, "mechanisms", frozenset(self.mechanisms))
        unknown = self.mechanisms - MECHANISMS
        if unknown:
            raise ValueError(f"unknown mechanisms: {sorted(unknown)}")
        if not self.mechanisms:
            raise ValueError(f"{self.name}: needs at least one mechanism")
        if self.weight <= 0:
            raise ValueError(f"{self.name}: weight must be positive")
        if not isinstance(self.generator, (Choices, IntRange, Bool)):
            raise TypeError(f"{self.name}: generator must be Choices/IntRange/Bool")


def render_value(
    spec: VariableSpec, rng: random.Random, *, startup: bool = False
) -> str:
    """One random value for spec; mirrors render_value in core/src/action/variable.cpp.

    startup=True strips surrounding single quotes: the config-file layer
    quotes on its own (postgres add_config writes k = 'v').
    """
    gen = spec.generator
    if isinstance(gen, Choices):
        value = rng.choice(gen.values)
    elif isinstance(gen, IntRange):
        k = rng.randint(gen.min, gen.max)
        value = f"{k * gen.step}{gen.suffix}"
        if spec.flavor == "postgres" and gen.suffix and not startup:
            value = f"'{value}'"
    elif isinstance(gen, Bool):
        on = rng.random() < 0.5
        if spec.flavor == "postgres":
            value = "on" if on else "off"
        else:
            value = "ON" if on else "OFF"
    else:
        raise TypeError(f"unknown generator: {gen!r}")
    if startup and len(value) >= 2 and value[0] == value[-1] == "'":
        value = value[1:-1]
    return value


class VariablePool:
    """Immutable-ish list of specs; all mutators return a new pool."""

    def __init__(self, specs: Iterable[VariableSpec] = ()) -> None:
        self._specs = tuple(specs)
        keys = [(s.flavor, s.name) for s in self._specs]
        if len(keys) != len(set(keys)):
            raise ValueError("duplicate variable in pool")

    @property
    def specs(self) -> tuple[VariableSpec, ...]:
        return self._specs

    def __len__(self) -> int:
        return len(self._specs)

    def __iter__(self) -> Iterator[VariableSpec]:
        return iter(self._specs)

    def __add__(self, other: VariablePool) -> VariablePool:
        return VariablePool(self._specs + other._specs)

    def filter(
        self,
        *,
        flavor: str | None = None,
        tier: Tier | None = None,
        max_tier: Tier | None = None,
        mechanism: str | None = None,
    ) -> VariablePool:
        out: Iterable[VariableSpec] = self._specs
        if flavor is not None:
            out = [s for s in out if s.flavor == flavor]
        if tier is not None:
            out = [s for s in out if s.tier == tier]
        if max_tier is not None:
            out = [s for s in out if s.tier <= max_tier]
        if mechanism is not None:
            out = [s for s in out if mechanism in s.mechanisms]
        return VariablePool(out)

    def extend(self, specs: Iterable[VariableSpec]) -> VariablePool:
        return VariablePool(self._specs + tuple(specs))

    def remove(self, name: str) -> VariablePool:
        kept = [s for s in self._specs if s.name != name]
        if len(kept) == len(self._specs):
            raise ValueError(f"no such variable: {name}")
        return VariablePool(kept)

    def to_config(self) -> Any:
        """Bound VariableConfig with the runtime-mechanism specs."""
        out = []
        for s in self._specs:
            runtime = s.mechanisms & RUNTIME_MECHANISMS
            if not runtime:
                continue
            spec = _sw.VariableSpec()
            spec.name = s.name
            spec.flavor = s.flavor
            spec.weight = s.weight
            spec.min_version = s.min_version
            spec.max_version = s.max_version
            spec.mechanisms = sorted(runtime)
            g = s.generator
            if isinstance(g, Choices):
                spec.set_choices(list(g.values))
            elif isinstance(g, IntRange):
                spec.set_int_range(g.min, g.max, step=g.step, suffix=g.suffix)
            elif isinstance(g, Bool):
                spec.set_bool()
            else:
                raise TypeError(f"unknown generator: {g!r}")
            out.append(spec)
        cfg = _sw.VariableConfig()
        # bound vector member: assign the whole list, append does not persist
        cfg.specs = out
        return cfg

    def roll_startup(
        self, rng: random.Random, count: int | None = None
    ) -> dict[str, str]:
        """Roll values for startup-capable specs, ready for add_config.

        count > number of candidates clamps to all of them (no error).
        """
        if count is not None and count < 0:
            raise ValueError("count must be non-negative")
        candidates = [s for s in self._specs if "startup" in s.mechanisms]
        if count is not None and count < len(candidates):
            remaining = list(candidates)
            picked = []
            for _ in range(count):
                choice = rng.choices(remaining, weights=[s.weight for s in remaining])[
                    0
                ]
                remaining.remove(choice)
                picked.append(choice)
            candidates = picked
        return {s.name: render_value(s, rng, startup=True) for s in candidates}


def _derive_seed(seed: int, name: str) -> int:
    # FNV-1a, mirrors derive_seed in core/src/workload.cpp
    h = (1469598103934665603 ^ seed) & 0xFFFFFFFFFFFFFFFF
    for c in name.encode():
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def startup_rng(seed: int, run: int = 0) -> random.Random:
    """Seed-derived RNG for startup rolls; seed 0 = entropy, like workers."""
    if seed == 0:
        return random.Random()
    return random.Random(_derive_seed(seed, f"startup-{run}"))


def _pg(
    name: str,
    generator: Generator,
    mechanisms: Iterable[str],
    tier: Tier = Tier.safe,
    weight: int = 10,
    min_version: int = 0,
    max_version: int = 0,
) -> VariableSpec:
    return VariableSpec(
        name=name,
        flavor="postgres",
        generator=generator,
        mechanisms=frozenset(mechanisms),
        tier=tier,
        weight=weight,
        min_version=min_version,
        max_version=max_version,
    )


def _my(
    name: str,
    generator: Generator,
    mechanisms: Iterable[str],
    tier: Tier = Tier.safe,
    weight: int = 10,
    min_version: int = 0,
    max_version: int = 0,
) -> VariableSpec:
    return VariableSpec(
        name=name,
        flavor="mysql",
        generator=generator,
        mechanisms=frozenset(mechanisms),
        tier=tier,
        weight=weight,
        min_version=min_version,
        max_version=max_version,
    )


# Catalog rules: PG string/size values carry SQL quotes ('64MB'), stripped
# automatically on the startup path. Version gates use server_version_num
# style for PG (150000) and X*10000+Y*100+Z for MySQL (80030). Ranges are
# sized for test boxes.
PG_VARIABLES = VariablePool(
    [
        # safe: session tuning knobs
        _pg(
            "work_mem",
            Choices(("'64kB'", "'1MB'", "'16MB'", "'256MB'")),
            ("session", "startup"),
        ),
        _pg(
            "maintenance_work_mem",
            Choices(("'1MB'", "'16MB'", "'256MB'")),
            ("session", "startup"),
        ),
        # session SET errors once temp tables were touched in the session,
        # keep startup-only
        _pg(
            "temp_buffers",
            Choices(("'800kB'", "'8MB'", "'64MB'")),
            ("startup",),
        ),
        _pg(
            "random_page_cost",
            Choices(("1.0", "1.1", "4.0", "10.0")),
            ("session", "startup"),
        ),
        _pg(
            "effective_cache_size",
            Choices(("'64MB'", "'1GB'", "'8GB'")),
            ("session", "startup"),
        ),
        _pg(
            "default_statistics_target",
            IntRange(1, 100, step=10),
            ("session", "startup"),
        ),
        _pg(
            "cursor_tuple_fraction",
            Choices(("0.01", "0.1", "1.0")),
            ("session", "startup"),
        ),
        _pg("join_collapse_limit", IntRange(1, 12), ("session", "startup")),
        _pg("from_collapse_limit", IntRange(1, 12), ("session", "startup")),
        _pg("enable_seqscan", Bool(), ("session", "startup"), weight=5),
        _pg("enable_indexscan", Bool(), ("session", "startup"), weight=5),
        _pg("enable_bitmapscan", Bool(), ("session", "startup"), weight=5),
        _pg("enable_hashjoin", Bool(), ("session", "startup"), weight=5),
        _pg("enable_mergejoin", Bool(), ("session", "startup"), weight=5),
        _pg("enable_nestloop", Bool(), ("session", "startup"), weight=5),
        _pg("jit", Bool(), ("session", "startup")),
        _pg(
            "max_parallel_workers_per_gather",
            IntRange(0, 4),
            ("session", "startup"),
        ),
        # safe: sighup class
        _pg(
            "checkpoint_completion_target",
            Choices(("0.1", "0.5", "0.9")),
            ("reload", "startup"),
        ),
        _pg(
            "checkpoint_timeout",
            Choices(("'30s'", "'1min'", "'5min'")),
            ("reload", "startup"),
        ),
        _pg(
            "max_wal_size",
            Choices(("'128MB'", "'1GB'", "'4GB'")),
            ("reload", "startup"),
        ),
        _pg(
            "min_wal_size",
            Choices(("'32MB'", "'80MB'", "'256MB'")),
            ("reload", "startup"),
        ),
        _pg(
            "bgwriter_delay",
            Choices(("'10ms'", "'200ms'", "'2s'")),
            ("reload", "startup"),
        ),
        _pg(
            "wal_writer_delay",
            Choices(("'1ms'", "'200ms'", "'2s'")),
            ("reload", "startup"),
        ),
        _pg(
            "autovacuum_naptime",
            Choices(("'1s'", "'15s'", "'1min'")),
            ("reload", "startup"),
        ),
        # lz4 needs >=14, zstd >=15; one gate for the whole choice set.
        # values lz4/zstd also require the server binary built with those
        # codecs; drop them for codec-less builds.
        _pg(
            "wal_compression",
            Choices(("off", "pglz", "lz4", "zstd")),
            ("reload", "startup"),
            min_version=150000,
        ),
        # safe: startup only
        _pg(
            "shared_buffers",
            Choices(("'128MB'", "'256MB'", "'1GB'")),
            ("startup",),
        ),
        _pg("wal_buffers", Choices(("'64kB'", "'1MB'", "'16MB'")), ("startup",)),
        # semantics
        _pg(
            "default_transaction_isolation",
            Choices(("'read committed'", "'repeatable read'", "'serializable'")),
            ("session",),
            tier=Tier.semantics,
        ),
        _pg(
            "statement_timeout",
            Choices(("0", "'500ms'", "'5s'")),
            ("session",),
            tier=Tier.semantics,
        ),
        _pg(
            "lock_timeout",
            Choices(("0", "'100ms'", "'2s'")),
            ("session",),
            tier=Tier.semantics,
        ),
        _pg(
            "deadlock_timeout",
            Choices(("'10ms'", "'1s'", "'2s'")),
            ("session",),
            tier=Tier.semantics,
        ),
        _pg(
            "plan_cache_mode",
            Choices(("auto", "force_custom_plan", "force_generic_plan")),
            ("session",),
            tier=Tier.semantics,
        ),
        # single choice on purpose: tde scenarios extend the list with tde_heap
        _pg(
            "default_table_access_method",
            Choices(("heap",)),
            ("session",),
            tier=Tier.semantics,
        ),
        _pg("autovacuum", Bool(), ("reload",), tier=Tier.semantics),
        # disruptive: can legitimately lose/corrupt data on crash, breaks
        # crash-recovery validation
        _pg(
            "synchronous_commit",
            Choices(("on", "off", "local")),
            ("reload", "startup"),
            tier=Tier.disruptive,
        ),
        _pg("fsync", Bool(), ("reload", "startup"), tier=Tier.disruptive),
        _pg(
            "full_page_writes",
            Bool(),
            ("reload", "startup"),
            tier=Tier.disruptive,
        ),
        _pg(
            "default_transaction_read_only",
            Bool(),
            ("session",),
            tier=Tier.disruptive,
        ),
        _pg(
            "idle_in_transaction_session_timeout",
            Choices(("'500ms'", "'5s'")),
            ("session",),
            tier=Tier.disruptive,
        ),
    ]
)

MYSQL_VARIABLES = VariablePool(
    [
        # safe: session/global tuning knobs
        _my(
            "sort_buffer_size",
            IntRange(32, 512, step=1024),
            ("session", "global", "startup"),
        ),
        _my(
            "join_buffer_size",
            IntRange(128, 4096, step=1024),
            ("session", "global", "startup"),
        ),
        _my(
            "tmp_table_size",
            IntRange(1, 64, step=1048576),
            ("session", "global", "startup"),
        ),
        _my(
            "max_heap_table_size",
            IntRange(1, 64, step=1048576),
            ("session", "global", "startup"),
        ),
        _my(
            "read_buffer_size",
            IntRange(2, 64, step=8192),
            ("session", "global", "startup"),
        ),
        _my(
            "optimizer_search_depth",
            IntRange(0, 62),
            ("session", "global", "startup"),
        ),
        _my(
            "eq_range_index_dive_limit",
            IntRange(0, 200),
            ("session", "global", "startup"),
        ),
        _my(
            "optimizer_switch",
            Choices(
                (
                    "'index_merge=off'",
                    "'index_merge=on'",
                    "'mrr=off'",
                    "'batched_key_access=on,mrr_cost_based=off'",
                )
            ),
            ("session", "global"),
        ),
        # online buffer pool resize: rich bug source, weighted up
        _my(
            "innodb_buffer_pool_size",
            IntRange(128, 512, step=1048576),
            ("global", "startup"),
            weight=20,
        ),
        _my("innodb_adaptive_hash_index", Bool(), ("global", "startup")),
        # stay at or under the innodb_io_capacity_max default of 2000
        _my(
            "innodb_io_capacity",
            IntRange(1, 20, step=100),
            ("global", "startup"),
        ),
        _my(
            "innodb_flush_neighbors",
            Choices(("0", "1", "2")),
            ("global", "startup"),
        ),
        _my(
            "innodb_lru_scan_depth",
            Choices(("100", "1024", "4096")),
            ("global", "startup"),
        ),
        _my(
            "innodb_max_dirty_pages_pct",
            IntRange(0, 99),
            ("global", "startup"),
        ),
        _my(
            "innodb_redo_log_capacity",
            IntRange(8, 512, step=1048576),
            ("global", "startup"),
            min_version=80030,
        ),
        _my(
            "table_open_cache",
            Choices(("400", "2000", "8000")),
            ("global", "startup"),
        ),
        # safe: startup only
        _my(
            "innodb_buffer_pool_instances",
            Choices(("1", "2", "8")),
            ("startup",),
        ),
        _my("innodb_flush_method", Choices(("fsync", "O_DIRECT")), ("startup",)),
        _my("performance_schema", Choices(("ON", "OFF")), ("startup",)),
        # semantics
        _my(
            "transaction_isolation",
            Choices(
                (
                    "'READ-COMMITTED'",
                    "'REPEATABLE-READ'",
                    "'SERIALIZABLE'",
                    "'READ-UNCOMMITTED'",
                )
            ),
            ("session",),
            tier=Tier.semantics,
        ),
        _my(
            "innodb_lock_wait_timeout",
            IntRange(1, 50),
            ("session", "global"),
            tier=Tier.semantics,
        ),
        _my(
            "max_execution_time",
            Choices(("0", "500", "5000")),
            ("session",),
            tier=Tier.semantics,
        ),
        _my(
            "innodb_strict_mode",
            Bool(),
            ("session", "global"),
            tier=Tier.semantics,
        ),
        _my("foreign_key_checks", Bool(), ("session",), tier=Tier.semantics),
        # no ANSI_QUOTES: it would break generated SQL
        _my(
            "sql_mode",
            Choices(
                (
                    "''",
                    "'STRICT_TRANS_TABLES'",
                    "'STRICT_TRANS_TABLES,NO_ZERO_DATE'",
                    "'TRADITIONAL'",
                )
            ),
            ("session", "global"),
            tier=Tier.semantics,
        ),
        # disruptive
        _my("read_only", Bool(), ("global",), tier=Tier.disruptive),
        _my("super_read_only", Bool(), ("global",), tier=Tier.disruptive),
        _my(
            "innodb_flush_log_at_trx_commit",
            Choices(("0", "1", "2")),
            ("global", "startup"),
            tier=Tier.disruptive,
        ),
        _my(
            "sync_binlog",
            Choices(("0", "1", "1000")),
            ("global", "startup"),
            tier=Tier.disruptive,
        ),
        # runtime changes only allow ON <-> DETECT_ONLY
        _my(
            "innodb_doublewrite",
            Choices(("ON", "DETECT_ONLY")),
            ("global",),
            tier=Tier.disruptive,
            min_version=80030,
        ),
        _my("unique_checks", Bool(), ("session",), tier=Tier.disruptive),
    ]
)


def preset(flavor: str, max_tier: Tier = Tier.safe) -> VariablePool:
    """Curated catalog for a flavor, filtered up to max_tier."""
    if flavor == "postgres":
        pool = PG_VARIABLES
    elif flavor == "mysql":
        pool = MYSQL_VARIABLES
    else:
        raise ValueError(f"unknown flavor: {flavor}")
    return pool.filter(max_tier=max_tier)
