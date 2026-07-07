#pragma once

#include "action/action.hpp"

namespace action {

// Throws ActionException("empty-metadata") when there are no tables.
metadata::table_cptr find_random_table(metadata::Context const &metaCtx,
                                       ps_random &rand);

} // namespace action
