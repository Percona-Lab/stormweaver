"""Stable-test example: deterministic, always-green, real server."""

import stormweaver as sw


def test_create_insert_select(postgres_server_session, sw_connect):
    postgres_server_session.createdb("stabledb")
    conn = sw_connect("stabledb")
    r = conn.execute("CREATE TABLE t1 (id int primary key, v text)")
    assert r.success()
    r = conn.execute(
        "INSERT INTO t1 SELECT g, 'row-' || g FROM generate_series(1, 100) g"
    )
    assert r.success()
    assert r.affected_rows == 100
    r = conn.execute("SELECT count(*) FROM t1")
    assert r.success()


def test_seeded_setup_is_deterministic(postgres_server_session, sw_connect):
    postgres_server_session.createdb("wldb")

    def connect():
        return sw_connect("wldb")

    metadata = sw.Metadata()
    params = sw.WorkloadParams()
    params.seed = 4242
    setup = sw.Worker("stable-setup-1", connect, params, metadata)
    setup.create_random_tables(2)
    assert metadata.size() == 2
    assert setup.validate_metadata()


def test_seeded_single_worker_workload(postgres_server_session, sw_connect):
    postgres_server_session.createdb("workloaddb")

    def connect():
        return sw_connect("workloaddb")

    metadata = sw.Metadata()
    setup_params = sw.WorkloadParams()
    setup_params.seed = 777
    setup = sw.Worker("stable-wl-setup", connect, setup_params, metadata)
    setup.create_random_tables(2)

    workload = sw.Workload(
        workers=1,
        duration=3,
        registry=sw.default_action_registry(),
        metadata=metadata,
        node_factory=connect,
        seed=777,
        worker_name_prefix="stable-wl-",
    )
    workload.run()

    stats = workload.worker_statistics()
    assert len(stats) == 1
    total_success = sum(s.total_success_count() for s in stats)
    assert total_success > 0

    validator = sw.Worker("stable-wl-validator", connect, sw.WorkloadParams(), metadata)
    assert validator.validate_metadata()
