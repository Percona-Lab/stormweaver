
#include "action/helper.hpp"
#include "action/action.hpp"

namespace action {

metadata::table_cptr find_random_table(metadata::Metadata const &metaCtx,
                                       ps_random &rand) {
  if (metaCtx.size() == 0)
    throw ActionException("empty-metadata",
                          "Can't find random table: metadata is empty");

  for (int i = 0; i < 10; ++i) {
    // select a random table from metadata
    std::size_t idx = rand.random_number<std::size_t>(0, metaCtx.size() - 1);
    metadata::table_cptr table = metaCtx[idx];
    if (table != nullptr) {
      return table;
    }
  }

  throw ActionException("empty-metadata",
                        "Can't find random table: no result in 10 tries");
}

} // namespace action
