/**
 * @file tests/unit/platform/test_stream_send_cancellation.cpp
 * @brief Stalled native sends retire without closing another session's socket.
 */
#include "src/platform/common.h"
#include "src/stream_packet_owner.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
  struct send_fixture_t {
    int socket;
    bool fallback = false;
    int interrupts = 0;
    std::promise<void> polling;
    bool announced = false;
    int sendmsg_calls = 0;
    int sendmmsg_calls = 0;
  };
  thread_local send_fixture_t *fixture = nullptr;
  struct fixture_scope_t {
    explicit fixture_scope_t(send_fixture_t &value) { fixture = &value; }
    ~fixture_scope_t() { fixture = nullptr; }
  };

  void drain(int socket) {
    std::array<char, 2048> bytes {};
    while (::recv(socket, bytes.data(), bytes.size(), MSG_DONTWAIT) > 0) {}
  }
}

extern "C" ssize_t __real_sendmsg(int, const msghdr *, int);
extern "C" int __real_sendmmsg(int, mmsghdr *, unsigned int, int);
extern "C" int __real_poll(pollfd *, nfds_t, int);

// Only translate the fixture's IP addressing/ancillary metadata to a connected
// AF_UNIX pair. The production send loops and the kernel's full send buffer,
// EAGAIN, nonblocking flags, and poll wait are all real. Every other call passes
// through unchanged; no host network or route configuration is needed.
extern "C" ssize_t __wrap_sendmsg(int socket, const msghdr *message, int flags) {
  if (!fixture || fixture->socket != socket) return __real_sendmsg(socket, message, flags);
  ++fixture->sendmsg_calls;
  EXPECT_NE(flags & MSG_DONTWAIT, 0);
  if (fixture->fallback) {
    errno = EOPNOTSUPP;
    return -1;
  }
  auto translated = *message;
  translated.msg_name = nullptr;
  translated.msg_namelen = 0;
  translated.msg_control = nullptr;
  translated.msg_controllen = 0;
  return __real_sendmsg(socket, &translated, flags | MSG_NOSIGNAL);
}

extern "C" int __wrap_sendmmsg(int socket, mmsghdr *messages, unsigned int count, int flags) {
  if (!fixture || fixture->socket != socket) return __real_sendmmsg(socket, messages, count, flags);
  ++fixture->sendmmsg_calls;
  EXPECT_NE(flags & MSG_DONTWAIT, 0);
  std::vector<mmsghdr> translated(messages, messages + count);
  for (auto &message : translated) {
    message.msg_hdr.msg_name = nullptr;
    message.msg_hdr.msg_namelen = 0;
    message.msg_hdr.msg_control = nullptr;
    message.msg_hdr.msg_controllen = 0;
  }
  const auto sent = __real_sendmmsg(socket, translated.data(), count, flags | MSG_NOSIGNAL);
  for (int i = 0; i < sent; ++i) messages[i].msg_len = translated[i].msg_len;
  return sent;
}

extern "C" int __wrap_poll(pollfd *descriptors, nfds_t count, int timeout) {
  if (!fixture || count != 1 || descriptors[0].fd != fixture->socket) {
    return __real_poll(descriptors, count, timeout);
  }
  EXPECT_GT(timeout, 0);
  EXPECT_LE(timeout, 50);
  if (!fixture->announced) {
    fixture->announced = true;
    fixture->polling.set_value();
  }
  if (fixture->interrupts-- > 0) {
    errno = EINTR;
    return -1;
  }
  return __real_poll(descriptors, count, timeout);
}

namespace {
  void stalled_send_retires(bool batch, bool fallback, int interrupts) {
    int sockets[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets), 0);
    auto cleanup = util::fail_guard([&] { ::close(sockets[0]); ::close(sockets[1]); });
    int capacity = 4096;
    ASSERT_EQ(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)), 0);
    std::array<char, 1024> bytes {};
    int filled = 0;
    while (::send(sockets[0], bytes.data(), bytes.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {
      ASSERT_LT(++filled, 128);
    }
    ASSERT_EQ(errno, EAGAIN);
    ASSERT_GT(filled, 0);

    int first_session = 1, peer_session = 2;
    stream_packets::owner_t owner {&first_session}, peer {&peer_session};
    auto destination = owner.destination();
    auto active = destination.acquire();
    send_fixture_t state {sockets[0], fallback, interrupts};
    auto polling = state.polling.get_future();
    auto address = boost::asio::ip::make_address("127.0.0.1");
    auto sender = std::async(std::launch::async,
      [&, delivery = std::move(active)]() mutable {
        fixture_scope_t scope {state};
        bool sent;
        if (batch) {
          std::vector<platf::buffer_descriptor_t> buffers {{bytes.data(), bytes.size()}};
          platf::batched_send_info_t info {nullptr, 0, buffers, 512, 0, 2,
            static_cast<std::uintptr_t>(sockets[0]), address, 9, address, delivery.cancellation()};
          sent = platf::send_batch(info);
        } else {
          platf::send_info_t info {nullptr, 0, bytes.data(), bytes.size(),
            static_cast<std::uintptr_t>(sockets[0]), address, 9, address, delivery.cancellation()};
          sent = platf::send(info);
        }
        delivery.reset();
        return sent;
      });

    EXPECT_EQ(polling.wait_for(1s), std::future_status::ready);
    owner.close();
    auto retirement = std::async(std::launch::async, [&] { owner.close_and_wait(); });
    const auto finished = sender.wait_for(500ms);
    EXPECT_EQ(finished, std::future_status::ready);
    if (finished != std::future_status::ready) {
      // Release only this fixture's full buffer if a regression left a sender
      // blocked, so the negative test can fail without stranding a thread.
      drain(sockets[1]);
    }
    EXPECT_FALSE(sender.get());
    EXPECT_EQ(retirement.wait_for(1s), std::future_status::ready);
    retirement.get();
    EXPECT_FALSE(destination.acquire());
    EXPECT_GE(state.sendmsg_calls, 1);
    EXPECT_EQ(state.sendmmsg_calls > 0, fallback);

    // The same descriptor remains usable by another live session after A
    // retires. No shutdown(), close(), or socket-wide timeout is used above.
    drain(sockets[1]);
    auto surviving = peer.destination().acquire();
    send_fixture_t survivor {sockets[0]};
    fixture_scope_t scope {survivor};
    platf::send_info_t info {nullptr, 0, bytes.data(), bytes.size(),
      static_cast<std::uintptr_t>(sockets[0]), address, 9, address, surviving.cancellation()};
    EXPECT_TRUE(platf::send(info));
    EXPECT_EQ(::recv(sockets[1], bytes.data(), bytes.size(), MSG_DONTWAIT), bytes.size());
  }
}

TEST(StreamSendCancellationTests, FullBufferSingleSendRetiresAndPeerCanUseSocket) {
  stalled_send_retires(false, false, 0);
}
TEST(StreamSendCancellationTests, FullBufferGsoSendRetiresAndPeerCanUseSocket) {
  stalled_send_retires(true, false, 0);
}
TEST(StreamSendCancellationTests, FullBufferSendmmsgFallbackRetiresAndPeerCanUseSocket) {
  stalled_send_retires(true, true, 0);
}
TEST(StreamSendCancellationTests, InterruptedFullBufferWaitStillObservesRetirement) {
  stalled_send_retires(false, false, 4);
}
