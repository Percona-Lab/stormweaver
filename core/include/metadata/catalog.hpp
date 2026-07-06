#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "random.hpp"

/*
  Catalog API
  ===========

  Thread-safe, id-keyed object store for database object metadata.

  - Identity is a never-reused ObjectId handed out by Registry::nextId().
  - Records are immutable after publish (shared_ptr<const T>); every change
    copies the current record, modifies the copy and republishes it. No
    record-level synchronization exists anywhere.
  - Readers hold the shared lock for a single map operation; writers hold
    the exclusive lock for a single merge. No lock is ever held while SQL
    executes.
  - update() applies a delta to the CURRENT record, which may differ from
    the snapshot the caller built its SQL from. Deltas must address
    sub-objects by name and skip pieces that no longer apply.
  - Name index: renames are ordinary deltas, the catalog maintains the
    index. On collision (possible under merge-order ambiguity) the last
    publish wins; the displaced object stays reachable by id.
*/

namespace metadata {

using ObjectId = std::uint64_t;

struct ObjectBase {
  ObjectId id = 0;
  std::uint64_t version = 0; // bumped on publish; observability only
  std::string name;
};

// Typed reference to another catalog object. Stored as id so it survives
// renames; resolved to a name at SQL-build time. id 0 = no reference.
template <typename T> struct Ref {
  ObjectId id = 0;

  explicit operator bool() const { return id != 0; }
  auto operator<=>(Ref const &other) const = default;
};

template <typename T>
concept CatalogObject = std::derived_from<T, ObjectBase>;

template <CatalogObject T> using object_cptr = std::shared_ptr<const T>;

struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view sv) const {
    return std::hash<std::string_view>{}(sv);
  }
};

template <CatalogObject T> class Catalog {
public:
  object_cptr<T> byId(ObjectId id) const {
    std::shared_lock lock(mutex_);
    auto it = slots_.find(id);
    return it == slots_.end() ? nullptr : it->second.rec;
  }

  object_cptr<T> byName(std::string_view name) const {
    std::shared_lock lock(mutex_);
    auto it = names_.find(name);
    if (it == names_.end()) {
      return nullptr;
    }
    return slots_.at(it->second).rec;
  }

  object_cptr<T> randomPick(ps_random &rand) const {
    std::shared_lock lock(mutex_);
    if (sampling_.empty()) {
      return nullptr;
    }
    auto idx = rand.random_number<std::size_t>(0, sampling_.size() - 1);
    return slots_.at(sampling_[idx]).rec;
  }

  std::size_t size() const {
    std::shared_lock lock(mutex_);
    return slots_.size();
  }

  std::vector<object_cptr<T>> snapshotAll() const {
    std::shared_lock lock(mutex_);
    std::vector<object_cptr<T>> result;
    result.reserve(sampling_.size());
    for (auto id : sampling_) {
      result.push_back(slots_.at(id).rec);
    }
    return result;
  }

  bool insert(T &&obj) {
    if (obj.id == 0) {
      return false;
    }
    auto rec = std::make_shared<T>(std::move(obj));
    std::unique_lock lock(mutex_);
    // grow ahead of time so push_back below cannot throw
    if (sampling_.size() == sampling_.capacity()) {
      sampling_.reserve(std::max<std::size_t>(8, sampling_.capacity() * 2));
    }
    auto [it, inserted] =
        slots_.try_emplace(rec->id, Slot{rec, sampling_.size()});
    if (!inserted) {
      return false;
    }
    sampling_.push_back(rec->id);
    names_.insert_or_assign(rec->name, rec->id); // last publish wins
    return true;
  }

  template <typename Delta>
    requires std::is_invocable_r_v<bool, Delta, T &>
  bool update(ObjectId id, Delta &&delta) {
    std::unique_lock lock(mutex_);
    auto it = slots_.find(id);
    if (it == slots_.end()) {
      return false; // concurrent DROP won; delta is discarded
    }
    T copy = *it->second.rec;
    if (!delta(copy)) {
      return false;
    }
    copy.id = id; // deltas must not change identity
    copy.version = it->second.rec->version + 1;
    // build the new record before touching indexes so an allocation
    // failure cannot leave a name entry pointing at nothing
    auto rec = std::make_shared<T>(std::move(copy));
    if (rec->name != it->second.rec->name) {
      auto nameIt = names_.find(it->second.rec->name);
      if (nameIt != names_.end() && nameIt->second == id) {
        names_.erase(nameIt);
      }
      names_.insert_or_assign(rec->name, id); // last publish wins
    }
    it->second.rec = std::move(rec);
    return true;
  }

  bool erase(ObjectId id) {
    std::unique_lock lock(mutex_);
    auto it = slots_.find(id);
    if (it == slots_.end()) {
      return false;
    }
    const auto pos = it->second.pos;
    const auto lastId = sampling_.back();
    sampling_[pos] = lastId;
    slots_.at(lastId).pos = pos;
    sampling_.pop_back();
    auto nameIt = names_.find(it->second.rec->name);
    if (nameIt != names_.end() && nameIt->second == id) {
      names_.erase(nameIt);
    }
    slots_.erase(it);
    return true;
  }

  void reset() {
    std::unique_lock lock(mutex_);
    slots_.clear();
    sampling_.clear();
    names_.clear();
  }

private:
  struct Slot {
    object_cptr<T> rec;
    std::size_t pos; // index into sampling_
  };

  mutable std::shared_mutex mutex_;
  std::unordered_map<ObjectId, Slot> slots_;
  std::vector<ObjectId> sampling_;
  std::unordered_map<std::string, ObjectId, StringHash, std::equal_to<>> names_;
};

template <CatalogObject... Kinds> class Registry {
public:
  ObjectId nextId() { return nextId_.fetch_add(1, std::memory_order_relaxed); }

  template <typename T> Catalog<T> &get() {
    return std::get<Catalog<T>>(catalogs_);
  }
  template <typename T> Catalog<T> const &get() const {
    return std::get<Catalog<T>>(catalogs_);
  }

  // Clears all catalogs. Deliberately does NOT rewind nextId_: ids are
  // never reused, even across resets.
  void reset() {
    std::apply([](auto &...catalog) { (catalog.reset(), ...); }, catalogs_);
  }

private:
  std::tuple<Catalog<Kinds>...> catalogs_;
  std::atomic<ObjectId> nextId_{1};
};

} // namespace metadata
