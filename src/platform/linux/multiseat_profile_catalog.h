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
  /// The Default Space value that means Desktop. No Space can use this id.
  inline constexpr std::string_view desktop_profile_key = "desktop";

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
    /// Devices whose Default Space is Desktop. Never also a Space's client_keys.
    std::vector<std::string> desktop_default_clients;
  };

  struct loaded_catalog_t {
    catalog_t catalog;
    std::shared_ptr<void> lease;
  };

  [[nodiscard]] std::optional<catalog_t> decode(std::string_view payload);
  // Throws on invalid state rather than serializing a partially valid catalog.
  [[nodiscard]] std::string encode(const catalog_t &catalog);
  [[nodiscard]] std::optional<loaded_catalog_t> load(const std::filesystem::path &path);

  /// A request refused for a reason the person can act on. Static text, so a caller can pass it on.
  struct refusal_t {
    std::string_view code, message, action;
  };
  inline constexpr refusal_t desktop_access_required {"desktop_access_required",
    "Give this device Desktop Access before making Desktop its Default Space.",
    "Tick it under Desktop Access, then save its Default Space again."};
  inline constexpr refusal_t space_access_required {"space_access_required",
    "Allow this device under that Space's Device Access before making it the Default Space.",
    "Tick it under the Space's Device Access, then save its Default Space again."};

  struct change_result_t {
    private_state_file::write_status_e status = private_state_file::write_status_e::not_committed;
    std::string error;
    std::optional<refusal_t> refusal;
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
  // Sets where a device opens first: a Space it may already open, or desktop_profile_key for
  // Desktop, which needs Desktop Access once the device has any Space. Saving a default never
  // changes what the device may open: leaving a Space default keeps that Space under Device
  // Access. An empty profile_key is the explicit removal from every Space and from a Desktop default.
  [[nodiscard]] change_result_t set_assignment(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key);
  // Allowing Desktop does not change a Default Space. Removing Desktop Access also ends a Desktop default.
  [[nodiscard]] change_result_t set_desktop_access(const std::filesystem::path &path,
    std::string_view client_key, bool allowed);
  // Allowing a device does not change its Default Space. Disallowing it removes the Space from the
  // device entirely, a Default Space included.
  [[nodiscard]] change_result_t set_access(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key, bool allowed);
  // Supported Gamescope or Steam workloads only. Immutable local images, fresh
  // private storage, and an owned bridge for Steam. No pulls or host binds.
  [[nodiscard]] change_result_t create(const std::filesystem::path &path,
    std::string_view name, std::string_view image, container::host_t &host,
    const workload_plan_t &workload = {workload_kind_e::gamescope, "input-pong-v1"});
  struct space_create_request_t {
    std::string request_id, source_profile_id, name;
    /**
     * The launcher family the new Space runs, when the client picked one rather
     * than naming a Space to copy. Exactly one of this and source_profile_id is
     * set: they answer the same question, and a client that sent both
     * disagreeing would be ambiguous. Either way the new Space copies the image
     * of an existing Space of that family, so no download can be triggered from
     * this path.
     */
    std::string family;
    bool operator==(const space_create_request_t &) const = default;
  };
  [[nodiscard]] bool valid_space_create_request(const space_create_request_t &request);
  [[nodiscard]] std::optional<space_create_request_t> decode_space_create_request(std::string_view payload);
  // Request identity also names the new profile. Retrying the same request can
  // confirm its existing catalog entry but never copies or adopts another home.
  [[nodiscard]] change_result_t create_space(const std::filesystem::path &path,
    const space_create_request_t &request, container::host_t &host);
  struct first_space_request_t {
    std::string request_id, name;
  };
  [[nodiscard]] bool valid_first_space_request(const first_space_request_t &request);
  // First-space storage transaction. Only a missing or empty private catalog
  // can gain its first entry. Matching retries preserve assignments and homes.
  // The caller must obtain image from the approved runtime installer; this does
  // not select a GPU, assign a device, configure or activate the controller.
  // `profile` is the launcher family of the admitted runtime that image is, so
  // a first Heroic Space is made the same way a first Steam one is.
  [[nodiscard]] change_result_t create_first_space(const std::filesystem::path &path,
    const first_space_request_t &request, std::string_view image, std::string_view profile,
    container::host_t &host);
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

  /// One Space moved to another gaming runtime. The caller decides the move
  /// from what it read (from_image and its media contract) and names the
  /// verified local image to move to and what that runtime needs of a home.
  struct runtime_move_t {
    std::string profile_id;
    std::string from_image, from_media_contract;
    std::string to_image, to_media_contract;
    runtime_profile_e to_profile = runtime_profile_e::unknown;
    std::uint32_t to_uid = 0, to_gid = 0;
    bool operator==(const runtime_move_t &) const = default;
  };
  [[nodiscard]] bool valid_runtime_move(const runtime_move_t &move);
  enum class runtime_move_outcome_e {
    moved,                   ///< the Space now launches to_image
    already_moved,           ///< it already did; the unchanged catalog was saved again to confirm it
    invalid,                 ///< not a valid move
    not_found,               ///< no such Space in the catalog
    space_changed,           ///< its runtime is neither from_image nor to_image any more
    profile_mismatch,        ///< the new runtime runs a different kind of Space
    media_contract_mismatch, ///< the new runtime streams under a different media contract
    identity_mismatch,       ///< the new runtime's account is not the one the home belongs to
    docker_unavailable,      ///< Docker did not answer before anything changed
    storage_unverified,      ///< the home is missing or not the storage Polaris made; nothing changed
    not_saved,               ///< the catalog was busy, unsafe or could not be written
  };
  struct runtime_move_result_t {
    runtime_move_outcome_e outcome = runtime_move_outcome_e::invalid;
    /// durability_uncertain when the write may or may not have landed; callers fail closed.
    private_state_file::write_status_e status = private_state_file::write_status_e::not_committed;
    std::string previous_image;  ///< what the Space launched before, when it was found
    explicit operator bool() const {
      return outcome == runtime_move_outcome_e::moved || outcome == runtime_move_outcome_e::already_moved;
    }
  };
  // Changes only the Space's image under the catalog's own lock: its id, name,
  // home volume, network, target and device access stay as they are. The home
  // is checked, not touched: Docker must list it and it must be exactly the
  // volume Polaris made for this Space. It is never initialized again, since the
  // only thing creation did to it was give the empty volume to the account both
  // runtimes run as, and the worker mounts it without copying image content.
  // The write is one atomic replacement, so a retry after a crash reads either
  // the old image and moves it, or the new one and confirms it.
  [[nodiscard]] runtime_move_result_t move_runtime(const std::filesystem::path &path,
    const runtime_move_t &move, container::host_t &host);
  int command(int argc, char **argv);
}  // namespace multiseat::profiles
#endif
