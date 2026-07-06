#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <exception>
#include <set>
#include <thread>
#include <vector>

#include "metadata/catalog.hpp"
#include "random.hpp"

namespace {
struct Widget : metadata::ObjectBase {
  int payload = 0;
};
} // namespace

TEST_CASE("Catalog survives concurrent mixed operations", "[catalog][stress]") {
  metadata::Registry<Widget> registry;
  auto &catalog = registry.get<Widget>();

  constexpr std::size_t threadCount = 8;
  constexpr std::size_t opsPerThread = 5000;

  std::vector<std::exception_ptr> failures(threadCount);

  std::vector<std::thread> threads;
  threads.reserve(threadCount);

  for (std::size_t t = 0; t < threadCount; ++t) {
    threads.emplace_back([&registry, &catalog, &failures, t]() {
      ps_random rand(t + 1);

      try {
        for (std::size_t op = 0; op < opsPerThread; ++op) {
          if (t == 0 && op % 1000 == 0) {
            registry.reset(); // production resets during churn; deterministic
                              // coverage
          }
          switch (rand.random_number<std::size_t>(0, 5)) {
          case 0: { // insert, sometimes with a pooled (colliding) name
            Widget w;
            w.id = registry.nextId();
            w.name =
                rand.random_number<std::size_t>(0, 3) == 0
                    ? fmt::format("n{}", rand.random_number<std::size_t>(0, 15))
                    : fmt::format("w{}", w.id);
            catalog.insert(std::move(w));
            break;
          }
          case 1: { // erase random
            auto victim = catalog.randomPick(rand);
            if (victim != nullptr) {
              catalog.erase(victim->id);
            }
            break;
          }
          case 2: { // update payload
            auto target = catalog.randomPick(rand);
            if (target != nullptr) {
              catalog.update(target->id, [](Widget &w) {
                w.payload += 1;
                return true;
              });
            }
            break;
          }
          case 3: { // rename into a small shared pool to force collisions
            auto target = catalog.randomPick(rand);
            if (target != nullptr) {
              auto newName =
                  fmt::format("n{}", rand.random_number<std::size_t>(0, 15));
              catalog.update(target->id, [&](Widget &w) {
                w.name = newName;
                return true;
              });
            }
            break;
          }
          case 4: { // reads
            auto byPick = catalog.randomPick(rand);
            if (byPick != nullptr) {
              (void)catalog.byId(byPick->id);
              (void)catalog.byName(byPick->name);
            }
            break;
          }
          case 5: { // full snapshot
            (void)catalog.snapshotAll();
            break;
          }
          }
        }
      } catch (...) {
        failures[t] = std::current_exception();
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }

  for (auto const &failure : failures) {
    if (failure) {
      REQUIRE_NOTHROW(std::rethrow_exception(failure));
    }
  }

  // single-threaded invariant checks
  auto all = catalog.snapshotAll();
  REQUIRE(catalog.size() == all.size());

  std::set<metadata::ObjectId> uniqueIds;
  for (auto const &rec : all) {
    uniqueIds.insert(rec->id);
  }
  REQUIRE(uniqueIds.size() == all.size());

  for (auto const &rec : all) {
    auto byId = catalog.byId(rec->id);
    REQUIRE(byId != nullptr);
    REQUIRE(byId->id == rec->id);

    auto byName = catalog.byName(rec->name);
    if (byName != nullptr) {
      // name may have been won by another record; whoever owns it must
      // actually carry that name
      REQUIRE(byName->name == rec->name);
    }
  }
}
