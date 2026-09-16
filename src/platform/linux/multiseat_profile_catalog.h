/**
 * @file src/platform/linux/multiseat_profile_catalog.h
 * @brief Private profile storage and administrative Docker provisioning.
 */
#pragma once
#ifdef __linux__

#include "multiseat_container_backend.h"
#include "src/private_state_file.h"

namespace multiseat::profiles {
  inline constexpr std::size_t maximum_catalog_bytes = 4 * 1024 * 1024;

  struct entry_t {
    container::profile_t storage;
    std::string name;
    workload_plan_t workload;
    std::vector<std::string> client_keys;
    bool archived = false;
    std::vector<std::string> access_clients;
  };

  struct catalog_t {
    std::uint32_t owner_uid = 0;
    std::uint32_t owner_gid = 0;
    std::vector<entry_t> profiles;
    std::vector<std::string> desktop_clients;
  };

  struct loaded_catalog_t {
    catalog_t catalog;
    std::shared_ptr<void> lease;
  };

  [[nodiscard]] std::optional<catalog_t> decode(std::string_view payload);
  // Throws on invalid state rather than serializing a partially valid catalog.
  [[nodiscard]] std::string encode(const catalog_t &catalog);
  [[nodiscard]] std::optional<loaded_catalog_t> load(const std::filesystem::path &path);

  struct change_result_t {
    private_state_file::write_status_e status = private_state_file::write_status_e::not_committed;
    std::string error;
    std::string profile_key;
    // Retain these on any failure after provisioning starts. Never silently
    // adopt, reinitialize, or delete a volume after an uncertain transaction.
    std::string volume_name;
    std::string initializer_name;
    std::string network_name;
    explicit operator bool() const { return status == private_state_file::write_status_e::committed; }
  };

  [[nodiscard]] change_result_t initialize(const std::filesystem::path &path,
    std::uint32_t uid, std::uint32_t gid);
  [[nodiscard]] change_result_t assign(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key);
  [[nodiscard]] change_result_t unassign(const std::filesystem::path &path,
    std::string_view client_key);
  // One atomic move between profiles, or unassignment with an empty profile.
  [[nodiscard]] change_result_t set_assignment(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key);
  // Additional access does not change the default assignment.
  [[nodiscard]] change_result_t set_desktop_access(const std::filesystem::path &path,
    std::string_view client_key, bool allowed);
  [[nodiscard]] change_result_t set_access(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key, bool allowed);
  // Supported Gamescope or Steam workloads only. Immutable local images, fresh
  // private storage, and an owned bridge for Steam. No pulls or host binds.
  [[nodiscard]] change_result_t create(const std::filesystem::path &path,
    std::string_view name, std::string_view image, container::host_t &host,
    const workload_plan_t &workload = {workload_kind_e::gamescope, "input-pong-v1"});
  struct steam_create_request_t {
    std::string request_id, source_profile_id, name;
    bool operator==(const steam_create_request_t &) const = default;
  };
  [[nodiscard]] bool valid_steam_create_request(const steam_create_request_t &request);
  [[nodiscard]] std::optional<steam_create_request_t> decode_steam_create_request(std::string_view payload);
  // Request identity also names the new profile. Retrying the same request can
  // confirm its existing catalog entry but never copies or adopts another home.
  [[nodiscard]] change_result_t create_steam(const std::filesystem::path &path,
    const steam_create_request_t &request, container::host_t &host);
  struct first_steam_request_t {
    std::string request_id, name;
  };
  [[nodiscard]] bool valid_first_steam_request(const first_steam_request_t &request);
  // First-space storage transaction. Only a missing or empty private catalog
  // can gain its first entry. Matching retries preserve assignments and homes.
  // The caller must obtain image from the approved runtime installer; this does
  // not select a GPU, assign a device, configure or activate the controller.
  [[nodiscard]] change_result_t create_first_steam(const std::filesystem::path &path,
    const first_steam_request_t &request, std::string_view image, container::host_t &host);
  // remove archives a Space and keeps its home; remove_for_good deletes both.
  enum class edit_operation_e { rename, remove, restore, remove_for_good };
  struct edit_request_t {
    edit_operation_e operation = edit_operation_e::rename;
    std::string profile_id, name;
    // Removing for good only: the Space's current name exactly as the person
    // typed it, and the request's own identity so a retry can be confirmed.
    std::string confirm_name, request_id;
    bool operator==(const edit_request_t &) const = default;
  };
  [[nodiscard]] bool valid_edit_request(const edit_request_t &request);
  [[nodiscard]] std::optional<edit_request_t> decode_edit_request(std::string_view payload);
  // Catalog-only edits. Removal also unassigns devices; homes and networks are
  // retained in a restorable catalog entry. The controller owner must quiesce launches and release its lease.
  // Removing for good is not a catalog-only edit and is refused here.
  [[nodiscard]] change_result_t edit(const std::filesystem::path &path, const edit_request_t &request);

  enum class removal_outcome_e {
    removed,              ///< home, Steam network and record are gone
    invalid,              ///< not a valid removal for good
    not_found,            ///< no such Space in the catalog
    name_mismatch,        ///< the typed name is not the Space's current name
    last_space,           ///< the only Steam Space; new Spaces are made from an existing one
    not_saved,            ///< the catalog was busy, unsafe or could not be written
    docker_unavailable,   ///< Docker did not answer before anything changed
    storage_unverified,   ///< the home is not the storage Polaris made for this Space; nothing changed
    storage_in_use,       ///< a container still uses the home; nothing changed
    storage_not_removed,  ///< Docker did not confirm the home is gone; the Space stays archived
    record_not_removed,   ///< the home is gone but the record could not be removed; the Space stays archived
  };
  struct removal_result_t {
    removal_outcome_e outcome = removal_outcome_e::invalid;
    /// durability_uncertain when any catalog write was uncertain; callers fail closed.
    private_state_file::write_status_e status = private_state_file::write_status_e::not_committed;
    bool archived = false;      ///< the Space was archived before Docker deleted anything
    std::string kept_volume;    ///< the Docker volume still holding its games and saves, when it was kept
    std::string kept_network;   ///< its Docker network, when Docker did not remove it
    explicit operator bool() const { return outcome == removal_outcome_e::removed; }
  };
  // Removes one Space for good. Every refusal that needs no change comes first:
  // the typed name, the last Steam Space, Docker answering, the home being exactly
  // the volume Polaris created for this Space (local driver, no driver options,
  // its label, its own mountpoint) and no container using it. Then the Space is
  // archived, Docker deletes the home and the Steam network, and the record goes.
  // A failure after the archive leaves an archived Space that a retry can finish.
  // Nothing is deleted outside Docker, and nothing Polaris did not create.
  [[nodiscard]] removal_result_t remove_for_good(const std::filesystem::path &path,
    const edit_request_t &request, container::host_t &host,
    std::chrono::milliseconds wait_for_users = std::chrono::seconds(20));
  int command(int argc, char **argv);
}  // namespace multiseat::profiles
#endif
