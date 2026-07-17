import pytest
import stormweaver as sw


def test_version():
    assert sw.__version__ == "0.1.0"


def test_metadata_empty():
    md = sw.Metadata()
    assert md.size() == 0


def test_default_registry_weighted():
    reg = sw.default_action_registry()
    assert reg.size() > 0
    assert reg.total_weight() > 0


def test_default_registry_per_flavor():
    pg = sw.default_action_registry()
    mysql = sw.default_action_registry("mysql")

    assert pg.size() == mysql.size()
    assert pg.has("create_normal_table")
    assert mysql.has("create_normal_table")

    original = mysql.get("create_normal_table").weight
    mysql.get("create_normal_table").weight = original + 1
    assert pg.get("create_normal_table").weight == original
    mysql.get("create_normal_table").weight = original


def test_registry_insert_remove():
    reg = sw.ActionRegistry()
    reg.make_custom_sql("noop", "SELECT 1", 5)
    assert reg.has("noop")
    reg.remove("noop")
    assert not reg.has("noop")


def test_factory_ref_survives_registry_gc():
    import gc

    reg = sw.ActionRegistry()
    reg.make_custom_sql("noop", "SELECT 1", 5)
    f = reg.get("noop")
    del reg
    gc.collect()
    assert f.name == "noop"  # reference_internal keeps registry alive


def test_config_roundtrip():
    cfg = sw.AllConfig()
    cfg.ddl.access_methods = ["heap"]
    assert cfg.ddl.access_methods == ["heap"]


def test_transaction_config_roundtrip():
    cfg = sw.AllConfig()
    cfg.transaction.commit_prob = 80
    cfg.transaction.error_mode = "abort"
    cfg.transaction.mysql_ddl_mode = "exclude"
    cfg.transaction.isolation_weights.serializable = 5
    assert cfg.transaction.commit_prob == 80
    assert cfg.transaction.error_mode == "abort"
    assert cfg.transaction.mysql_ddl_mode == "exclude"
    assert cfg.transaction.isolation_weights.serializable == 5
    with pytest.raises(ValueError):
        cfg.transaction.error_mode = "explode"


def test_dml_config_roundtrip():
    cfg = sw.AllConfig()
    assert cfg.dml.update_min == 1
    assert cfg.dml.update_max == 10
    assert cfg.dml.lock_weights.none == 1
    assert cfg.dml.lock_weights.for_update == 1
    assert cfg.dml.lock_weights.for_share == 1
    cfg.dml.update_min = 2
    cfg.dml.update_max = 5
    cfg.dml.lock_weights.none = 0
    cfg.dml.lock_weights.for_update = 3
    cfg.dml.lock_weights.for_share = 2
    assert cfg.dml.update_min == 2
    assert cfg.dml.update_max == 5
    assert cfg.dml.lock_weights.none == 0
    assert cfg.dml.lock_weights.for_update == 3
    assert cfg.dml.lock_weights.for_share == 2


def test_connect_mysql_exists_and_fails_cleanly():
    with pytest.raises(RuntimeError):
        sw.connect_mysql(host="127.0.0.1", port=1, log_name="mysql-nope")


def test_worker_exposes_checksums():
    assert callable(getattr(sw.Worker, "calculate_database_checksums", None))


def test_statistics_bindings_exposed():
    core = sw._stormweaver
    for name in ("action_names", "action_stats", "transaction_stats"):
        assert callable(getattr(core.WorkerStatistics, name, None))
    for name in ("row_histograms", "success_rate"):
        assert callable(getattr(core.ActionStatistics, name, None))
    assert hasattr(core.ActionStatistics, "action_error_names")
    assert hasattr(core.ActionStatistics, "sql_error_codes")
    assert callable(getattr(core.TimingStatistics, "histogram", None))
    assert hasattr(core, "TransactionStatistics")
    for field in (
        "committed",
        "rolled_back_intentional",
        "rolled_back_error",
        "implicit_commits",
        "savepoint_rollbacks",
        "sub_actions_ok",
        "sub_actions_fail",
    ):
        assert hasattr(core.TransactionStatistics, field)
    for name in ("total", "has_data", "sub_histogram"):
        assert callable(getattr(core.TransactionStatistics, name, None))


def test_variable_spec_roundtrip():
    s = sw.VariableSpec()
    s.name = "work_mem"
    s.flavor = "postgres"
    s.mechanisms = ["session", "startup"]
    s.weight = 20
    s.min_version = 150000
    s.set_choices(["'64MB'", "'256MB'"])
    assert s.name == "work_mem"
    assert s.flavor == "postgres"
    assert set(s.mechanisms) == {"session", "startup"}
    assert s.weight == 20
    assert s.min_version == 150000

    s.flavor = "mysql"
    assert s.flavor == "mysql"

    with pytest.raises(ValueError):
        s.flavor = "oracle"
    with pytest.raises(ValueError):
        s.mechanisms = ["telepathy"]


def test_variable_config_attaches_to_all_config():
    s = sw.VariableSpec()
    s.name = "jit"
    s.flavor = "postgres"
    s.mechanisms = ["session"]
    s.set_bool()

    cfg = sw.VariableConfig()
    cfg.specs = [s]
    assert len(cfg.specs) == 1
    assert cfg.specs[0].name == "jit"

    ac = sw.AllConfig()
    ac.variables = cfg
    assert len(ac.variables.specs) == 1


def test_variable_actions_dormant_in_default_registry():
    for flavor in ("pg", "mysql"):
        reg = sw.default_action_registry(flavor)
        for name in (
            "set_session_variable",
            "set_global_variable",
            "reload_global_variable",
        ):
            assert reg.has(name)
            assert reg.get(name).weight == 0
