-- This is a very basic sample testcase what stormweaver can do with some explanatory comments
--
-- Generally when writing scripts, keep in mind that each stormweaver worker thread will load this file and process it
-- (not yet the case, but will be in the near future)
-- That is to make sure that all workers can work with customizations here
-- To avoid issues related to this, put only variables/function definitions to the global scope, no statement executions

-- found in scripts/
-- additional files can be added and used there, and of course existing files edited
-- idea is that common helper code should go there
require("tde_helper")

-- log messages, debug, info, warn, error, only a single string parameter for now (TODO: variable arguments)
info("Loading script!")

-- this is a callback function referenced later, used to initialize the database
function db_setup(worker)
	-- for example configuring pg_tde for it
	init_pg_tde_only_for_db(worker:sql_connection())
	-- or creating tables and loading some data
	worker:create_random_tables(5)
	worker:generate_initial_data() -- TODO: this method needs some parameters...
end

-- another callback function, called after establishing any database connection
function conn_settings(sqlconn)
	-- note: this is executed even before db_setup above, but it's not an issue, a failure won't stop us
	sqlconn:execute_query("SET default_table_access_method=tde_heap;")
end

-- main function executed directly by stormweaver
function main()
	datadir = getenv("PG_DATADIR", "datadir")
	installdir = getenv("PG_INST", "/home/dutow/work/pg17inst/")

	-- The first initializes a new datadir, the second uses an existing data dir
	-- we need some additional helpers here like a check to see if the directory exists
	pg = initPostgresDatadir(installdir, datadir)
	-- n2 = registerPostgresDatadir('/home/dutow/work/pg17inst/', 'datadir', {params})

	-- Appends to the server configuration, can also overwrite existing settings, as postgres only uses the last
	-- entry if there are duplicates
	pg:add_config({ shared_preload_libraries = "pg_tde" })

	if not pg:start() then
		-- returning from main stops execution
		error("Node couldn't start")
		return
	end

	pg:dropdb("stormweaver")
	pg:createdb("stormweaver")

	-- Actions (mostly sql statements) are kept in action registries.
	-- There an be multiple of them, this just takes a reference to the default registry to a variable
	-- nodes/workers (later) also copy the registry, so modifications of the default registry won't affect
	-- things that were already set up
	default_ref = defaultActionRegistry()

	-- We can add custom SQL statements to registries, this example vacuums a single table
	default_ref:makeCustomTableSqlAction("vacuum_full_table", "VACUUM FULL {table};", 2)

	-- creates a "node", which is just a connection setup
	n1 = setup_node_pg({
		host = "localhost",
		port = 5432,
		user = "dutow",
		password = "",
		database = "stormweaver",
		on_connect = conn_settings,
	})

	-- Modifies the default registry again, but the node already copied the default registry above
	-- this doesn't affect the existing node
	default_ref:makeCustomTableSqlAction("truncate_table", "TRUNCATE {table};", 2)

	-- initializes the node, this calls the db_setup callback above
	n1:init(db_setup)

	-- we can also modify the registry of the node directly
	-- this doesn't affect the default registry
	n1:possibleActions():makeCustomTableSqlAction("reindex", "REINDEX TABLE {table};", 1)

	-- creates a workload
	-- similarly this copies the registry from the node to the workers,
	-- later modifications to the node won't be effective
	t1 = n1:initRandomWorkload({ run_seconds = 10, worker_count = 5 })

	-- this modifies the second worker to use the latest version of the default registry
	-- effect: worker 2 will run truncate, but not reindex
	t1:worker(2):possibleActions():use(default_ref)

	-- change the weight of alter table for worker 3
	t1:worker(3):possibleActions():get("alter_table").weight = 127

	-- add a custom action to worker 4. This is not table based
	t1:worker(4):possibleActions():makeCustomSqlAction("checkpoint", "CHECKPOINT;", 1)

	for workloadIdx = 1, 50 do
		-- Start the workload
		t1:run()

		-- one second later, no longer execute alter table at all on worker 3
		sleep(1000)
		if workloadIdx == 1 then
			t1:worker(3):possibleActions():remove("alter_table")
		end

		-- wait for the tests to complete
		t1:wait_completion()

		-- restart the server (TODO: kill9 not yet implemented)
		pg:restart(10)

		t1:reconnect_workers()
	end

	pg:stop(10)
end
