
#include <sol/sol.hpp>

class LuaCtx {
public:
  bool process_file(std::string const &filename);

private:
  sol::state lua;
};
