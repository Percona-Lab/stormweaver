import itertools
from collections.abc import Iterator

import pytest

from stormweaver.testing.node import PgTestNode
from stormweaver.testing.util import pg_install_dir

# unique node name per test: same-named nodes share one server log file,
# which breaks log-offset waits across tests
_seq = itertools.count(1)


@pytest.fixture
def pg_test_node() -> Iterator[PgTestNode]:
    try:
        install = pg_install_dir()
    except RuntimeError as e:
        pytest.skip(str(e))
    with PgTestNode.fresh(install, name=f"fixture-{next(_seq)}") as node:
        yield node
