require("common")

-- produces various wal errors: incorrect magic number, incorrect resource provider,  record with incorrect prev-link, ...

function setup_tables(worker)
	-- comment out next line for non encrypted run
	init_pg_tde_only_for_db(worker:sql_connection())
	worker:create_random_tables(5)
	worker:generate_initial_data()
end

function conn_settings(sqlconn)
	-- comment out next line for non encrypted run
	sqlconn:execute_query("SET default_table_access_method=tde_heap;")
end

function main(argv)
	args = argparser:parse(argv)
	conffile = parse_config(args)
	pgconfig = PgConf.new(conffile["default"])

	pgm = PgManager.new(pgconfig)
	pgm:setupAndStartPrimary(conn_settings, { shared_preload_libraries = "pg_tde" })
	pgm.primaryNode:init(setup_tables)

	pg1 = pgm:get(1)

	t1 = pgm.primaryNode:initRandomWorkload({ run_seconds = 20, worker_count = 5 })
	for workloadIdx = 1, 50 do
		t1:run()

		t1:wait_completion()

		pg1:kill9()
		sleep(1000)
		pg1:start()

		pg1:wait_ready(200)

		t1:reconnect_workers()
	end

	pg1:stop(10)
end
