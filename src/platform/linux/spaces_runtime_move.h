/**
 * @file src/platform/linux/spaces_runtime_move.h
 * @brief Moving a Space to the gaming runtime for the NVIDIA driver this PC now runs.
 *
 * The NVIDIA runtime carries the NVIDIA userspace for one driver version, and a
 * Space launches the exact image it was created from. After a driver update the
 * two disagree, NVML and NVENC fail inside the worker, and the Space cannot
 * start. A move points the Space at the runtime for the loaded driver and keeps
 * its home, so the Steam sign-in and the installed games stay.
 */
#pragma once
#ifdef __linux__
#include "multiseat_launch_service.h"
#include "multiseat_profile_catalog.h"
#include "spaces_runtime.h"
#include "spaces_setup.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <thread>

namespace multiseat::spaces {
  /// What one runtime image was built for.
  struct image_runtime_t {
    bool known = false;  ///< read from the compiled catalog or from the image's own labels
    std::string runtime_id;  ///< the catalog entry that is this image, when one is
    std::string profile, media_contract;
    std::string nvidia_driver;  ///< empty for a runtime without NVIDIA userspace
    /// "host" for an image that borrows the machine's driver, empty otherwise.
    std::string nvidia_source;
    bool operator==(const image_runtime_t &) const = default;
  };
  /// The catalog entry whose image this is, without asking Docker.
  [[nodiscard]] std::optional<image_runtime_t> catalog_image_runtime(std::string_view image,
    const std::vector<runtime_t> &catalog);
  /// The labels of one `docker image inspect IMAGE` reply. Nothing unless the reply describes
  /// exactly that image and every label it needs is well formed.
  [[nodiscard]] std::optional<image_runtime_t> labeled_image_runtime(std::string_view image,
    std::string_view inspection);

  /// An image ID names immutable content, so what it was built for never changes. Only answers
  /// read from the image are kept; a missing image or a silent Docker is asked again next time.
  class image_runtime_cache_t {
  public:
    [[nodiscard]] std::optional<image_runtime_t> find(const std::string &image) const;
    void remember(const std::string &image, const image_runtime_t &runtime);

  private:
    mutable std::mutex mutex_;
    std::map<std::string, image_runtime_t> entries_;
  };
  [[nodiscard]] image_runtime_cache_t &image_runtime_cache();

  /// The compiled catalog first, then the cache, then one bounded `docker image inspect`.
  [[nodiscard]] image_runtime_t identify_image(container::host_t &host, std::string_view image,
    const std::vector<runtime_t> &catalog, image_runtime_cache_t *cache = nullptr);
  /// Whether a Space launching this image needs this machine's driver files: a runtime this build
  /// lists as borrowing them, or an older build of one, known by the labels it was built with.
  [[nodiscard]] bool image_borrows_host_driver(container::host_t &host, std::string_view image,
    const std::vector<runtime_t> &catalog, image_runtime_cache_t *cache = nullptr);

  /// The image names an NVIDIA driver and the loaded driver is a different, readable version.
  [[nodiscard]] bool driver_mismatch(const image_runtime_t &image, const std::optional<std::string> &host_driver);

  /// What the Spaces page shows for one Space: runtime_driver, host_driver, runtime_mismatch and,
  /// when it mismatches, runtime_move naming the catalog runtime for the loaded driver and whether
  /// Docker already holds it verified.
  [[nodiscard]] nlohmann::json describe_space_runtime(const image_runtime_t &image,
    const std::optional<std::string> &host_driver, const runtime_choice_t &choice, runtime_image_e target);
  /// describe_space_runtime for each Space, keyed by its id. Docker is asked once about each
  /// image the catalog does not name, on any graphics, and about the runtime a Space would move
  /// to only when a move is on offer. Both answers are kept.
  [[nodiscard]] nlohmann::json describe_space_runtimes(container::host_t &host,
    const std::vector<profile_summary_t> &profiles, const std::vector<runtime_t> &catalog,
    const std::optional<std::string> &host_driver, image_runtime_cache_t *images,
    runtime_inspection_cache_t *targets);

  /// The launchers a Space can be made for on this PC, for the page's picker: family, has_space,
  /// installed and runtime_id. One this PC already runs a Space for lends the next Space its
  /// image, so it is listed as installed and Docker is not asked. Any other is listed only when
  /// this build has a runtime for it here, with whether Docker already holds that runtime.
  [[nodiscard]] nlohmann::json describe_launchers(container::host_t &host,
    const std::vector<profile_summary_t> &profiles, const std::vector<runtime_t> &catalog,
    const std::optional<std::string> &host_driver, runtime_inspection_cache_t *targets);

  /// The launch guard: false only when the image is known to be built for an NVIDIA driver
  /// other than the one loaded now. Logs the refusal with both versions.
  [[nodiscard]] bool runtime_matches_loaded_driver(std::string_view image);

  /// Everything a move is decided on, read just before it is accepted.
  struct move_facts_t {
    bool admin_available = false;
    std::optional<profile_summary_t> space;
    image_runtime_t image;  ///< what the Space's current image was built for
    std::optional<std::string> host_driver;
    runtime_choice_t choice;  ///< the catalog runtime for the loaded driver
    runtime_image_e target = runtime_image_e::unverifiable;  ///< what Docker holds for it
    std::uint32_t home_uid = 0, home_gid = 0;  ///< the account the Space's home belongs to
    bool space_active = false, streaming = false, changing = false;
    bool setup_running = false, host_setup_running = false;
  };
  struct move_decision_t {
    profile_launch_result_t result;  ///< 202 to start, 200 when already moved, otherwise the refusal
    std::optional<runtime_t> target;
  };
  /// Why a move cannot start, most lasting reason first: the Space and its runtime, then what
  /// is happening on the host right now. runtime_id is the runtime the page offered.
  [[nodiscard]] move_decision_t decide_move(const move_facts_t &facts, std::string_view runtime_id);

  struct move_request_t {
    std::string request_id, profile_id, runtime_id;
    bool operator==(const move_request_t &) const = default;
  };
  [[nodiscard]] std::optional<move_request_t> decode_move_request(std::string_view payload);

  /// Everything the first Space of a launcher is decided on, read just before it is accepted.
  struct create_facts_t {
    bool admin_available = false;
    runtime_choice_t choice;  ///< the catalog runtime for that launcher on this PC
    runtime_image_e target = runtime_image_e::unverifiable;  ///< what Docker holds for it
    bool streaming = false, changing = false, setup_running = false, host_setup_running = false;
  };
  /// Why the first Space of a launcher cannot be started: what this build publishes, then what
  /// is happening on the host right now. A download is never begun under a running stream.
  [[nodiscard]] move_decision_t decide_create(const create_facts_t &facts);

  struct move_operations_t {
    std::function<move_facts_t(const move_request_t &)> facts;
    /// Checks Docker for the runtime and downloads it when missing, verified against the catalog.
    std::function<runtime_install_result_t(const runtime_t &, std::stop_token)> install;
    /// Hands the catalog change to the Spaces owner; 202 means it is still running.
    std::function<profile_launch_result_t(const profiles::runtime_move_t &)> move;
    std::function<create_facts_t(const profiles::space_create_request_t &)> create_facts;
    /// Makes the Space once its runtime is on this PC; 202 means it is still being made.
    std::function<profile_launch_result_t(const profiles::space_create_request_t &)> create;
  };

  /// A move request's answer. Its words can come from Docker's download, so they are owned here.
  struct move_answer_t {
    int status = 503;
    std::string code, message, action;
  };

  /// One move at a time, remembered in memory. The catalog is the durable record: a Polaris
  /// that stops midway loses the job, and the Space shows as moved or still mismatched.
  class move_service_t {
  public:
    explicit move_service_t(move_operations_t operations,
      std::chrono::milliseconds retry_delay = std::chrono::milliseconds(500));
    ~move_service_t();
    move_service_t(const move_service_t &) = delete;
    move_service_t &operator=(const move_service_t &) = delete;
    /// 202 started or still running, 200 done, otherwise a refusal with its code. The same
    /// request asked again answers with its job, running or finished.
    [[nodiscard]] move_answer_t submit(const move_request_t &request);
    /// The first Space of a launcher whose runtime is not on this PC: the same job, download
    /// and verify first, with making the Space where a move would be. Making a Space any other
    /// way downloads nothing, and a Space cannot be made without its runtime, so this is the one
    /// road to a second launcher. A request that failed may be asked again: what failed is
    /// almost always the download, and the request identity names the Space, not the attempt.
    [[nodiscard]] move_answer_t submit_create(const profiles::space_create_request_t &request);
    /// The last job, or null: kind (move or create), request_id, profile_id, runtime_id,
    /// nvidia_driver, state (downloading, moving, creating, done or failed), code, message and
    /// action. A create job has no profile_id and names its family and the Space's name.
    [[nodiscard]] nlohmann::json snapshot() const;
    /// A job is downloading or moving.
    [[nodiscard]] bool active() const;
    void shutdown();

  private:
    struct job_t {
      move_request_t request;
      runtime_t target;
      profiles::runtime_move_t move;
      std::optional<profiles::space_create_request_t> creation;  ///< set for the first Space of a launcher
      std::string state, code, message, action;
      int status = 202;
    };
    void work(std::stop_token stop);
    void finish(int status, std::string state, std::string code, std::string message, std::string action);
    move_operations_t operations_;
    std::chrono::milliseconds retry_delay_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<job_t> job_;
    bool active_ = false, deciding_ = false, closing_ = false;
    std::jthread worker_;
  };

  [[nodiscard]] std::shared_ptr<move_service_t> make_move_service();
  bool install_move_service(const std::shared_ptr<move_service_t> &service);
  void uninstall_move_service(const std::shared_ptr<move_service_t> &service);
  [[nodiscard]] std::shared_ptr<move_service_t> installed_move_service();
}  // namespace multiseat::spaces
#endif
