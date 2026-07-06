
#pragma once

#include "action/action.hpp"
#include "metadata.hpp"

namespace action {

metadata::table_cptr find_random_table(metadata::Metadata const &metaCtx,
                                       ps_random &rand);

}
