require("pg_simple")

function main(argv)
	if fs.is_directory("backups") then
		warning("Deleting old stormweaver run backups")
		fs.delete_directory("backups")
	end

	simple_pg_single(argv, function(args)
		pgm:basebackup(1, "-D", "backups/backup_0", "-U", "stormweaver", "-c", "fast")

		for workloadIdx = 1, tonumber(args["repeat"]) do
			t1:run()
			t1:wait_completion()
			pgm:get(1, 10)

			prevBackupDir = "backups/backup_" .. tostring(workloadIdx - 1)
			backupDir = "backups/backup_" .. tostring(workloadIdx)

			pgm:basebackup(
				1,
				"-D",
				backupDir,
				"-U",
				"stormweaver",
				"-c",
				"fast",
				"-i",
				prevBackupDir .. "/backup_manifest"
			)

			w = pgm.primaryNode:make_worker("verification")
			w:calculate_database_checksums(backupDir .. ".checksum")
		end

		combine = { "-o", "datadirs/datadir_pr" }
		table.insert(combine, "backups/backup_0")

		for workloadIdx = 1, tonumber(args["repeat"]) do
			info("Restoring and verifying incremental #" .. tostring(workloadIdx))
			pgm:stop(1, 10)
			fs.delete_directory("datadirs/datadir_pr")

			table.insert(combine, "backups/backup_" .. tostring(workloadIdx))
			pgm:combinebackup(table.unpack(combine))
			pgm:start(1)

			backupChecksumFile = "backups/backup_" .. tostring(workloadIdx) .. ".checksum"
			checksumFile = "datadirs/datadir_pr/db.checksum"
			w = pgm.primaryNode:make_worker("reset")
			w:reset_metadata()
			w:discover_existing_schema()
			w:calculate_database_checksums(checksumFile)

			bp = BackgroundProcess.start("checksumdiff", "/usr/bin/diff", checksumFile, backupChecksumFile)
			if not bp:waitUntilExits() then
				error("Backup checksum verification failed with incremental #" .. tostring(workloadIdx))
			end
		end
	end)
end
