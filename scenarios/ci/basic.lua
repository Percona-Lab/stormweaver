require("pg_simple")

function main(argv)
	simple_pg_single(argv, function(args)
		for workloadIdx = 1, tonumber(args["repeat"]) do
			t1:run()
			t1:wait_completion()
			pgm:get(1):restart(10)

			w = pgm.primaryNode:make_worker("verification")
			db_files_entropy(w, "datadirs/datadir_pr/")
		end

		pgm:get(1):stop(10)
	end)
end
