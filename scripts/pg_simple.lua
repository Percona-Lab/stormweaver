require("common")
require("entropy")

use_tde = false

function db_setup(worker)
	if use_tde then
		init_pg_tde_only_for_db(worker:sql_connection())
	end
	worker:create_random_tables(5)
end

function conn_settings(sqlconn)
	if use_tde then
		sqlconn:execute_query("SET default_table_access_method=tde_heap;")
	end
end

function add_standard_actions(reg)
	reg:makeCustomSqlAction("checkpoint", "CHECKPOINT;", 10)
	reg:makeCustomTableSqlAction("vacuum_full_table", "VACUUM FULL {table};", 20)
	reg:makeCustomTableSqlAction("truncate_table", "TRUNCATE {table};", 20)
	reg:makeCustomTableSqlAction("reindex_table", "REINDEX TABLE {table};", 20)
end

argparser:option("-d --duration", "Duration in seconds", "10")
argparser:option("-w --workers", "Number of workers", "5")
argparser:option("-r --repeat", "Number of workloads / repeats", "50")

argparser:option("--tde")({
	choices = { "on", "off" },
	default = "on",
})
argparser:option("--pgsm")({
	choices = { "on", "off" },
	default = "off",
})

function simple_pg_single(argv, test)
	args = argparser:parse(argv)

	default_reg = defaultActionRegistry()
	add_standard_actions(default_reg)

	conffile = parse_config(args)
	pgconfig = PgConf.new(conffile["default"])
	pgm = PgManager.new(pgconfig)

	additional_settings = { shared_preload_libraries = "" }

	if args["tde"] == "on" then
		info("Running tests with pg_tde")
		use_tde = true
		additional_settings["shared_preload_libraries"] = additional_settings["shared_preload_libraries"] .. "pg_tde"
	end
	if args["pgsm"] == "on" then
		info("Running tests with pg_stat_monitor")
		additional_settings["shared_preload_libraries"] = additional_settings["shared_preload_libraries"]
			.. "pg_stat_monitor"
	end

	pgm:setupAndStartPrimary(conn_settings, additional_settings)
	pgm.primaryNode:init(db_setup)

	params = WorkloadParams.new()
	params.duration_in_seconds = tonumber(args["duration"])
	params.number_of_workers = tonumber(args["workers"])
	t1 = pgm.primaryNode:initRandomWorkload(params)

	test(args)
end
