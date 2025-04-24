local module = {}
module.__index = module

function module.new(pgconfig)
	o = {
		pgconfig = pgconfig,
		primary_setup = false,
		servers = {},
		primaryNode = nil,
		primaryReplNode = nil,
		nextReplica = 1,
	}
	setmetatable(o, module)
	return o
end

function module:start(index)
	if not self.servers[index]:start() then
		error("Node couldn't start")
	end
end

function module:createdb(...)
	local t = { ... }
	return self.servers[1]:createdb(table.unpack(t))
end

function module:createuser(...)
	local t = { ... }
	return self.servers[1]:createuser(table.unpack(t))
end

function module:get(index)
	return self.servers[index]
end

function module:connect(index, user, db, conn_callback)
	return setup_node_pg({
		host = "localhost",
		port = tonumber(self.servers[index]:serverPort()),
		user = user,
		password = "",
		database = db,
		on_connect = conn_callback,
	})
end

function module:setupAndStartPrimary(connectionCallback, extra_args)
	if primary_setup == true then
		error("setupPrimary called multiple times")
	end
	installdir = pgconfig:pgroot()

	primary_port = pgconfig:nextPort()
	primary_datadir = pgconfig:useDatadir("datadir_pr")
	primary = initPostgresDatadir(installdir, primary_datadir)

	fs.create_directory(primary_datadir .. "/sock/")

	if extra_args ~= nil then
		primary:add_config(extra_args)
	end
	primary:add_config({
		port = tostring(primary_port),
		listen_addresses = "'*'",
		logging_collector = "on",
		log_directory = "'logs'",
		log_filename = "'server.log'",
		log_min_messages = "'info'",
		unix_socket_directories = "sock",
	})

	primary:add_hba("host", "replication", "repuser", "127.0.0.1/32", "trust")

	table.insert(self.servers, primary)

	primaryIdx = #self.servers

	self:start(primaryIdx)

	self:createdb("stormweaver")
	self:createuser("stormweaver", { "-s" })

	self.primaryNode = self:connect(1, "stormweaver", "stormweaver", connectionCallback)
	self.primaryNode:init(function(worker)
		worker:sql_connection():execute_query("CREATE USER repuser replication; SELECT pg_reload_conf();")
	end)

	return primaryIdx
end

function module:setupAndStartAReplica(connectionCallback, extra_args)
	if self.primaryReplNode == nil then
		self.primaryReplNode = self:connect(1, "repuser", "stormweaver", connectionCallback)
	end

	replicaId = tostring(self.nextReplica)
	self.nextReplica = self.nextReplica + 1

	installdir = pgconfig:pgroot()
	replica_port = pgconfig:nextPort()
	replica_datadir = pgconfig:useDatadir("datadir_repl" .. replicaId)

	pgRep = initBasebackupFrom(
		installdir,
		replica_datadir,
		self.primaryReplNode,
		"--checkpoint=fast",
		"-R",
		"-C",
		"--slot=slot" .. replicaId
	)

	table.insert(self.servers, pgRep)
	replicaIdx = #self.servers

	fs.create_directory(replica_datadir .. "/sock/")

	if extra_args ~= nil then
		primary:add_config(extra_args)
	end
	pgRep:add_config({
		port = tostring(replica_port),
		listen_addresses = "'*'",
		logging_collector = "on",
		log_directory = "'logs'",
		log_filename = "'server.log'",
		log_min_messages = "'info'",
		unix_socket_directories = replica_datadir .. "/sock/",
	})

	if not pgRep:start() then
		error("Replica couldn't start")
	end

	info("Waiting for replica " .. replicaId .. " to become available")
	pgRep:wait_ready(200) -- TODO: should be less

	return replicaIdx
end

function module:pgroot()
	conf_value = self.config["pgroot"]
	if conf_value == nil then
		conf_value = ""
	end
	return getenv("PGROOT", self.config["pgroot"])
end

function module:datadirPath(datadir)
	return self.config["datadir_root"] .. "/" .. datadir
end

function module:useDatadir(datadir)
	path = self:datadirPath(datadir)
	if not self.datadirs[datadir] then
		if fs.is_directory(path) then
			warning("datadir '" .. path .. "' already exist, deleting.")
			fs.delete_directory(path)
		end
		self.datadirs[datadir] = true
	end

	return path
end

return module
