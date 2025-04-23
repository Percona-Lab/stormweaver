local argparse = require("argparse")

argparser = argparse("stormweaver", "")
argparser:argument("scenario", "Scenario file")
argparser:option("-c --config", "Configuration file.", "config/stormweaver.toml")
argparser:option(
	"-i --install_dir",
	"PostgreSQL installation directory (if specified, overrides configuration value for default server)",
	""
)

function parse_config(args)
	conffile = toml.decodeFromFile(args["config"])
	if string.len(args["install_dir"]) > 0 then
		info("Overriding default postgres installation directory with " .. args["install_dir"])
		conffile["default"]["pgroot"] = args["install_dir"]
	end

	return conffile
end
