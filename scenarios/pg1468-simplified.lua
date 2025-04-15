
require 'common'

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

function main()
	confdir = getenv("SWCONF", "config/")
    conffile = toml.decodeFromFile(confdir .. "/stormweaver.toml")
	pgconfig = PgConf.new(conffile["default"])

	installdir = pgconfig:pgroot()
    primary_port = pgconfig:nextPort()

        --pgm = PgManager.setup(pgconfig)
	--pgm:setupPrimary() -- server 0
	--pgm:start(0)
	--pgm:setupAndStartStreamingReplicas(2):wait() -- server 1,2

	primary_datadir = pgconfig:useDatadir("datadir_pr")

	pg1 = initPostgresDatadir(installdir, primary_datadir)

	pg1:add_config({
		shared_preload_libraries = "pg_tde",
		port = tostring(primary_port),
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

	pg1:createdb("stormweaver")
	pg1:createuser("stormweaver", {"-s"})

	n1 = setup_node_pg({
		host = "localhost",
		port = primary_port,
		user = "stormweaver",
		password = "",
		database = "stormweaver",
		on_connect = conn_settings,
	})

	n1:init(setup_tables)

	t1 = n1:initRandomWorkload({ run_seconds = 20, worker_count = 5 })
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
