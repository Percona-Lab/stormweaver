import stormweaver as sw
import stormweaver.testing as st
from conftest import PG_DIR, requires_pg

pytestmark = requires_pg


def test_hybrid_deterministic_and_random():
    assert PG_DIR is not None
    with st.PgTestNode.fresh(PG_DIR, name="hybrid") as node:
        # deterministic setup
        node.safe_sql("CREATE TABLE audit_marker (phase text)")
        node.safe_sql("INSERT INTO audit_marker VALUES ('setup done')")

        # randomized burst on the same database
        registry = sw.ActionRegistry()
        registry.use(sw.default_action_registry("pg"))
        for name in ("create_partition", "drop_partition"):
            if registry.has(name):
                registry.remove(name)

        metadata = sw.Metadata()
        setup = sw.Worker(
            "hybrid-setup",
            lambda: node.raw_connect("hybrid-setup"),
            sw.WorkloadParams(),
            metadata,
        )
        setup.create_random_tables(3)

        wl = sw.Workload(
            workers=2,
            duration=3,
            registry=registry,
            metadata=metadata,
            node_factory=node.raw_connect,
        )
        wl.run()

        total = sum(s.total_success_count() for s in wl.worker_statistics())
        assert total > 0

        # deterministic asserts after the chaos
        assert node.sql_value("SELECT phase FROM audit_marker") == "setup done"
        for table in metadata.table_names():
            node.sql_value(f'SELECT count(*) FROM "{table}"')
        node.safe_sql("CREATE TABLE post_chaos (i int)")


if __name__ == "__main__":
    st.main()
