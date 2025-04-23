local module = {}
module.__index = module

function module.new(config)
	o = { config = config, currentPort = tonumber(config["port_start"]), datadirs = {} }
	setmetatable(o, module)
	return o
end

function module:nextPort()
	p = self.currentPort
	self.currentPort = self.currentPort + 1
	return p
end

function module:pgroot()
	return self.config["pgroot"]
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
