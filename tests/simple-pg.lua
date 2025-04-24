package.path = "scripts/?.lua;scripts_3p/?.lua;" .. package.path

require("common")
function db_setup(worker)
	worker:create_random_tables(5)
	worker:generate_initial_data()
end

function main(argv)
	args = parse_args(argv)
	conffile = parse_config(args)
	pgconfig = PgConf.new(conffile["default"])

	pgm = PgManager.new(pgconfig)
	pgm:setupAndStartPrimary()

	pgm.primaryNode:init(db_setup)

	t1 = pgm.primaryNode:initRandomWorkload({ run_seconds = 10, worker_count = 2 })

	t1:run()
	t1:wait_completion()

	pgm:get(1):stop(10)
end
