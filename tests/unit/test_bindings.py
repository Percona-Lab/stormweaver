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


def test_connect_mysql_exists_and_fails_cleanly():
    with pytest.raises(RuntimeError):
        sw.connect_mysql(host="127.0.0.1", port=1, log_name="mysql-nope")


def test_worker_exposes_checksums():
    assert callable(getattr(sw.Worker, "calculate_database_checksums", None))
