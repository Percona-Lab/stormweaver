local module = {}
module.__index = module

function module.new (config)
    o = { config = config, currentPort = tonumber(config["port_start"]) }
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

  return module