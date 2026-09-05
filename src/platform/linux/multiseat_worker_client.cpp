/**
 * @file src/platform/linux/multiseat_worker_client.cpp
 * @brief Authenticated controller-side Unix transport for multiseat workers.
 */
#include "multiseat_worker_client.h"

#ifdef __linux__

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace multiseat::worker_ipc {
  namespace {
    using monotonic_clock_t = std::chrono::steady_clock;
    using deadline_t = monotonic_clock_t::time_point;

    struct socket_identity_t {
      std::uint64_t device = 0;
      std::uint64_t inode = 0;

      bool operator==(const socket_identity_t &) const = default;
    };

    struct channel_state_t {
      int descriptor = -1;
      channel_e channel = channel_e::control;
      sequence_guard_t incoming;
      sequence_guard_t outgoing;
    };

    bool valid_timeout(std::chrono::milliseconds timeout) {
      constexpr auto maximum = std::chrono::seconds {30};
      return timeout > std::chrono::milliseconds::zero() && timeout <= maximum;
    }

    bool socket_path_fits(const std::filesystem::path &path) {
      sockaddr_un address {};
      const auto &native = path.native();
      return path.is_absolute() &&
             path.lexically_normal() == path &&
             !native.empty() &&
             native.size() < sizeof(address.sun_path) &&
             native.find('\0') == std::string::npos;
    }

    bool private_socket_node(
      const std::filesystem::path &path,
      std::uint32_t owner_uid,
      socket_identity_t &identity
    ) {
      struct stat metadata {};
      if (::lstat(path.c_str(), &metadata) != 0 ||
          !S_ISSOCK(metadata.st_mode) ||
          static_cast<std::uint32_t>(metadata.st_uid) != owner_uid ||
          (metadata.st_mode & 07777) != 0600) {
        return false;
      }
      identity = {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
      };
      return true;
    }

    void close_channel(channel_state_t &state) noexcept {
      if (state.descriptor >= 0) {
        (void) ::close(state.descriptor);
        state.descriptor = -1;
      }
    }

    int remaining_milliseconds(deadline_t deadline) {
      const auto remaining = deadline - monotonic_clock_t::now();
      if (remaining <= monotonic_clock_t::duration::zero()) {
        return 0;
      }
      const auto rounded = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining + std::chrono::milliseconds {1} - monotonic_clock_t::duration {1}
      );
      return static_cast<int>(std::min<std::int64_t>(
        rounded.count(),
        std::numeric_limits<int>::max()
      ));
    }

    transport_status_e wait_for(int descriptor, short events, deadline_t deadline) {
      while (true) {
        const auto remaining = remaining_milliseconds(deadline);
        if (remaining <= 0) {
          return transport_status_e::timeout;
        }
        pollfd request {
          .fd = descriptor,
          .events = events,
          .revents = 0,
        };
        const auto result = ::poll(&request, 1, remaining);
        if (result > 0) {
          if ((request.revents & POLLNVAL) != 0) {
            return transport_status_e::closed;
          }
          return transport_status_e::applied;
        }
        if (result == 0) {
          return transport_status_e::timeout;
        }
        if (errno != EINTR) {
          return transport_status_e::io_error;
        }
      }
    }

    transport_status_e read_exact(
      int descriptor,
      std::span<std::uint8_t> destination,
      deadline_t deadline
    ) {
      std::size_t offset = 0;
      while (offset < destination.size()) {
        const auto ready = wait_for(descriptor, POLLIN, deadline);
        if (ready != transport_status_e::applied) {
          return ready;
        }
        const auto result = ::recv(
          descriptor,
          destination.data() + offset,
          destination.size() - offset,
          0
        );
        if (result > 0) {
          offset += static_cast<std::size_t>(result);
          continue;
        }
        if (result == 0) {
          return transport_status_e::closed;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        return transport_status_e::io_error;
      }
      return transport_status_e::applied;
    }

    transport_status_e write_exact(
      int descriptor,
      std::span<const std::uint8_t> source,
      deadline_t deadline
    ) {
      std::size_t offset = 0;
      while (offset < source.size()) {
        const auto ready = wait_for(descriptor, POLLOUT, deadline);
        if (ready != transport_status_e::applied) {
          return ready;
        }
        const auto result = ::send(
          descriptor,
          source.data() + offset,
          source.size() - offset,
          MSG_NOSIGNAL
        );
        if (result > 0) {
          offset += static_cast<std::size_t>(result);
          continue;
        }
        if (result < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
          continue;
        }
        return result == 0 ? transport_status_e::closed : transport_status_e::io_error;
      }
      return transport_status_e::applied;
    }

    transport_status_e receive_frame(
      channel_state_t &state,
      const endpoint_identity_t &identity,
      deadline_t deadline,
      std::size_t maximum_payload,
      frame_t &frame
    ) {
      std::array<std::uint8_t, header_size> header {};
      auto status = read_exact(state.descriptor, header, deadline);
      if (status != transport_status_e::applied) {
        return status;
      }
      const auto header_result = parse_frame(
        header,
        state.channel,
        identity.slot,
        identity.generation
      );
      if (header_result.status == parse_status_e::rejected ||
          header_result.required < header_size ||
          header_result.required - header_size > maximum_payload ||
          header_result.required > header_size +
                                   (state.channel == channel_e::control ?
                                      max_control_payload :
                                      max_media_payload)) {
        return transport_status_e::protocol_rejected;
      }
      std::vector<std::uint8_t> encoded(header_result.required);
      std::copy(header.begin(), header.end(), encoded.begin());
      if (encoded.size() > header.size()) {
        status = read_exact(
          state.descriptor,
          std::span {encoded}.subspan(header.size()),
          deadline
        );
        if (status != transport_status_e::applied) {
          return status;
        }
      }
      const auto parsed = parse_frame(
        encoded,
        state.channel,
        identity.slot,
        identity.generation
      );
      if (parsed.status != parse_status_e::complete ||
          !parsed.frame ||
          parsed.consumed != encoded.size() ||
          !state.incoming.accept(parsed.frame->sequence)) {
        return transport_status_e::protocol_rejected;
      }
      frame = *parsed.frame;
      return transport_status_e::applied;
    }

    transport_status_e send_frame(
      channel_state_t &state,
      frame_t frame,
      deadline_t deadline
    ) {
      const auto expected_sequence = state.outgoing.next();
      if (expected_sequence == 0) {
        return transport_status_e::protocol_rejected;
      }
      frame.channel = state.channel;
      frame.sequence = expected_sequence;
      std::vector<std::uint8_t> encoded;
      try {
        encoded = encode_frame(frame);
      } catch (...) {
        return transport_status_e::protocol_rejected;
      }
      const auto status = write_exact(state.descriptor, encoded, deadline);
      if (status != transport_status_e::applied) {
        return status;
      }
      if (!state.outgoing.accept(expected_sequence)) {
        return transport_status_e::protocol_rejected;
      }
      return transport_status_e::applied;
    }

    transport_status_e connect_socket(
      const std::filesystem::path &path,
      std::uint32_t owner_uid,
      std::chrono::milliseconds timeout,
      int &descriptor
    ) {
      descriptor = -1;
      if (!socket_path_fits(path)) {
        return transport_status_e::invalid_argument;
      }
      struct stat presence {};
      if (::lstat(path.c_str(), &presence) != 0) {
        return errno == ENOENT ?
                 transport_status_e::unavailable :
                 transport_status_e::peer_rejected;
      }
      socket_identity_t before;
      if (!private_socket_node(path, owner_uid, before)) {
        return transport_status_e::peer_rejected;
      }

      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto &native = path.native();
      std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
      auto connected = ::socket(
        AF_UNIX,
        SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
        0
      );
      if (connected < 0) {
        return transport_status_e::io_error;
      }
      const auto deadline = monotonic_clock_t::now() + timeout;
      const auto result = ::connect(
        connected,
        reinterpret_cast<const sockaddr *>(&address),
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native.size() + 1)
      );
      if (result != 0) {
        if (errno != EINPROGRESS) {
          (void) ::close(connected);
          return errno == ENOENT || errno == ECONNREFUSED ?
                   transport_status_e::unavailable :
                   transport_status_e::io_error;
        }
        const auto ready = wait_for(connected, POLLOUT, deadline);
        if (ready != transport_status_e::applied) {
          (void) ::close(connected);
          return ready;
        }
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(
              connected,
              SOL_SOCKET,
              SO_ERROR,
              &socket_error,
              &error_size
            ) != 0 || socket_error != 0) {
          (void) ::close(connected);
          return socket_error == ENOENT || socket_error == ECONNREFUSED ?
                   transport_status_e::unavailable :
                   transport_status_e::io_error;
        }
      }

      ucred credentials {};
      socklen_t credentials_size = sizeof(credentials);
      socket_identity_t after;
      if (::getsockopt(
            connected,
            SOL_SOCKET,
            SO_PEERCRED,
            &credentials,
            &credentials_size
          ) != 0 ||
          credentials_size != sizeof(credentials) ||
          credentials.pid <= 0 ||
          static_cast<std::uint32_t>(credentials.uid) != owner_uid ||
          !private_socket_node(path, owner_uid, after) ||
          after != before) {
        (void) ::close(connected);
        return transport_status_e::peer_rejected;
      }
      descriptor = connected;
      return transport_status_e::applied;
    }

    transport_status_e authenticate_channel(
      channel_state_t &state,
      const endpoint_identity_t &identity,
      const capability_t &capability,
      std::chrono::milliseconds timeout
    ) {
      const auto deadline = monotonic_clock_t::now() + timeout;
      frame_t challenge_frame;
      auto status = receive_frame(
        state,
        identity,
        deadline,
        challenge_size,
        challenge_frame
      );
      if (status != transport_status_e::applied ||
          challenge_frame.message != message_e::challenge ||
          challenge_frame.payload.size() != challenge_size) {
        return status == transport_status_e::applied ?
                 transport_status_e::protocol_rejected :
                 status;
      }
      challenge_t challenge {};
      std::copy(
        challenge_frame.payload.begin(),
        challenge_frame.payload.end(),
        challenge.begin()
      );
      const auto controller_proof = authentication_proof(
        capability,
        proof_role_e::controller,
        state.channel,
        identity,
        challenge
      );
      if (!controller_proof) {
        return transport_status_e::authentication_rejected;
      }
      status = send_frame(
        state,
        {
          .channel = state.channel,
          .message = message_e::authenticate,
          .slot = identity.slot,
          .generation = identity.generation,
          .payload = std::vector<std::uint8_t>(
            controller_proof->begin(),
            controller_proof->end()
          ),
        },
        deadline
      );
      if (status != transport_status_e::applied) {
        return status;
      }
      frame_t authenticated;
      status = receive_frame(
        state,
        identity,
        deadline,
        proof_size,
        authenticated
      );
      if (status != transport_status_e::applied) {
        return status;
      }
      if (authenticated.message != message_e::authenticated) {
        return transport_status_e::protocol_rejected;
      }
      if (!verify_authentication_proof(
            capability,
            proof_role_e::worker,
            state.channel,
            identity,
            challenge,
            authenticated.payload
          )) {
        return transport_status_e::authentication_rejected;
      }
      return transport_status_e::applied;
    }
  }  // namespace

  struct controller_client_t::implementation_t {
    mutable std::mutex mutex;
    endpoint_identity_t identity;
    capability_t capability {};
    channel_state_t control;
    channel_state_t media {.channel = channel_e::media};
    controller_client_options_t options;
    bool has_capability = false;

    void close_locked() noexcept {
      close_channel(media);
      close_channel(control);
      OPENSSL_cleanse(capability.data(), capability.size());
      has_capability = false;
    }

    [[nodiscard]] bool connected_locked() const {
      return control.descriptor >= 0 && media.descriptor >= 0 && has_capability;
    }
  };

  controller_client_t::controller_client_t() :
      implementation_(std::make_unique<implementation_t>()) {
  }

  controller_client_t::~controller_client_t() {
    close();
  }

  transport_status_e controller_client_t::connect(
    const authority_handle_t &authority,
    controller_client_options_t options
  ) {
    std::scoped_lock lock {implementation_->mutex};
    implementation_->close_locked();
    if (!authority.active() ||
        !valid_identity(authority.identity()) ||
        authority.owner_uid() != static_cast<std::uint32_t>(::geteuid()) ||
        !valid_timeout(options.connect_timeout) ||
        !valid_timeout(options.handshake_timeout) ||
        !valid_timeout(options.io_timeout)) {
      return transport_status_e::invalid_argument;
    }

    implementation_->identity = authority.identity();
    std::copy(
      authority.capability().begin(),
      authority.capability().end(),
      implementation_->capability.begin()
    );
    implementation_->has_capability = true;
    implementation_->options = options;
    implementation_->control = channel_state_t {};
    implementation_->media = channel_state_t {.channel = channel_e::media};

    auto status = connect_socket(
      authority.paths().control_socket,
      authority.owner_uid(),
      options.connect_timeout,
      implementation_->control.descriptor
    );
    if (status == transport_status_e::applied) {
      status = authenticate_channel(
        implementation_->control,
        implementation_->identity,
        implementation_->capability,
        options.handshake_timeout
      );
    }
    if (status == transport_status_e::applied) {
      status = connect_socket(
        authority.paths().media_socket,
        authority.owner_uid(),
        options.connect_timeout,
        implementation_->media.descriptor
      );
    }
    if (status == transport_status_e::applied) {
      status = authenticate_channel(
        implementation_->media,
        implementation_->identity,
        implementation_->capability,
        options.handshake_timeout
      );
    }
    if (status != transport_status_e::applied) {
      implementation_->close_locked();
    }
    return status;
  }

  transport_status_e controller_client_t::heartbeat(channel_e channel) {
    std::scoped_lock lock {implementation_->mutex};
    if (channel != channel_e::control && channel != channel_e::media) {
      return transport_status_e::invalid_argument;
    }
    if (!implementation_->connected_locked()) {
      return transport_status_e::closed;
    }
    auto &state = channel == channel_e::control ?
                    implementation_->control :
                    implementation_->media;
    const auto deadline = monotonic_clock_t::now() + implementation_->options.io_timeout;
    auto status = send_frame(
      state,
      {
        .channel = channel,
        .message = message_e::heartbeat,
        .slot = implementation_->identity.slot,
        .generation = implementation_->identity.generation,
      },
      deadline
    );
    frame_t response;
    if (status == transport_status_e::applied) {
      status = receive_frame(
        state,
        implementation_->identity,
        deadline,
        0,
        response
      );
      if (status == transport_status_e::applied &&
          response.message != message_e::heartbeat_ack) {
        status = transport_status_e::protocol_rejected;
      }
    }
    if (status != transport_status_e::applied) {
      implementation_->close_locked();
    }
    return status;
  }

  transport_status_e controller_client_t::shutdown() {
    std::scoped_lock lock {implementation_->mutex};
    if (!implementation_->connected_locked()) {
      return transport_status_e::closed;
    }
    const auto deadline = monotonic_clock_t::now() + implementation_->options.io_timeout;
    auto status = send_frame(
      implementation_->control,
      {
        .channel = channel_e::control,
        .message = message_e::shutdown,
        .slot = implementation_->identity.slot,
        .generation = implementation_->identity.generation,
      },
      deadline
    );
    frame_t response;
    if (status == transport_status_e::applied) {
      status = receive_frame(
        implementation_->control,
        implementation_->identity,
        deadline,
        0,
        response
      );
      if (status == transport_status_e::applied &&
          response.message != message_e::shutdown_ack) {
        status = transport_status_e::protocol_rejected;
      }
    }
    implementation_->close_locked();
    return status;
  }

  void controller_client_t::close() noexcept {
    std::scoped_lock lock {implementation_->mutex};
    implementation_->close_locked();
  }

  bool controller_client_t::connected() const noexcept {
    std::scoped_lock lock {implementation_->mutex};
    return implementation_->connected_locked();
  }

}  // namespace multiseat::worker_ipc

#endif
