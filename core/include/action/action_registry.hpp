
#pragma once

#include "action/all.hpp"
#include "sql_variant/generic.hpp"

#include <cstdint>
#include <mutex>

namespace action {

enum class ActionType : std::uint8_t { ddl, dml, transaction, other };

class ActionRegistry;

struct BuildContext {
  AllConfig const &config;
  ActionRegistry const &registry; // registry the factory was picked from
};

using action_build_t =
    std::function<std::unique_ptr<action::Action>(BuildContext const &)>;

struct ActionFactory {
  std::string name;
  action_build_t builder;
  std::size_t weight;
  ActionType type = ActionType::other;
  // false: statement refuses to run in a transaction block (ALTER SYSTEM),
  // transaction composites skip it
  bool txn_safe = true;
};

class ActionRegistry {
public:
  ActionRegistry();
  ActionRegistry(ActionRegistry const &o);
  ActionRegistry(ActionRegistry &&o) noexcept;

  ActionRegistry &operator=(ActionRegistry const &o);
  ActionRegistry &operator=(ActionRegistry &&o) noexcept;

  std::size_t insert(ActionFactory const &action);

  void remove(std::string const &name);

  ActionFactory operator[](std::string const &name) const;

  ActionFactory &getReference(std::string const &name);

  void makeCustomSqlAction(std::string const &name, std::string const &sql,
                           std::size_t weight,
                           ActionType type = ActionType::other);

  void makeCustomTableSqlAction(std::string const &name, std::string const &sql,
                                std::size_t weight,
                                ActionType type = ActionType::other);

  void use(ActionRegistry const &other);

  std::size_t size() const;
  std::size_t totalWeight() const;
  bool has(std::string name) const;

  ActionFactory lookupByWeightOffset(std::size_t offset) const;

  ActionRegistry
  filtered(std::function<bool(ActionFactory const &)> const &pred) const;

private:
  std::vector<ActionFactory> factories;
  mutable std::mutex mutex;
};

ActionRegistry &default_registy();
ActionRegistry &default_registry(sql_variant::flavor flav);

}; // namespace action
