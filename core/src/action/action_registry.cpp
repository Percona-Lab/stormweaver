
#include <algorithm>
#include <numeric>
#include <utility>

#include "action/action_registry.hpp"
#include "action/dml.hpp"

namespace {

using namespace action;

struct TableRef {
  metadata::table_cptr ptr;
};

// built inside the function (not as namespace-scope statics) so construction
// happens on first use of the registry, not at program startup - avoids
// throwing-static-initialization / init-order issues for globals that build
// strings, functions and lambdas.
ActionRegistry initializeDefaultRegisty() {
  ActionRegistry ar;

  ActionFactory createNormalTable{
      .name = "create_normal_table",
      .builder =
          [](AllConfig const &config) {
            auto ctx = std::make_unique<TableRef>();
            auto createTable = std::make_unique<CreateTable>(
                config.ddl, metadata::Table::Type::normal);
            createTable->setSuccessCallback(
                [c = ctx.get()](metadata::table_cptr ptr) {
                  c->ptr = std::move(ptr);
                });
            return std::make_unique<CompositeAction<
                std::unique_ptr<TableRef>, std::unique_ptr<CreateTable>,
                std::unique_ptr<RepeatAction<InsertData>>>>(
                std::move(ctx), std::move(createTable),
                std::make_unique<RepeatAction<InsertData>>(
                    InsertData(config.dml, 1000,
                               [c = ctx.get()]() { return c->ptr; }),
                    1));
          },
      .weight = 100};

  ActionFactory createPartitionedTable{
      .name = "create_partitioned_table",
      .builder =
          [](AllConfig const &config) {
            auto ctx = std::make_unique<TableRef>();
            auto createTable = std::make_unique<CreateTable>(
                config.ddl, metadata::Table::Type::partitioned);
            createTable->setSuccessCallback(
                [c = ctx.get()](metadata::table_cptr ptr) {
                  c->ptr = std::move(ptr);
                });
            return std::make_unique<CompositeAction<
                std::unique_ptr<TableRef>, std::unique_ptr<CreateTable>,
                std::unique_ptr<RepeatAction<InsertData>>>>(
                std::move(ctx), std::move(createTable),
                std::make_unique<RepeatAction<InsertData>>(
                    InsertData(config.dml, 1000,
                               [c = ctx.get()]() { return c->ptr; }),
                    1));
          },
      .weight = 100};

  ActionFactory dropTable{.name = "drop_table",
                          .builder =
                              [](AllConfig const &config) {
                                return std::make_unique<DropTable>(config.ddl);
                              },
                          .weight = 100};

  ActionFactory alterTable{.name = "alter_table",
                           .builder =
                               [](AllConfig const &config) {
                                 return std::make_unique<AlterTable>(
                                     config.ddl,
                                     BitFlags<AlterSubcommand>::AllSet());
                               },
                           .weight = 100};

  ActionFactory renameTable{.name = "rename_table",
                            .builder =
                                [](AllConfig const &config) {
                                  return std::make_unique<RenameTable>(
                                      config.ddl);
                                },
                            .weight = 100};

  ActionFactory createIndex{.name = "create_index",
                            .builder =
                                [](AllConfig const &config) {
                                  return std::make_unique<CreateIndex>(
                                      config.ddl);
                                },
                            .weight = 100};

  ActionFactory dropIndex{.name = "drop_index",
                          .builder =
                              [](AllConfig const &config) {
                                return std::make_unique<DropIndex>(config.ddl);
                              },
                          .weight = 100};

  ActionFactory createPartition{.name = "create_partition",
                                .builder =
                                    [](AllConfig const &config) {
                                      return std::make_unique<CreatePartition>(
                                          config.ddl);
                                    },
                                .weight = 100};

  ActionFactory dropPartition{.name = "drop_partition",
                              .builder =
                                  [](AllConfig const &config) {
                                    return std::make_unique<DropPartition>(
                                        config.ddl);
                                  },
                              .weight = 100};

  ActionFactory insertSomeData{.name = "insert_some_data",
                               .builder =
                                   [](AllConfig const &config) {
                                     return std::make_unique<InsertData>(
                                         config.dml, 10);
                                   },
                               .weight = 1000};

  ActionFactory deleteSomeData{.name = "delete_some_data",
                               .builder =
                                   [](AllConfig const &config) {
                                     return std::make_unique<DeleteData>(
                                         config.dml);
                                   },
                               .weight = 1000};

  ActionFactory updateOneRow{.name = "update_one_row",
                             .builder =
                                 [](AllConfig const &config) {
                                   return std::make_unique<UpdateOneRow>(
                                       config.dml);
                                 },
                             .weight = 1000};

  ar.insert(createNormalTable);
  ar.insert(createPartitionedTable);
  ar.insert(dropTable);
  ar.insert(alterTable);
  ar.insert(renameTable);
  ar.insert(createIndex);
  ar.insert(dropIndex);
  ar.insert(createPartition);
  ar.insert(dropPartition);
  ar.insert(insertSomeData);
  ar.insert(deleteSomeData);
  ar.insert(updateOneRow);

  return ar;
}

} // namespace

namespace action {

ActionRegistry::ActionRegistry() = default;

ActionRegistry::ActionRegistry(ActionRegistry const &o) {
  std::unique_lock<std::mutex> lk(o.mutex);
  factories = o.factories;
};

ActionRegistry::ActionRegistry(ActionRegistry &&o) noexcept {
  std::unique_lock<std::mutex> lk(o.mutex);
  factories = std::move(o.factories);
};

ActionRegistry &ActionRegistry::operator=(ActionRegistry const &o) {
  factories = o.factories;
  return *this;
}

ActionRegistry &ActionRegistry::operator=(ActionRegistry &&o) noexcept {
  std::unique_lock<std::mutex> lk(o.mutex);
  factories = std::move(o.factories);
  return *this;
}

std::size_t ActionRegistry::insert(ActionFactory const &action) {

  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::ranges::find_if(
      factories, [&](auto const &f) { return f.name == action.name; });

  if (it != factories.end()) {
    throw ActionException(
        "action-already-exists",
        fmt::format("Action {} already exists in this registy", action.name));
  }

  factories.push_back(action);
  return factories.size() - 1;
}

void ActionRegistry::remove(std::string const &name) {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::ranges::find_if(factories,
                                 [&](auto const &f) { return f.name == name; });
  if (it == factories.end()) {
    throw ActionException(
        "action-not-found",
        fmt::format("Action {} does not exists in this registy", name));
  }

  factories.erase(it);
}

ActionFactory ActionRegistry::operator[](std::string const &name) const {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::ranges::find_if(factories,
                                 [&](auto const &f) { return f.name == name; });
  if (it == factories.end()) {
    throw ActionException(
        "action-not-found",
        fmt::format("Action {} does not exists in this registy", name));
  }
  return *it;
}

ActionFactory &ActionRegistry::getReference(std::string const &name) {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::ranges::find_if(factories,
                                 [&](auto const &f) { return f.name == name; });
  if (it == factories.end()) {
    throw ActionException(
        "action-not-found",
        fmt::format("Action {} does not exists in this registy", name));
  }
  // TODO issue: lock no longer held after return, detected by tsan
  return *it;
}

std::size_t ActionRegistry::size() const {
  std::unique_lock<std::mutex> lk(mutex);

  return factories.size();
}

std::size_t ActionRegistry::totalWeight() const {
  std::unique_lock<std::mutex> lk(mutex);

  return std::accumulate(factories.begin(), factories.end(), std::size_t{0},
                         [](std::size_t const &a, ActionFactory const &b) {
                           return a + b.weight;
                         });
}

bool ActionRegistry::has(std::string name) const {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::ranges::find_if(factories,
                                 [&](auto const &f) { return f.name == name; });
  return it != factories.end();
}

ActionFactory ActionRegistry::lookupByWeightOffset(std::size_t offset) const {
  std::unique_lock<std::mutex> lk(mutex);

  std::size_t accum = 0;
  auto it = std::ranges::find_if(factories, [&](auto const &f) {
    accum += f.weight;
    return accum >= offset;
  });

  if (it == factories.end()) {
    throw ActionException(
        "weight-offset-out-of-range",
        fmt::format("Weight offset {} is outside of this registy", offset));
  }

  return *it;
}

void ActionRegistry::makeCustomSqlAction(std::string const &name,
                                         std::string const &sql,
                                         std::size_t weight) {
  insert(ActionFactory{.name = name,
                       .builder =
                           [sql](AllConfig const &config) {
                             return std::make_unique<CustomSql>(
                                 config.custom, sql, CustomSql::inject_t{});
                           },
                       .weight = weight});
}

void ActionRegistry::makeCustomTableSqlAction(std::string const &name,
                                              std::string const &sql,
                                              std::size_t weight) {
  insert(ActionFactory{.name = name,
                       .builder =
                           [sql](AllConfig const &config) {
                             return std::make_unique<CustomSql>(
                                 config.custom, sql,
                                 CustomSql::inject_t{"table"});
                           },
                       .weight = weight});
}

ActionRegistry &default_registy() {
  static ActionRegistry instance = initializeDefaultRegisty();
  return instance;
}

void ActionRegistry::use(ActionRegistry const &other) { *this = other; }

} // namespace action
