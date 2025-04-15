
#include <spdlog/spdlog.h>

#include "scripting/luactx.hpp"

int main(int argc, char **argv) {

  spdlog::set_level(spdlog::level::debug);

  spdlog::info("Starting stormweaver");

  if (argc < 2) {
    spdlog::error("Not enough arguments! Usage: stormweaver <scenario_name>");
    return 1;
  }

  LuaContext ctx(spdlog::default_logger());

  ctx.loadScript(argv[1]);

  auto &lua = ctx.ctx();

  // run main function

  auto script_main = lua.get<sol::protected_function>("main");

  if (script_main.valid()) {
    spdlog::info("Starting lua main");
    sol::protected_function_result main_result = script_main();
    if (!main_result.valid()) {
      sol::error err = main_result;
      spdlog::error("Scenario script main function failed: {}", err.what());
      return 3;
    }
  } else {
    spdlog::error("Script doesn't contain a main function, doing nothing");
    return 4;
  }

  spdlog::info("Pstress exiting normally");

  return 0;
}
