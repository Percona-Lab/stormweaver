#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "metadata/table.hpp"
#include "random.hpp"

/*
  Transaction-aware access layer over the catalog.

  Context wraps a TableRegistry plus an optional TxnBuffer. Without a
  buffer every call forwards straight to the catalog (autocommit,
  today's behavior). With a buffer, writes append to an op-log and
  reads merge the buffered state over the global catalog, so a
  transaction sees its own uncommitted changes - like the database
  session does.

  The op-log replays against the catalog on COMMIT (publishAll) through
  the same id-keyed merge used by autocommit actions; a transaction
  publishing late is indistinguishable from a slow action publishing
  late. Savepoints are log marks; rolling back truncates the log and
  rebuilds the overlay.

  Buffered update deltas outlive the action's execute() call. They MUST
  capture by value; a [&] capture dangles at COMMIT replay.

  A TxnBuffer belongs to one worker; nothing here locks.
*/

namespace metadata {

template <CatalogObject T> class TxnBuffer {
public:
  using delta_fn = std::function<bool(T &)>;

  std::size_t mark() const { return log_.size(); }

  bool empty() const { return log_.empty(); }

  void insert(T rec) {
    applyInsert(rec);
    log_.push_back(InsertOp{std::move(rec)});
  }

  bool update(ObjectId id, delta_fn delta, Catalog<T> const &global) {
    if (!applyUpdate(id, delta, global)) {
      return false;
    }
    log_.push_back(UpdateOp{id, std::move(delta)});
    return true;
  }

  bool erase(ObjectId id, Catalog<T> const &global) {
    if (!applyErase(id, global)) {
      return false;
    }
    log_.push_back(EraseOp{id});
    return true;
  }

  void rollbackTo(std::size_t m, Catalog<T> const &global) {
    log_.resize(std::min(m, log_.size()));
    rebuildOverlay(global);
  }

  void discard() {
    log_.clear();
    local_.clear();
    erased_.clear();
  }

  // COMMIT succeeded: replay the log through the ordinary id-keyed merge.
  // Return values are deliberately ignored - losing a race to a concurrent
  // worker (update after erase, duplicate name) is tolerated drift, same
  // as in autocommit mode.
  void publishAll(Catalog<T> &global) {
    for (auto &op : log_) {
      std::visit(
          [&](auto &o) {
            using O = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<O, InsertOp>) {
              std::ignore = global.insert(std::move(o.rec));
            } else if constexpr (std::is_same_v<O, UpdateOp>) {
              std::ignore = global.update(o.id, o.delta);
            } else {
              std::ignore = global.erase(o.id);
            }
          },
          op);
    }
    discard();
  }

  // --- overlay reads (merged-view helpers used by CatalogView) ---

  bool erasedContains(ObjectId id) const { return erased_.contains(id); }

  object_cptr<T> localById(ObjectId id) const {
    auto it = local_.find(id);
    return it == local_.end() ? nullptr : it->second;
  }

  object_cptr<T> localByName(std::string_view name) const {
    for (auto const &[id, rec] : local_) {
      if (rec->name == name) {
        return rec;
      }
    }
    return nullptr;
  }

  bool localContains(ObjectId id) const { return local_.contains(id); }

  // id-sorted for deterministic iteration (unordered_map order is not
  // reproducible across runs; determinism matters for seeded replay)
  std::vector<object_cptr<T>> localSorted() const {
    std::vector<object_cptr<T>> out;
    out.reserve(local_.size());
    for (auto const &[id, rec] : local_) {
      out.push_back(rec);
    }
    std::ranges::sort(out, {}, [](auto const &r) { return r->id; });
    return out;
  }

private:
  struct InsertOp {
    T rec;
  };
  struct UpdateOp {
    ObjectId id;
    delta_fn delta;
  };
  struct EraseOp {
    ObjectId id;
  };
  using Op = std::variant<InsertOp, UpdateOp, EraseOp>;

  void applyInsert(T const &rec) {
    erased_.erase(rec.id);
    local_[rec.id] = std::make_shared<T>(rec);
  }

  bool applyUpdate(ObjectId id, delta_fn const &delta,
                   Catalog<T> const &global) {
    if (erased_.contains(id)) {
      return false;
    }
    object_cptr<T> base = localById(id);
    if (base == nullptr) {
      base = global.byId(id);
    }
    if (base == nullptr) {
      return false;
    }
    T copy = *base;
    if (!delta(copy)) {
      return false;
    }
    copy.id = id;
    local_[id] = std::make_shared<T>(std::move(copy));
    return true;
  }

  bool applyErase(ObjectId id, Catalog<T> const &global) {
    if (erased_.contains(id)) {
      return false;
    }
    const bool known = local_.contains(id) || global.byId(id) != nullptr;
    if (!known) {
      return false;
    }
    local_.erase(id);
    erased_.insert(id);
    return true;
  }

  void rebuildOverlay(Catalog<T> const &global) {
    local_.clear();
    erased_.clear();
    for (auto &op : log_) {
      std::visit(
          [&](auto &o) {
            using O = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<O, InsertOp>) {
              applyInsert(o.rec);
            } else if constexpr (std::is_same_v<O, UpdateOp>) {
              std::ignore = applyUpdate(o.id, o.delta, global);
            } else {
              std::ignore = applyErase(o.id, global);
            }
          },
          op);
    }
  }

  std::vector<Op> log_;
  std::unordered_map<ObjectId, object_cptr<T>> local_;
  std::unordered_set<ObjectId> erased_;
};

/* Value type combining a catalog with an optional transaction buffer.
   Mirrors the Catalog API so action bodies keep their shape. */
template <CatalogObject T> class CatalogView {
public:
  CatalogView(Catalog<T> &catalog, TxnBuffer<T> *txn)
      : catalog_(&catalog), txn_(txn) {}

  [[nodiscard]] object_cptr<T> byId(ObjectId id) const {
    if (txn_ != nullptr) {
      if (txn_->erasedContains(id)) {
        return nullptr;
      }
      if (auto rec = txn_->localById(id)) {
        return rec;
      }
    }
    return catalog_->byId(id);
  }

  [[nodiscard]] object_cptr<T> byName(std::string_view name) const {
    if (txn_ == nullptr) {
      return catalog_->byName(name);
    }
    if (auto rec = txn_->localByName(name)) {
      return rec;
    }
    auto rec = catalog_->byName(name);
    if (rec != nullptr &&
        (txn_->erasedContains(rec->id) || txn_->localContains(rec->id))) {
      // erased in-trx, or renamed in-trx (global name entry is stale here)
      return nullptr;
    }
    return rec;
  }

  object_cptr<T> randomPick(ps_random &rand) const {
    if (txn_ == nullptr) {
      return catalog_->randomPick(rand);
    }
    auto all = snapshotAll();
    if (all.empty()) {
      return nullptr;
    }
    return all[rand.random_number<std::size_t>(0, all.size() - 1)];
  }

  [[nodiscard]] std::size_t size() const {
    if (txn_ == nullptr) {
      return catalog_->size();
    }
    return snapshotAll().size();
  }

  [[nodiscard]] std::vector<object_cptr<T>> snapshotAll() const {
    if (txn_ == nullptr) {
      return catalog_->snapshotAll();
    }
    std::vector<object_cptr<T>> out;
    for (auto &rec : catalog_->snapshotAll()) {
      if (!txn_->erasedContains(rec->id) && !txn_->localContains(rec->id)) {
        out.push_back(std::move(rec));
      }
    }
    for (auto &rec : txn_->localSorted()) {
      out.push_back(std::move(rec));
    }
    return out;
  }

  bool insert(T &&obj) {
    if (txn_ == nullptr) {
      return catalog_->insert(std::move(obj));
    }
    if (obj.id == 0) {
      return false;
    }
    txn_->insert(std::move(obj));
    return true;
  }

  bool update(ObjectId id, TxnBuffer<T>::delta_fn delta) {
    if (txn_ == nullptr) {
      return catalog_->update(id, std::move(delta));
    }
    return txn_->update(id, std::move(delta), *catalog_);
  }

  bool erase(ObjectId id) {
    if (txn_ == nullptr) {
      return catalog_->erase(id);
    }
    return txn_->erase(id, *catalog_);
  }

private:
  Catalog<T> *catalog_;
  TxnBuffer<T> *txn_;
};

class Context {
public:
  explicit Context(TableRegistry &reg) : reg_(&reg) {}
  Context(TableRegistry &reg, TxnBuffer<Table> *txn) : reg_(&reg), txn_(txn) {}

  // single-kind today: txn_ is Table-typed; a second catalog kind needs
  // per-kind buffers
  template <typename T> [[nodiscard]] CatalogView<T> get() const {
    return CatalogView<T>(reg_->get<T>(), txn_);
  }

  ObjectId nextId() { return reg_->nextId(); }

  TableRegistry &registry() { return *reg_; }

private:
  TableRegistry *reg_;
  TxnBuffer<Table> *txn_ = nullptr;
};

} // namespace metadata
