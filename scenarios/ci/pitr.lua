require("pg_simple")

function tail_grep(pattern, file, timeout)
	for s = 1, timeout do
		if os.execute("grep '" .. pattern .. "' '" .. file .. "'") then
			return true
		end
		sleep(1000)
	end
	return false
end

function main(argv)
	if fs.is_directory("backups") then
		warning("Deleting old stormweaver run backups")
		fs.delete_directory("backups")
	end

	if fs.is_directory("archive") then
		warning("Deleting old stormweaver run archive")
		fs.delete_directory("archive")
	end

	os.execute("mkdir archive")

	simple_pg_single(argv, function(args)
		pgm:basebackup(1, "-D", "backups/backup_0", "-U", "stormweaver", "-c", "fast")

		times = {}

		for workloadIdx = 1, tonumber(args["repeat"]) + 1 do
			t1:run()
			t1:wait_completion()
			pgm:get(1, 10)

			sleep(3000)

			t = os.time()

			w = pgm.primaryNode:make_worker("verification")
			w:calculate_database_checksums("backups/" .. t .. ".checksum")

			table.insert(times, t)

			sleep(3000)
		end

		fs.delete_directory("archive-copy")
		os.execute("cp -r archive archive-copy")

		for workloadIdx = 1, tonumber(args["repeat"]) do
			pgm:stop(1, 10)

			fs.delete_directory("archive")
			os.execute("cp -r archive-copy archive")

			info("Restoring and verifying PITR #" .. tostring(workloadIdx) .. " - " .. tostring(times[workloadIdx]))
			fs.delete_directory("datadirs/datadir_pr")

			os.execute("cp -r backups/backup_0 datadirs/datadir_pr")
			os.execute("touch datadirs/datadir_pr/recovery.signal")

			pgm:get(1):add_config({
				restore_command = "'cp " .. fs.absolute("archive/%f") .. ' "%p"\'',
				recovery_target_time = "'" .. os.date("%Y-%m-%d %X", tostring(times[workloadIdx])) .. "'",
			})

			pgm:start(1)

			if not tail_grep("pausing at the end of recovery", "datadirs/datadir_pr/logs/server.log", 200) then
				error("Couldn't complete PITR")
			end

			backupChecksumFile = "backups/" .. tostring(times[workloadIdx]) .. ".checksum"
			checksumFile = "datadirs/datadir_pr/db.checksum"
			w = pgm.primaryNode:make_worker("reset")

			w:sql_connection():execute_query("SELECT pg_wal_replay_resume()")
			w:reset_metadata()
			w:discover_existing_schema()
			w:calculate_database_checksums(checksumFile)

			ret = os.execute("diff " .. checksumFile .. " " .. backupChecksumFile)
			if not ret then
				error("Backup checksum verification failed with incremental #" .. tostring(workloadIdx))
			end
		end
	end)
end
