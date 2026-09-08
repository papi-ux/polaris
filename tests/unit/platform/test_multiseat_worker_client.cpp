#include "src/platform/linux/multiseat_worker_client.h"

#include <gtest/gtest.h>

#ifdef __linux__

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

namespace {
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

  endpoint_identity_t identity_for(std::uint64_t generation = 11) {
    return {
      .controller_epoch = "controller-c3d4",
      .logical_gpu_id = "gpu-primary",
      .slot = 0,
      .generation = generation,
      .worker_name = "polaris-worker-controller-c3d4-11",
    };
  }

  capability_factory_t deterministic_capability(std::uint8_t value) {
    return [value](capability_t &capability) {
      capability.fill(value);
      return true;
    };
  }

  provider_catalog_selection_t test_provider_selection() {
    return {
      .compositor = multiseat::compositor_e::gamescope,
      .workload = {
        .kind = multiseat::workload_kind_e::steam,
        .target_id = "client-test-workload",
      },
    };
  }

  authority_handle_t create_authority(
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

  bool write_all(int descriptor, std::span<const std::uint8_t> bytes) {
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

  bool read_all(int descriptor, std::span<std::uint8_t> bytes) {
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

  bool send_test_frame(int descriptor, const frame_t &frame) {
    try {
      const auto encoded = encode_frame(frame);
      return write_all(descriptor, encoded);
    } catch (...) {
      return false;
    }
  }

  bool read_test_frame(
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
  };

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
            if (!send_test_frame(connection, {
                                               .channel = channel,
                                               .message = message_e::video,
                                               .slot = output_identity.slot,
                                               .generation = output_identity.generation,
                                               .sequence = outgoing++,
                                               .payload = {'v', 'i', 'd', 'e', 'o'},
                                             }) ||
                !send_test_frame(connection, {
                                               .channel = channel,
                                               .message = message_e::audio,
                                               .slot = output_identity.slot,
                                               .generation = output_identity.generation,
                                               .sequence = outgoing++,
                                               .payload = {'a', 'u', 'd', 'i', 'o'},
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
    std::atomic<bool> stopped_ = false;
    std::atomic<bool> failed_ = false;
  };

  controller_client_options_t short_options() {
    return {
      .connect_timeout = 1000ms,
      .handshake_timeout = 1000ms,
      .io_timeout = 1000ms,
    };
  }
}  // namespace

TEST(MultiseatWorkerClient, AuthenticatesBothChannelsHeartbeatsAndShutsDown) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x21)};
  auto authority = create_authority(store, identity_for(), "generation-11");
  fake_worker_t worker {authority};
  controller_client_t client;

  EXPECT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  EXPECT_TRUE(client.connected());
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(client.shutdown(), transport_status_e::applied);
  EXPECT_FALSE(client.connected());
  worker.stop();
  EXPECT_FALSE(worker.failed());
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, AttachesAndRoutesInputFeedbackAndEncodedMedia) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x30)};
  auto authority = create_authority(store, identity_for(), "generation-data-plane");
  fake_worker_t worker {authority};
  controller_client_t client;

  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  EXPECT_FALSE(client.data_plane_attached());
  const std::vector<std::uint8_t> input {'i', 'n', 'p', 'u', 't'};
  EXPECT_EQ(client.send_input(input), transport_status_e::closed);
  EXPECT_TRUE(client.connected());
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  EXPECT_TRUE(client.data_plane_attached());
  EXPECT_EQ(client.attach_data_plane(), transport_status_e::invalid_argument);
  ASSERT_EQ(client.send_input(input), transport_status_e::applied);
  EXPECT_EQ(worker.input(), input);

  std::vector<std::uint8_t> feedback;
  ASSERT_EQ(client.receive_feedback(feedback), transport_status_e::applied);
  EXPECT_EQ(feedback, (std::vector<std::uint8_t> {'r', 'u', 'm', 'b', 'l', 'e'}));

  // Media may arrive before a heartbeat acknowledgement. The client keeps
  // those exact-seat packets in order rather than mistaking them for the ACK.
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  encoded_media_packet_t media;
  ASSERT_EQ(client.receive_media(media), transport_status_e::applied);
  EXPECT_EQ(media.message, message_e::video);
  EXPECT_EQ(media.payload, (std::vector<std::uint8_t> {'v', 'i', 'd', 'e', 'o'}));
  ASSERT_EQ(client.receive_media(media), transport_status_e::applied);
  EXPECT_EQ(media.message, message_e::audio);
  EXPECT_EQ(media.payload, (std::vector<std::uint8_t> {'a', 'u', 'd', 'i', 'o'}));

  EXPECT_EQ(client.shutdown(), transport_status_e::applied);
  worker.stop();
  EXPECT_FALSE(worker.failed());
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, RejectsCrossGenerationMediaAndClosesBothChannels) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x31)};
  auto authority = create_authority(store, identity_for(), "generation-cross-route");
  fake_worker_t worker {authority, fake_behavior_e::cross_routed_media};
  controller_client_t client;

  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  encoded_media_packet_t media;
  EXPECT_EQ(client.receive_media(media), transport_status_e::protocol_rejected);
  EXPECT_FALSE(client.connected());
  EXPECT_FALSE(client.data_plane_attached());
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::closed);

  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, RejectsWrongWorkerProofAndRollsBackConnection) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x22)};
  auto authority = create_authority(store, identity_for(), "generation-bad-proof");
  fake_worker_t worker {authority, fake_behavior_e::bad_worker_proof, false};
  controller_client_t client;

  EXPECT_EQ(
    client.connect(authority, short_options()),
    transport_status_e::authentication_rejected
  );
  EXPECT_FALSE(client.connected());
  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, RejectsReplayedAuthenticationAndWrongGeneration) {
  temporary_root_t first_root;
  authority_store_t first_store {first_root.path(), deterministic_capability(0x23)};
  auto first = create_authority(first_store, identity_for(), "generation-replay");
  fake_worker_t replay {
    first,
    fake_behavior_e::replay_authenticated_sequence,
    false,
  };
  controller_client_t client;
  EXPECT_EQ(
    client.connect(first, short_options()),
    transport_status_e::protocol_rejected
  );
  replay.stop();
  EXPECT_EQ(first_store.remove(first), authority_status_e::applied);

  temporary_root_t second_root;
  authority_store_t second_store {second_root.path(), deterministic_capability(0x24)};
  auto second = create_authority(second_store, identity_for(), "generation-mismatch");
  fake_worker_t mismatched {second, fake_behavior_e::wrong_generation, false};
  EXPECT_EQ(
    client.connect(second, short_options()),
    transport_status_e::protocol_rejected
  );
  EXPECT_FALSE(client.connected());
  mismatched.stop();
  EXPECT_EQ(second_store.remove(second), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, HandshakeDeadlineIsBounded) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x25)};
  auto authority = create_authority(store, identity_for(), "generation-timeout");
  fake_worker_t worker {authority, fake_behavior_e::stall, false};
  controller_client_t client;
  auto options = short_options();
  options.handshake_timeout = 20ms;
  const auto started = std::chrono::steady_clock::now();

  EXPECT_EQ(client.connect(authority, options), transport_status_e::timeout);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
  EXPECT_FALSE(client.connected());
  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, ReplayedHeartbeatResponseClosesBothChannels) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x2b)};
  auto authority = create_authority(store, identity_for(), "generation-heartbeat-replay");
  fake_worker_t worker {
    authority,
    fake_behavior_e::replay_heartbeat_sequence,
  };
  controller_client_t client;

  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  EXPECT_EQ(
    client.heartbeat(channel_e::control),
    transport_status_e::protocol_rejected
  );
  EXPECT_FALSE(client.connected());
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::closed);
  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, HandshakeRejectsPhaseOversizeBeforePayloadRead) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x2a)};
  auto authority = create_authority(store, identity_for(), "generation-phase-limit");
  fake_worker_t worker {
    authority,
    fake_behavior_e::oversized_handshake_payload,
    false,
  };
  controller_client_t client;

  EXPECT_EQ(
    client.connect(authority, short_options()),
    transport_status_e::protocol_rejected
  );
  EXPECT_FALSE(client.connected());
  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, MissingMediaSocketRollsBackAuthenticatedControl) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x26)};
  auto authority = create_authority(store, identity_for(), "generation-no-media");
  fake_worker_t worker {authority, fake_behavior_e::healthy, false};
  controller_client_t client;

  EXPECT_EQ(client.connect(authority, short_options()), transport_status_e::unavailable);
  EXPECT_FALSE(client.connected());
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::closed);
  worker.stop();
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, UnsafeSocketModeIsRejectedBeforeAuthentication) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x27)};
  auto authority = create_authority(store, identity_for(), "generation-mode");
  const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(descriptor, 0);
  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  const auto native = authority.paths().control_socket.native();
  std::copy(native.begin(), native.end(), address.sun_path);
  address.sun_path[native.size()] = '\0';
  ASSERT_EQ(::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native.size() + 1)), 0);
  ASSERT_EQ(::chmod(authority.paths().control_socket.c_str(), 0660), 0);
  ASSERT_EQ(::listen(descriptor, 1), 0);
  controller_client_t client;

  EXPECT_EQ(client.connect(authority, short_options()), transport_status_e::peer_rejected);
  EXPECT_FALSE(client.connected());
  EXPECT_EQ(::close(descriptor), 0);
}

TEST(MultiseatWorkerClient, InvalidOptionsAndInactiveAuthorityFailWithoutIO) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x28)};
  auto authority = create_authority(store, identity_for(), "generation-options");
  controller_client_t client;
  auto invalid = short_options();
  invalid.connect_timeout = 0ms;

  EXPECT_EQ(client.connect(authority, invalid), transport_status_e::invalid_argument);
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
  EXPECT_EQ(client.connect(authority, short_options()), transport_status_e::invalid_argument);
  EXPECT_EQ(
    client.heartbeat(static_cast<channel_e>(99)),
    transport_status_e::invalid_argument
  );
}

TEST(MultiseatWorkerClient, TwoWorkersRemainIndependentWhenOneShutsDown) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x29)};
  auto first = create_authority(store, identity_for(11), "generation-first");
  auto second_identity = identity_for(12);
  second_identity.worker_name = "polaris-worker-controller-c3d4-12";
  auto second = create_authority(store, second_identity, "generation-second");
  fake_worker_t first_worker {first};
  fake_worker_t second_worker {second};
  controller_client_t first_client;
  controller_client_t second_client;

  ASSERT_EQ(first_client.connect(first, short_options()), transport_status_e::applied);
  ASSERT_EQ(second_client.connect(second, short_options()), transport_status_e::applied);
  EXPECT_EQ(first_client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(second_client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(first_client.shutdown(), transport_status_e::applied);
  EXPECT_FALSE(first_client.connected());
  EXPECT_TRUE(second_client.connected());
  EXPECT_EQ(second_client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_EQ(second_client.shutdown(), transport_status_e::applied);

  first_worker.stop();
  second_worker.stop();
  EXPECT_FALSE(first_worker.failed());
  EXPECT_FALSE(second_worker.failed());
  EXPECT_EQ(store.remove(first), authority_status_e::applied);
  EXPECT_EQ(store.remove(second), authority_status_e::applied);
}

TEST(MultiseatWorkerClient, BlockedMediaDoesNotBlockHeartbeatOrClose) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x41)};
  auto authority = create_authority(store, identity_for(), "concurrent-media");
  fake_worker_t worker {authority};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  encoded_media_packet_t packet;
  ASSERT_EQ(client.receive_media(packet), transport_status_e::applied);
  ASSERT_EQ(client.receive_media(packet), transport_status_e::applied);
  auto pending = std::async(std::launch::async, [&] {
    return client.receive_media(packet);
  });
  ASSERT_EQ(pending.wait_for(50ms), std::future_status::timeout);
  auto heartbeat = std::async(std::launch::async, [&] {
    return client.heartbeat(channel_e::control);
  });
  const auto progress = heartbeat.wait_for(250ms);
  EXPECT_EQ(progress, std::future_status::ready) << "idle media monopolized the control channel";
  if (progress == std::future_status::ready) {
    EXPECT_EQ(heartbeat.get(), transport_status_e::applied);
  }
  const auto began = std::chrono::steady_clock::now();
  client.close();
  EXPECT_LT(std::chrono::steady_clock::now() - began, 250ms) << "close waited behind media I/O";
  EXPECT_EQ(pending.get(), transport_status_e::closed);
  EXPECT_TRUE(packet.payload.empty());
  worker.stop();
}

TEST(MultiseatWorkerClient, FeedbackWaitDoesNotBlockInputProducingFeedback) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x42)};
  auto authority = create_authority(store, identity_for(), "concurrent-feedback");
  fake_worker_t worker {authority};
  controller_client_t client;
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  std::vector<std::uint8_t> feedback;
  auto pending = std::async(std::launch::async, [&] {
    return client.receive_feedback(feedback);
  });
  ASSERT_EQ(pending.wait_for(50ms), std::future_status::timeout);
  const std::vector<std::uint8_t> input {'i', 'n', 'p', 'u', 't'};
  auto sent = std::async(std::launch::async, [&] {
    return client.send_input(input);
  });
  const auto progress = sent.wait_for(250ms);
  EXPECT_EQ(progress, std::future_status::ready) << "feedback wait prevented its input request";
  if (progress == std::future_status::ready) {
    EXPECT_EQ(sent.get(), transport_status_e::applied);
    EXPECT_EQ(pending.get(), transport_status_e::applied);
    EXPECT_EQ(feedback, (std::vector<std::uint8_t> {'r', 'u', 'm', 'b', 'l', 'e'}));
  }
  client.close();
  worker.stop();
}

namespace {
  struct scripted_channel_t {
    int descriptor;
    channel_e channel;
    endpoint_identity_t identity;
    std::uint64_t incoming = 2;
    std::uint64_t outgoing = 3;

    bool expect(message_e message) {
      frame_t frame;
      const bool received = read_test_frame(descriptor, channel, identity, frame);
      EXPECT_TRUE(received);
      if (!received) {
        return false;
      }
      EXPECT_EQ(frame.message, message);
      EXPECT_EQ(frame.sequence, incoming++);
      return frame.message == message;
    }

    bool send(message_e message, std::vector<std::uint8_t> payload = {}) {
      return send_test_frame(descriptor, {.channel = channel, .message = message, .slot = identity.slot, .generation = identity.generation, .sequence = outgoing++, .payload = std::move(payload)});
    }

    void await_close() {
      std::array<std::uint8_t, 1> byte {};
      (void) ::recv(descriptor, byte.data(), byte.size(), 0);
    }
  };

  class MultiseatWorkerClientConcurrency: public testing::Test {
  protected:
    temporary_root_t root;
    authority_store_t store {root.path(), deterministic_capability(0x55)};
    authority_handle_t authority = create_authority(store, identity_for(), "concurrent-transport");
    std::unique_ptr<fake_worker_t> worker;
    controller_client_t client;

    void start(std::function<bool(int, channel_e)> script = {}, controller_client_options_t options = short_options()) {
      worker = std::make_unique<fake_worker_t>(authority, fake_behavior_e::healthy, true, std::move(script));
      ASSERT_EQ(client.connect(authority, options), transport_status_e::applied);
    }

    void expect_retired() {
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (client.connected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
      EXPECT_FALSE(client.connected());
      EXPECT_FALSE(client.data_plane_attached());
    }
  };
}  // namespace

TEST_F(MultiseatWorkerClientConcurrency, SilenceDoesNotExpireReaders) {
  auto options = short_options();
  options.io_timeout = 50ms;
  start({}, options);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  // Neither channel has an outstanding operation. Idle feedback is normal.
  std::this_thread::sleep_for(150ms);
  EXPECT_TRUE(client.connected());
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_EQ(client.send_input(std::array<std::uint8_t, 1> {7}), transport_status_e::applied);
  std::vector<std::uint8_t> feedback;
  EXPECT_EQ(client.receive_feedback(feedback), transport_status_e::applied);
}

TEST_F(MultiseatWorkerClientConcurrency, EmptyConsumerDeadlineStillRetiresBothChannels) {
  auto options = short_options();
  options.io_timeout = 50ms;
  start({}, options);
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  std::vector<std::uint8_t> feedback {1, 2, 3};
  EXPECT_EQ(client.receive_feedback(feedback), transport_status_e::timeout);
  EXPECT_TRUE(feedback.empty());
  EXPECT_FALSE(client.connected());
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::closed);
}

TEST_F(MultiseatWorkerClientConcurrency, OrderedMediaAndEndMarkersCanShareAttachBurst) {
  start([&](int fd, channel_e channel) {
    if (channel != channel_e::media) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (!peer.expect(message_e::attach)) {
      return true;
    }
    std::vector<std::uint8_t> burst;
    for (auto message : {message_e::attached, message_e::video, message_e::discontinuity, message_e::audio, message_e::end_of_stream}) {
      const bool data = message == message_e::video || message == message_e::audio;
      const auto encoded = encode_frame({.channel = channel, .message = message, .slot = peer.identity.slot, .generation = peer.identity.generation, .sequence = peer.outgoing++, .payload = data ? std::vector<std::uint8_t> {7} : std::vector<std::uint8_t> {}});
      burst.insert(burst.end(), encoded.begin(), encoded.end());
    }
    EXPECT_TRUE(write_all(fd, burst));
    peer.await_close();
    return true;
  });
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  for (auto message : {message_e::video, message_e::discontinuity, message_e::audio, message_e::end_of_stream}) {
    encoded_media_packet_t packet;
    ASSERT_EQ(client.receive_media(packet), transport_status_e::applied);
    EXPECT_EQ(packet.message, message);
  }
}

TEST_F(MultiseatWorkerClientConcurrency, MediaBeforeAttachAckIsRejected) {
  start([&](int fd, channel_e channel) {
    if (channel != channel_e::media) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (peer.expect(message_e::attach)) {
      (void) peer.send(message_e::video, {1});
    }
    peer.await_close();
    return true;
  });
  EXPECT_EQ(client.attach_data_plane(), transport_status_e::protocol_rejected);
  EXPECT_FALSE(client.connected());
}

TEST_F(MultiseatWorkerClientConcurrency, UnexpectedAckDoesNotCompleteRequest) {
  start([&](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (peer.expect(message_e::heartbeat)) {
      (void) peer.send(message_e::input_ack);
    }
    peer.await_close();
    return true;
  });
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::protocol_rejected);
  EXPECT_FALSE(client.connected());
}

TEST_F(MultiseatWorkerClientConcurrency, UnsolicitedAckRetiresConnection) {
  auto release = std::make_shared<std::promise<void>>();
  auto permitted = release->get_future().share();
  start([&, permitted](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (permitted.wait_for(1s) == std::future_status::ready) {
      (void) peer.send(message_e::heartbeat_ack);
    }
    peer.await_close();
    return true;
  });
  release->set_value();
  expect_retired();
}

TEST_F(MultiseatWorkerClientConcurrency, DuplicateAckRetiresConnection) {
  start([&](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (peer.expect(message_e::heartbeat)) {
      auto first = encode_frame({.channel = channel, .message = message_e::heartbeat_ack, .slot = peer.identity.slot, .generation = peer.identity.generation, .sequence = peer.outgoing++});
      const auto second = encode_frame({.channel = channel, .message = message_e::heartbeat_ack, .slot = peer.identity.slot, .generation = peer.identity.generation, .sequence = peer.outgoing++});
      first.insert(first.end(), second.begin(), second.end());
      EXPECT_TRUE(write_all(fd, first));
    }
    peer.await_close();
    return true;
  });
  // The first ACK completed; its duplicate must not keep the owner usable.
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
  expect_retired();
}

TEST_F(MultiseatWorkerClientConcurrency, TimeoutClosesBeforeAnotherRequestCanAcceptLateAck) {
  auto received = std::make_shared<std::promise<void>>();
  auto request_received = received->get_future().share();
  auto options = short_options();
  options.io_timeout = 100ms;
  start([&, received](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (!peer.expect(message_e::heartbeat)) {
      return true;
    }
    received->set_value();
    // Waiting for EOF is deterministic: the late ACK is attempted only after
    // the first caller's timeout has retired the connection.
    peer.await_close();
    EXPECT_FALSE(peer.send(message_e::heartbeat_ack));
    return true;
  },
        options);
  auto first = std::async(std::launch::async, [&] {
    return client.heartbeat(channel_e::control);
  });
  ASSERT_EQ(request_received.wait_for(1s), std::future_status::ready);
  auto second = std::async(std::launch::async, [&] {
    return client.heartbeat(channel_e::control);
  });
  EXPECT_EQ(first.get(), transport_status_e::timeout);
  EXPECT_EQ(second.get(), transport_status_e::closed);
  EXPECT_FALSE(client.connected());
}

TEST_F(MultiseatWorkerClientConcurrency, CloseCancelsPartialReadAndAllRequestWaiters) {
  auto received = std::make_shared<std::promise<void>>();
  auto request_received = received->get_future().share();
  start([&, received](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (!peer.expect(message_e::heartbeat)) {
      return true;
    }
    const auto partial = encode_frame({.channel = channel, .message = message_e::heartbeat_ack, .slot = peer.identity.slot, .generation = peer.identity.generation, .sequence = peer.outgoing++});
    EXPECT_TRUE(write_all(fd, std::span {partial}.first(1)));
    received->set_value();
    peer.await_close();
    return true;
  });
  auto first = std::async(std::launch::async, [&] {
    return client.heartbeat(channel_e::control);
  });
  ASSERT_EQ(request_received.wait_for(1s), std::future_status::ready);
  auto second = std::async(std::launch::async, [&] {
    return client.heartbeat(channel_e::control);
  });
  const auto began = std::chrono::steady_clock::now();
  client.close();
  EXPECT_EQ(first.get(), transport_status_e::closed);
  EXPECT_EQ(second.get(), transport_status_e::closed);
  EXPECT_LT(std::chrono::steady_clock::now() - began, 250ms);
}

TEST_F(MultiseatWorkerClientConcurrency, PartialFrameDeadlineRetiresSilentPeer) {
  auto options = short_options();
  options.io_timeout = 50ms;
  start([&](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (!peer.expect(message_e::heartbeat)) {
      return true;
    }
    const std::array<std::uint8_t, 1> partial {0};
    EXPECT_TRUE(write_all(fd, partial));
    peer.await_close();
    return true;
  },
        options);
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::timeout);
  EXPECT_FALSE(client.connected());
}

TEST_F(MultiseatWorkerClientConcurrency, ShutdownAckSurvivesEarlierMediaEof) {
  auto shutdown = std::make_shared<std::promise<void>>();
  auto shutdown_requested = shutdown->get_future().share();
  auto media_closed = std::make_shared<std::promise<void>>();
  auto closed = media_closed->get_future().share();
  start([&, shutdown, shutdown_requested, media_closed, closed](int fd, channel_e channel) {
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (channel == channel_e::media) {
      if (shutdown_requested.wait_for(1s) == std::future_status::ready) {
        (void) ::shutdown(fd, SHUT_RDWR);
      }
      media_closed->set_value();
      return true;
    }
    if (!peer.expect(message_e::shutdown)) {
      return true;
    }
    shutdown->set_value();
    if (closed.wait_for(1s) == std::future_status::ready) {
      // Ensure EOF dispatch has an opportunity before the control ACK.
      std::this_thread::sleep_for(50ms);
      EXPECT_TRUE(peer.send(message_e::shutdown_ack));
    }
    return true;
  });
  EXPECT_EQ(client.shutdown(), transport_status_e::applied);
  EXPECT_FALSE(client.connected());
}

TEST_F(MultiseatWorkerClientConcurrency, ZeroPayloadMarkersStillConsumeFrameBudget) {
  auto overflow = std::make_shared<std::promise<void>>();
  auto overflow_allowed = overflow->get_future().share();
  start([&, overflow_allowed](int fd, channel_e channel) {
    if (channel != channel_e::media) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (!peer.expect(message_e::attach) || !peer.send(message_e::attached)) {
      return true;
    }
    for (int i = 0; i < 64; ++i) {
      if (!peer.send(message_e::end_of_stream)) {
        return true;
      }
    }
    if (!peer.expect(message_e::heartbeat) || !peer.send(message_e::heartbeat_ack)) {
      return true;
    }
    if (overflow_allowed.wait_for(1s) == std::future_status::ready) {
      EXPECT_TRUE(peer.send(message_e::end_of_stream));
    }
    peer.await_close();
    return true;
  });
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  // The heartbeat ACK is after all 64 markers on the same ordered channel.
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_TRUE(client.connected());
  overflow->set_value();
  expect_retired();
  encoded_media_packet_t packet {.payload = {1}};
  EXPECT_EQ(client.receive_media(packet), transport_status_e::protocol_rejected);
  EXPECT_TRUE(packet.payload.empty());
}

TEST_F(MultiseatWorkerClientConcurrency, PayloadBudgetIsSharedAcrossControlAndMedia) {
  start([&](int fd, channel_e channel) {
    if (channel != channel_e::media) {
      return false;
    }
    scripted_channel_t peer {fd, channel, authority.identity()};
    if (!peer.expect(message_e::attach) || !peer.send(message_e::attached)) {
      return true;
    }
    for (int i = 0; i < 2; ++i) {
      if (!peer.send(message_e::video, std::vector<std::uint8_t>(max_media_payload, 7))) {
        return true;
      }
    }
    if (peer.expect(message_e::heartbeat)) {
      EXPECT_TRUE(peer.send(message_e::heartbeat_ack));
    }
    peer.await_close();
    return true;
  });
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  ASSERT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_TRUE(client.connected());
  // The normal control peer replies with an input ACK followed by six bytes
  // of feedback. Those six bytes exceed the shared 32 MiB queue budget.
  EXPECT_EQ(client.send_input(std::array<std::uint8_t, 1> {7}), transport_status_e::applied);
  expect_retired();
}

TEST_F(MultiseatWorkerClientConcurrency, ReconnectRetiresOldReceiveWithoutTouchingNewWorker) {
  start();
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  encoded_media_packet_t old_packet;
  ASSERT_EQ(client.receive_media(old_packet), transport_status_e::applied);
  ASSERT_EQ(client.receive_media(old_packet), transport_status_e::applied);
  auto old_receive = std::async(std::launch::async, [&] {
    return client.receive_media(old_packet);
  });
  ASSERT_EQ(old_receive.wait_for(50ms), std::future_status::timeout);
  std::vector<std::uint8_t> feedback;
  auto old_feedback = std::async(std::launch::async, [&] {
    return client.receive_feedback(feedback);
  });
  ASSERT_EQ(old_feedback.wait_for(50ms), std::future_status::timeout);
  auto second_identity = identity_for(12);
  second_identity.worker_name = "polaris-worker-controller-c3d4-12";
  auto second = create_authority(store, second_identity, "replacement");
  fake_worker_t replacement {second};
  ASSERT_EQ(client.connect(second, short_options()), transport_status_e::applied);
  EXPECT_EQ(old_receive.get(), transport_status_e::closed);
  EXPECT_EQ(old_feedback.get(), transport_status_e::closed);
  EXPECT_TRUE(old_packet.payload.empty());
  EXPECT_TRUE(feedback.empty());
  worker->stop();
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_EQ(client.send_input(std::array<std::uint8_t, 1> {9}), transport_status_e::applied);
  EXPECT_EQ(client.shutdown(), transport_status_e::applied);
}

TEST(MultiseatWorkerClient, CloseCancelsAuthenticationWithoutWaitingForItsDeadline) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x61)};
  auto authority = create_authority(store, identity_for(), "cancel-auth");
  fake_worker_t worker {authority, fake_behavior_e::stall, false};
  controller_client_t client;
  auto options = short_options();
  options.handshake_timeout = 5s;
  auto connecting = std::async(std::launch::async, [&] {
    return client.connect(authority, options);
  });
  ASSERT_EQ(connecting.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(client.lease_connection(), controller_connection_t {});
  const auto began = std::chrono::steady_clock::now();
  client.close();
  EXPECT_EQ(connecting.wait_for(50ms), std::future_status::ready);
  EXPECT_NE(connecting.get(), transport_status_e::applied);
  EXPECT_LT(std::chrono::steady_clock::now() - began, 75ms);
  EXPECT_FALSE(client.connected());
  EXPECT_EQ(client.lease_connection(), controller_connection_t {});
}

namespace {
  int client_socket_for(const std::filesystem::path &path) {
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      const int fd = std::stoi(entry.path().filename().string());
      sockaddr_un peer {};
      socklen_t size = sizeof(peer);
      if (::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &size) == 0 &&
          peer.sun_family == AF_UNIX && size > offsetof(sockaddr_un, sun_path) &&
          std::string(peer.sun_path, strnlen(peer.sun_path, sizeof(peer.sun_path))) == path.native()) {
        return fd;
      }
    }
    return -1;
  }

  void check_backpressured_writer(bool reconnect) {
    temporary_root_t root;
    authority_store_t store {root.path(), deterministic_capability(0x71)};
    auto authority = create_authority(store, identity_for(), "blocked-writer");
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();
    auto arrived = std::make_shared<std::promise<void>>();
    auto input_arrived = arrived->get_future();
    fake_worker_t worker {authority, fake_behavior_e::healthy, true, [&, released, arrived](int fd, channel_e channel) {
                            if (channel != channel_e::control) {
                              return false;
                            }
                            scripted_channel_t peer {fd, channel, authority.identity()};
                            if (!peer.expect(message_e::attach) || !peer.send(message_e::attached)) {
                              return true;
                            }
                            pollfd readable {.fd = fd, .events = POLLIN, .revents = 0};
                            int result;
                            do {
                              result = ::poll(&readable, 1, 1000);
                            } while (result < 0 && errno == EINTR);
                            std::uint8_t prefix = 0;
                            if (result > 0 && (readable.revents & POLLIN) &&
                                ::recv(fd, &prefix, 1, MSG_PEEK | MSG_DONTWAIT) == 1) {
                              arrived->set_value();
                            }
                            // Read no input bytes until after cancellation, so a frame larger than
                            // the client's constrained send buffer necessarily blocks write_exact.
                            (void) released.wait_for(2s);
                            return true;
                          }};
    controller_client_t client;
    ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
    ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
    const int old_socket = client_socket_for(authority.paths().control_socket);
    ASSERT_GE(old_socket, 0);
    const int requested_buffer = 4096;
    ASSERT_EQ(::setsockopt(old_socket, SOL_SOCKET, SO_SNDBUF, &requested_buffer, sizeof(requested_buffer)), 0);
    int actual_buffer = 0;
    socklen_t size = sizeof(actual_buffer);
    ASSERT_EQ(::getsockopt(old_socket, SOL_SOCKET, SO_SNDBUF, &actual_buffer, &size), 0);
    ASSERT_LT(static_cast<std::size_t>(actual_buffer), max_control_payload);
    const std::vector<std::uint8_t> payload(max_control_payload, 0x71);
    auto writing = std::async(std::launch::async, [&] {
      return client.send_input(payload);
    });
    ASSERT_EQ(input_arrived.wait_for(1s), std::future_status::ready);
    ASSERT_EQ(writing.wait_for(50ms), std::future_status::timeout);
    // An independently authenticated channel still makes progress.
    EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
    auto second_identity = identity_for(12);
    second_identity.worker_name = "polaris-worker-controller-c3d4-12";
    auto second = create_authority(store, second_identity, "after-blocked-writer");
    fake_worker_t replacement {second};
    const auto began = std::chrono::steady_clock::now();
    if (reconnect) {
      EXPECT_EQ(client.connect(second, short_options()), transport_status_e::applied);
    } else {
      client.close();
    }
    EXPECT_EQ(writing.wait_for(250ms), std::future_status::ready);
    EXPECT_NE(writing.get(), transport_status_e::applied);
    EXPECT_LT(std::chrono::steady_clock::now() - began, 250ms);
    release->set_value();
    worker.stop();
    // Reserve the exact retired socket number for an unrelated socket. Late
    // cleanup must not shutdown/close it, even when the new worker stops.
    std::array<int, 2> unrelated {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, unrelated.data()), 0);
    int receiver = old_socket;
    int sender = unrelated[1];
    if (unrelated[1] == old_socket) {
      sender = unrelated[0];
    } else if (unrelated[0] != old_socket) {
      receiver = ::fcntl(unrelated[0], F_DUPFD_CLOEXEC, old_socket);
      EXPECT_EQ(receiver, old_socket);
      EXPECT_EQ(::close(unrelated[0]), 0);
    }
    if (!reconnect) {
      EXPECT_EQ(client.connect(second, short_options()), transport_status_e::applied);
    }
    EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
    client.close();
    replacement.stop();
    const std::array<std::uint8_t, 1> marker {0x55};
    EXPECT_TRUE(write_all(sender, marker));
    std::array<std::uint8_t, 1> observed {};
    EXPECT_EQ(::recv(receiver, observed.data(), observed.size(), MSG_DONTWAIT), 1);
    EXPECT_EQ(observed, marker);
    EXPECT_EQ(::close(sender), 0);
    EXPECT_EQ(::close(receiver), 0);
  }
}  // namespace

TEST(MultiseatWorkerClient, CloseCancelsBackpressuredWriterWithoutTouchingReusedDescriptor) {
  check_backpressured_writer(false);
}

TEST(MultiseatWorkerClient, ReconnectCancelsBackpressuredWriterWithoutTouchingReusedDescriptor) {
  check_backpressured_writer(true);
}

TEST(MultiseatWorkerClient, NewConnectionRetiresAnOverlappingStalledHandshake) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x72)};
  auto first = create_authority(store, identity_for(), "stalled-handshake");
  fake_worker_t stalled {first, fake_behavior_e::stall, false};
  auto second_identity = identity_for(12);
  second_identity.worker_name = "polaris-worker-controller-c3d4-12";
  auto second = create_authority(store, second_identity, "new-handshake");
  fake_worker_t replacement {second};
  controller_client_t client;
  auto options = short_options();
  options.handshake_timeout = 5s;
  auto older = std::async(std::launch::async, [&] {
    return client.connect(first, options);
  });
  ASSERT_EQ(older.wait_for(20ms), std::future_status::timeout);
  ASSERT_EQ(client.connect(second, options), transport_status_e::applied);
  EXPECT_EQ(older.wait_for(50ms), std::future_status::ready);
  EXPECT_NE(older.get(), transport_status_e::applied);
  EXPECT_TRUE(client.connected());
  EXPECT_EQ(client.attach_data_plane(), transport_status_e::applied);
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(client.shutdown(), transport_status_e::applied);
}

TEST(MultiseatWorkerConnection, EmptyLeaseRejectsOperationsAndClearsOutput) {
  controller_client_t client;
  const auto lease = client.lease_connection();
  EXPECT_EQ(lease, controller_connection_t {});
  EXPECT_FALSE(lease.identity());
  EXPECT_FALSE(lease.connected());
  EXPECT_FALSE(lease.data_plane_attached());
  EXPECT_EQ(lease.attach_data_plane(), transport_status_e::closed);
  EXPECT_EQ(lease.send_input(std::array<std::uint8_t, 1> {1}), transport_status_e::closed);
  EXPECT_EQ(lease.heartbeat(channel_e::control), transport_status_e::closed);
  EXPECT_EQ(lease.shutdown(), transport_status_e::closed);
  std::vector<std::uint8_t> feedback {1};
  encoded_media_packet_t packet {.message = message_e::audio, .payload = {2}};
  EXPECT_EQ(lease.receive_feedback(feedback), transport_status_e::closed);
  EXPECT_EQ(lease.receive_media(packet), transport_status_e::closed);
  EXPECT_TRUE(feedback.empty());
  EXPECT_EQ(packet, encoded_media_packet_t {});
  lease.close();
}

TEST_F(MultiseatWorkerClientConcurrency, LeaseUsesExistingAuthenticationAndDroppingCopyKeepsItAlive) {
  start();
  const auto lease = client.lease_connection();
  EXPECT_EQ(lease.identity(), authority.identity());
  {
    const auto copy = client.lease_connection();
    EXPECT_EQ(copy, lease);
    ASSERT_EQ(copy.attach_data_plane(), transport_status_e::applied);
    EXPECT_EQ(copy.send_input(std::array<std::uint8_t, 1> {9}), transport_status_e::applied);
  }
  EXPECT_TRUE(client.data_plane_attached());
  EXPECT_TRUE(lease.data_plane_attached());
  std::vector<std::uint8_t> feedback;
  encoded_media_packet_t packet;
  EXPECT_EQ(lease.receive_feedback(feedback), transport_status_e::applied);
  EXPECT_EQ(feedback, (std::vector<std::uint8_t> {'r', 'u', 'm', 'b', 'l', 'e'}));
  ASSERT_EQ(lease.receive_media(packet), transport_status_e::applied);
  EXPECT_EQ(packet.message, message_e::video);
  EXPECT_EQ(lease.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(lease.shutdown(), transport_status_e::applied);
  EXPECT_FALSE(client.connected());
  EXPECT_FALSE(lease.connected());
  EXPECT_EQ(client.lease_connection(), controller_connection_t {});
  EXPECT_EQ(lease.identity(), authority.identity());
}

TEST_F(MultiseatWorkerClientConcurrency, OldLeaseCannotReadOrCloseSameIdentityReplacement) {
  start();
  const auto old = client.lease_connection();
  ASSERT_EQ(old.attach_data_plane(), transport_status_e::applied);
  // Both old packets stay queued. Recreate only this fixture's listening
  // sockets while the old accepted connection remains alive. The authenticated
  // identity is intentionally identical, but these are different connections.
  ASSERT_TRUE(std::filesystem::remove(authority.paths().control_socket));
  ASSERT_TRUE(std::filesystem::remove(authority.paths().media_socket));
  fake_worker_t replacement {authority};
  ASSERT_EQ(client.connect(authority, short_options()), transport_status_e::applied);
  const auto current = client.lease_connection();
  EXPECT_EQ(current.identity(), old.identity());
  EXPECT_NE(current, old);
  EXPECT_FALSE(old.connected());
  EXPECT_EQ(old.attach_data_plane(), transport_status_e::closed);
  EXPECT_EQ(old.send_input(std::array<std::uint8_t, 1> {1}), transport_status_e::closed);
  EXPECT_EQ(old.heartbeat(channel_e::media), transport_status_e::closed);
  EXPECT_EQ(old.shutdown(), transport_status_e::closed);
  old.close();
  worker->stop();
  ASSERT_EQ(current.attach_data_plane(), transport_status_e::applied);
  encoded_media_packet_t stale {.message = message_e::video, .payload = {1}};
  EXPECT_EQ(old.receive_media(stale), transport_status_e::closed);
  EXPECT_TRUE(stale.payload.empty());
  std::vector<std::uint8_t> feedback {1};
  EXPECT_EQ(old.receive_feedback(feedback), transport_status_e::closed);
  EXPECT_TRUE(feedback.empty());
  encoded_media_packet_t packet;
  ASSERT_EQ(current.receive_media(packet), transport_status_e::applied);
  EXPECT_EQ(packet.payload, (std::vector<std::uint8_t> {'v', 'i', 'd', 'e', 'o'}));
  ASSERT_EQ(current.receive_media(packet), transport_status_e::applied);
  EXPECT_EQ(packet.payload, (std::vector<std::uint8_t> {'a', 'u', 'd', 'i', 'o'}));
  EXPECT_EQ(current.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(current.shutdown(), transport_status_e::applied);
}

TEST_F(MultiseatWorkerClientConcurrency, ReconnectCancelsBothLeaseWaitsAndRetainedCopiesStayOld) {
  auto options = short_options();
  options.io_timeout = 5s;
  start({}, options);
  const auto old = client.lease_connection();
  ASSERT_EQ(old.attach_data_plane(), transport_status_e::applied);
  encoded_media_packet_t packet;
  ASSERT_EQ(old.receive_media(packet), transport_status_e::applied);
  ASSERT_EQ(old.receive_media(packet), transport_status_e::applied);
  auto media = std::async(std::launch::async, [old, &packet] {
    return old.receive_media(packet);
  });
  std::vector<std::uint8_t> feedback;
  auto control = std::async(std::launch::async, [old, &feedback] {
    return old.receive_feedback(feedback);
  });
  ASSERT_EQ(media.wait_for(20ms), std::future_status::timeout);
  ASSERT_EQ(control.wait_for(20ms), std::future_status::timeout);
  auto next_identity = identity_for(12);
  next_identity.worker_name = "polaris-worker-controller-c3d4-12";
  auto next_authority = create_authority(store, next_identity, "lease-replacement");
  fake_worker_t replacement {next_authority};
  ASSERT_EQ(client.connect(next_authority, options), transport_status_e::applied);
  EXPECT_EQ(media.wait_for(100ms), std::future_status::ready);
  EXPECT_EQ(control.wait_for(100ms), std::future_status::ready);
  EXPECT_EQ(media.get(), transport_status_e::closed);
  EXPECT_EQ(control.get(), transport_status_e::closed);
  EXPECT_TRUE(packet.payload.empty());
  EXPECT_TRUE(feedback.empty());
  const auto current = client.lease_connection();
  EXPECT_EQ(current.identity(), next_identity);
  EXPECT_EQ(old.identity(), authority.identity());
  old.close();
  EXPECT_EQ(current.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(current.shutdown(), transport_status_e::applied);
}

TEST(MultiseatWorkerConnection, LeaseOutlivesClientButClientDestructionRetiresDelivery) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability(0x73)};
  auto authority = create_authority(store, identity_for(), "lease-destruction");
  fake_worker_t worker {authority};
  auto client = std::make_unique<controller_client_t>();
  auto options = short_options();
  options.io_timeout = 5s;
  ASSERT_EQ(client->connect(authority, options), transport_status_e::applied);
  const auto lease = client->lease_connection();
  ASSERT_EQ(lease.attach_data_plane(), transport_status_e::applied);
  std::vector<std::uint8_t> feedback;
  auto pending = std::async(std::launch::async, [lease, &feedback] {
    return lease.receive_feedback(feedback);
  });
  ASSERT_EQ(pending.wait_for(20ms), std::future_status::timeout);
  client.reset();
  EXPECT_EQ(pending.wait_for(100ms), std::future_status::ready);
  EXPECT_EQ(pending.get(), transport_status_e::closed);
  EXPECT_FALSE(lease.connected());
  EXPECT_EQ(lease.identity(), authority.identity());
  encoded_media_packet_t packet;
  EXPECT_EQ(lease.receive_media(packet), transport_status_e::closed);
  EXPECT_TRUE(packet.payload.empty());
  lease.close();
}

TEST_F(MultiseatWorkerClientConcurrency, ShutdownAdmissionPreventsNewLeasesBeforeAcknowledgement) {
  auto arrived = std::make_shared<std::promise<void>>();
  auto shutdown_arrived = arrived->get_future();
  std::promise<void> released;
  auto release = released.get_future().share();
  start([identity = authority.identity(), arrived, release](int fd, channel_e channel) {
    if (channel != channel_e::control) {
      return false;
    }
    scripted_channel_t peer {fd, channel, identity};
    if (!peer.expect(message_e::shutdown)) {
      return true;
    }
    arrived->set_value();
    release.wait();
    EXPECT_TRUE(peer.send(message_e::shutdown_ack));
    peer.await_close();
    return true;
  });
  const auto lease = client.lease_connection();
  auto shutting_down = std::async(std::launch::async, [lease] {
    return lease.shutdown();
  });
  const auto status = shutdown_arrived.wait_for(1s);
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(client.lease_connection(), controller_connection_t {});
  released.set_value();
  EXPECT_EQ(shutting_down.get(), transport_status_e::applied);
  EXPECT_FALSE(lease.connected());
}

#endif
