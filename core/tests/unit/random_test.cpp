#include <catch2/catch_test_macros.hpp>

#include "random.hpp"

TEST_CASE("fixed seed reproduces sequences", "[random]") {
  ps_random a(42);
  ps_random b(42);
  for (int i = 0; i < 100; ++i) {
    REQUIRE(a.random_number<int>(0, 1000000) ==
            b.random_number<int>(0, 1000000));
  }
  REQUIRE(a.random_string(5, 20) == b.random_string(5, 20));
}

TEST_CASE("different seeds diverge", "[random]") {
  ps_random a(1);
  ps_random b(2);
  bool differs = false;
  for (int i = 0; i < 32 && !differs; ++i) {
    differs =
        a.random_number<int>(0, 1000000) != b.random_number<int>(0, 1000000);
  }
  REQUIRE(differs);
}
