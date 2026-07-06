
#pragma once

#include "action/action.hpp"

#include <tuple>
#include <utility>

namespace action {

template <typename T>
concept usePointerSyntax =
    requires(T a, metadata::Metadata &metaCtx, ps_random &rand,
             sql_variant::LoggedSQL *connection) {
      a->execute(metaCtx, rand, connection);
    };

template <typename Context, typename... ActionTs>
class CompositeAction : public Action {
public:
  CompositeAction(Context &&ctx, ActionTs &&...actions)
      : ctx(std::move(ctx)), actions(std::move(actions)...) {}

  template <typename ActionT>
  static void executeHelper(ActionT const &action, metadata::Metadata &metaCtx,
                            ps_random &rand,
                            sql_variant::LoggedSQL *connection) {
    if constexpr (usePointerSyntax<ActionT>) {
      action->execute(metaCtx, rand, connection);
    } else {
      action.execute(metaCtx, rand, connection);
    }
  }

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override {
    std::apply(
        [&](const auto &...tupleArgs) {
          (executeHelper(tupleArgs, metaCtx, rand, connection), ...);
        },
        actions);
  }

private:
  // Context is just used by the composite setup, as a possible container for
  // callbacks during the composite setup
  Context ctx;
  std::tuple<ActionTs...> actions;
};

template <typename ActionT> class RepeatAction : public Action {
public:
  RepeatAction(ActionT &&action, std::size_t repeatCount)
      : action(std::move(action)), repeatCount(repeatCount) {}

  void execute(metadata::Metadata &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override {
    for (std::size_t idx = 0; idx < repeatCount; ++idx) {
      if constexpr (usePointerSyntax<ActionT>) {
        action->execute(metaCtx, rand, connection);
      } else {
        action.execute(metaCtx, rand, connection);
      }
    }
  }

private:
  ActionT action;
  std::size_t repeatCount;
};

} // namespace action
