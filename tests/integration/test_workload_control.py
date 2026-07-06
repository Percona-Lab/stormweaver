import pytest
import stormweaver as sw
from conftest import pg_user, requires_pg, running_pg

pytestmark = requires_pg


@pytest.fixture(scope="module")
def pg():
    with running_pg(26330, "wctl") as server:
        yield server


def _registry():
    reg = sw.ActionRegistry()

    @sw.action(reg, "py_select", weight=10)
    def py_select(metadata, rand, conn):
        conn.execute(f"SELECT {rand.number(1, 100)}")

    return reg


def _factory(pg, names):
    user = pg_user()

    def connect(name):
        names.append(name)
        return sw.connect_pg(
            host="localhost", port=pg.port, dbname="wctl", user=user, log_name=name
        )

    return connect


def test_worker_setup_called_per_worker_per_cycle(pg):
    seen = []
    wl = sw.Workload(
        workers=2,
        duration=1,
        repeat=2,
        registry=_registry(),
        metadata=sw.Metadata(),
        node_factory=_factory(pg, []),
        worker_setup=lambda worker, idx: seen.append(idx),
        worker_name_prefix="ws-",
    )
    wl.run()
    assert seen == [0, 1, 0, 1]


def test_start_wait_and_workers_property(pg):
    wl = sw.Workload(
        workers=2,
        duration=2,
        registry=_registry(),
        metadata=sw.Metadata(),
        node_factory=_factory(pg, []),
        worker_name_prefix="sw-",
    )
    assert wl.workers == []
    wl.start()
    assert len(wl.workers) == 2
    with pytest.raises(RuntimeError):
        wl.start()
    wl.wait()
    assert wl.workers == []
    with pytest.raises(RuntimeError):
        wl.wait()
    assert len(wl.worker_statistics()) == 2


def test_worker_names_unique_across_runs(pg):
    names = []
    wl = sw.Workload(
        workers=1,
        duration=1,
        registry=_registry(),
        metadata=sw.Metadata(),
        node_factory=_factory(pg, names),
        worker_name_prefix="uniq-",
    )
    wl.run()
    wl.run()
    assert len(names) == 2
    assert names[0] != names[1]
