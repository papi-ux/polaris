// Both actual generated interfaces must coexist, with distinct types and guards.
#include "slangmosh.hpp"
#include "slangmosh_scaler.hpp"
#include <type_traits>

int main() {
  using Codec = PyroWave::Shaders<int, int>;
  using Scaler = PolarisPyroWaveScaler::Shaders<int, int>;
  static_assert(!std::is_same_v<Codec, Scaler>);
  Codec codec;
  Scaler scaler;
  codec.dwt[2] = 17;
  scaler.scaler = 29;
  return codec.dwt[2] == 17 && scaler.scaler == 29 ? 0 : 1;
}
