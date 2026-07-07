
#include "action/helper.hpp"
#include "action/action.hpp"

namespace action {

metadata::table_cptr find_random_table(metadata::Context const &metaCtx,
                                       ps_random &rand) {
  auto table = metaCtx.get<metadata::Table>().randomPick(rand);
  if (table == nullptr) {
    throw ActionException("empty-metadata",
                          "Can't find random table: metadata is empty");
  }
  return table;
}

} // namespace action
