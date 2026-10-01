// The standalone binaries do not start Polaris logging/config/runtime.
// Supply real severity-logger objects declared by logging.h; in particular,
// an undefined `error` object must never resolve to libc's error() function.
#include "src/logging.h"
boost::log::sources::severity_logger<int> verbose(0),debug(1),info(2),warning(3),error(4),fatal(5);
