
#pragma once

#include "action/composite.hpp"
#include "action/custom.hpp"
#include "action/ddl.hpp"
#include "action/dml.hpp"
#include "action/transaction_config.hpp"

namespace action {

struct AllConfig {
  DdlConfig ddl;
  DmlConfig dml;
  CustomConfig custom;
  TransactionConfig transaction;
};

} // namespace action
