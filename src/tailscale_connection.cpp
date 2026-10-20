#include "tailscale_connection.h"

#include <charconv>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>
#include <boost/asio/ip/address.hpp>
#include <curl/curl.h>
#include "network.h"

namespace tailscale_connection {
  namespace {
    std::optional<std::string> normalized_ip(std::string_view value) {
      boost::system::error_code ec;
      auto address = boost::asio::ip::make_address(std::string(value), ec);
      if (ec) return std::nullopt;
      return net::addr_to_normalized_string(address);
    }

    bool contains_ip(const nlohmann::json &ips, const std::string &ip) {
      if (!ips.is_array()) return false;
      for (const auto &value : ips) {
        if (!value.is_string()) continue;
        if (normalized_ip(value.get_ref<const std::string &>()) == ip) return true;
      }
      return false;
    }

    // Go time.Time uses RFC3339 with optional nanoseconds and a numeric timezone.
    // Reject malformed dates, zero timestamps and unrepresentable arithmetic.
    std::optional<std::int64_t> timestamp_ms(std::string_view value) {
      if (value.size() < 20 || value.size() > 40 || value[4] != '-' || value[7] != '-' ||
          value[10] != 'T' || value[13] != ':' || value[16] != ':') return std::nullopt;
      auto number = [&](std::size_t pos, std::size_t size) -> int {
        int result = -1;
        const auto part = value.substr(pos, size);
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), result);
        return parsed.ec == std::errc {} && parsed.ptr == part.data() + part.size() ? result : -1;
      };
      const int year = number(0, 4), month = number(5, 2), day = number(8, 2);
      const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
      const std::chrono::year_month_day date {std::chrono::year {year},
        std::chrono::month {static_cast<unsigned>(month)}, std::chrono::day {static_cast<unsigned>(day)}};
      if (year < 1970 || !date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
      std::size_t pos = 19;
      int fractional = 0;
      if (value[pos] == '.') {
        const auto start = ++pos;
        while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') {
          if (pos - start < 3) fractional = fractional * 10 + value[pos] - '0';
          ++pos;
        }
        const auto digits = pos - start;
        if (digits == 0 || digits > 9) return std::nullopt;
        for (auto i = digits; i < 3; ++i) fractional *= 10;
      }
      int offset_minutes = 0;
      if (pos + 1 == value.size() && value[pos] == 'Z') {
        // UTC
      } else if (pos + 6 == value.size() && (value[pos] == '+' || value[pos] == '-') && value[pos + 3] == ':') {
        const int hours = number(pos + 1, 2), minutes = number(pos + 4, 2);
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) return std::nullopt;
        offset_minutes = (hours * 60 + minutes) * (value[pos] == '+' ? 1 : -1);
      } else return std::nullopt;
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::sys_days {date}.time_since_epoch()).count() +
        (hour * 3600LL + minute * 60LL + second - offset_minutes * 60LL) * 1000 + fractional;
    }

    [[maybe_unused]] std::size_t append_status(char *data, std::size_t size, std::size_t count, void *user) {
      auto &body = *static_cast<std::string *>(user);
      if (count > max_status_bytes || size > max_status_bytes ||
          (size != 0 && count > max_status_bytes / size)) return 0;
      const auto bytes = size * count;
      if (bytes > max_status_bytes - body.size()) return 0;
      body.append(data, bytes);
      return bytes;
    }
  }  // namespace

  nlohmann::json unknown(std::string_view scope, std::string_view reason, std::string_view address_kind) {
    return {{"schema_version", 1}, {"status", "unknown"}, {"address_kind", address_kind},
      {"tailscale", {{"confirmed", false}, {"transport", "unknown"}, {"scope", scope},
        {"source", "tailscaled_localapi"}, {"observed_at_ms", 0}, {"max_age_ms", max_age_ms}, {"reason", reason}}}};
  }

  nlohmann::json from_status(std::string_view status, std::string_view remote_ip,
                             std::string_view local_ip, std::int64_t observed_at_ms, std::string_view scope) {
    const auto kind = net::describe_client_network_path(remote_ip);
    auto output = unknown(scope, "peer_not_confirmed", kind);
    if (scope != "control_connection" && scope != "peer_status") return output;
    if (status.empty() || status.size() > max_status_bytes) return unknown(scope, "malformed_status", kind);
    const auto remote = normalized_ip(remote_ip), local = normalized_ip(local_ip);
    if (!remote || observed_at_ms <= 0) return output;
    try {
      const auto data = nlohmann::json::parse(status);
      if (!data.is_object()) return unknown(scope, "malformed_status", kind);
      // A daemon that is stopped, logged out or still starting cannot confirm anything: that is
      // the daemon being unavailable, not a peer that failed to match.
      if (data.value("BackendState", "") != "Running") return unknown(scope, "localapi_unavailable", kind);
      if (!data.contains("Peer") || !data["Peer"].is_object()) return unknown(scope, "malformed_status", kind);
      if (scope == "control_connection") {
        // An IPv4 range or matched peer alone cannot prove this TLS connection
        // used Tailscale; it must have reached the host's own tailnet address.
        if (!local || !data.contains("TailscaleIPs") || !contains_ip(data["TailscaleIPs"], *local)) return output;
      }
      const nlohmann::json *matched = nullptr;
      for (const auto &peer : data["Peer"]) {
        if (!peer.is_object() || !peer.contains("TailscaleIPs") || !contains_ip(peer["TailscaleIPs"], *remote)) continue;
        if (matched) return output;  // ambiguous peer identity
        matched = &peer;
      }
      if (!matched || !matched->value("Online", false)) return output;
      auto &evidence = output["tailscale"];
      output["status"] = "tailscale";
      evidence["confirmed"] = true;
      evidence["observed_at_ms"] = observed_at_ms;
      evidence["reason"] = "peer_idle";
      const auto written = timestamp_ms(matched->value("LastWrite", ""));
      if (!matched->value("Active", false) || !written || *written > observed_at_ms ||
          observed_at_ms - *written > max_age_ms) return output;
      const auto direct = matched->value("CurAddr", "");
      const auto peer_relay = matched->value("PeerRelay", "");
      const auto derp = matched->value("Relay", "");
      // Follow upstream status semantics, but fail closed on contradictory
      // direct/peer-relay fields. Relay names may exist alongside a direct path.
      std::string transport = "unknown";
      if (!direct.empty() && peer_relay.empty()) transport = "direct";
      else if (direct.empty() && !peer_relay.empty()) transport = "peer_relay";
      else if (direct.empty() && peer_relay.empty() && !derp.empty()) transport = "derp";
      evidence["transport"] = transport;
      evidence["reason"] = transport == "unknown" ? "transport_unknown" : "observed";
      return output;
    } catch (...) {
      return unknown(scope, "malformed_status", kind);
    }
  }

  std::optional<std::string> read_status_socket(std::string_view socket_path) {
#if defined(__linux__) && LIBCURL_VERSION_NUM >= 0x072800
    const std::string path {socket_path};
    CURL *curl = curl_easy_init();
    if (!curl) return std::nullopt;
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, "http://local-tailscaled.sock/localapi/v0/status?peers=true");
    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, path.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 300L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1250L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_status);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const auto result = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || code != 200) return std::nullopt;
    return body;
#else
    (void) socket_path;
    return std::nullopt;
#endif
  }

#ifdef POLARIS_TESTS
  namespace {
    std::mutex test_reader_mutex;
    status_collector_t::reader_t test_reader;
  }  // namespace

  void set_default_status_reader_for_tests(status_collector_t::reader_t reader) {
    std::lock_guard lock {test_reader_mutex};
    test_reader = std::move(reader);
  }
#endif

  std::optional<std::string> read_default_status() {
#ifdef POLARIS_TESTS
    {
      std::unique_lock lock {test_reader_mutex};
      if (test_reader) {
        auto reader = test_reader;
        lock.unlock();
        return reader();
      }
    }
#endif
    // No child process, privilege escalation or caller-selected socket.
    return read_status_socket(default_socket_path);
  }

  std::int64_t wall_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }

  nlohmann::json evaluate(const std::optional<std::string> &status, std::int64_t observed_at_ms,
                          std::string_view remote_ip, std::string_view local_ip, std::string_view scope) {
    const auto kind = net::describe_client_network_path(remote_ip);
#ifdef __linux__
    if (!status) return unknown(scope, "localapi_unavailable", kind);
    return from_status(*status, remote_ip, local_ip, observed_at_ms, scope);
#else
    (void) status;
    (void) observed_at_ms;
    (void) local_ip;
    return unknown(scope, "unsupported_platform", kind);
#endif
  }

  status_collector_t::status_collector_t(reader_t reader_, wall_clock_t clock_):
      reader {std::move(reader_)}, clock {std::move(clock_)}, worker {[this] { run(); }} {}

  status_collector_t::~status_collector_t() {
    stop();
  }

  bool status_collector_t::submit(waiter_t waiter) {
    {
      std::lock_guard lock {mutex};
      if (stopping || !waiter || waiting.size() + in_flight >= max_waiters) return false;
      waiting.push_back(std::move(waiter));
    }
    wake.notify_one();
#ifdef POLARIS_TESTS
    queued_for_tests.notify_all();
#endif
    return true;
  }

  void status_collector_t::stop() {
    std::deque<waiter_t> unanswered;
    {
      std::lock_guard lock {mutex};
      if (stopping && !worker.joinable()) return;
      stopping = true;
      unanswered.swap(waiting);
    }
    wake.notify_all();
#ifdef POLARIS_TESTS
    queued_for_tests.notify_all();
#endif
    // A read already under way finishes (bounded by the socket deadline) and answers its batch.
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    for (auto &waiter : unanswered) {
      try {
        waiter(std::nullopt, 0);
      } catch (...) {
      }
    }
  }

  std::size_t status_collector_t::reads() const {
    std::lock_guard lock {mutex};
    return read_count;
  }

#ifdef POLARIS_TESTS
  bool status_collector_t::wait_queued_for_tests(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock lock {mutex};
    queued_for_tests.wait_for(lock, timeout, [&] { return stopping || waiting.size() >= count; });
    return !stopping && waiting.size() >= count;
  }
#endif

  void status_collector_t::run() {
    for (;;) {
      std::deque<waiter_t> batch;
      {
        std::unique_lock lock {mutex};
        wake.wait(lock, [this] { return stopping || !waiting.empty(); });
        if (stopping) return;
        batch.swap(waiting);
        in_flight = batch.size();
        ++read_count;
      }
      std::optional<std::string> status;
      std::int64_t observed_at_ms = 0;
      try {
        status = reader ? reader() : std::nullopt;
        observed_at_ms = clock ? clock() : 0;
      } catch (...) {
        status.reset();
      }
      for (auto &waiter : batch) {
        try {
          waiter(status, observed_at_ms);
        } catch (...) {
        }
      }
      {
        std::lock_guard lock {mutex};
        in_flight = 0;
      }
      // The status (the daemon's peer inventory) goes out of scope here, unkept.
    }
  }
}  // namespace tailscale_connection
