
#pragma once

#include "action/action.hpp"
#include "bitflags.hpp"

namespace action {

struct DdlConfig {
  std::size_t min_table_count = 3;
  std::size_t max_table_count = 10;
  std::size_t max_column_count = 20;
  std::size_t max_alter_clauses = 5;
  std::vector<std::string> access_methods = {"heap", "tde_heap"};
};

class CreateTable : public Action {
public:
  CreateTable(DdlConfig const &config, metadata::Table::Type type);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DdlConfig config;
  metadata::Table::Type type;
};

class DropTable : public Action {
public:
  DropTable(DdlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DdlConfig config;
};

enum class AlterSubcommand {
  // TODO: implement more
  addColumn = 1 << 0,
  dropColumn = 1 << 1,
  changeColumn = 1 << 2,
  changeAccessMethod = 1 << 3,
};

class AlterTable : public Action {
public:
  AlterTable(DdlConfig const &config,
             BitFlags<AlterSubcommand> const &possibleCommands);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DdlConfig config;
  BitFlags<AlterSubcommand> possibleCommands;
};

class RenameTable : public Action {
public:
  RenameTable(DdlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DdlConfig config;
};

class CreateIndex : public Action {
public:
  CreateIndex(DdlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DdlConfig config;
};

class DropIndex : public Action {
public:
  DropIndex(DdlConfig const &config);

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  DdlConfig config;
};

}; // namespace action
