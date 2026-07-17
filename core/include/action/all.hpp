
#pragma once

#include "action/composite.hpp"
#include "action/custom.hpp"
#include "action/ddl.hpp"
#include "action/dml.hpp"
#include "action/transaction_config.hpp"
#include "action/variable.hpp"
#include "querygen/config.hpp"

namespace action {

struct AllConfig {
  DdlConfig ddl;
  DmlConfig dml;
  CustomConfig custom;
  TransactionConfig transaction;
  querygen::QueryGenConfig querygen;
  VariableConfig variables;
};

} // namespace action
