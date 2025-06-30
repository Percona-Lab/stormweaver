
#pragma once

#include "metadata.hpp"
#include "random.hpp"
#include "sql_variant/generic.hpp"

namespace action {

/* Actions are SQL statements. An action can result zero (in case of an error),
 * one (typical success) or more (in case of CASCADE operations) changes to the
 * metadata.
 * Actions are stateless, which should allow a retry-logic later.
 * */
class Action {
public:
  virtual ~Action();

  virtual void execute(metadata::Metadata &metaCtx, ps_random &rand,
                       sql_variant::LoggedSQL *connection) const = 0;
};

class ActionException : public std::exception {
public:
  ActionException(std::string const &message) : message(message) {}

  const char *what() const noexcept override { return message.c_str(); }

private:
  std::string message;
};

} // namespace action
