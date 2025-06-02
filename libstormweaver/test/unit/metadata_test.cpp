
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
