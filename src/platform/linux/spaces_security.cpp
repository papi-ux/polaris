#include "spaces_security.h"
#ifdef __linux__
#include "spaces_security_data.h"
#include "multiseat_steam_seccomp.h"
#include "src/utility.h"
#include <dlfcn.h>

namespace multiseat::spaces {
  security_status_t describe_security(const security_facts_t &f) {
    if (!f.seccomp) return {false, "package_missing", "Install the matching native Polaris package. Its required Steam security file is missing or different."};
    if (!f.selinux) return {true, "ready", "The packaged Steam security file is verified. This host does not require SELinux setup."};
    if (!f.kernel_probe) return {false, "probe_failed", "Polaris could not check the host security policy. Open Doctor & Support before continuing."};
    if (!f.enforcing) return {false, "not_enforcing", "Spaces requires SELinux enforcing on this host. Review your system security settings; setup will not change them."};
    if (!f.contexts || !f.rule || !f.receipt) return {false, "install_selinux", "Install or update the dedicated Spaces security support, then reopen Polaris and recheck. Existing manually installed policies need review before replacement."};
    return {true, "ready", "The matching Spaces policies and controller rule are installed, and SELinux is enforcing."};
  }

  security_facts_t inspect_security(container::host_t &host) {
    security_facts_t f;
    f.seccomp = host.trusted_data_file(std::filesystem::path(container::steam_seccomp_path), container::steam_seccomp_data);
    std::error_code error;
    f.selinux = std::filesystem::exists("/sys/fs/selinux/enforce", error);
    if (error) { f.selinux = true; return f; }
    if (!f.selinux) return f;
    void *library = dlopen("libselinux.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) return f;
    auto close = util::fail_guard([&] { dlclose(library); });
    const auto enforcing = reinterpret_cast<int (*)()>(dlsym(library, "security_getenforce"));
    const auto context = reinterpret_cast<int (*)(const char *)>(dlsym(library, "security_check_context"));
    if (!enforcing || !context) return f;
    const auto mode = enforcing();
    f.kernel_probe = mode == 0 || mode == 1;
    f.enforcing = mode == 1;
    const auto marker = "system_u:object_r:" + std::string(security_data::marker) + ":s0";
    f.contexts = context(marker.c_str()) == 0 &&
      context("system_u:object_r:polaris_multiseat_input_device_t:s0") == 0 &&
      context("system_u:system_r:polaris_nvidia_worker_t:s0") == 0;
    f.rule = host.trusted_data_file(std::filesystem::path(security_data::rule_path), security_data::rule);
    f.receipt = host.trusted_data_file(std::filesystem::path(security_data::receipt_path), security_data::receipt);
    return f;
  }

  std::optional<std::string> installed_worker_label(const security_facts_t &facts) {
    if (!describe_security(facts).ready) return std::nullopt;
    return facts.selinux ? "polaris_nvidia_worker_t" : "";
  }
}
#endif
