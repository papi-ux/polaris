/**
 * @file src/platform/linux/spaces_nvidia_libraries.h
 * @brief The NVIDIA userspace a host-driver Space borrows from this machine.
 *
 * A host-driver runtime image carries no driver libraries. Polaris resolves them
 * here, proves each one by its ELF header and its SONAME, and hands the backend an
 * exact list of read-only binds. Nothing is copied, relabelled or executed, and a
 * file that cannot be proven is a Host Setup finding rather than a broken launch.
 */
#pragma once
#ifdef __linux__
#include "multiseat_container_backend.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace multiseat::spaces {
  struct nvidia_architecture_t {
    std::string name;  ///< "amd64" or "i386"
    std::filesystem::path directory;  ///< where the container sees this ABI's libraries
    std::vector<std::filesystem::path> search;  ///< host directories, in order
    std::vector<std::string> required;  ///< sonames without which the ABI is unusable
  };

  struct nvidia_vendor_file_t {
    std::filesystem::path destination;  ///< fixed path inside the container
    std::vector<std::filesystem::path> sources;  ///< host candidates, in order
  };

  struct nvidia_contract_t {
    unsigned contract = 0;
    std::string minimum_driver;
    std::vector<std::string> library_prefixes;
    std::vector<nvidia_architecture_t> architectures;
    std::vector<nvidia_vendor_file_t> vendor_files;
  };

  /** One proven host library, named by the SONAME it will be published under. */
  struct host_library_t {
    std::string architecture;
    std::string soname;
    std::filesystem::path host_path;
  };

  /** A vendor description rewritten to name bare sonames the container can resolve. */
  struct host_vendor_file_t {
    std::filesystem::path destination;
    std::string contents;
  };

  struct host_driver_facts_t {
    std::string driver_version;
    unsigned contract = 0;
    std::vector<host_library_t> libraries;
    std::vector<host_vendor_file_t> vendor_files;
    /**
     * Empty when every architecture and vendor file resolved. Otherwise one of
     * driver_libraries_missing, driver_libraries_32bit_missing,
     * driver_libraries_untrusted, driver_configuration_missing or
     * driver_below_minimum, which Host Setup and the launch refusal both name.
     */
    std::string code;
    std::vector<std::string> missing;  ///< sonames or paths behind the code
    [[nodiscard]] bool ready() const { return code.empty(); }
  };

  [[nodiscard]] std::optional<nvidia_contract_t> decode_nvidia_contract(std::string_view payload);
  /** The contract compiled into this build, or nothing when it does not parse. */
  [[nodiscard]] const std::optional<nvidia_contract_t> &trusted_nvidia_contract();

  /** True when `candidate` is at least `minimum`, comparing dotted numbers. */
  [[nodiscard]] bool driver_at_least(std::string_view candidate, std::string_view minimum);

  /**
   * Resolve the host's driver files. `root` exists so tests can point the search
   * at a tree they built; production passes "/". Trust is asked of `host`, so a
   * test can model a world-writable library without creating one.
   */
  [[nodiscard]] host_driver_facts_t resolve_host_driver_libraries(
    const nvidia_contract_t &contract, const container::host_t &host,
    std::string_view loaded_driver, std::string_view minimum_driver,
    const std::filesystem::path &root = "/");

  /**
   * The binds those facts turn into: every library at its own SONAME under the
   * architecture's directory, then each vendor file Polaris wrote. Empty unless
   * the facts are ready.
   */
  [[nodiscard]] std::vector<container::host_driver_mount_t> host_driver_mounts(
    const nvidia_contract_t &contract, const host_driver_facts_t &facts,
    const std::filesystem::path &vendor_directory);

  /**
   * Write the rewritten vendor files under `directory`, which must be private to
   * Polaris. Returns false when any write fails; the caller then has no mounts.
   */
  [[nodiscard]] bool publish_vendor_files(
    const host_driver_facts_t &facts, const std::filesystem::path &directory);
}
#endif
