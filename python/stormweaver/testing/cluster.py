import getpass
import logging
import os
import tempfile
from pathlib import Path
from typing import TYPE_CHECKING

from stormweaver.backends.postgres import Postgres
from stormweaver.testing.util import alloc_port, ensure_logging

if TYPE_CHECKING:
    from stormweaver.testing.node import PgTestNode

logger = logging.getLogger(__name__)


def take_basebackup(node: PgTestNode, dest: str | Path | None = None) -> Path:
    if dest is None:
        dest = Path(tempfile.mkdtemp(prefix="sw-backup-", dir="/tmp")) / "data"
    dest = Path(dest)
    node.db.basebackup(dest, extra_args=["-X", "stream"])
    return dest


def _owned_parent(datadir: Path) -> Path | None:
    # only claim tmp dirs made by take_basebackup; never delete user dirs
    parent = datadir.parent
    if parent.parent == Path("/tmp") and parent.name.startswith("sw-backup-"):
        return parent
    return None


def _primary_conninfo(primary: PgTestNode, name: str) -> str:
    user = os.environ.get("PGUSER") or getpass.getuser()
    return f"host=localhost port={primary.port} user={user} application_name={name}"


# a standby is a pg_rewind source; pg_rewind chokes reading a socket file it
# finds inside PGDATA, so every standby keeps its socket outside the datadir.
def _standby_config(
    datadir: Path, port: int, primary: PgTestNode, name: str
) -> dict[str, str]:
    return {
        "port": str(port),
        "unix_socket_directories": str(datadir.resolve().parent),
        "primary_conninfo": _primary_conninfo(primary, name),
    }


def standby_from_backup(
    node_cls: type[PgTestNode],
    backup_dir: str | Path,
    primary: PgTestNode,
    *,
    name: str = "standby",
    port: int | None = None,
) -> PgTestNode:
    ensure_logging()
    backup_dir = Path(backup_dir)
    # server log name comes from datadir basename; rename to keep
    # multi-node logs distinguishable
    named = backup_dir.parent / name
    if named != backup_dir:
        backup_dir.rename(named)
    port = port or alloc_port()

    pg = Postgres(
        install_dir=primary.db.install_dir,
        datadir=named,
        init=False,
        port=port,
    )
    # backup carries the primary's config; appended overrides win (last wins)
    pg.add_config(_standby_config(named, port, primary, name))
    pg.set_signal("standby")

    node = node_cls(pg, primary.dbname, name)
    node._owned_dir = _owned_parent(named)
    try:
        node.start()
    except BaseException:
        node.close()
        raise
    return node


def rejoin_as_standby(node: PgTestNode, new_primary: PgTestNode) -> None:
    """Reconfigure a stopped node to follow new_primary as a hot standby.

    Used after pg_rewind: rewind copied the source's config over the target,
    so restore this node's own port/socket and point it at the new primary.
    """
    node.db.add_config(_standby_config(node.datadir, node.port, new_primary, node.name))
    node.db.set_signal("standby")
    node.start()


def wait_for_catchup(
    primary: PgTestNode, standby: PgTestNode, timeout: float = 60.0
) -> None:
    lsn = primary.sql_value("SELECT pg_current_wal_lsn()")
    standby.poll_until(
        "SELECT pg_last_wal_replay_lsn() >= $1::pg_lsn", "t", (lsn,), timeout=timeout
    )
