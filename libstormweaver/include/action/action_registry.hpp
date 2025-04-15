
#pragma once

#include "action/all.hpp"

#include <mutex>

namespace action {

class ActionException : public std::exception {
public:
  ActionException(std::string const &message) : message(message) {}

  const char *what() const noexcept override { return message.c_str(); }

private:
  std::string message;
};

using action_build_t =
    std::function<std::unique_ptr<action::Action>(action::AllConfig const &)>;

struct ActionFactory {
  std::string name;
  action_build_t builder;
  std::size_t weight;
};

class ActionRegistry {
public:
  ActionRegistry();
  ActionRegistry(ActionRegistry const &o);
  ActionRegistry(ActionRegistry &&o);

  ActionRegistry &operator=(ActionRegistry const &o);
  ActionRegistry &operator=(ActionRegistry &&o);

  std::size_t insert(ActionFactory const &factory);

  void remove(std::string const &name);

  ActionFactory operator[](std::string const &name) const;

  ActionFactory &getReference(std::string const &name);

  void makeCustomSqlAction(std::string const &name, std::string const &sql,
                           std::size_t weight);

  void makeCustomTableSqlAction(std::string const &name, std::string const &sql,
                                std::size_t weight);

  void use(ActionRegistry const &other);

  std::size_t size() const;
  std::size_t totalWeight() const;
  bool has(std::string name) const;

  ActionFactory const &lookupByWeightOffset(std::size_t offset) const;

private:
  std::vector<ActionFactory> factories;
  mutable std::mutex mutex;
};

ActionRegistry &default_registy();

}; // namespace action
