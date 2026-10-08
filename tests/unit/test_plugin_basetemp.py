import os
from types import SimpleNamespace

import pytest
from stormweaver import pytest_plugin as plugin


def _config(basetemp=None):
    return SimpleNamespace(
        option=SimpleNamespace(basetemp=basetemp), stash=pytest.Stash()
    )


def test_private_basetemp_removed_on_success():
    cfg = _config()
    plugin.pytest_configure(cfg)
    root = cfg.option.basetemp
    assert root.startswith("/tmp/sw-pytest-tmp-")
    assert os.path.isdir(root)
    plugin.pytest_sessionfinish(SimpleNamespace(config=cfg), 0)
    assert not os.path.exists(root)


def test_private_basetemp_kept_on_failure():
    cfg = _config()
    plugin.pytest_configure(cfg)
    root = cfg.option.basetemp
    plugin.pytest_sessionfinish(SimpleNamespace(config=cfg), 1)
    assert os.path.isdir(root)
    os.rmdir(root)


def test_explicit_basetemp_untouched(tmp_path):
    cfg = _config(basetemp=str(tmp_path / "bt"))
    plugin.pytest_configure(cfg)
    assert cfg.option.basetemp == str(tmp_path / "bt")
    plugin.pytest_sessionfinish(SimpleNamespace(config=cfg), 0)
    assert tmp_path.is_dir()
