/**
 * @file tests/unit/platform/multiseat_fake_worker.h
 * @brief A real-socket fake worker, shared by the transport and pump suites.
 *
 * Both suites need a worker that authenticates for an exact authority
 * generation, attaches its data plane and answers on both channels. Keeping
 * one definition here means a behavior one suite adds is the same behavior the
 * other sees, rather than two fakes drifting apart.
 */
#pragma once

#ifdef __linux__

  #include "src/platform/linux/multiseat_worker_client.h"


  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <cstddef>
  #include <cstdlib>
  #include <cstring>
  #include <fcntl.h>
  #include <filesystem>
  #include <functional>
  #include <future>
  #include <mutex>
  #include <poll.h>
  #include <span>
  #include <stdexcept>
  #include <string>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>
  #include <utility>
  #include <vector>
  #include <optional>

namespace multiseat_test {
  using namespace multiseat::worker_ipc;
  using namespace std::chrono_literals;

  class temporary_root_t {
  public:
    temporary_root_t() {
      std::array<char, 48> pattern {};
      const std::string prefix = "/tmp/polaris-seat-client-XXXXXX";
      std::copy(prefix.begin(), prefix.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {"temporary client root could not be created"};
      }
      path_ = created;
      if (::chmod(path_.c_str(), 0700) != 0) {
        throw std::runtime_error {"temporary client root mode could not be set"};
      }
    }

    ~temporary_root_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  inline endpoint_identity_t identity_for(std::uint64_t generation = 11) {
    return {
      .controller_epoch = "controller-c3d4",
      .logical_gpu_id = "gpu-primary",
      .slot = 0,
      .generation = generation,
      .worker_name = "polaris-worker-controller-c3d4-11",
    };
  }

  inline capability_factory_t deterministic_capability(std::uint8_t value) {
    return [value](capability_t &capability) {
      capability.fill(value);
      return true;
    };
  }

  inline provider_catalog_selection_t test_provider_selection() {
    return {
      .compositor = multiseat::compositor_e::gamescope,
      .workload = {
        .kind = multiseat::workload_kind_e::steam,
        .target_id = "client-test-workload",
      },
    };
  }

  inline authority_handle_t create_authority(
    authority_store_t &store,
    const endpoint_identity_t &identity,
    std::string runtime_namespace
  ) {
    auto result = store.create(
      identity,
      std::move(runtime_namespace),
      test_provider_selection()
    );
    if (!result.created()) {
      throw std::runtime_error {"test authority could not be created"};
    }
    return std::move(result.authority.value());
  }

  inline bool write_all(int descriptor, std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto result = ::send(
        descriptor,
        bytes.data() + offset,
        bytes.size() - offset,
        MSG_NOSIGNAL
      );
      if (result > 0) {
        offset += static_cast<std::size_t>(result);
      } else if (result < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

  inline bool read_all(int descriptor, std::span<std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto result = ::recv(
        descriptor,
        bytes.data() + offset,
        bytes.size() - offset,
        0
      );
      if (result > 0) {
        offset += static_cast<std::size_t>(result);
      } else if (result < 0 && errno == EINTR) {
        continue;
      } else {
        return false;
      }
    }
    return true;
  }

  inline bool send_test_frame(int descriptor, const frame_t &frame) {
    try {
      const auto encoded = encode_frame(frame);
      return write_all(descriptor, encoded);
    } catch (...) {
      return false;
    }
  }

  inline bool read_test_frame(
    int descriptor,
    channel_e channel,
    const endpoint_identity_t &identity,
    frame_t &frame
  ) {
    std::array<std::uint8_t, header_size> header {};
    if (!read_all(descriptor, header)) {
      return false;
    }
    const auto header_result = parse_frame(
      header,
      channel,
      identity.slot,
      identity.generation
    );
    if (header_result.status == parse_status_e::rejected ||
        header_result.required < header.size() ||
        header_result.required > header.size() +
                                   (channel == channel_e::control ?
                                      max_control_payload :
                                      max_media_payload)) {
      return false;
    }
    std::vector<std::uint8_t> encoded(header_result.required);
    std::copy(header.begin(), header.end(), encoded.begin());
    if (encoded.size() > header.size() &&
        !read_all(descriptor, std::span {encoded}.subspan(header.size()))) {
      return false;
    }
    const auto parsed = parse_frame(
      encoded,
      channel,
      identity.slot,
      identity.generation
    );
    if (parsed.status != parse_status_e::complete ||
        !parsed.frame ||
        parsed.consumed != encoded.size()) {
      return false;
    }
    frame = *parsed.frame;
    return true;
  }

  enum class fake_behavior_e {
    healthy,
    bad_worker_proof,
    replay_authenticated_sequence,
    replay_heartbeat_sequence,
    wrong_generation,
    cross_routed_media,
    oversized_handshake_payload,
    stall,
    media_contract,
  };

  inline media_config_t fake_media_config() {
    return {
      .video_codec = video_codec_e::h264,
      .profile_idc = 100,
      .level_idc = 42,
      .width = 1280,
      .height = 720,
      .fps_numerator = 60,
      .fps_denominator = 1,
      .bitrate_ceiling_kbps = 15000,
      .audio_codec = audio_codec_e::opus,
      .audio_channels = 2,
      .audio_frame_duration_us = 5000,
      .audio_sample_rate = 48000,
    };
  }

  class fake_worker_t {
  public:
    fake_worker_t(
      const authority_handle_t &authority,
      fake_behavior_e behavior = fake_behavior_e::healthy,
      bool include_media = true,
      std::function<bool(int, channel_e)> script = {}
    ):
        identity_(authority.identity()),
        paths_(authority.paths()),
        behavior_(behavior),
        include_media_(include_media),
        script_(std::move(script)) {
      std::copy(
        authority.capability().begin(),
        authority.capability().end(),
        capability_.begin()
      );
      control_listener_ = create_listener(paths_.control_socket);
      if (control_listener_ < 0) {
        throw std::runtime_error {"fake control listener could not be created"};
      }
      if (include_media_) {
        media_listener_ = create_listener(paths_.media_socket);
        if (media_listener_ < 0) {
          (void) ::close(control_listener_);
          throw std::runtime_error {"fake media listener could not be created"};
        }
      }
      control_thread_ = std::thread([this, listener = control_listener_]() {
        serve_listener(listener, channel_e::control);
      });
      if (include_media_) {
        media_thread_ = std::thread([this, listener = media_listener_]() {
          serve_listener(listener, channel_e::media);
        });
      }
    }

    ~fake_worker_t() {
      stop();
    }

    fake_worker_t(const fake_worker_t &) = delete;
    fake_worker_t &operator=(const fake_worker_t &) = delete;

    void stop() {
      if (stopped_.exchange(true)) {
        return;
      }
      if (control_listener_ >= 0) {
        (void) ::shutdown(control_listener_, SHUT_RDWR);
      }
      if (media_listener_ >= 0) {
        (void) ::shutdown(media_listener_, SHUT_RDWR);
      }
      if (control_thread_.joinable()) {
        control_thread_.join();
      }
      if (media_thread_.joinable()) {
        media_thread_.join();
      }
      if (control_listener_ >= 0) {
        (void) ::close(control_listener_);
        control_listener_ = -1;
      }
      if (media_listener_ >= 0) {
        (void) ::close(media_listener_);
        media_listener_ = -1;
      }
    }

    [[nodiscard]] bool failed() const {
      return failed_.load();
    }

    [[nodiscard]] std::vector<std::uint8_t> input() const {
      std::scoped_lock lock {input_mutex_};
      return input_;
    }

    [[nodiscard]] std::vector<message_e> contract_messages() const {
      std::scoped_lock lock {contract_mutex_};
      return contract_messages_;
    }

    [[nodiscard]] std::optional<frame_range_t> invalidated() const {
      std::scoped_lock lock {contract_mutex_};
      return invalidated_;
    }

  private:
    int create_listener(const std::filesystem::path &path) {
      const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (descriptor < 0) {
        return -1;
      }
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto native = path.native();
      if (native.size() >= sizeof(address.sun_path)) {
        (void) ::close(descriptor);
        return -1;
      }
      std::copy(native.begin(), native.end(), address.sun_path);
      address.sun_path[native.size()] = '\0';
      if (::bind(
            descriptor,
            reinterpret_cast<const sockaddr *>(&address),
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native.size() + 1)
          ) != 0 ||
          ::chmod(path.c_str(), 0600) != 0 ||
          ::listen(descriptor, 4) != 0) {
        (void) ::close(descriptor);
        return -1;
      }
      return descriptor;
    }

    void serve_listener(int listener, channel_e channel) {
      const auto connection = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (connection < 0) {
        if (!stopped_.load()) {
          failed_ = true;
        }
        return;
      }
      timeval timeout {.tv_sec = 1, .tv_usec = 0};
      (void) ::setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      (void) ::setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      if (behavior_ == fake_behavior_e::stall && channel == channel_e::control) {
        std::this_thread::sleep_for(100ms);
        (void) ::close(connection);
        return;
      }
      const auto authenticated = authenticate(connection, channel);
      if (!authenticated) {
        (void) ::close(connection);
        return;
      }

      if (script_ && script_(connection, channel)) {
        (void) ::close(connection);
        return;
      }

      std::uint64_t expected_incoming = 2;
      std::uint64_t outgoing = 3;
      bool attached = false;
      while (!stopped_.load()) {
        frame_t request;
        if (!read_test_frame(connection, channel, identity_, request)) {
          break;
        }
        if (request.sequence != expected_incoming++) {
          failed_ = true;
          break;
        }
        if (request.message == message_e::heartbeat) {
          const auto response_sequence =
            behavior_ == fake_behavior_e::replay_heartbeat_sequence &&
                channel == channel_e::control ?
              outgoing - 1 :
              outgoing;
          if (!send_test_frame(connection, {
                                             .channel = channel,
                                             .message = message_e::heartbeat_ack,
                                             .slot = identity_.slot,
                                             .generation = identity_.generation,
                                             .sequence = response_sequence,
                                           })) {
            break;
          }
          ++outgoing;
          continue;
        }
        if (request.message == message_e::attach) {
          if (attached || !send_test_frame(connection, {
                                                         .channel = channel,
                                                         .message = message_e::attached,
                                                         .slot = identity_.slot,
                                                         .generation = identity_.generation,
                                                         .sequence = outgoing++,
                                                       })) {
            failed_ = true;
            break;
          }
          attached = true;
          if (channel == channel_e::media) {
            auto output_identity = identity_;
            if (behavior_ == fake_behavior_e::cross_routed_media) {
              ++output_identity.generation;
            }
            if (behavior_ == fake_behavior_e::media_contract &&
                !send_test_frame(connection, {
                                               .channel = channel,
                                               .message = message_e::media_config,
                                               .slot = output_identity.slot,
                                               .generation = output_identity.generation,
                                               .sequence = outgoing++,
                                               .payload = encode_media_config(fake_media_config()),
                                             })) {
              break;
            }
            // Under the contract every payload carries the frame prefix; the
            // plain behavior keeps sending bare bytes, which is what the
            // transport suite asserts on.
            const auto media_payload = [this](const std::string_view bytes, const std::uint64_t index, const bool idr) {
              const std::span<const std::uint8_t> encoded {
                reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()
              };
              if (behavior_ != fake_behavior_e::media_contract) {
                return std::vector<std::uint8_t> {encoded.begin(), encoded.end()};
              }
              return encode_media_frame({.frame_index = index, .idr = idr}, encoded);
            };
            if (!send_test_frame(connection, {
                                               .channel = channel,
                                               .message = message_e::video,
                                               .slot = output_identity.slot,
                                               .generation = output_identity.generation,
                                               .sequence = outgoing++,
                                               .payload = media_payload("video", 1, true),
                                             }) ||
                !send_test_frame(connection, {
                                               .channel = channel,
                                               .message = message_e::audio,
                                               .slot = output_identity.slot,
                                               .generation = output_identity.generation,
                                               .sequence = outgoing++,
                                               .payload = media_payload("audio", 1, false),
                                             })) {
              break;
            }
          }
          continue;
        }
        if (channel == channel_e::control &&
            request.message == message_e::input && attached) {
          {
            std::scoped_lock lock {input_mutex_};
            input_ = request.payload;
          }
          if (!send_test_frame(connection, {
                                             .channel = channel,
                                             .message = message_e::input_ack,
                                             .slot = identity_.slot,
                                             .generation = identity_.generation,
                                             .sequence = outgoing++,
                                           }) ||
              !send_test_frame(connection, {
                                             .channel = channel,
                                             .message = message_e::feedback,
                                             .slot = identity_.slot,
                                             .generation = identity_.generation,
                                             .sequence = outgoing++,
                                             .payload = {'r', 'u', 'm', 'b', 'l', 'e'},
                                           })) {
            break;
          }
          continue;
        }
        if (channel == channel_e::control && attached &&
            (request.message == message_e::media_config_ack ||
             request.message == message_e::request_idr ||
             request.message == message_e::invalidate_ref_frames)) {
          {
            std::scoped_lock lock {contract_mutex_};
            contract_messages_.push_back(request.message);
            if (request.message == message_e::invalidate_ref_frames) {
              invalidated_ = parse_frame_range(request.payload);
            }
          }
          if (!send_test_frame(connection, {
                                             .channel = channel,
                                             .message = message_e::media_control_ack,
                                             .slot = identity_.slot,
                                             .generation = identity_.generation,
                                             .sequence = outgoing++,
                                           })) {
            break;
          }
          continue;
        }
        if (channel == channel_e::control && request.message == message_e::shutdown) {
          (void) send_test_frame(connection, {
                                               .channel = channel,
                                               .message = message_e::shutdown_ack,
                                               .slot = identity_.slot,
                                               .generation = identity_.generation,
                                               .sequence = outgoing,
                                             });
          break;
        }
        failed_ = true;
        break;
      }
      (void) ::close(connection);
    }

    bool authenticate(int connection, channel_e channel) {
      if (behavior_ == fake_behavior_e::oversized_handshake_payload &&
          channel == channel_e::control) {
        return send_test_frame(connection, {
                                             .channel = channel,
                                             .message = message_e::input,
                                             .slot = identity_.slot,
                                             .generation = identity_.generation,
                                             .sequence = 1,
                                             .payload = std::vector<std::uint8_t>(4096, 0x5a),
                                           });
      }
      challenge_t challenge {};
      challenge.fill(channel == channel_e::control ? 0x31 : 0x32);
      auto challenge_identity = identity_;
      if (behavior_ == fake_behavior_e::wrong_generation &&
          channel == channel_e::control) {
        ++challenge_identity.generation;
      }
      if (!send_test_frame(connection, {
                                         .channel = channel,
                                         .message = message_e::challenge,
                                         .slot = challenge_identity.slot,
                                         .generation = challenge_identity.generation,
                                         .sequence = 1,
                                         .payload = std::vector<std::uint8_t>(challenge.begin(), challenge.end()),
                                       })) {
        return false;
      }
      if (behavior_ == fake_behavior_e::wrong_generation &&
          channel == channel_e::control) {
        return false;
      }
      frame_t response;
      if (!read_test_frame(connection, channel, identity_, response) ||
          response.message != message_e::authenticate ||
          response.sequence != 1 ||
          !verify_authentication_proof(
            capability_,
            proof_role_e::controller,
            channel,
            identity_,
            challenge,
            response.payload
          )) {
        return false;
      }
      auto response_capability = capability_;
      if (behavior_ == fake_behavior_e::bad_worker_proof &&
          channel == channel_e::control) {
        response_capability.front() ^= 0xff;
      }
      const auto proof = authentication_proof(
        response_capability,
        proof_role_e::worker,
        channel,
        identity_,
        challenge
      );
      if (!proof) {
        return false;
      }
      return send_test_frame(connection, {
                                           .channel = channel,
                                           .message = message_e::authenticated,
                                           .slot = identity_.slot,
                                           .generation = identity_.generation,
                                           .sequence = behavior_ == fake_behavior_e::replay_authenticated_sequence && channel == channel_e::control ? 1U : 2U,
                                           .payload = std::vector<std::uint8_t>(proof->begin(), proof->end()),
                                         });
    }

    endpoint_identity_t identity_;
    authority_paths_t paths_;
    capability_t capability_ {};
    fake_behavior_e behavior_;
    bool include_media_ = true;
    std::function<bool(int, channel_e)> script_;
    int control_listener_ = -1;
    int media_listener_ = -1;
    std::thread control_thread_;
    std::thread media_thread_;
    mutable std::mutex input_mutex_;
    std::vector<std::uint8_t> input_;
    mutable std::mutex contract_mutex_;
    std::vector<message_e> contract_messages_;
    std::optional<frame_range_t> invalidated_;
    std::atomic<bool> stopped_ = false;
    std::atomic<bool> failed_ = false;
  };

  inline controller_client_options_t short_options() {
    return {
      .connect_timeout = 1000ms,
      .handshake_timeout = 1000ms,
      .io_timeout = 1000ms,
    };
  }
}  // namespace multiseat_test

#endif
