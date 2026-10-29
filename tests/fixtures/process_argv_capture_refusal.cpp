// Stand-in for platf::run_process_argv_capture, linked only into the fast test_polaris target.
// update_status.cpp reaches the host update helper through multiseat::container::local_host_t,
// whose process runner lives in src/platform/linux/misc.cpp with the rest of the platform layer.
// No fast test may start a host process: a call fails the running test and reports nothing run.
#include "src/platform/linux/misc.h"

#include <gtest/gtest.h>

namespace platf {
  process_output_t run_process_argv_capture(
    const std::vector<std::string> &argv,
    std::chrono::milliseconds,
    std::size_t,
    std::stop_token,
    bool
  ) {
    ADD_FAILURE() << "the fast test target does not run host processes: "
                  << (argv.empty() ? std::string {} : argv.front());
    return {};
  }
}  // namespace platf
