
#include <algorithm>
#include <random>
#include <string>

// TODO: add constructor with fixed seed
class ps_random {
public:
  ps_random();

  std::string random_string(std::size_t min_length, std::size_t max_length);

  template <typename T> T random_number(T min, T max) {
    if constexpr (std::is_floating_point_v<T>) {
      std::uniform_real_distribution<> len_dist(min, max);
      return len_dist(rng);
    } else {
      std::uniform_int_distribution<> len_dist(min, max);
      return len_dist(rng);
    }
  }

  template <typename T> T random_number() {
    return random_number<T>(std::numeric_limits<T>::min(),
                            std::numeric_limits<T>::max());
  }

  bool random_bool() { return random_number<bool>(0, 1); }

  template <typename T> void shuffle(T &container) {
    std::ranges::shuffle(container, rng);
  }

private:
  std::uint64_t seed;
  std::mt19937_64 rng;
};
