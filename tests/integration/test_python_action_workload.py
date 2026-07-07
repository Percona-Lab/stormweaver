import getpass
import os
import shutil
import tempfile
from pathlib import Path

import pytest
import stormweaver as sw

_PG = Path(
    os.environ.get(
        "STORMWEAVER_PG_DIR", str(Path.home() / ".local/share/stormweaver-pg18")
    )
)

pytestmark = pytest.mark.skipif(
    not (_PG / "bin" / "initdb").exists(), reason="no postgres installation"
)


def _start_pg(port: int) -> tuple[sw.Postgres, str]:
    # tmp_path can be long enough to blow postgres' unix socket path limit
    # (107 bytes); use a short /tmp dir instead, cleaned up by the caller.
    datadir = tempfile.mkdtemp(dir="/tmp")
    pg = sw.Postgres(
        install_dir=str(_PG), datadir=str(Path(datadir) / "data"), port=port
    )
    pg.start()
    assert pg.wait_ready(), "postgres did not become ready"
    pg.createdb("pyact")
    return pg, datadir


def test_python_action_runs_in_workload():
    pg, datadir = _start_pg(26310)
    try:
        user = os.environ.get("PGUSER") or getpass.getuser()

        def connect() -> sw.LoggedSQL:
            return sw.connect_pg(
                host="localhost",
                port=pg.port,
                dbname="pyact",
                user=user,
                log_name="pyact-select",
            )

        reg = sw.ActionRegistry()

        @sw.action(reg, "py_select", weight=10)
        def py_select(metadata: object, rand: sw.Random, conn: sw.LoggedSQL) -> None:
            conn.execute(f"SELECT {rand.number(1, 100)}")

        metadata = sw.Metadata()
        workload = sw.Workload(
            workers=2,
            duration=5,
            repeat=1,
            registry=reg,
            metadata=metadata,
            node_factory=connect,
            action_config=sw.AllConfig(),
            seed=1,
            worker_name_prefix="pyact-select-",
        )
        workload.run()

        total_success = sum(
            stats.action_success_count("py_select")
            for stats in workload.worker_statistics()
        )
        assert total_success > 0
    finally:
        pg.stop()
        shutil.rmtree(datadir, ignore_errors=True)


def test_python_action_exception_is_recorded_not_fatal():
    pg, datadir = _start_pg(26311)
    try:
        user = os.environ.get("PGUSER") or getpass.getuser()

        def connect() -> sw.LoggedSQL:
            return sw.connect_pg(
                host="localhost",
                port=pg.port,
                dbname="pyact",
                user=user,
                log_name="pyact-raise",
            )

        reg = sw.ActionRegistry()

        @sw.action(reg, "py_raise", weight=10)
        def py_raise(metadata: object, rand: sw.Random, conn: sw.LoggedSQL) -> None:
            raise RuntimeError("boom")

        metadata = sw.Metadata()
        workload = sw.Workload(
            workers=2,
            duration=5,
            repeat=1,
            registry=reg,
            metadata=metadata,
            node_factory=connect,
            action_config=sw.AllConfig(),
            seed=1,
            worker_name_prefix="pyact-raise-",
        )
        # must survive: exceptions in python actions are recorded, not fatal
        workload.run()

        total_failures = sum(
            stats.action_failure_count("py_raise")
            for stats in workload.worker_statistics()
        )
        assert total_failures > 0
    finally:
        pg.stop()
        shutil.rmtree(datadir, ignore_errors=True)


def test_select_then_modify_actions_run():
    pg, datadir = _start_pg(26312)
    try:
        user = os.environ.get("PGUSER") or getpass.getuser()

        def connect() -> sw.LoggedSQL:
            return sw.connect_pg(
                host="localhost",
                port=pg.port,
                dbname="pyact",
                user=user,
                log_name="pyact-selmod",
            )

        # copy, never mutate the process-wide default registry (same
        # convention as scenario._base_registry)
        registry = sw.ActionRegistry()
        registry.use(sw.default_action_registry())
        for name in [
            "drop_table",
            "alter_table",
            "rename_table",
            "create_index",
            "drop_index",
            "drop_partition",
            "delete_some_data",
            "update_one_row",
        ]:
            if registry.has(name):
                registry.remove(name)

        metadata = sw.Metadata()
        workload = sw.Workload(
            workers=2,
            duration=5,
            repeat=1,
            registry=registry,
            metadata=metadata,
            node_factory=connect,
            action_config=sw.AllConfig(),
            seed=1,
            worker_name_prefix="pyact-selmod-",
        )
        workload.run()

        stats = workload.worker_statistics()
        deletes = sum(s.action_success_count("delete_selected") for s in stats)
        updates = sum(s.action_success_count("update_selected") for s in stats)
        transactions = sum(s.action_success_count("transaction") for s in stats)
        # transaction sub-actions draw from the same registry, so the new
        # actions also run inside transactions here; partitioned tables stay
        # in the mix to cover the partition-key SET-clause exclusion
        assert deletes > 0
        assert updates > 0
        assert transactions > 0
    finally:
        pg.stop()
        shutil.rmtree(datadir, ignore_errors=True)
