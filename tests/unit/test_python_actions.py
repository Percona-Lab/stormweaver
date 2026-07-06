import stormweaver as sw


def test_register_python_action():
    reg = sw.ActionRegistry()

    @sw.action(reg, "py_noop", weight=7)
    def py_noop(metadata, rand, conn):
        pass

    assert reg.has("py_noop")
    assert reg.total_weight() == 7


def test_random_seeded_is_deterministic():
    r1 = sw.Random(1234)
    r2 = sw.Random(1234)

    assert [r1.number(0, 1_000_000) for _ in range(20)] == [
        r2.number(0, 1_000_000) for _ in range(20)
    ]
    assert sw.Random(1234).string(5, 5) == sw.Random(1234).string(5, 5)


def test_random_number_range():
    r = sw.Random(1)
    for _ in range(100):
        assert 10 <= r.number(10, 20) <= 20


def test_random_boolean_returns_bool():
    r = sw.Random(1)
    assert isinstance(r.boolean(), bool)
