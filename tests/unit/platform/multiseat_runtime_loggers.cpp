/**
 * @file tests/unit/platform/multiseat_runtime_loggers.cpp
 * @brief The host log's severity loggers, for the standalone multiseat test executable.
 */
// test_multiseat_runtime stays independent of Polaris's runtime and does not link
// src/logging.cpp. The container backend logs why it refused a worker start, so this
// executable supplies the same logger objects logging.h declares, at the same levels;
// tests read what they write through tests_log_capture.h. An undefined `error` must
// never resolve to libc's error() function.
#include "src/logging.h"

boost::log::sources::severity_logger<int> verbose(0), debug(1), info(2), warning(3), error(4), fatal(5);
