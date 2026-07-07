#pragma once

#include "action/action.hpp"
#include "action/action_registry.hpp"

namespace action {

/* Runs several randomly picked sub-actions inside one database
   transaction. Metadata deltas buffer in a TxnBuffer and publish only
   after COMMIT succeeds; ROLLBACK (chosen, or forced by an error)
   discards them. */
class TransactionAction : public Action {
public:
  TransactionAction(AllConfig config, ActionRegistry pool);

  void execute(metadata::Context &metaCtx, ps_random &rand,
               sql_variant::LoggedSQL *connection) const override;

private:
  AllConfig allConfig;
  ActionRegistry poolAll;   // everything except transaction-typed actions
  ActionRegistry poolNoDdl; // additionally without DDL (mysql exclude mode)
};

} // namespace action
