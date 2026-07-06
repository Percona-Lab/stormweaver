
#include "metadata.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <iostream>
#include <thread>

TEST_CASE("Empty metadata is sane", "[metadata]") {
  metadata::Metadata meta;

  REQUIRE(meta.size() == 0);
  REQUIRE(meta[0] == nullptr);
}

TEST_CASE("Tables can be inserted into metadata", "[metadata]") {
  metadata::Metadata meta;

  {
    auto reservation = meta.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "foo";
    reservation.complete();
  }

  REQUIRE(meta.size() == 1);
  REQUIRE(meta[0]->name == "foo");
}

TEST_CASE("Double completed reservations are not allowed", "[metadata]") {
  metadata::Metadata meta;

  {
    auto reservation = meta.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "foo";
    reservation.complete();
    REQUIRE_THROWS_WITH(reservation.complete(), "Double complete not allowed");
  }

  REQUIRE(meta.size() == 1);
  REQUIRE(meta[0]->name == "foo");
}

TEST_CASE("complete not allowed after cancelling reservation", "[metadata]") {
  metadata::Metadata meta;

  {
    auto reservation = meta.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "foo";
    reservation.cancel();
    REQUIRE_THROWS_WITH(reservation.complete(),
                        "Complete on invalid reservation");
  }

  REQUIRE(meta.size() == 0);
  REQUIRE(meta[0] == nullptr);
}

TEST_CASE("Tables insertion into metadata can be cancelled", "[metadata]") {
  metadata::Metadata meta;

  {
    auto reservation = meta.createTable();
    reservation.table()->name = "foo";
    reservation.cancel();
  }

  REQUIRE(meta.size() == 0);
  REQUIRE(meta[0] == nullptr);
}

inline void insert4tables(metadata::Metadata &meta) {
  {
    auto reservation = meta.createTable();
    reservation.table()->name = "foo";
    reservation.complete();
  }

  {
    auto reservation = meta.createTable();
    reservation.table()->name = "bar";
    reservation.complete();
  }

  {
    auto reservation = meta.createTable();
    reservation.table()->name = "moo";
    reservation.complete();
  }

  {
    auto reservation = meta.createTable();
    reservation.table()->name = "boo";
    reservation.complete();
  }
}

TEST_CASE("Multiple tables can be inserted into metadata", "[metadata]") {
  metadata::Metadata meta;

  insert4tables(meta);

  REQUIRE(meta.size() == 4);
  REQUIRE(meta[0]->name == "foo");
  REQUIRE(meta[1]->name == "bar");
  REQUIRE(meta[2]->name == "moo");
  REQUIRE(meta[3]->name == "boo");
}

TEST_CASE("Tables can be inserted into metadata in parallel", "[metadata]") {
  metadata::Metadata meta;

  auto reservation1 = meta.createTable();
  reservation1.table()->name = "foo";

  auto reservation2 = meta.createTable();
  reservation2.table()->name = "bar";

  auto reservation3 = meta.createTable();
  reservation3.table()->name = "moo";

  reservation2.complete();

  auto reservation4 = meta.createTable();
  reservation4.table()->name = "boo";

  reservation4.complete();
  reservation1.complete();
  reservation3.complete();

  REQUIRE(meta.size() == 4);
  REQUIRE(meta[0]->name == "bar");
  REQUIRE(meta[1]->name == "boo");
  REQUIRE(meta[2]->name == "foo");
  REQUIRE(meta[3]->name == "moo");
}

TEST_CASE("Metadata table insertion fails over limit", "[metadata]") {
  metadata::Metadata meta;

  const auto maxSize = metadata::limits::maximum_table_count;

  const std::size_t reservationCount = 3;
  const auto insertFirstCount = maxSize - reservationCount;

  for (std::size_t i = 0; i < insertFirstCount; ++i) {
    auto reservation = meta.createTable();
    reservation.table()->name = "foo";
    reservation.table()->name += std::to_string(i);
    reservation.complete();
  }

  // we should be able to reserve 3 more tables

  std::vector<metadata::Metadata::Reservation> reserves;

  for (std::size_t i = 0; i < reservationCount; ++i) {
    auto reserv = meta.createTable();
    REQUIRE(reserv.open());
    reserves.push_back(std::move(reserv));
  }

  auto reserv = meta.createTable();
  REQUIRE(!reserv.open());

  reserves[2].cancel();

  reserv = meta.createTable();
  REQUIRE(reserv.open());
}

TEST_CASE("Tables can be altered in metadata", "[metadata]") {
  metadata::Metadata meta;

  insert4tables(meta);

  SECTION("A single alter works") {
    auto reservation = meta.alterTable(1);
    reservation.table()->name = "barbar";
    reservation.complete();

    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "barbar");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "boo");
  }

  SECTION("Alters can be interleaved on different tables") {
    auto reservation = meta.alterTable(1);
    reservation.table()->name = "bar";

    auto reservation2 = meta.alterTable(2);
    reservation2.table()->name = "moobar";
    reservation2.complete();
    reservation.complete();

    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moobar");
    REQUIRE(meta[3]->name == "boo");
  }

  SECTION("Alters can be cancelled") {
    auto reservation = meta.alterTable(1);
    reservation.table()->name = "barbar";
    reservation.cancel();

    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "boo");
  }

  SECTION("With double alter, the second blocks and up to date") {
    metadata::Metadata meta;

    insert4tables(meta);

    auto res1 = meta.alterTable(2);
    decltype(res1) res2;

    std::atomic<bool> alter_created = false;
    // waits for res1, as it holds the end lock
    std::thread thr_do_create([&]() {
      res2 = meta.alterTable(2);
      alter_created = true;
      REQUIRE(res2.table()->name == "moobar");
      res2.table()->name = "moobarbar";
      res2.complete();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(!alter_created);

    res1.table()->name = "moobar";
    res1.complete();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(alter_created);

    thr_do_create.join();

    REQUIRE(meta.size() == 4);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moobarbar");
    REQUIRE(meta[3]->name == "boo");
  }
}

TEST_CASE("Tables can be deleted in metadata", "[metadata]") {
  metadata::Metadata meta;

  insert4tables(meta);

  SECTION("A single table can be dropped in the middle") {

    meta.dropTable(1).complete();

    REQUIRE(meta.size() == 3);
    REQUIRE(meta[0]->name == "foo");
    // REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    // boo got moved as it was last
    REQUIRE(meta[1]->name == "boo");
  }

  SECTION("A single table can be dropped at the start") {
    metadata::Metadata meta;

    insert4tables(meta);

    meta.dropTable(0).complete();

    REQUIRE(meta.size() == 3);
    // REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    // boo got moved as it was last
    REQUIRE(meta[0]->name == "boo");
  }

  SECTION("A single table can be dropped at the end") {
    metadata::Metadata meta;

    insert4tables(meta);

    meta.dropTable(3).complete();

    REQUIRE(meta.size() == 3);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    // REQUIRE(meta[0]->name == "boo");
  }

  SECTION("Interleaved deletes don't conflict") {
    metadata::Metadata meta;

    insert4tables(meta);

    auto res1 = meta.dropTable(2);
    auto res2 = meta.dropTable(1);

    res2.complete();
    res1.complete();

    REQUIRE(meta.size() == 2);
    REQUIRE(meta[0]->name == "foo");
    // REQUIRE(meta[1]->name == "bar");
    // REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[1]->name == "boo");
  }

  SECTION("Interleaved deletes work at the end") {
    metadata::Metadata meta;

    insert4tables(meta);

    auto res1 = meta.dropTable(3);

    std::atomic<bool> delete_thread_completed = false;
    // waits for res1, as it holds the end lock
    std::jthread thr_do_create([&]() {
      auto res2 = meta.dropTable(2);
      res2.complete();
      delete_thread_completed = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(res1.open());

    res1.complete();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(!res1.open());
    REQUIRE(meta.size() == 2);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    // REQUIRE(meta[2]->name == "moo");
    // REQUIRE(meta[3]->name == "boo");
  }

  SECTION("Interleaved deletes work at the end, other direction") {
    metadata::Metadata meta;

    insert4tables(meta);

    auto res1 = meta.dropTable(3);
    auto res2 = meta.dropTable(2);

    res1.complete();
    res2.complete();

    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    // REQUIRE(meta[2]->name == "moo");
    // REQUIRE(meta[3]->name == "boo");
  }

  SECTION("Deletes can be cancelled") {
    metadata::Metadata meta;

    insert4tables(meta);

    auto res = meta.dropTable(3);
    res.cancel();

    REQUIRE(meta.size() == 4);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "boo");
  }

  SECTION("With double delete, the second blocks and invalid") {
    metadata::Metadata meta;

    insert4tables(meta);

    auto res1 = meta.dropTable(3);
    decltype(res1) res2;

    std::atomic<bool> delete_thread_completed = false;
    // waits for res1, as it holds the end lock
    std::jthread thr_do_create([&]() {
      res2 = meta.dropTable(3);
      delete_thread_completed = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(!delete_thread_completed);

    res1.complete();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(delete_thread_completed);

    REQUIRE(!res2.open());
    REQUIRE(meta.size() == 3);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    // REQUIRE(meta[3]->name == "boo");
  }
}

TEST_CASE("Interleaved delete and create works", "[metadata]") {
  metadata::Metadata meta;

  insert4tables(meta);

  SECTION("DROP in middle, Create") {
    auto deleteR = meta.dropTable(1);

    auto createR = meta.createTable();
    createR.table()->name = "foofoo";

    deleteR.complete();
    createR.complete();

    REQUIRE(meta.size() == 4);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "boo");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "foofoo");
  }

  SECTION("Create, DROP in middle") {
    auto deleteR = meta.dropTable(1);

    auto createR = meta.createTable();
    createR.table()->name = "foofoo";

    createR.complete();
    deleteR.complete();

    REQUIRE(meta.size() == 4);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "foofoo");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "boo");
  }

  SECTION("DROP at the end, Create") {
    auto deleteR = meta.dropTable(3);

    auto createR = meta.createTable();
    createR.table()->name = "foofoo";

    deleteR.complete();
    createR.complete();

    REQUIRE(meta.size() == 4);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "foofoo");
  }

  SECTION("Create, DROP at the end") {
    auto deleteR = meta.dropTable(3);

    decltype(deleteR) createR;

    std::atomic<bool> create_thread_completed = false;
    // waits for deleteR, as it holds the lock
    std::jthread thr_do_create([&]() {
      createR = meta.createTable();
      createR.table()->name = "foofoo";
      createR.complete();
      create_thread_completed = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // no new record created yet
    REQUIRE(!create_thread_completed);
    REQUIRE(meta.size() == 4);
    REQUIRE(createR.open());
    deleteR.complete();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    REQUIRE(create_thread_completed);
    REQUIRE(!createR.open());

    REQUIRE(meta.size() == 4);
    REQUIRE(meta[0]->name == "foo");
    REQUIRE(meta[1]->name == "bar");
    REQUIRE(meta[2]->name == "moo");
    REQUIRE(meta[3]->name == "foofoo");
  }
}

TEST_CASE("Metadata comparison operators work correctly", "[metadata]") {
  metadata::Metadata meta1, meta2;

  // Empty metadata should be equal
  REQUIRE(meta1 == meta2);
  REQUIRE_FALSE(meta1 != meta2);

  // Add a table to meta1
  {
    auto reservation = meta1.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "test_table";
    reservation.table()->engine = "heap";

    metadata::Column col;
    col.name = "id";
    col.type = metadata::ColumnType::INT;
    col.primary_key = true;
    reservation.table()->columns.push_back(col);

    reservation.complete();
  }

  // Now they should be different
  REQUIRE_FALSE(meta1 == meta2);
  REQUIRE(meta1 != meta2);

  // Add same table to meta2
  {
    auto reservation = meta2.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "test_table";
    reservation.table()->engine = "heap";

    metadata::Column col;
    col.name = "id";
    col.type = metadata::ColumnType::INT;
    col.primary_key = true;
    reservation.table()->columns.push_back(col);

    reservation.complete();
  }

  // Now they should be equal again
  REQUIRE(meta1 == meta2);
  REQUIRE_FALSE(meta1 != meta2);
}

TEST_CASE("Metadata copy constructor works correctly", "[metadata]") {
  metadata::Metadata original;

  // Add a table with various features
  {
    auto reservation = original.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "test_table";
    reservation.table()->engine = "heap";
    reservation.table()->tablespace = "test_space";

    metadata::Column col1;
    col1.name = "id";
    col1.type = metadata::ColumnType::INT;
    col1.primary_key = true;
    col1.auto_increment = true;
    reservation.table()->columns.push_back(col1);

    metadata::Column col2;
    col2.name = "name";
    col2.type = metadata::ColumnType::VARCHAR;
    col2.length = 100;
    col2.nullable = true;
    reservation.table()->columns.push_back(col2);

    metadata::Index idx;
    idx.name = "idx_name";
    idx.unique = false;
    metadata::IndexColumn idx_col;
    idx_col.column_name = "name";
    idx_col.ordering = metadata::IndexOrdering::asc;
    idx.fields.push_back(idx_col);
    reservation.table()->indexes.push_back(idx);

    reservation.complete();
  }

  // Copy construct
  metadata::Metadata copy(original);

  // Should be equal
  REQUIRE(copy == original);
  REQUIRE(copy.size() == 1);

  auto copied_table = copy[0];
  REQUIRE(copied_table != nullptr);
  REQUIRE(copied_table->name == "test_table");
  REQUIRE(copied_table->columns.size() == 2);
  REQUIRE(copied_table->indexes.size() == 1);
}

TEST_CASE("Metadata reset function works correctly", "[metadata]") {
  metadata::Metadata meta;

  // Add multiple tables
  for (int i = 0; i < 3; ++i) {
    auto reservation = meta.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "table_" + std::to_string(i);
    reservation.complete();
  }

  REQUIRE(meta.size() == 3);

  // Reset
  meta.reset();

  // Should be empty now
  REQUIRE(meta.size() == 0);
  REQUIRE(meta[0] == nullptr);
  REQUIRE(meta[1] == nullptr);
  REQUIRE(meta[2] == nullptr);
}

TEST_CASE("Column comparison operators work correctly", "[metadata]") {
  metadata::Column col1, col2;

  // Default columns should be equal
  REQUIRE(col1 == col2);
  REQUIRE_FALSE(col1 != col2);

  // Change one field
  col1.name = "test_col";
  REQUIRE_FALSE(col1 == col2);
  REQUIRE(col1 != col2);

  // Make them equal again
  col2.name = "test_col";
  REQUIRE(col1 == col2);

  // Test different field types
  col1.type = metadata::ColumnType::VARCHAR;
  col1.length = 100;
  col1.primary_key = true;
  col1.foreign_key_references = "other_table";

  col2.type = metadata::ColumnType::VARCHAR;
  col2.length = 100;
  col2.primary_key = true;
  col2.foreign_key_references = "other_table";

  REQUIRE(col1 == col2);
}

TEST_CASE("Metadata comparison is order-independent", "[metadata]") {
  metadata::Metadata meta1, meta2;

  // Add tables to meta1 in one order
  {
    auto reservation = meta1.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "table_a";

    metadata::Column col1;
    col1.name = "id";
    col1.type = metadata::ColumnType::INT;
    reservation.table()->columns.push_back(col1);

    metadata::Column col2;
    col2.name = "name";
    col2.type = metadata::ColumnType::VARCHAR;
    reservation.table()->columns.push_back(col2);

    reservation.complete();
  }

  {
    auto reservation = meta1.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "table_b";

    metadata::Column col;
    col.name = "data";
    col.type = metadata::ColumnType::TEXT;
    reservation.table()->columns.push_back(col);

    reservation.complete();
  }

  // Add same tables to meta2 in different order and with columns in different
  // order
  {
    auto reservation = meta2.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "table_b";

    metadata::Column col;
    col.name = "data";
    col.type = metadata::ColumnType::TEXT;
    reservation.table()->columns.push_back(col);

    reservation.complete();
  }

  {
    auto reservation = meta2.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "table_a";

    // Add columns in different order
    metadata::Column col2;
    col2.name = "name";
    col2.type = metadata::ColumnType::VARCHAR;
    reservation.table()->columns.push_back(col2);

    metadata::Column col1;
    col1.name = "id";
    col1.type = metadata::ColumnType::INT;
    reservation.table()->columns.push_back(col1);

    reservation.complete();
  }

  // Should be equal despite different order
  REQUIRE(meta1 == meta2);
  REQUIRE_FALSE(meta1 != meta2);
}

TEST_CASE("Table comparison is order-independent for columns and indexes",
          "[metadata]") {
  metadata::Table table1, table2;

  table1.name = "test_table";
  table1.engine = "heap";

  table2.name = "test_table";
  table2.engine = "heap";

  // Add columns to table1 in one order
  metadata::Column col1;
  col1.name = "id";
  col1.type = metadata::ColumnType::INT;
  col1.primary_key = true;
  table1.columns.push_back(col1);

  metadata::Column col2;
  col2.name = "name";
  col2.type = metadata::ColumnType::VARCHAR;
  col2.length = 100;
  table1.columns.push_back(col2);

  // Add indexes to table1
  metadata::Index idx1;
  idx1.name = "idx_name";
  idx1.unique = false;
  metadata::IndexColumn idx_col1;
  idx_col1.column_name = "name";
  idx_col1.ordering = metadata::IndexOrdering::asc;
  idx1.fields.push_back(idx_col1);
  table1.indexes.push_back(idx1);

  metadata::Index idx2;
  idx2.name = "idx_id";
  idx2.unique = true;
  metadata::IndexColumn idx_col2;
  idx_col2.column_name = "id";
  idx_col2.ordering = metadata::IndexOrdering::desc;
  idx2.fields.push_back(idx_col2);
  table1.indexes.push_back(idx2);

  // Add same columns to table2 in different order
  table2.columns.push_back(col2); // name first
  table2.columns.push_back(col1); // id second

  // Add same indexes to table2 in different order
  table2.indexes.push_back(idx2); // idx_id first
  table2.indexes.push_back(idx1); // idx_name second

  // Should be equal despite different order
  REQUIRE(table1 == table2);
  REQUIRE_FALSE(table1 != table2);
}

TEST_CASE("Index comparison is order-dependent for fields", "[metadata]") {
  metadata::Index idx1, idx2;

  idx1.name = "composite_idx";
  idx1.unique = false;

  idx2.name = "composite_idx";
  idx2.unique = false;

  // Add fields to idx1 in one order
  metadata::IndexColumn field1;
  field1.column_name = "col_a";
  field1.ordering = metadata::IndexOrdering::asc;
  idx1.fields.push_back(field1);

  metadata::IndexColumn field2;
  field2.column_name = "col_b";
  field2.ordering = metadata::IndexOrdering::desc;
  idx1.fields.push_back(field2);

  // Add same fields to idx2 in different order
  idx2.fields.push_back(field2); // col_b first
  idx2.fields.push_back(field1); // col_a second

  // Should NOT be equal because field order is important for indexes
  REQUIRE_FALSE(idx1 == idx2);
  REQUIRE(idx1 != idx2);

  // But if we add fields in same order, they should be equal
  metadata::Index idx3;
  idx3.name = "composite_idx";
  idx3.unique = false;
  idx3.fields.push_back(field1); // col_a first
  idx3.fields.push_back(field2); // col_b second

  REQUIRE(idx1 == idx3);
  REQUIRE_FALSE(idx1 != idx3);
}

TEST_CASE("Metadata debug output functions work correctly", "[metadata]") {
  metadata::Metadata meta;

  // Add a test table
  {
    auto reservation = meta.createTable();
    REQUIRE(reservation.open());
    reservation.table()->name = "debug_test_table";
    reservation.table()->engine = "heap";

    metadata::Column col;
    col.name = "id";
    col.type = metadata::ColumnType::INT;
    col.primary_key = true;
    col.auto_increment = true;
    reservation.table()->columns.push_back(col);

    metadata::Index idx;
    idx.name = "test_idx";
    idx.unique = true;
    metadata::IndexColumn idx_col;
    idx_col.column_name = "id";
    idx_col.ordering = metadata::IndexOrdering::asc;
    idx.fields.push_back(idx_col);
    reservation.table()->indexes.push_back(idx);

    reservation.complete();
  }

  // Test debug output functions don't crash and produce non-empty output
  std::string debug_output = meta.debug_dump();
  REQUIRE_FALSE(debug_output.empty());
  REQUIRE(debug_output.find("debug_test_table") != std::string::npos);
  REQUIRE(debug_output.find("id INT") != std::string::npos);
  REQUIRE(debug_output.find("test_idx UNIQUE") != std::string::npos);
}
