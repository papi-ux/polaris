/**
 * @file src/pipewire_producer_binding.h
 * @brief Portable model of the PipeWire graph facts that bind a capture stream to its producer.
 *
 * The capture's own core feeds registry globals into graph_t, then asks resolve() for a
 * capture_producer::binding_t after a completed core sync. Nothing here touches PipeWire, so
 * every rule can be exercised without a server.
 *
 * Only server-set facts are trusted (PipeWire 1.0.5 and 1.6.9 sources, pinned in the evidence):
 * - pipewire.sec.pid / pipewire.sec.uid on a Client global come from SO_PEERCRED at connect and
 *   no client can change any pipewire.sec.* key;
 * - client.id on a client-created Node is overwritten with the owning client's global id and
 *   filtered out of later client updates;
 * - link.output.node / link.input.node on a Link global are the actual port owners.
 * Names, the requested target, application.process.id and any parent process are never proof.
 */
#pragma once

#include "src/capture_producer_binding.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipewire_producer_binding {
  /** Fixed reason codes. A binding is verified only with reason "verified". */
  namespace reason {
    inline constexpr std::string_view verified = "verified";
    inline constexpr std::string_view tracking_disabled = "producer_tracking_disabled";
    inline constexpr std::string_view tracking_unavailable = "producer_tracking_unavailable";
    inline constexpr std::string_view wrong_thread = "binding_on_pipewire_thread";
    inline constexpr std::string_view capture_not_running = "capture_not_running";
    inline constexpr std::string_view capture_stopping = "capture_stopping";
    inline constexpr std::string_view read_timeout = "read_timeout";
    inline constexpr std::string_view cancelled = "read_cancelled";
    inline constexpr std::string_view core_error = "pipewire_core_error";
    inline constexpr std::string_view server_version_unsupported = "server_version_unsupported";
    inline constexpr std::string_view graph_changed = "pipewire_graph_changed";
    inline constexpr std::string_view graph_identity_invalid = "graph_identity_invalid";
    inline constexpr std::string_view self_client_unknown = "capture_client_unknown";
    inline constexpr std::string_view self_credentials_absent = "capture_credentials_absent";
    inline constexpr std::string_view pid_namespace_mismatch = "pid_namespace_mismatch";
    inline constexpr std::string_view capture_node_unknown = "capture_node_unknown";
    inline constexpr std::string_view capture_node_not_owned = "capture_node_not_owned";
    inline constexpr std::string_view source_not_linked = "source_not_linked";
    inline constexpr std::string_view source_ambiguous = "source_ambiguous";
    inline constexpr std::string_view source_unattributed = "source_unattributed";
    inline constexpr std::string_view producer_client_not_visible = "producer_client_not_visible";
    inline constexpr std::string_view producer_credentials_absent = "producer_credentials_absent";
    inline constexpr std::string_view producer_uid_mismatch = "producer_uid_mismatch";
  }  // namespace reason

  inline constexpr std::uint32_t invalid_id = std::numeric_limits<std::uint32_t>::max();

  /** Strict unsigned decimal: digits only, no sign, space or leading zero (except "0"), no overflow. */
  inline std::optional<std::uint64_t> parse_decimal(std::string_view text, std::uint64_t max) {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0')) return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text) {
      if (ch < '0' || ch > '9') return std::nullopt;
      const auto digit = static_cast<std::uint64_t>(ch - '0');
      if (digit > max || value > (max - digit) / 10) return std::nullopt;
      value = value * 10 + digit;
    }
    return value;
  }

  inline std::optional<std::uint32_t> parse_id(std::string_view text) {
    // SPA_ID_INVALID is never a real global id.
    const auto value = parse_decimal(text, std::numeric_limits<std::uint32_t>::max() - 1);
    if (!value) return std::nullopt;
    return static_cast<std::uint32_t>(*value);
  }

  /** A PID as the server prints it ("%d"): a positive int, and never 1 (init cannot be a peer here). */
  inline std::optional<std::int64_t> parse_pid(std::string_view text) {
    const auto value = parse_decimal(text, static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
    if (!value || *value <= 1) return std::nullopt;
    return static_cast<std::int64_t>(*value);
  }

  /** A UID as the server prints it ("%d"). A negative rendering of a large uid is refused. */
  inline std::optional<std::uint64_t> parse_uid(std::string_view text) {
    return parse_decimal(text, static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
  }

  /** "major.minor.micro" with strict decimal parts; anything else is refused. */
  inline std::optional<std::array<std::uint64_t, 3>> parse_version(std::string_view text) {
    std::array<std::uint64_t, 3> parts {};
    for (std::size_t i = 0; i < parts.size(); ++i) {
      const auto dot = text.find('.');
      const auto part = i + 1 < parts.size() ? text.substr(0, dot) : text;
      if (i + 1 < parts.size() && dot == std::string_view::npos) return std::nullopt;
      const auto value = parse_decimal(part, 1'000'000);
      if (!value) return std::nullopt;
      parts[i] = *value;
      if (i + 1 < parts.size()) text.remove_prefix(dot + 1);
    }
    return parts;
  }

  /**
   * Exact server versions whose credential, client.id and link semantics were audited. A future
   * or otherwise unaudited runtime version is unknown until its primary source is reviewed.
   */
  inline constexpr std::array<std::array<std::uint64_t, 3>, 3> audited_server_versions {{{1, 0, 5}, {1, 6, 8}, {1, 6, 9}}};

  inline bool server_version_supported(std::string_view version) {
    const auto parsed = parse_version(version);
    return parsed && std::find(audited_server_versions.begin(), audited_server_versions.end(), *parsed) != audited_server_versions.end();
  }

  enum class kind_e {
    client,
    node,
    link,
  };

  struct global_t {
    kind_e kind = kind_e::node;
    std::map<std::string, std::string, std::less<>> props;

    std::optional<std::string_view> prop(std::string_view key) const {
      const auto found = props.find(key);
      if (found == props.end()) return std::nullopt;
      return std::string_view {found->second};
    }
  };

  /** The registry interface type names, mapped to the kinds this model keeps. */
  inline std::optional<kind_e> kind_of(std::string_view type) {
    if (type == "PipeWire:Interface:Client") return kind_e::client;
    if (type == "PipeWire:Interface:Node") return kind_e::node;
    if (type == "PipeWire:Interface:Link") return kind_e::link;
    return std::nullopt;
  }

  /**
   * Registry state of ONE core. Events are applied in delivery order on the PipeWire loop.
   * revision counts every change to a kept global, so a reader can tell that the graph moved.
   */
  class graph_t {
  public:
    void global(std::uint32_t id, std::string_view type, std::map<std::string, std::string, std::less<>> props) {
      const auto kind = kind_of(type);
      if (!kind || id == invalid_id) return;
      globals_[id] = global_t {*kind, std::move(props)};
      ++revision_;
    }

    void global_remove(std::uint32_t id) {
      if (globals_.erase(id) != 0) ++revision_;
    }

    const global_t *find(std::uint32_t id, kind_e kind) const {
      const auto found = globals_.find(id);
      return found != globals_.end() && found->second.kind == kind ? &found->second : nullptr;
    }

    const std::map<std::uint32_t, global_t> &globals() const { return globals_; }
    std::uint64_t revision() const { return revision_; }

  private:
    std::map<std::uint32_t, global_t> globals_;
    std::uint64_t revision_ = 0;
  };

  /** Everything resolve() needs, gathered under the capture loop lock after a completed sync. */
  struct snapshot_t {
    bool tracking = false;
    bool running = false;
    bool synced = false;
    bool timed_out = false;
    bool cancelled = false;
    bool core_error = false;
    // The capture stream was replaced, or a kept global changed, between the request and the sync.
    bool generation_changed = false;
    std::string server_version;
    std::uint32_t self_client_id = invalid_id;  ///< pw_proxy_get_bound_id of pw_core_get_client
    std::int64_t self_pid = 0;                   ///< getpid()
    std::uint64_t self_uid = 0;                  ///< getuid()
    std::uint32_t capture_node_id = invalid_id;  ///< pw_stream_get_node_id
    const graph_t *graph = nullptr;
  };

  namespace detail {
    inline capture_producer::binding_t refuse(capture_producer::binding_t binding, std::string_view why) {
      binding.verified = false;
      binding.reason = std::string {why};
      binding.producer_pid = 0;
      binding.producer_uid = 0;
      return binding;
    }

    inline std::optional<std::uint64_t> serial_of(const global_t &global) {
      const auto text = global.prop("object.serial");
      if (!text) return std::nullopt;
      const auto serial = parse_decimal(*text, std::numeric_limits<std::uint64_t>::max());
      if (!serial || *serial == 0) return std::nullopt;
      return serial;
    }
  }  // namespace detail

  /**
   * Bind the capture node to the process that owns its one actual source.
   *
   * Order matters and every step fails closed:
   * 1. the read itself completed (tracking, running, synced, no timeout, cancellation or core error);
   * 2. the server is a version whose semantics were audited;
   * 3. this core's own Client global is the proxy-bound one and carries this process's pid and uid,
   *    which is what makes the server's PID namespace and credentials comparable with ours;
   * 4. the capture node is ours (server-set client.id equals our client);
   * 5. the links into it name exactly one source node;
   * 6. that source's server-set client.id names a visible Client with server credentials.
   */
  inline capture_producer::binding_t resolve(const snapshot_t &in) {
    using detail::refuse;
    capture_producer::binding_t binding;
    binding.server_version = in.server_version;
    if (!in.tracking) return refuse(binding, reason::tracking_disabled);
    if (in.cancelled) return refuse(binding, reason::cancelled);
    if (!in.running) return refuse(binding, reason::capture_not_running);
    if (in.core_error) return refuse(binding, reason::core_error);
    if (in.timed_out || !in.synced || !in.graph) return refuse(binding, reason::read_timeout);
    if (in.generation_changed) return refuse(binding, reason::graph_changed);
    if (!server_version_supported(in.server_version)) return refuse(binding, reason::server_version_unsupported);

    const auto &graph = *in.graph;
    if (in.self_client_id == invalid_id) return refuse(binding, reason::self_client_unknown);
    const auto *self = graph.find(in.self_client_id, kind_e::client);
    if (!self) return refuse(binding, reason::self_client_unknown);
    const auto self_serial = detail::serial_of(*self);
    if (!self_serial) return refuse(binding, reason::graph_identity_invalid);
    binding.capture_client = {in.self_client_id, *self_serial};
    const auto self_pid_text = self->prop("pipewire.sec.pid");
    const auto self_uid_text = self->prop("pipewire.sec.uid");
    const auto self_pid = self_pid_text ? parse_pid(*self_pid_text) : std::nullopt;
    const auto self_uid = self_uid_text ? parse_uid(*self_uid_text) : std::nullopt;
    if (!self_pid || !self_uid) return refuse(binding, reason::self_credentials_absent);
    // A server in another PID namespace, or a core opened over an fd another process connected
    // (portal remotes), reports someone else here. Then no PID the server reports is ours to read.
    if (*self_pid != in.self_pid || *self_uid != in.self_uid) return refuse(binding, reason::pid_namespace_mismatch);

    if (in.capture_node_id == invalid_id) return refuse(binding, reason::capture_node_unknown);
    const auto *capture = graph.find(in.capture_node_id, kind_e::node);
    if (!capture) return refuse(binding, reason::capture_node_unknown);
    const auto capture_serial = detail::serial_of(*capture);
    if (!capture_serial) return refuse(binding, reason::graph_identity_invalid);
    binding.capture_node = {in.capture_node_id, *capture_serial};
    const auto capture_owner_text = capture->prop("client.id");
    const auto capture_owner = capture_owner_text ? parse_id(*capture_owner_text) : std::nullopt;
    if (!capture_owner || *capture_owner != in.self_client_id) return refuse(binding, reason::capture_node_not_owned);

    std::set<std::uint32_t> sources;
    for (const auto &[id, global] : graph.globals()) {
      if (global.kind != kind_e::link) continue;
      const auto input_text = global.prop("link.input.node");
      const auto input = input_text ? parse_id(*input_text) : std::nullopt;
      if (!input) {
        // A link whose sink cannot be read might be ours: the graph is not fully known.
        return refuse(binding, reason::graph_identity_invalid);
      }
      if (*input != in.capture_node_id) continue;
      const auto output_text = global.prop("link.output.node");
      const auto output = output_text ? parse_id(*output_text) : std::nullopt;
      const auto serial = detail::serial_of(global);
      if (!output || !serial) return refuse(binding, reason::graph_identity_invalid);
      binding.links.push_back({{id, *serial}, *output, *input});
      sources.insert(*output);
    }
    if (sources.empty()) return refuse(binding, reason::source_not_linked);
    if (sources.size() != 1) return refuse(binding, reason::source_ambiguous);

    const auto source_id = *sources.begin();
    const auto *source = graph.find(source_id, kind_e::node);
    if (!source) return refuse(binding, reason::source_unattributed);
    const auto source_serial = detail::serial_of(*source);
    if (!source_serial) return refuse(binding, reason::graph_identity_invalid);
    binding.source_node = {source_id, *source_serial};
    const auto owner_text = source->prop("client.id");
    const auto owner = owner_text ? parse_id(*owner_text) : std::nullopt;
    if (!owner) return refuse(binding, reason::source_unattributed);

    const auto *producer = graph.find(*owner, kind_e::client);
    if (!producer) return refuse(binding, reason::producer_client_not_visible);
    const auto producer_serial = detail::serial_of(*producer);
    if (!producer_serial) return refuse(binding, reason::graph_identity_invalid);
    binding.producer_client = {*owner, *producer_serial};
    const auto pid_text = producer->prop("pipewire.sec.pid");
    const auto uid_text = producer->prop("pipewire.sec.uid");
    const auto pid = pid_text ? parse_pid(*pid_text) : std::nullopt;
    const auto uid = uid_text ? parse_uid(*uid_text) : std::nullopt;
    if (!pid || !uid) return refuse(binding, reason::producer_credentials_absent);
    if (*uid != in.self_uid) return refuse(binding, reason::producer_uid_mismatch);

    binding.producer_pid = *pid;
    binding.producer_uid = *uid;
    binding.verified = true;
    binding.reason = std::string {reason::verified};
    return binding;
  }
}  // namespace pipewire_producer_binding
