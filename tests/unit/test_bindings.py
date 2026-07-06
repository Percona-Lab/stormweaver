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
