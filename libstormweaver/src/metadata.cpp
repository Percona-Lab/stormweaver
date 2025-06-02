
#include "metadata.hpp"

#include <iostream>

namespace metadata {

Metadata::Reservation::Reservation()
    : storage_(nullptr), table_(nullptr), drop_(false), index_(Metadata::npos),
      lock_() {}

Metadata::Reservation::Reservation(Metadata *storage, Metadata::table_t table,
                                   bool drop, std::size_t index,
                                   std::unique_lock<std::shared_mutex> &&lock)
    : storage_(storage), table_(table), drop_(drop), index_(index),
      lock_(std::move(lock)) {
  //
}

Metadata::Reservation::~Reservation() {
  if (open()) {
    cancel();
  }
}

Metadata::Reservation::Reservation(Metadata::Reservation &&other) noexcept
    : storage_(other.storage_), table_(std::move(other.table_)),
      drop_(other.drop_), index_(other.index_), lock_(std::move(other.lock_)) {

  other.index_ = npos;
  other.storage_ = nullptr;
  other.table_ = nullptr;
}

Metadata::Reservation &
Metadata::Reservation::operator=(Metadata::Reservation &&other) noexcept {
  storage_ = other.storage_;
  table_ = std::move(other.table_);
  drop_ = other.drop_;
  index_ = other.index_;
  lock_ = std::move(other.lock_);

  other.index_ = npos;
  other.storage_ = nullptr;
  other.table_ = nullptr;

  return *this;
}

void Metadata::Reservation::complete() {
  if (storage_ == nullptr) {
    throw MetadataException("Complete on invalid reservation");
  }
  if (!lock_.owns_lock() && index_ != Metadata::npos) {
    // If this is not a CREATE / SELECT INTO, this means we already
    // completed/cancelled CREATE / SELECT INTO also sets index_ on complete /
    // cancel, to prevent double cancel/complete

    throw MetadataException("Double complete not allowed");
  }
  if (index_ != Metadata::npos) {
    // ALTER/DROP/... anything that refers a specific table(slot)
    // The slot was locked before creating the Reservation, it is definitely
    // there. It is safe to update and release the lock, but DROP might need to
    // defragment.
    if (!drop_) { // ALTER and other modification DDL statements
      storage_->data_.tables[index_] = table_;
      lock_.unlock();
    } else { // DROP
      bool completed = false;
      while (!completed) {
        // size()-1 is safe: we have at least 1 element, locked by this
        // Reservation
        if (index_ == storage_->size() - 1) {
          // this is the last item, no defragmentation is needed. But might
          // conflict with CREATE, as it can try to insert a new record after
          // this right now. This is mitigated by CREATE locking the last
          // record. As we currently hold that, it has to wait. It is safe to
          // just delete and release the lock.
          storage_->data_.tables[index_] = nullptr;
          // tableCount is safe to decrease here:
          // * Concurrent DROP will find the new last record and lock it
          // * Concurrent CREATE will find the new last record, and will also
          // try to lock the one after that (the one now DROPped), and will wait
          // until we release the lock
          storage_->data_.tableCount--;
          storage_->data_.reservedSize--;
          storage_->data_.movedToMap[index_] = Metadata::npos;
          lock_.unlock();
          completed = true;
        } else {
          // Not the last item, we have to lock the last and move it to ensure
          // no holes in the table.

          const auto lastIndex = storage_->size() - 1;
          std::unique_lock<std::shared_mutex> innerLock(
              storage_->data_.tableLocks[lastIndex]);
          if (storage_->data_.tables[lastIndex] != nullptr &&
              lastIndex == storage_->size() - 1) {
            // We locked the last item. No need to lock the after-the-last item,
            // as CREATE TABLE also tries to lock the last item, which is now
            // locked. It is safe to move and empty it, CREATE will handle the
            // conflict.

            storage_->data_.tables[index_] = storage_->data_.tables[lastIndex];
            lock_.unlock();
            // Similarly, it is safe to decrease tableCount here, for the same
            // reasoning as above.
            storage_->data_.tableCount--;
            storage_->data_.reservedSize--;
            storage_->data_.tables[lastIndex] = nullptr;
            storage_->data_.movedToMap[lastIndex] = index_;
            innerLock.unlock();

            completed = true;
          } // else: either a DELETE or a CREATE happened before our lock, the
            // lock we hold doesn't point to the last record. Retry from the
            // beginning, as now we might point to the last table in case of
            // another DELETE.
        }
      }
    }
  } else { // CREATE TABLE or SELECT INTO, which creates a new table

    // We do not actually hold a lock, only have an index reservation at this
    // point!

    bool completed = false;
    while (!completed) {
      std::unique_lock<std::shared_mutex> outerLock;
      const auto nextIndex = storage_->size();
      if (storage_->size() == 0) {
        // empty container. No alter/dro should happen, just lock the first row
        // also not possible to lock the last table, since we don't have one
      } else {
        // Try to lock the last item first
        const auto lastIndex = nextIndex - 1;
        outerLock = std::unique_lock<std::shared_mutex>(
            storage_->data_.tableLocks[lastIndex]);

        if (storage_->data_.tables[lastIndex] == nullptr ||
            nextIndex != storage_->size()) {
          // This is no longer the last item. Either a CREATE or a DELETE
          // succeeded. Try again.
          continue;
        }
      }

      // Size is always modified while holding the lock of the last item.
      // Since we hold that lock, we can be sure that now the last item remains
      // the last.

      // Try to lock the index after the last
      // Both DELETE and CREATE try to lock the last item first. Since we hold
      // the lock for that, and verified that it is indeed the last item, this
      // should always succeed.

      // TODO: debug assert about overindexing

      std::scoped_lock<std::shared_mutex> innerLock(
          storage_->data_.tableLocks[nextIndex]);

      // TODO: debug assert about the field being nullptr in the vector

      storage_->data_.tables[nextIndex] = table_;
      // we hold the lock both for the current last item, and the one after it
      // increasing tableCount here is safe.
      // Also since we already inserted the new item, it will be correctly
      // returned by operator[] at this point
      storage_->data_.tableCount++;

      index_ = nextIndex;

      completed = true;
      // locks are released automatically
    }
  }
}

void Metadata::Reservation::cancel() {
  if (index_ == Metadata::npos && storage_ != nullptr) {
    // Cancelling the delete, the reserved slot is freed
    storage_->data_.reservedSize--;
  }
  storage_ = nullptr;
  table_ = nullptr;
  index_ = npos;
  if (lock_.owns_lock())
    lock_.unlock();
}

bool Metadata::Reservation::open() const {
  return storage_ != nullptr && (lock_.owns_lock() || index_ == npos);
}
Metadata::index_t Metadata::Reservation::index() const { return index_; }
Metadata::table_t Metadata::Reservation::table() const { return table_; }

Metadata::Metadata() { data_.movedToMap.fill(npos); }

Metadata::Reservation Metadata::createTable() {

  if (data_.reservedSize < limits::maximum_table_count) {

    auto res = ++data_.reservedSize;

    if (res > limits::maximum_table_count) {
      --data_.reservedSize;
      return Reservation();
    }

    return Reservation(this, std::make_shared<Table>(), false, npos, {});
  }

  return Reservation(nullptr, nullptr, false, npos, {});
}

Metadata::Reservation Metadata::alterTable(index_t idx) {
  // assert: idx < limits::maximum_table_count
  std::unique_lock<std::shared_mutex> mtx(data_.tableLocks[idx]);
  auto table = data_.tables[idx];
  if (table == nullptr) {
    return Reservation();
  }
  return Reservation(this, std::make_shared<Table>(*(table)), false, idx,
                     std::move(mtx));
}

Metadata::Reservation Metadata::dropTable(index_t idx) {
  // assert: idx < limits::maximum_table_count
  std::unique_lock<std::shared_mutex> mtx(data_.tableLocks[idx]);
  auto table = data_.tables[idx];
  if (table == nullptr) {
    return Reservation();
  }
  return Reservation(this, table, true, idx, std::move(mtx));
}

Metadata::index_t Metadata::size() const { return data_.tableCount; }

table_cptr Metadata::operator[](Metadata::index_t idx) const {
  // assert: idx < limits::maximum_table_count
  std::shared_lock<std::shared_mutex> mtx(data_.tableLocks[idx]);
  return data_.tables[idx];
}

} // namespace metadata
