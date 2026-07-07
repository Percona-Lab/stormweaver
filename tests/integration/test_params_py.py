import pytest
import stormweaver as sw
from conftest import pg_user, requires_pg, running_pg

pytestmark = requires_pg


def _connect(pg, dbname):
    return sw.connect_pg(
        host="localhost",
        port=pg.port,
        dbname=dbname,
        user=pg_user(),
        log_name="params-test",
    )


def test_params_roundtrip():
    with running_pg(26340, "paramsdb") as pg:
        conn = _connect(pg, "paramsdb")
        conn.safe_execute("CREATE TABLE pt (a int, b text, c bytea, d boolean)")
        conn.safe_execute(
            "INSERT INTO pt VALUES ($1, $2, $3, $4)",
            [42, "it's a test", b"\x00\xff", True],
        )
        res = conn.safe_execute(
            "SELECT b, encode(c, 'hex'), d FROM pt WHERE a = $1", [42]
        )
        assert res.rows() == [["it's a test", "00ff", "t"]]

        conn.safe_execute(
            "INSERT INTO pt VALUES ($1, $2, $3, $4)", [1, None, None, False]
        )
        assert conn.safe_execute("SELECT count(*) FROM pt WHERE b IS NULL").rows() == [
            ["1"]
        ]


def test_safe_execute_raises_sql_error():
    with running_pg(26341, "errdb") as pg:
        conn = _connect(pg, "errdb")
        with pytest.raises(sw.SqlError) as ei:
            conn.safe_execute("SELECT * FROM missing_table")
        assert ei.value.error_code == "42P01"
        assert ei.value.error_class == "other"

        bad = conn.execute("SELECT * FROM missing_table")
        assert not bad.success()
        assert bad.error_class == "other"


def test_unsupported_param_type():
    with running_pg(26342, "typedb") as pg:
        conn = _connect(pg, "typedb")
        with pytest.raises(TypeError):
            conn.execute("SELECT $1", [object()])
        with pytest.raises(TypeError):
            conn.execute("SELECT $1", "not-a-params-sequence")
