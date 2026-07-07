
#pragma once

#include "action/action.hpp"

#include <functional>

namespace action {

struct LockWeights {
  std::size_t none = 1;
  std::size_t for_update = 1;
  std::size_t for_share = 1;
};

struct DmlConfig {
  std::size_t deleteMin = 1;
  std::size_t deleteMax = 100;
  std::size_t updateMin = 1;
  std::size_t updateMax = 10;
  LockWeights lockWeights;
};

using TableLocator = std::function<metadata::table_cptr()>;

class UpdateOneRow : public Action {
public:
  UpdateOneRow(DmlConfig const &config);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
};

class DeleteData : public Action {
public:
  DeleteData(DmlConfig const &config);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
};

/* SELECT random pks first, modify them in a second statement. Relies on
   the enclosing transaction when inside TransactionAction; otherwise
   wraps itself in BEGIN...COMMIT. */
class SelectThenDelete : public Action {
public:
  SelectThenDelete(DmlConfig const &config);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
};

class SelectThenUpdate : public Action {
public:
  SelectThenUpdate(DmlConfig const &config);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
};

class InsertData : public Action {
public:
  InsertData(DmlConfig const &config, std::size_t rows, TableLocator locator);
  InsertData(DmlConfig const &config, std::size_t rows);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DmlConfig config;
  TableLocator locator;
  std::size_t rows;
};

}; // namespace action
