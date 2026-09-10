/**
 * @file src/platform/linux/multiseat_worker_client.cpp
 * @brief Authenticated controller-side Unix transport for multiseat workers.
 */
#include "multiseat_worker_client.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <condition_variable>
  #include <cstddef>
  #include <cstring>
  #include <deque>
  #include <fcntl.h>
  #include <limits>
  #include <mutex>
  #include <openssl/crypto.h>
  #include <optional>
  #include <poll.h>
  #include <span>
  #include <sys/eventfd.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/un.h>
  #include <thread>
  #include <unistd.h>
  #include <utility>
  #include <vector>

namespace multiseat::worker_ipc {
  namespace {
    using monotonic_clock_t = std::chrono::steady_clock;
    using deadline_t = monotonic_clock_t::time_point;
    constexpr std::size_t maximum_pending_frames = 64;
    constexpr std::size_t maximum_pending_payload = 32 * 1024 * 1024;

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

    transport_status_e wait_for(int descriptor, short events, deadline_t deadline, int cancellation = -1) {
      while (true) {
        const auto remaining = remaining_milliseconds(deadline);
        if (remaining <= 0) {
          return transport_status_e::timeout;
        }
        pollfd requests[] {{.fd = descriptor, .events = events, .revents = 0}, {.fd = cancellation, .events = POLLIN, .revents = 0}};
        const auto result = ::poll(requests, 2, remaining);
        if (result > 0) {
          if (requests[1].revents != 0 || (requests[0].revents & POLLNVAL) != 0) {
            return transport_status_e::closed;
          }
          return transport_status_e::applied;
        }
        if (result == 0) {
          // Long idle waits are split at poll's signed-int millisecond limit.
          // Only the actual deadline may retire the connection.
          continue;
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
      auto parsed = parse_frame(
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
      frame = std::move(*parsed.frame);
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
      int &descriptor,
      int cancellation
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
        const auto ready = wait_for(connected, POLLOUT, deadline, cancellation);
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
            ) != 0 ||
            socket_error != 0) {
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

    bool asynchronous_message(channel_e channel, message_e message) {
      if (channel == channel_e::control) {
        return message == message_e::feedback;
      }
      return message == message_e::video ||
             message == message_e::audio ||
             message == message_e::end_of_stream ||
             message == message_e::discontinuity;
    }
  }  // namespace

  struct controller_connection_t::implementation_t {
    struct channel_t {
      channel_state_t wire;
      std::thread reader;
      std::deque<frame_t> pending;
      bool attached = false;
      bool request_busy = false;
      std::optional<message_e> expected_ack;
      bool acknowledged = false;
    };

    const endpoint_identity_t identity;
    const controller_client_options_t options;
    const int cancellation = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    channel_t control;
    channel_t media;
    mutable std::mutex mutex;
    std::condition_variable changed;
    // Publication and shutdown of descriptors use only this short lock.
    // Neither socket I/O nor a reader join holds it.
    std::mutex descriptor_mutex;
    std::atomic<bool> terminal {false};
    transport_status_e failure = transport_status_e::closed;
    std::size_t pending_payload = 0;
    bool authenticated = false;
    bool attaching = false;
    bool stopping = false;

    implementation_t(endpoint_identity_t identity, controller_client_options_t options):
        identity(std::move(identity)),
        options(options) {
      media.wire.channel = channel_e::media;
    }

    ~implementation_t() {
      cancel(transport_status_e::closed);
      if (control.reader.joinable()) {
        control.reader.join();
      }
      if (media.reader.joinable()) {
        media.reader.join();
      }
      // Public operations retain this owner until their writers and waits
      // retire. No descriptor can be recycled while an old operation uses it.
      close_channel(control.wire);
      close_channel(media.wire);
      if (cancellation >= 0) {
        (void) ::close(cancellation);
      }
    }

    // The state lock linearizes failure with request admission and delivery.
    // Never release a timed-out request's gate before retiring its owner.
    void retire_locked(transport_status_e reason) noexcept {
      if (terminal.exchange(true)) {
        return;
      }
      failure = reason;
      control.pending.clear();
      media.pending.clear();
      pending_payload = 0;
      // Keep a dispatched ACK: it completed before this terminal event.
      changed.notify_all();
    }

    void interrupt_io() noexcept {
      std::scoped_lock descriptors {descriptor_mutex};
      const std::uint64_t signal = 1;
      if (cancellation >= 0) {
        while (::write(cancellation, &signal, sizeof(signal)) < 0 && errno == EINTR) {}
      }
      for (const auto *channel : {&control, &media}) {
        if (channel->wire.descriptor >= 0) {
          (void) ::shutdown(channel->wire.descriptor, SHUT_RDWR);
        }
      }
    }

    void cancel(transport_status_e reason) noexcept {
      {
        std::scoped_lock lock {mutex};
        retire_locked(reason);
      }
      interrupt_io();
    }

    transport_status_e install_socket(channel_t &channel, const std::filesystem::path &path, std::uint32_t uid) {
      int descriptor = -1;
      // A private cancellation descriptor also interrupts an unpublished
      // connecting socket. Retirement prevents later publication.
      auto status = connect_socket(path, uid, options.connect_timeout, descriptor, cancellation);
      std::scoped_lock lock {descriptor_mutex};
      if (terminal.load()) {
        if (descriptor >= 0) {
          (void) ::close(descriptor);
        }
        return transport_status_e::closed;
      }
      if (status == transport_status_e::applied) {
        channel.wire.descriptor = descriptor;
      }
      return status;
    }

    transport_status_e initialize(const authority_handle_t &authority) {
      if (cancellation < 0) {
        cancel(transport_status_e::unavailable);
        return transport_status_e::unavailable;
      }
      capability_t capability {};
      std::copy(authority.capability().begin(), authority.capability().end(), capability.begin());
      auto status = install_socket(control, authority.paths().control_socket, authority.owner_uid());
      if (status == transport_status_e::applied) {
        status = authenticate_channel(control.wire, identity, capability, options.handshake_timeout);
      }
      if (status == transport_status_e::applied) {
        status = install_socket(media, authority.paths().media_socket, authority.owner_uid());
      }
      if (status == transport_status_e::applied) {
        status = authenticate_channel(media.wire, identity, capability, options.handshake_timeout);
      }
      OPENSSL_cleanse(capability.data(), capability.size());
      if (status != transport_status_e::applied) {
        cancel(status);
        return status;
      }
      try {
        // Readers borrow this owner. Its destructor cancels and joins them;
        // readers never hold a shared_ptr that could destroy/join itself.
        control.reader = std::thread([this] {
          read_channel(control);
        });
        media.reader = std::thread([this] {
          read_channel(media);
        });
      } catch (...) {
        cancel(transport_status_e::unavailable);
        return transport_status_e::unavailable;
      }
      std::scoped_lock lock {mutex};
      if (terminal.load()) {
        return transport_status_e::closed;
      }
      authenticated = true;
      return transport_status_e::applied;
    }

    bool connected() const {
      std::scoped_lock lock {mutex};
      return authenticated && !terminal.load();
    }

    bool attached() const {
      std::scoped_lock lock {mutex};
      return authenticated && !terminal.load() && !stopping && control.attached && media.attached;
    }

    void read_channel(channel_t &channel) noexcept {
      try {
        while (!terminal.load()) {
          // Silence is normal, especially for feedback. Once any byte is
          // available, the complete frame has the existing bounded deadline.
          auto status = wait_for(channel.wire.descriptor, POLLIN, deadline_t::max());
          frame_t frame;
          if (status == transport_status_e::applied) {
            status = receive_frame(channel.wire, identity, monotonic_clock_t::now() + options.io_timeout, channel.wire.channel == channel_e::control ? max_control_payload : max_media_payload, frame);
          }
          if (status != transport_status_e::applied) {
            {
              std::scoped_lock lock {mutex};
              // The worker closes media just after sending the control
              // shutdown ACK. Let the control reader consume that ACK.
              if (status == transport_status_e::closed && stopping && &channel == &media && !terminal.load()) {
                return;
              }
            }
            cancel(status);
            return;
          }
          bool rejected = false;
          {
            std::scoped_lock lock {mutex};
            if (terminal.load()) {
              return;
            }
            if (asynchronous_message(channel.wire.channel, frame.message)) {
              if (!channel.attached) {
                rejected = true;
              } else if (!stopping) {
                if (control.pending.size() + media.pending.size() >= maximum_pending_frames ||
                    frame.payload.size() > maximum_pending_payload - pending_payload) {
                  rejected = true;
                } else {
                  pending_payload += frame.payload.size();
                  channel.pending.push_back(std::move(frame));
                  changed.notify_all();
                }
              }
            } else if (!channel.request_busy || !channel.expected_ack ||
                       *channel.expected_ack != frame.message || channel.acknowledged) {
              rejected = true;
            } else {
              channel.acknowledged = true;
              // The worker can send data immediately after this ACK. The
              // reader, not the awakened caller, owns this phase transition.
              if (frame.message == message_e::attached) {
                channel.attached = true;
              }
              changed.notify_all();
            }
            if (rejected) {
              retire_locked(transport_status_e::protocol_rejected);
            }
          }
          if (rejected) {
            interrupt_io();
            return;
          }
        }
      } catch (...) {
        cancel(transport_status_e::io_error);
      }
    }

    transport_status_e request(channel_t &channel, message_e message, message_e expected, std::span<const std::uint8_t> payload = {}) {
      const auto deadline = monotonic_clock_t::now() + options.io_timeout;
      std::unique_lock lock {mutex};
      if (!changed.wait_until(lock, deadline, [&] {
            return terminal.load() || stopping || !channel.request_busy;
          })) {
        retire_locked(transport_status_e::timeout);
        lock.unlock();
        interrupt_io();
        return transport_status_e::timeout;
      }
      if (!authenticated || terminal.load() || stopping) {
        return transport_status_e::closed;
      }
      if (message == message_e::input && !(control.attached && media.attached)) {
        return transport_status_e::closed;
      }
      channel.request_busy = true;
      channel.expected_ack = expected;
      channel.acknowledged = false;
      if (message == message_e::shutdown) {
        stopping = true;
        control.pending.clear();
        media.pending.clear();
        pending_payload = 0;
        changed.notify_all();
      }
      lock.unlock();
      // Allocate only after admission: at most one outgoing frame per
      // channel, irrespective of the number of callers waiting for its ACK.
      auto status = transport_status_e::io_error;
      try {
        status = send_frame(channel.wire, {.channel = channel.wire.channel, .message = message, .slot = identity.slot, .generation = identity.generation, .payload = std::vector<std::uint8_t>(payload.begin(), payload.end())}, deadline);
      } catch (...) {
        status = transport_status_e::io_error;
      }
      if (status != transport_status_e::applied) {
        cancel(status);
      }
      lock.lock();
      if (status == transport_status_e::applied) {
        if (!changed.wait_until(lock, deadline, [&] {
              return channel.acknowledged || terminal.load();
            })) {
          status = transport_status_e::timeout;
        } else {
          status = channel.acknowledged ? transport_status_e::applied : failure;
        }
      }
      if (status != transport_status_e::applied) {
        retire_locked(status);
      }
      channel.request_busy = false;
      channel.expected_ack.reset();
      channel.acknowledged = false;
      changed.notify_all();
      lock.unlock();
      if (status != transport_status_e::applied) {
        interrupt_io();
      }
      return status;
    }

    transport_status_e attach() {
      {
        std::scoped_lock lock {mutex};
        if (!authenticated || terminal.load() || stopping) {
          return transport_status_e::closed;
        }
        if (attaching || control.attached || media.attached) {
          return transport_status_e::invalid_argument;
        }
        attaching = true;
      }
      auto status = request(control, message_e::attach, message_e::attached);
      if (status == transport_status_e::applied) {
        status = request(media, message_e::attach, message_e::attached);
      }
      if (status != transport_status_e::applied) {
        cancel(status);
      }
      return status;
    }

    transport_status_e receive(channel_t &channel, frame_t &frame) {
      const auto deadline = monotonic_clock_t::now() + options.io_timeout;
      std::unique_lock lock {mutex};
      if (terminal.load()) {
        return failure;
      }
      if (!authenticated || !(control.attached && media.attached) || stopping) {
        return transport_status_e::closed;
      }
      if (!changed.wait_until(lock, deadline, [&] {
            return terminal.load() || stopping || !channel.pending.empty();
          })) {
        retire_locked(transport_status_e::timeout);
        lock.unlock();
        interrupt_io();
        return transport_status_e::timeout;
      }
      if (terminal.load()) {
        return failure;
      }
      if (stopping) {
        return transport_status_e::closed;
      }
      frame = std::move(channel.pending.front());
      pending_payload -= frame.payload.size();
      channel.pending.pop_front();
      return transport_status_e::applied;
    }
  };

  struct controller_client_t::implementation_t {
    using connection_t = controller_connection_t::implementation_t;

    mutable std::mutex mutex;
    std::shared_ptr<connection_t> current;

    std::shared_ptr<connection_t> snapshot() const {
      std::scoped_lock lock {mutex};
      return current;
    }

    std::shared_ptr<connection_t> exchange(std::shared_ptr<connection_t> replacement) {
      std::scoped_lock lock {mutex};
      if (current) {
        // Retire delivery before publishing the replacement. Leases have no
        // client lookup, so this lock order fences their final delivery check.
        std::scoped_lock delivery {current->mutex};
        current->retire_locked(transport_status_e::closed);
      }
      return std::exchange(current, std::move(replacement));
    }
  };

  controller_client_t::controller_client_t():
      implementation_(std::make_unique<implementation_t>()) {}

  controller_client_t::~controller_client_t() {
    close();
  }

  transport_status_e controller_client_t::connect(const authority_handle_t &authority, controller_client_options_t options) {
    std::shared_ptr<implementation_t::connection_t> candidate;
    if (authority.active() && valid_identity(authority.identity()) &&
        authority.owner_uid() == static_cast<std::uint32_t>(::geteuid()) &&
        valid_timeout(options.connect_timeout) && valid_timeout(options.handshake_timeout) && valid_timeout(options.io_timeout)) {
      candidate = std::make_shared<implementation_t::connection_t>(authority.identity(), options);
    }
    const auto previous = implementation_->exchange(candidate);
    if (previous) {
      previous->cancel(transport_status_e::closed);
    }
    if (!candidate) {
      return transport_status_e::invalid_argument;
    }
    const auto status = candidate->initialize(authority);
    if (status == transport_status_e::applied && implementation_->snapshot() != candidate) {
      return transport_status_e::closed;
    }
    return status;
  }

  controller_connection_t::controller_connection_t(std::shared_ptr<implementation_t> connection):
      connection_(std::move(connection)) {}

  std::optional<endpoint_identity_t> controller_connection_t::identity() const {
    return connection_ ? std::optional {connection_->identity} : std::nullopt;
  }

  controller_connection_t controller_client_t::lease_connection() const {
    std::scoped_lock owner {implementation_->mutex};
    const auto &connection = implementation_->current;
    if (!connection) {
      return {};
    }
    std::scoped_lock state {connection->mutex};
    if (!connection->authenticated || connection->terminal.load() || connection->stopping) {
      return {};
    }
    return controller_connection_t {connection};
  }

  transport_status_e controller_client_t::attach_data_plane() {
    return controller_connection_t {implementation_->snapshot()}.attach_data_plane();
  }

  transport_status_e controller_client_t::send_input(std::span<const std::uint8_t> payload) {
    return controller_connection_t {implementation_->snapshot()}.send_input(payload);
  }

  transport_status_e controller_client_t::receive_feedback(std::vector<std::uint8_t> &payload) {
    return controller_connection_t {implementation_->snapshot()}.receive_feedback(payload);
  }

  transport_status_e controller_client_t::receive_media(encoded_media_packet_t &packet) {
    return controller_connection_t {implementation_->snapshot()}.receive_media(packet);
  }

  transport_status_e controller_client_t::heartbeat(channel_e channel) {
    return controller_connection_t {implementation_->snapshot()}.heartbeat(channel);
  }

  transport_status_e controller_client_t::shutdown() {
    return controller_connection_t {implementation_->snapshot()}.shutdown();
  }

  void controller_client_t::close() noexcept {
    controller_connection_t {implementation_->exchange({})}.close();
  }

  bool controller_client_t::connected() const noexcept {
    return controller_connection_t {implementation_->snapshot()}.connected();
  }

  bool controller_client_t::data_plane_attached() const noexcept {
    return controller_connection_t {implementation_->snapshot()}.data_plane_attached();
  }

  transport_status_e controller_connection_t::attach_data_plane() const {
    const auto &connection = connection_;
    return connection ? connection->attach() : transport_status_e::closed;
  }

  transport_status_e controller_connection_t::send_input(std::span<const std::uint8_t> payload) const {
    if (payload.empty() || payload.size() > max_control_payload) {
      return transport_status_e::invalid_argument;
    }
    const auto &connection = connection_;
    return connection ? connection->request(connection->control, message_e::input, message_e::input_ack, payload) : transport_status_e::closed;
  }

  transport_status_e controller_connection_t::receive_feedback(std::vector<std::uint8_t> &payload) const {
    payload.clear();
    const auto &connection = connection_;
    if (!connection) {
      return transport_status_e::closed;
    }
    frame_t frame;
    const auto status = connection->receive(connection->control, frame);
    if (status != transport_status_e::applied) {
      return status;
    }
    std::scoped_lock delivery {connection->mutex};
    if (connection->terminal.load() || connection->stopping) {
      return transport_status_e::closed;
    }
    payload = std::move(frame.payload);
    return transport_status_e::applied;
  }

  transport_status_e controller_connection_t::receive_media(encoded_media_packet_t &packet) const {
    packet = {};
    const auto &connection = connection_;
    if (!connection) {
      return transport_status_e::closed;
    }
    frame_t frame;
    const auto status = connection->receive(connection->media, frame);
    if (status != transport_status_e::applied) {
      return status;
    }
    std::scoped_lock delivery {connection->mutex};
    if (connection->terminal.load() || connection->stopping) {
      return transport_status_e::closed;
    }
    packet = {.message = frame.message, .payload = std::move(frame.payload)};
    return transport_status_e::applied;
  }

  transport_status_e controller_connection_t::heartbeat(channel_e channel) const {
    if (channel != channel_e::control && channel != channel_e::media) {
      return transport_status_e::invalid_argument;
    }
    const auto &connection = connection_;
    if (!connection) {
      return transport_status_e::closed;
    }
    return connection->request(channel == channel_e::control ? connection->control : connection->media, message_e::heartbeat, message_e::heartbeat_ack);
  }

  transport_status_e controller_connection_t::shutdown() const {
    const auto &connection = connection_;
    if (!connection) {
      return transport_status_e::closed;
    }
    const auto status = connection->request(connection->control, message_e::shutdown, message_e::shutdown_ack);
    connection->cancel(transport_status_e::closed);
    return status;
  }

  void controller_connection_t::close() const noexcept {
    const auto &connection = connection_;
    if (connection) {
      connection->cancel(transport_status_e::closed);
    }
  }

  bool controller_connection_t::connected() const noexcept {
    const auto &connection = connection_;
    return connection && connection->connected();
  }

  bool controller_connection_t::data_plane_attached() const noexcept {
    const auto &connection = connection_;
    return connection && connection->attached();
  }

}  // namespace multiseat::worker_ipc

#endif
