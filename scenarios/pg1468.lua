require("common")

-- produces various wal errors: incorrect magic number, incorrect resource provider,  record with incorrect prev-link, ...

function setup_tables(worker)
	init_pg_tde_globally(worker:sql_connection())
	worker:create_random_tables(5)
	worker:generate_initial_data()
end

function conn_settings(sqlconn)
	sqlconn:execute_query("SET default_table_access_method=tde_heap;")
end

function setupAdditionalActions()
	-- todo: a random number injection would be nice
	-- self.datadirs[datadir] = true
	defaultActionRegistry():makeCustomSqlAction(
		"sp1",
		"SELECT pg_tde_set_server_key('principal_key_test1','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp2",
		"SELECT pg_tde_set_server_key('principal_key_test2','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp3",
		"SELECT pg_tde_set_server_key('principal_key_test3','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp4",
		"SELECT pg_tde_set_server_key('principal_key_test4','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp5",
		"SELECT pg_tde_set_server_key('principal_key_test5','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp6",
		"SELECT pg_tde_set_server_key('principal_key_test6','reg_file','false');",
		5
	)
	--defaultActionRegistry():makeCustomSqlAction("as1", "ALTER SYSTEM SET pg_tde.wal_encrypt = on;", 10)
	--defaultActionRegistry():makeCustomSqlAction("as2", "ALTER SYSTEM SET pg_tde.wal_encrypt = off;", 10)
end

function main(argv)
	setupAdditionalActions()

	args = argparser:parse(argv)
	conffile = parse_config(args)
	pgconfig = PgConf.new(conffile["default"])

	pgm = PgManager.new(pgconfig)
	pgm:setupAndStartPrimary(conn_settings)
	pgm:setupAndStartAReplica(conn_settings)

	pgm.primaryNode:init(setup_tables)

	pg1 = pgm:get(1)
	pg2 = pgm:get(2)

	t1 = pgm.primaryNode:initRandomWorkload({ run_seconds = 20, worker_count = 5 })
	for workloadIdx = 1, 50 do
		t1:run()

		sleep(33000)

		pg2:kill9()
		pg2:start()

		if not pg2:is_running() then
			error("Replica can't start up after kill9")
			return
		end

		t1:wait_completion()

		info("Waiting for replica to become available")

		pg1:restart(10)

		t1:reconnect_workers()

		pg2:wait_ready(200)
	end

	pg1:stop(10)
end
