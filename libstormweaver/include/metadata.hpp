
#pragma once

#include <array>
#include <atomic>
#include <boost/container/small_vector.hpp>
#include <exception>
#include <limits>
#include <mutex>
#include <shared_mutex>

/*
  Metadata API
  ===============

  TODO: this api is now a bit misused

  Purpose
  -------

  stormweaver tries to execute valid SQL statements, including DDL statements.
  Instead of always querying the database before DDL to figure out valid
  options, it tries to keep track of what's there in the database.

  This is done by the Metadata API, every DDL statement modifies this after
  executing an SQL instruction.

  DDL (and DML) can still fail, because this can't guarantee that the Metadata
  is always up to date, as stormweaver only update it after the DDL SQL was
  successfully executed.

  This could be prevented by locking the table during the entire duration of the
  DDL, but that would be counterproductive, as one of the main points of
  stormweaver is to test potentially conflicting concurrent DDL, and locking
  would prevent this.

  Design
  ------

  This API aims to be

  (1) thread-safe, and
  (2) low (locking/searching) overhead

  For the use cases pquery uses it.

  The basic concept is simple:

  1. The Metadata class contains shared pointers to tables and a (write) mutex
  for each table

  2. These are both stored in arrays, with enough sizes (200 by default) for
  most usescases. And if more tables are needed, this can be increased with a
  rebuild.

  3. shared_ptr access/write is an atomic operation. SELECTS are done by threads
  getting a shared pointer, and holding the ref count as long as needed.

  4. If a thread executes a modifying DDL, it executes the following sequence:

    1. get the shared_ptr for the table(s)
    2. build and execute the SQL statement, wait for success
    3. after success, aquire the mutex, with a non-const shared pointer
       this shared_ptr is actually a copy of the original shared_ptr, only
       this thread references it. The vector and every other thread keeps
       pointing to the other shared_ptr until completion
    4. modify the shared_ptr and call the return operation on the Metadata.
       This exchanges the shared_ptr in the vector and releases the lock
    5. For DDL affecting multiple tables, 3 and 4 happens in a loop, only
       locking one table at a time.

  5. DROPs are a bit more complex becaue of defragmentation: if it leaves
  holes in the data, that means data can be effectively anywhere, and
  stormweaver has to search the entire array every time when looking for a
  specific table by name or other property. If it defragments the table by
  moving the last item to the hole created in DROP, stormweaver can have an
  issue of "losing" the table during an alter, between steps (1) and (3).

  To mitigate this, Metadata has another vector, where it stores the "last item
  from here was moved to" indexes. If a Table gets moved after a DROP, the
  DROP operation also updates this to the index where it moved the table.

  It is still possible to lose the table in the unlucky scenario when:

    1. the ALTERed table is the last at the beginning of the operation
    2. any other table is DROPped
    3. a new table is CREATEd
    4. another table is DROPped
    5. the ALTER SQL finally completes, and the thread acquires the lock.

  But this is quite specific and should be rare. In this case, the code can fall
  back to a slower linear search. If the linear search fails, another thread
  DROPped the table in the meantime, and nothing needs to be updated.

  Another remaining downside of this is that for defragmentation to work, the
  DROP thread has to hold two locks: the DROPped table lock and the last table
  lock at the same time. This shouldn't cause any deadlock, because:

    * two DROPs can't deadlock, because they will be able to lock the last
      table in sequence. And if one of the DROPped tables is the last one, it
      doesn't need to defragment. But the code has to recheck if the table is
      the last on every try, because this might change. (if the table waiting
      for the last lock is the table before the last, it might become the last
      table)
    * all other operations only lock one table - except CREATE TABLE, for that,
      see the next point.
    * this will be true even later when we add foreign keys and cascade, or
      inheritance: we can change the affected tables one by one, we can't
      guarantee that the metadata is up to date even without them.

  5. For CREATE TABLE, stormweaver has to reserve a slot BEFORE creating the
  table, and locking the record. This is because the Metadata table array is
  limited, and otherwise another thread could use up the last remaining open
  slot in the meantime. This ensures that if a CREATE TABLE SQL is executed and
  succeeds, Metadata has a slot to put it somewhere.

  One possible problem with this approach is that concurrent DROP and CREATE
  can cause defragmentation: if a CREATE reserves an index, but then a DROP
  shrinks the array, now the CREATE insert a record too late into the array,
  leaving a gap.

  This is solved by the closing logic: before actually updating the shared_ptr
  in the array, CREATE locks the record before. Since DROP also wants to lock
  that record, this only succeeds if there's no DROP in progress. If CREATE
  can lock the record, and it's not empty, it can insert the new record to the
  original place. If it's empty, it releases the original lock, and repeats the
  process with the newly acquired lock.

  Another important detail is that "reserving an index" means "reserving a
  space", not an actual specific value. CREATE only locks down on a specific
  index before it wants to update the shared_ptr. This ensures that two CREATEs
  can complete mostly in parallel and in any order, which can be important
  especially for CREATE TABLE AS SELECT... statements, which can have long
  execution times.

  6. Dynamic allocations (vectors) are limited by using non allocating
  boost containers. This helps with copy times, and locality also should result
  in better performance.

  7. Strings are kept as std::strings, because small string optimization
  usually means it doesn't allocate up to 15 (22) characters, which should be
  true for most things. And default values of long fields wouldn't fit in
  static_strings anyway.

  As 22 bytes is a significant improvement over 15, in case we have slighly
  longer column names and functional indexes, it is strongly recommended to
  build stormweaver with libc++.

  8. While it is unlikely, it is possible for the [] operator to return a
  nullptr. Generally the completion code is very careful about this, and
  metadata[size()-1] != nullptr is always true, but since there is no locking
  during data retrieval, it is possible that the size changes between calling
  size() and operator[].

  Possible further improvements
  -----------------------------

  * Instead of a linear search, metadata could use and update a sorted lookup
    array when needed, so it could use logarithmic search. While it couldn't
    guarantee that this array is always 100% up to date, this could speed up
    most of the name/property based searches.
  * As the stored metadata can diverge from what's actually in the database in
    case of internal logic errors, a "sanity check" periodic operation or thread
    could query the schema in the DB, and update Metadata when needed.
  * Metadata currently has no internal statistics (row count, table data size,
    previous query performance on the table, ...), but this could be an
    interesting addition
  * Instead of a single mutex, Metadata could use one mutex for alters, and
    another level for CREATE/DROP - those would need to lock both. This could
    ensure that ALTERing the last table can happen in parallel with CREATE/DROP
    without interruption. This could either result in an improvement or a
    performance drop, needs investigation...
  * Maybe movedToMap would be better with pairs, <whatWasMoved,whereItMoved>?
    Again, could be an improvement or a performance drop...

*/

#include <boost/static_string/static_string.hpp>
#include <cstdint>
#include <string>

namespace metadata {

class MetadataException : public std::exception {
public:
  MetadataException(std::string const &message) : message(message) {}

  const char *what() const noexcept override { return message.c_str(); }

private:
  std::string message;
};

namespace limits {
const constexpr std::size_t maximum_table_count = 200;
const constexpr std::size_t optimized_column_count = 32;
const constexpr std::size_t optimized_index_column_count = 10;
const constexpr std::size_t optimized_index_count = 16;
} // namespace limits

enum class ColumnType { INT, CHAR, VARCHAR, REAL, BOOL, BYTEA, TEXT };

enum class Generated { notGenerated, stored, virt };

struct Column {
  std::string name; // TODO: fixed buffers for strings?
  ColumnType type;

  std::size_t length = 0;

  std::string default_value;

  Generated generated = Generated::notGenerated;

  bool nullable = false;
  bool primary_key = false;
  bool auto_increment = false;
  // Perona Server for MySQL specific
  bool compressed = false; // percona type compressed
};

enum class IndexOrdering { default_, asc, desc };

struct IndexColumn {
  std::string column_name; // column + ordering or func(columns...)

  IndexOrdering getOrdering();
  bool isFunctionIndex();
};

struct Index {
  // TODO: support postgres functional indexes?
  std::string name;

  boost::container::small_vector<std::string,
                                 limits::optimized_index_column_count>
      fields;
};

struct Table {

  enum class Type { normal, partitioned, temporary };

  std::string name;
  std::string engine;           // or access method
  std::string mysql_row_format; // mysql specific
  std::string tablespace;

  // number of partitions
  // other partition information

  int mysql_key_block_size = 0; // mysql specific, TODO why is this an int?

  bool mysql_compression; // mysql specific
  bool encryption;

  boost::container::small_vector<Column, limits::optimized_column_count>
      columns;
  boost::container::small_vector<Index, limits::optimized_index_count> indexes;
};

using table_ptr = std::shared_ptr<Table>;
using table_cptr = std::shared_ptr<const Table>;

class Metadata {
public:
  using table_t = table_ptr;
  using container_t = std::array<table_t, limits::maximum_table_count>;
  using index_t = std::size_t;

  static const constexpr index_t npos = std::numeric_limits<index_t>::max();

  class Reservation {
  public:
    Reservation();

    Reservation(Reservation const &) = delete;
    Reservation(Reservation &&other) noexcept;

    Reservation &operator=(Reservation const &) = delete;
    Reservation &operator=(Reservation &&other) noexcept;

    // implicit complete, if the thread doesn't call cancel, it assumes success
    ~Reservation();

    void complete(); // release the lock and write the data back
    void cancel();   // release the lock without writing back

    bool open() const; // are we holding the lock?
    index_t index() const;
    table_t table() const;

  private:
    Reservation(Metadata *storage, table_t table, bool drop_, std::size_t index,
                std::unique_lock<std::shared_mutex> &&lock);

    Metadata *storage_;
    table_t table_;
    bool drop_;

    /* Index references the table object in the array to be modified. It is
     always filled, except for CREATE, where it is initially npos, and only gets
     assigned in complete().
     If the code explicitly calls complete(), index can
     be safely read and used after. */
    std::size_t index_;
    std::unique_lock<std::shared_mutex> lock_;

    friend class Metadata;
  };

  Metadata();

  // If no slots are left, returns an invalid (non open) Reservation
  Reservation createTable();
  template <typename func_t> index_t createTable(func_t func) {
    Reservation res = createTable();
    func(res);
    return res.index();
  }

  // If not found, returns an invalid (non-open) Reservation
  Reservation alterTable(index_t idx);
  template <typename func_t> bool alterTable(index_t idx, func_t func) {
    Reservation res = alterTable(idx);
    func(res);
    return res.index() != npos;
  }

  // If not found, returns an invalid (non-open) Reservation
  Reservation dropTable(index_t idx);
  template <typename func_t> bool dropTable(index_t idx, func_t func) {
    Reservation res = dropTable(idx);
    func(res);
    return res.index() != npos;
  }

  index_t size() const;

  // Might return nullptr. It is very unlikely, but still needs to be checked
  table_cptr operator[](index_t idx) const;

private:
  struct InternalData {
    container_t tables;
    mutable std::array<std::shared_mutex, limits::maximum_table_count>
        tableLocks;
    std::array<index_t, limits::maximum_table_count> movedToMap;
    std::atomic<std::size_t> tableCount;
    std::atomic<std::size_t> reservedSize;
  } data_;
};

} // namespace metadata
