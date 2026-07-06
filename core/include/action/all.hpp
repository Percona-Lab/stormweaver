
#pragma once

#include "action/composite.hpp"
#include "action/custom.hpp"
#include "action/ddl.hpp"
#include "action/dml.hpp"

namespace action {

struct AllConfig {
  DdlConfig ddl;
  DmlConfig dml;
  CustomConfig custom;
};

} // namespace action
