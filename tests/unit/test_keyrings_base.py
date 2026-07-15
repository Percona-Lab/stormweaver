import pytest
from stormweaver.keyrings.base import ExternalService, Keyring, RunningService
from stormweaver.keyrings.file import FileKeyring


class FakeService(RunningService):
    def __init__(self):
        self.calls = []

    def start(self):
        self.calls.append("start")

    def stop(self):
        self.calls.append("stop")

    def wipe_state(self):
        self.calls.append("wipe")


class DummyKeyring(Keyring):
    kind = "dummy"


def test_file_keyring_no_service(tmp_path):
    k = FileKeyring(tmp_path / "kr.per")
    assert k.kind == "file"
    assert not k.has_service
    assert not k.managed
    with pytest.raises(RuntimeError, match="no backing service"):
        k.restart_service()
    k.close()


def test_managed_service_restart_and_close():
    svc = FakeService()
    k = DummyKeyring(service=svc)
    assert k.managed
    k.restart_service()
    assert svc.calls == ["stop", "start"]
    k.restart_service(fresh=True)
    assert svc.calls == ["stop", "start", "stop", "wipe", "start"]
    k.close()
    assert svc.calls[-1] == "stop"


def test_external_service_unmanaged():
    k = DummyKeyring(service=ExternalService())
    assert not k.managed
    # restart hits stop() first, which is the forbidden call
    with pytest.raises(RuntimeError, match="cannot be stopped"):
        k.restart_service()
    k.close()  # must not try to stop the external service


def test_context_manager_owned_workdir(tmp_path):
    wd = tmp_path / "wd"
    wd.mkdir()
    (wd / "junk").write_text("x")
    with DummyKeyring(workdir=wd, owns_workdir=True):
        pass
    assert not wd.exists()
