
#include <numeric>

#include "action/action_registry.hpp"
#include "action/dml.hpp"

namespace {

using namespace action;

ActionFactory createNormalTable{"create_normal_table",
                                [](AllConfig const &config) {
                                  return std::make_unique<CreateTable>(
                                      config.ddl,
                                      metadata::Table::Type::normal);
                                },
                                100};

ActionFactory dropTable{"drop_table",
                        [](AllConfig const &config) {
                          return std::make_unique<DropTable>(config.ddl);
                        },
                        100};

ActionFactory alterTable{"alter_table",
                         [](AllConfig const &config) {
                           return std::make_unique<AlterTable>(
                               config.ddl, BitFlags<AlterSubcommand>::AllSet());
                         },
                         100};

ActionFactory renameTable{"rename_table",
                          [](AllConfig const &config) {
                            return std::make_unique<RenameTable>(config.ddl);
                          },
                          100};

ActionFactory createIndex{"create_index",
                          [](AllConfig const &config) {
                            return std::make_unique<CreateIndex>(config.ddl);
                          },
                          100};

ActionFactory dropIndex{"drop_index",
                        [](AllConfig const &config) {
                          return std::make_unique<DropIndex>(config.ddl);
                        },
                        100};

ActionFactory insertSomeData{"insert_some_data",
                             [](AllConfig const &config) {
                               return std::make_unique<InsertData>(config.dml,
                                                                   10);
                             },
                             1000};

ActionFactory deleteSomeData{"delete_some_data",
                             [](AllConfig const &config) {
                               return std::make_unique<DeleteData>(config.dml);
                             },
                             1000};

ActionFactory updateOneRow{"update_one_row",
                           [](AllConfig const &config) {
                             return std::make_unique<UpdateOneRow>(config.dml);
                           },
                           1000};

ActionRegistry initializeDefaultRegisty() {
  ActionRegistry ar;

  ar.insert(createNormalTable);
  ar.insert(dropTable);
  ar.insert(alterTable);
  ar.insert(renameTable);
  ar.insert(createIndex);
  ar.insert(dropIndex);
  ar.insert(insertSomeData);
  ar.insert(deleteSomeData);
  ar.insert(updateOneRow);

  return ar;
}

ActionRegistry defaultRegistryInstance = initializeDefaultRegisty();
} // namespace

namespace action {

ActionRegistry::ActionRegistry() {}

ActionRegistry::ActionRegistry(ActionRegistry const &o) {
  std::unique_lock<std::mutex> lk(o.mutex);
  factories = o.factories;
};

ActionRegistry::ActionRegistry(ActionRegistry &&o) {
  std::unique_lock<std::mutex> lk(o.mutex);
  factories = std::move(o.factories);
};

ActionRegistry &ActionRegistry::operator=(ActionRegistry const &o) {
  factories = o.factories;
  return *this;
}

ActionRegistry &ActionRegistry::operator=(ActionRegistry &&o) {
  std::unique_lock<std::mutex> lk(o.mutex);
  factories = std::move(o.factories);
  return *this;
}

std::size_t ActionRegistry::insert(ActionFactory const &action) {

  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::find_if(factories.begin(), factories.end(),
                         [&](auto const &f) { return f.name == action.name; });

  if (it != factories.end()) {
    throw ActionException(
        fmt::format("Action {} already exists in this registy", action.name));
  }

  factories.push_back(action);
  return factories.size() - 1;
}

void ActionRegistry::remove(std::string const &name) {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::find_if(factories.begin(), factories.end(),
                         [&](auto const &f) { return f.name == name; });
  if (it == factories.end()) {
    throw ActionException(
        fmt::format("Action {} does not exists in this registy", name));
  }

  factories.erase(it);
}

ActionFactory ActionRegistry::operator[](std::string const &name) const {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::find_if(factories.begin(), factories.end(),
                         [&](auto const &f) { return f.name == name; });
  if (it == factories.end()) {
    throw ActionException(
        fmt::format("Action {} does not exists in this registy", name));
  }
  return *it;
}

ActionFactory &ActionRegistry::getReference(std::string const &name) {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::find_if(factories.begin(), factories.end(),
                         [&](auto const &f) { return f.name == name; });
  if (it == factories.end()) {
    throw ActionException(
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

  return std::accumulate(
      factories.begin(), factories.end(), 0,
      [](int const &a, ActionFactory const &b) { return a + b.weight; });
  ;
}

bool ActionRegistry::has(std::string name) const {
  std::unique_lock<std::mutex> lk(mutex);

  auto it = std::find_if(factories.begin(), factories.end(),
                         [&](auto const &f) { return f.name == name; });
  return it != factories.end();
}

ActionFactory const &
ActionRegistry::lookupByWeightOffset(std::size_t offset) const {
  std::unique_lock<std::mutex> lk(mutex);

  std::size_t accum = 0;
  auto it =
      std::find_if(factories.begin(), factories.end(), [&](auto const &f) {
        accum += f.weight;
        return accum >= offset;
      });

  if (it == factories.end()) {
    throw ActionException(
        fmt::format("Weight offset {} is outside of this registy", offset));
  }

  return *it;
}

void ActionRegistry::makeCustomSqlAction(std::string const &name,
                                         std::string const &sql,
                                         std::size_t weight) {
  insert(ActionFactory{name,
                       [sql](AllConfig const &config) {
                         return std::make_unique<CustomSql>(
                             config.custom, sql, CustomSql::inject_t{});
                       },
                       weight});
}

void ActionRegistry::makeCustomTableSqlAction(std::string const &name,
                                              std::string const &sql,
                                              std::size_t weight) {
  insert(ActionFactory{name,
                       [sql](AllConfig const &config) {
                         return std::make_unique<CustomSql>(
                             config.custom, sql, CustomSql::inject_t{"table"});
                       },
                       weight});
}

ActionRegistry &default_registy() { return defaultRegistryInstance; }

void ActionRegistry::use(ActionRegistry const &other) { *this = other; }

} // namespace action
