
#include "random.hpp"

#include <algorithm>
#include <functional>
#include <random>
#include <vector>

namespace {
typedef std::vector<char> char_array;

char_array charset() {
  return char_array({'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A',
                     'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
                     'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
                     'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',
                     'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
                     't', 'u', 'v', 'w', 'x', 'y', 'z'});
};

inline std::string random_str(size_t length,
                              std::function<char(void)> rand_char) {
  std::string str(length, 0);
  std::generate_n(str.begin(), length, rand_char);
  return str;
}

} // namespace
  //

ps_random::ps_random() : seed(std::random_device{}()), rng(seed) {}

std::string ps_random::random_string(std::size_t min_length,
                                     std::size_t max_length) {
  const auto ch_set = charset();

  std::uniform_int_distribution<> dist(0, ch_set.size() - 1);
  auto randchar = [&]() { return ch_set[dist(rng)]; };

  auto length = random_number(min_length, max_length);

  return random_str(length, randchar);
}
