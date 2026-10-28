/**
 * @file src/nvhttp_query.cpp
 * @brief Preserve the existing launch-query lookup and lenient field decoders.
 */
#include "nvhttp_query.h"
#include "utility.h"

#include <stdexcept>

namespace nvhttp {
  std::string get_arg(const args_t &args, const char *name, const char *default_value) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  std::string decode_client_rikey(const args_t &args) {
    return util::from_hex_vec(get_arg(args, "rikey"), true);
  }

  std::int64_t decode_client_corever(const args_t &args) {
    return util::from_view(get_arg(args, "corever", "0"));
  }

  std::uint32_t decode_client_iv_prefix(const args_t &args) {
    return util::endian::big<std::uint32_t>(util::from_view(get_arg(args, "rikeyid")));
  }
}
