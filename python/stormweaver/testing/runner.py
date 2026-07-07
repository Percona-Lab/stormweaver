import sys
from pathlib import Path
from typing import NoReturn

from stormweaver.testing.util import ensure_logging


def main(extra_args: list[str] | None = None) -> NoReturn:
    """Standalone entry for test files: python t/test_foo.py [pytest args].

    When extra_args is given it replaces the CLI arguments entirely.
    """
    # local import: keep pytest out of plain `import stormweaver.testing`
    import pytest

    target = sys.argv[0]
    ensure_logging(Path(target).stem)
    argv = [target, "-v", *(extra_args if extra_args is not None else sys.argv[1:])]
    raise SystemExit(int(pytest.main(argv)))
