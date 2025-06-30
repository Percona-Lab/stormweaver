
#pragma once

#include "action/action.hpp"

#include <functional>

namespace action {

struct DmlConfig {
  std::size_t deleteMin = 1;
  std::size_t deleteMax = 100;
};

using TableLocator = std::function<metadata::table_cptr()>;

class UpdateOneRow : public Action {
public:
  UpdateOneRow(DmlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
};

class DeleteData : public Action {
public:
  DeleteData(DmlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
};

class InsertData : public Action {
public:
  InsertData(DmlConfig const &config, std::size_t rows,
             TableLocator const &locator);
  InsertData(DmlConfig const &config, std::size_t rows);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
  TableLocator locator;
  std::size_t rows;
};

}; // namespace action
