from pathlib import Path

import pytest
import stormweaver.testing as st
from conftest import TDE_DIR, requires_pg_tde
from rewind_tde import TdeRewind
from stormweaver.keyrings import open_keyring
from stormweaver.keyrings.base import Keyring
from stormweaver.keyrings.file import FileKeyring

pytestmark = requires_pg_tde


def test_tde_rewind_driver_smoke(tmp_path):
    with TdeRewind(
        TDE_DIR,
        cipher="aes_128",
        keyring=FileKeyring(tmp_path / "kr.per"),
        debug=True,
    ) as rw:
        p = rw.setup_primary()
        p.safe_sql("CREATE TABLE t (d text)")
        p.safe_sql("INSERT INTO t VALUES ('base')")
        p.safe_sql("CHECKPOINT")
        s = rw.create_standby()
        p.safe_sql("INSERT INTO t VALUES ('before promotion')")
        rw.promote_standby()
        p.safe_sql("INSERT INTO t VALUES ('old primary diverge')")
        s.safe_sql("INSERT INTO t VALUES ('standby diverge')")
        rw.run_rewind("local")
        rows = [r[0] for r in p.safe_sql("SELECT d FROM t ORDER BY d")]
        assert "standby diverge" in rows
        assert "old primary diverge" not in rows


MODES = ["local", "remote", "archive"]
CIPHERS = ["aes_128", "aes_256"]


@pytest.mark.parametrize("cipher", CIPHERS)
@pytest.mark.parametrize("mode", MODES)
def test_pg_tde_rewind(mode, cipher, tmp_path):
    run_rewind_scenario(TDE_DIR, mode, cipher, FileKeyring(tmp_path / "kr.per"))


@pytest.mark.parametrize("kind", st.keyring_params())
def test_pg_tde_rewind_keyrings(kind, tmp_path):
    with open_keyring(kind, tmp_path / "keyring") as keyring:
        run_rewind_scenario(TDE_DIR, "local", "aes_128", keyring)


def run_rewind_scenario(install_dir, mode: str, cipher: str, keyring: Keyring) -> None:
    with TdeRewind(install_dir, cipher=cipher, keyring=keyring, debug=True) as rw:
        p = rw.setup_primary()
        has_tablespace = p.server_version_num >= 170000

        if has_tablespace:
            p.safe_sql("CREATE TABLESPACE space_test LOCATION ''")
            p.safe_sql("CREATE TABLE space_tbl (d text) TABLESPACE space_test")
            p.safe_sql("INSERT INTO space_tbl VALUES ('in primary, before promotion')")
        p.safe_sql("CREATE TABLE tbl1 (d text)")
        p.safe_sql("INSERT INTO tbl1 VALUES ('in primary')")
        p.safe_sql("CREATE TABLE trunc_tbl (d text)")
        p.safe_sql("INSERT INTO trunc_tbl VALUES ('in primary')")
        p.safe_sql("CREATE TABLE tail_tbl (id integer, d text)")
        p.safe_sql("INSERT INTO tail_tbl VALUES (0, 'in primary')")
        p.safe_sql("CREATE TABLE drop_tbl (d text)")
        p.safe_sql("INSERT INTO drop_tbl VALUES ('in primary')")
        p.safe_sql("CHECKPOINT")

        s = rw.create_standby()

        p.safe_sql("INSERT INTO tbl1 VALUES ('in primary, before promotion')")
        p.safe_sql("INSERT INTO trunc_tbl VALUES ('in primary, before promotion')")
        p.safe_sql(
            "INSERT INTO tail_tbl SELECT g, 'in primary, before promotion: ' || g"
            " FROM generate_series(1, 10000) g"
        )
        p.safe_sql("CHECKPOINT")

        rw.promote_standby()

        # diverge the old primary (and the standby, for tbl1/space_tbl)
        p.safe_sql("INSERT INTO tbl1 VALUES ('in primary, after promotion')")
        s.safe_sql("INSERT INTO tbl1 VALUES ('in standby, after promotion')")
        p.safe_sql(
            "INSERT INTO trunc_tbl SELECT 'in primary, after promotion: ' || g"
            " FROM generate_series(1, 10000) g"
        )
        # DELETE+VACUUM shrinks the file (cannot TRUNCATE: new relfilenode)
        p.safe_sql("DELETE FROM tail_tbl WHERE id > 10")
        p.safe_sql("VACUUM tail_tbl")
        p.safe_sql("INSERT INTO drop_tbl VALUES ('in primary, after promotion')")
        p.safe_sql("DROP TABLE drop_tbl")
        if has_tablespace:
            p.safe_sql("INSERT INTO space_tbl VALUES ('in primary, after promotion')")
            s.safe_sql("INSERT INTO space_tbl VALUES ('in standby, after promotion')")

        rw.run_rewind(mode)

        def col(sql):
            return [r[0] for r in p.safe_sql(sql)]

        assert col("SELECT d FROM tbl1 ORDER BY d") == [
            "in primary",
            "in primary, before promotion",
            "in standby, after promotion",
        ]
        assert col("SELECT d FROM trunc_tbl ORDER BY d") == [
            "in primary",
            "in primary, before promotion",
        ]
        assert p.sql_value("SELECT count(*) FROM tail_tbl") == "10001"
        assert col("SELECT d FROM drop_tbl") == ["in primary"]
        if has_tablespace:
            assert col("SELECT d FROM space_tbl ORDER BY d") == [
                "in primary, before promotion",
                "in standby, after promotion",
            ]

        violations = st.check_mode_recursive(p.datadir)
        assert violations == [], f"PGDATA permission violations: {violations}"


def _snapshot(root: Path) -> dict[str, int]:
    # dry-run must not create or resize anything under the target datadir
    return {
        str(p.relative_to(root)): p.stat().st_size
        for p in root.rglob("*")
        if p.is_file() and not p.is_symlink()
    }


def test_pg_tde_rewind_bad_args(tmp_path):
    with TdeRewind(
        TDE_DIR,
        cipher="aes_128",
        keyring=FileKeyring(tmp_path / "kr.per"),
        debug=True,
    ) as rw:
        p = rw.setup_primary()
        p.safe_sql("CREATE TABLE tbl1 (d text)")
        p.safe_sql("INSERT INTO tbl1 VALUES ('in primary')")
        p.safe_sql("CHECKPOINT")
        s = rw.create_standby()
        p.safe_sql("INSERT INTO tbl1 VALUES ('before promotion')")
        rw.promote_standby()
        p.safe_sql("INSERT INTO tbl1 VALUES ('old primary diverge')")
        s.safe_sql("INSERT INTO tbl1 VALUES ('standby diverge')")

        source = s.datadir
        target = p.datadir

        def rewind(*extra):
            return p.run_bin(
                "pg_tde_rewind",
                "--debug",
                f"--source-pgdata={source}",
                f"--target-pgdata={target}",
                "--no-sync",
                *extra,
            )

        # target still running -> forced recovery step cannot run
        st.command_fails(rewind(), msg="pg_tde_rewind with running target")
        st.command_fails(
            rewind("--no-ensure-shutdown"),
            msg="pg_tde_rewind --no-ensure-shutdown with running target",
        )

        # stop target cleanly; source (standby) still running -> must refuse
        p.stop()
        st.command_fails(
            rewind("--no-ensure-shutdown"),
            msg="pg_tde_rewind with running source",
        )

        # both stopped -> dry-run succeeds and mutates nothing
        s.stop()
        before = _snapshot(target)
        st.command_ok(rewind("--dry-run"), "pg_tde_rewind --dry-run")
        assert _snapshot(target) == before


def add_arguments(parser):
    from stormweaver import keyrings

    parser.add_argument("--mode", choices=MODES, default="local")
    parser.add_argument("--cipher", choices=CIPHERS, default="aes_128")
    parser.add_argument("--keyring", choices=list(keyrings.KINDS), default="file")


def main(args):
    import shutil
    import tempfile

    from stormweaver import keyrings

    install_dir = args.install_dir or TDE_DIR
    # /tmp keeps the unix socket path under the 107-byte AF_UNIX limit
    workdir = Path(tempfile.mkdtemp(prefix="sw-rewind-", dir="/tmp"))
    try:
        with keyrings.open_keyring(args.keyring, workdir / "keyring") as keyring:
            run_rewind_scenario(install_dir, args.mode, args.cipher, keyring)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    print(
        f"pg_tde rewind scenario passed: mode={args.mode} cipher={args.cipher}"
        f" keyring={args.keyring}"
    )
    return 0


if __name__ == "__main__":
    st.main()
