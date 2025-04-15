
#pragma once

#include "action/action.hpp"

namespace action {

struct DmlConfig {
  std::size_t deleteMin = 1;
  std::size_t deleteMax = 100;
};

class UpdateOneRow : public Action {
public:
  UpdateOneRow(DmlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
  std::size_t rows;
};

class DeleteData : public Action {
public:
  DeleteData(DmlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
  std::size_t rows;
};

class InsertData : public Action {
public:
  InsertData(DmlConfig const &config, metadata::table_cptr table,
             std::size_t rows);
  InsertData(DmlConfig const &config, std::size_t rows);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
  metadata::table_cptr table;
  std::size_t rows;
};

}; // namespace action
