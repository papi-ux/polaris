/** Read-only verification of packaged and explicitly installed Spaces policy. */
#pragma once
#ifdef __linux__
#include "multiseat_container_host.h"
#include <optional>

namespace multiseat::spaces {
  struct security_facts_t {
    bool seccomp = false;
    bool selinux = false;
    bool kernel_probe = false, enforcing = false, contexts = false, rule = false, receipt = false;
  };
  struct security_status_t {
    bool ready = false;
    std::string code, detail;
  };
  security_status_t describe_security(const security_facts_t &facts);
  security_facts_t inspect_security(container::host_t &host);
  // Empty label means SELinux is absent. Missing optional means not ready.
  std::optional<std::string> installed_worker_label(const security_facts_t &facts);
}
#endif
