import random

import pytest
from stormweaver import variables
from stormweaver.variables import (
    Bool,
    Choices,
    IntRange,
    Tier,
    VariablePool,
    VariableSpec,
    render_value,
)


def _spec(name="work_mem", flavor="postgres", mechanisms=("session",), **kw):
    kw.setdefault("generator", Choices(("'64MB'",)))
    return VariableSpec(
        name=name, flavor=flavor, mechanisms=frozenset(mechanisms), **kw
    )


def test_spec_validation():
    with pytest.raises(ValueError, match="flavor"):
        _spec(flavor="oracle")
    with pytest.raises(ValueError, match="mechanism"):
        _spec(mechanisms=("telepathy",))
    with pytest.raises(ValueError, match="mechanism"):
        _spec(mechanisms=())
    with pytest.raises(ValueError, match="weight"):
        _spec(weight=0)
    with pytest.raises(ValueError):
        Choices(())
    with pytest.raises(ValueError):
        IntRange(5, 1)
    with pytest.raises(ValueError):
        IntRange(-1, 5)
    with pytest.raises(ValueError):
        IntRange(1, 5, step=0)
    with pytest.raises(TypeError):
        _spec(generator=IntRange)  # class, not an instance


def test_pool_operations():
    a = _spec("work_mem")
    b = _spec("jit", generator=Bool(), tier=Tier.semantics)
    c = _spec("sort_buffer_size", flavor="mysql", mechanisms=("session", "global"))
    pool = VariablePool([a, b, c])

    assert len(pool.filter(flavor="postgres")) == 2
    assert len(pool.filter(max_tier=Tier.safe)) == 2
    assert len(pool.filter(tier=Tier.semantics)) == 1
    assert len(pool.filter(mechanism="global")) == 1
    assert len(pool.remove("jit")) == 2
    with pytest.raises(ValueError, match="no such"):
        pool.remove("nope")
    with pytest.raises(ValueError, match="duplicate"):
        VariablePool([a, a])
    assert len(VariablePool([a]) + VariablePool([c])) == 2


def test_pool_specs_is_read_only():
    a = _spec("work_mem")
    pool = VariablePool([a])
    assert isinstance(pool.specs, tuple)
    with pytest.raises(AttributeError):
        pool.specs = ()  # type: ignore[misc]
    with pytest.raises(AttributeError):
        pool.specs.append(a)  # tuples have no append


def test_to_config_runtime_only():
    runtime = _spec("work_mem", mechanisms=("session", "startup"))
    startup_only = _spec("shared_buffers", mechanisms=("startup",))
    cfg = VariablePool([runtime, startup_only]).to_config()
    assert len(cfg.specs) == 1
    assert cfg.specs[0].name == "work_mem"
    assert set(cfg.specs[0].mechanisms) == {"session"}


def test_roll_startup_deterministic_and_stripped():
    pool = VariablePool(
        [
            _spec(
                "shared_buffers",
                mechanisms=("startup",),
                generator=Choices(("'128MB'", "'256MB'")),
            ),
            _spec(
                "wal_buffers",
                mechanisms=("startup",),
                generator=IntRange(1, 16, step=64, suffix="kB"),
            ),
            _spec("work_mem", mechanisms=("session",)),
        ]
    )
    r1 = pool.roll_startup(variables.startup_rng(42))
    r2 = pool.roll_startup(variables.startup_rng(42))
    assert r1 == r2
    assert set(r1) == {"shared_buffers", "wal_buffers"}
    assert "'" not in r1["shared_buffers"]
    assert r1["wal_buffers"].endswith("kB")

    partial = pool.roll_startup(variables.startup_rng(42), count=1)
    assert len(partial) == 1

    with pytest.raises(ValueError, match="non-negative"):
        pool.roll_startup(variables.startup_rng(42), count=-1)


def test_render_value_matches_cpp_rules():
    rng = random.Random(1)
    choices_spec = _spec(generator=Choices(("a", "b", "c")))
    assert render_value(choices_spec, rng) in ("a", "b", "c")

    pg_range_spec = _spec(generator=IntRange(1, 4, step=64, suffix="kB"))
    value = render_value(pg_range_spec, rng)
    assert value.startswith("'") and value.endswith("kB'")

    mysql_bool_spec = _spec(flavor="mysql", generator=Bool())
    assert render_value(mysql_bool_spec, rng) in ("ON", "OFF")


def test_startup_rng_seed_zero_is_entropy():
    # no assertion on values, only that both paths construct
    variables.startup_rng(0)
    a = variables.startup_rng(7).random()
    b = variables.startup_rng(7).random()
    assert a == b


def test_derive_seed_matches_fnv_reference():
    # FNV-1a("startup-0") with seed 1, hand-computed from the C++ constants:
    # h = 1469598103934665603 ^ 1, then per byte of b"startup-0":
    # h ^= byte; h = (h * 1099511628211) & (2**64 - 1)
    expected = 4464071010508767226
    h = variables._derive_seed(1, "startup-0")
    assert h == expected
    assert variables._derive_seed(2, "startup-0") != h
    assert variables._derive_seed(1, "startup-1") != h


def test_catalog_sanity():
    # pool constructors already enforce spec validity + name uniqueness;
    # check flavor consistency and tier coverage
    assert all(s.flavor == "postgres" for s in variables.PG_VARIABLES)
    assert all(s.flavor == "mysql" for s in variables.MYSQL_VARIABLES)
    for pool in (variables.PG_VARIABLES, variables.MYSQL_VARIABLES):
        tiers = {s.tier for s in pool}
        assert tiers == {Tier.safe, Tier.semantics, Tier.disruptive}


def test_presets():
    safe_pg = variables.preset("postgres")
    assert len(safe_pg) > 0
    assert all(s.tier == Tier.safe for s in safe_pg)
    more = variables.preset("postgres", max_tier=Tier.semantics)
    assert len(more) > len(safe_pg)
    with pytest.raises(ValueError):
        variables.preset("oracle")


def test_catalog_mechanism_shape():
    # PG has no runtime-global mechanism, MySQL has no reload
    assert len(variables.PG_VARIABLES.filter(mechanism="global")) == 0
    assert len(variables.MYSQL_VARIABLES.filter(mechanism="reload")) == 0
    # each flavor's safe preset feeds both of its runtime actions
    assert len(variables.preset("postgres").filter(mechanism="session")) > 0
    assert len(variables.preset("postgres").filter(mechanism="reload")) > 0
    assert len(variables.preset("mysql").filter(mechanism="session")) > 0
    assert len(variables.preset("mysql").filter(mechanism="global")) > 0
    # and both have startup material
    assert len(variables.preset("postgres").filter(mechanism="startup")) > 0
    assert len(variables.preset("mysql").filter(mechanism="startup")) > 0
