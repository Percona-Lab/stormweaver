require("common")
require("entropy")

use_tde = false

function db_setup(worker)
	if use_tde == "on" then
		init_pg_tde_only_for_db(worker:sql_connection())
	end
	if use_tde == "on_wal" then
		init_pg_tde_globally(worker:sql_connection())
	end
	worker:create_random_tables(5)
end

function conn_settings(sqlconn)
	if use_tde ~= "off" then
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
	choices = { "on", "on_wal", "off" },
	default = "on",
})
argparser:option("--pgsm")({
	choices = { "on", "off" },
	default = "off",
})

argparser:flag("--clear-logs")

function simple_pg_single(argv, test)
	args = argparser:parse(argv)

	if args["clear_logs"] then
		info("Clearing old log files")
		fs.delete_directory("logs")
	end

	default_reg = defaultActionRegistry()
	add_standard_actions(default_reg)

	conffile = parse_config(args)
	pgconfig = PgConf.new(conffile["default"])
	pgm = PgManager.new(pgconfig)

	additional_settings = { shared_preload_libraries = "", summarize_wal = "on" }

	if args["tde"] ~= "off" then
		info("Running tests with pg_tde = " .. args["tde"])
		use_tde = args["tde"]
		additional_settings["shared_preload_libraries"] = additional_settings["shared_preload_libraries"] .. "pg_tde"
	end
	if args["pgsm"] == "on" then
		info("Running tests with pg_stat_monitor")
		additional_settings["shared_preload_libraries"] = additional_settings["shared_preload_libraries"]
			.. "pg_stat_monitor"
	end

	pgm:setupAndStartPrimary(conn_settings, additional_settings)
	pgm.primaryNode:init(db_setup)

	pgm:restart(1, 10) -- Restart as init can potentially use ALTER SYSTEM

	params = WorkloadParams.new()
	params.duration_in_seconds = tonumber(args["duration"])
	params.number_of_workers = tonumber(args["workers"])
	t1 = pgm.primaryNode:initRandomWorkload(params)

	test(args)
end
