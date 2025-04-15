require("tde_helper")

-- produces various wal errors: incorrect magic number, incorrect resource provider,  record with incorrect prev-link, ...

function init_replication_on_primary(worker)
	worker:sql_connection():execute_query("CREATE USER repuser replication;")
	init_pg_tde_globally(worker:sql_connection())
	worker:create_random_tables(5)
	worker:generate_initial_data()
end

function conn_settings(sqlconn)
	sqlconn:execute_query("SET default_table_access_method=tde_heap;")
end

function main()
	installdir = getenv("PG_INST", "/home/dutow/work/pg17inst/")
	primary_datadir = "datadir_pr"
	replica_datadir = "datadir_repl"

	-- todo: a random number injection would be nice
	defaultActionRegistry():makeCustomSqlAction(
		"sp1",
		"SELECT pg_tde_set_server_principal_key('principal_key_test1','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp2",
		"SELECT pg_tde_set_server_principal_key('principal_key_test2','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp3",
		"SELECT pg_tde_set_server_principal_key('principal_key_test3','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp4",
		"SELECT pg_tde_set_server_principal_key('principal_key_test4','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp5",
		"SELECT pg_tde_set_server_principal_key('principal_key_test5','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction(
		"sp6",
		"SELECT pg_tde_set_server_principal_key('principal_key_test6','reg_file','false');",
		5
	)
	defaultActionRegistry():makeCustomSqlAction("as1", "ALTER SYSTEM SET pg_tde.wal_encrypt = on;", 10)
	defaultActionRegistry():makeCustomSqlAction("as2", "ALTER SYSTEM SET pg_tde.wal_encrypt = off;", 10)

	pg1 = initPostgresDatadir(installdir, primary_datadir)

	pg1:add_config({
		shared_preload_libraries = "pg_tde",
		port = "5432",
		listen_addresses = "'*'",
		logging_collector = "on",
		log_directory = "'logs'",
		log_filename = "'server.log'",
		log_min_messages = "'info'",
	})

	pg1:add_hba("host", "replication", "repuser", "127.0.0.1/32", "trust")

	if not pg1:start() then
		error("Node couldn't start")
		return
	end

	pg1:dropdb("stormweaver")
	pg1:createdb("stormweaver")

	n1 = setup_node_pg({
		host = "localhost",
		port = 5432,
		user = "dutow",
		password = "",
		database = "stormweaver",
		on_connect = conn_settings,
	})

	n1:init(init_replication_on_primary)
	pg1:restart(10)

	n1repl = setup_node_pg({
		host = "localhost",
		port = 5432,
		user = "repuser",
		password = "",
		database = "stormweaver",
		on_connect = conn_settings,
	})

	pg2 = initBasebackupFrom(installdir, replica_datadir, n1repl, "--checkpoint=fast", "-R", "-C", "--slot=slotname")

	pg2:add_config({
		shared_preload_libraries = "pg_tde",
		port = "5433",
		listen_addresses = "'*'",
		logging_collector = "on",
		log_directory = "'logs'",
		log_filename = "'server.log'",
		log_min_messages = "'info'",
	})

	if not pg2:start() then
		error("Replica couldn't start")
		return
	end

	info("Waiting for replica to become available")
	pg2:wait_ready(200)

	t1 = n1:initRandomWorkload({ run_seconds = 20, worker_count = 5 })
	for workloadIdx = 1, 50 do
		t1:run()

		sleep(33000)

		pg2:kill9()
		-- todo: add functionality to wait until it properly started up
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
