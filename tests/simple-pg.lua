package.path = "scripts/?.lua;scripts_3p/?.lua;" .. package.path

require("common")
function db_setup(worker)
	worker:create_random_tables(5)
end

function main(argv)
	args = parse_args(argv)
	conffile = parse_config(args)
	pgconfig = PgConf.new(conffile["default"])

	pgm = PgManager.new(pgconfig)
	pgm:setupAndStartPrimary()

	pgm.primaryNode:init(db_setup)

	params = WorkloadParams.new()
	params.duration_in_seconds = 10
	params.number_of_workers = 2
	t1 = pgm.primaryNode:initRandomWorkload(params)

	t1:run()
	t1:wait_completion()

	pgm:get(1):stop(10)
end
