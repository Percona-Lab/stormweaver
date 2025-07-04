-- Work in progress testcase / helper script for metadata validation
-- As currently it's not perfect, this can't be a proper automatic test, as it fails

require("common")
require("entropy")

function db_setup(worker)
	worker:create_random_tables(5)
end

function conn_settings(sqlconn) end

function main(argv)
	default_ref = defaultActionRegistry()

	default_ref:makeCustomTableSqlAction("vacuum_full_table", "VACUUM FULL {table};", 2)

	args = argparser:parse(argv)
	conffile = parse_config(args)

	pgconfig = PgConf.new(conffile["default"])

	pgm = PgManager.new(pgconfig)
	pgm:setupAndStartPrimary(conn_settings, { shared_preload_libraries = "pg_tde" })

	default_ref:makeCustomTableSqlAction("truncate_table", "TRUNCATE {table};", 2)

	pgm.primaryNode:init(db_setup)

	pgm.primaryNode:possibleActions():makeCustomTableSqlAction("reindex", "REINDEX TABLE {table};", 1)

	params = WorkloadParams.new()
	params.duration_in_seconds = 10
	params.number_of_workers = 1
	t1 = pgm.primaryNode:initRandomWorkload(params)

	for workloadIdx = 1, 500 do
		t1:run()
		pgm:get(1):restart(10)

		w = pgm.primaryNode:make_worker("verification")
		if not w:validate_metadata() then
			error("Internal erorr, metadata validation failed: either metadata or metadata_populator bug")
		end
	end

	pgm:get(1):stop(10)
end
