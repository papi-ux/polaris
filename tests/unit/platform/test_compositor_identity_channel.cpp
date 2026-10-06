/**
 * @file tests/unit/platform/test_compositor_identity_channel.cpp
 * @brief The supervisor → host compositor pidfd handoff and its pinned lease, against real kernel
 *        objects: forked children, pidfds, SEQPACKET credentials and listening UNIX sockets.
 */
#include <gtest/gtest.h>

#ifdef __linux__
  #include "compositor_identity_fixture.h"

  #include <atomic>
  #include <fstream>
  #include <initializer_list>
  #include <memory>
  #include <span>
  #include <vector>

namespace {
  using compositor_identity::exec_e;
  using compositor_identity_fixture::controlled_child_t;
  using compositor_identity_fixture::open_descriptor_count;
  using compositor_identity_fixture::open_pidfd;

  /** Send one hand-built message from this process, with the given descriptors. */
  bool send_raw(int fd, std::span<const unsigned char> bytes, std::initializer_list<int> descriptors) {
    iovec body {const_cast<unsigned char *>(bytes.data()), bytes.size()};
    std::vector<char> control(descriptors.size() ? CMSG_SPACE(sizeof(int) * descriptors.size()) : 0);
    msghdr message {};
    message.msg_iov = &body;
    message.msg_iovlen = 1;
    if (descriptors.size()) {
      message.msg_control = control.data();
      message.msg_controllen = control.size();
      auto *rights = CMSG_FIRSTHDR(&message);
      rights->cmsg_level = SOL_SOCKET;
      rights->cmsg_type = SCM_RIGHTS;
      rights->cmsg_len = CMSG_LEN(sizeof(int) * descriptors.size());
      std::memcpy(CMSG_DATA(rights), std::data(descriptors), sizeof(int) * descriptors.size());
    }
    return sendmsg(fd, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(bytes.size());
  }

  compositor_identity::payload_bytes_t payload_for(std::int64_t pid, std::uint64_t starttime,
                                                   std::uint32_t version = compositor_identity::k_payload_version,
                                                   std::uint32_t reserved = 0) {
    compositor_identity::payload_t payload;
    payload.version = version;
    payload.reserved = reserved;
    payload.pid = pid;
    payload.starttime = starttime;
    return compositor_identity::encode_payload(payload);
  }

  /// A child that has exec'd, but whose handoff the test sends by hand.
  controlled_child_t::options_t unsent() {
    controlled_child_t::options_t options;
    options.send = false;
    options.close_sender = false;
    return options;
  }

  /// Hand-send `bytes` with `descriptors` to a fresh child's channel and return the refusal reason.
  std::string refusal(controlled_child_t &child, std::span<const unsigned char> bytes,
                      std::initializer_list<int> descriptors) {
    EXPECT_TRUE(send_raw(child.sender.get(), bytes, descriptors));
    const auto result = child.receive();
    EXPECT_FALSE(result.identity);
    return std::string(result.reason);
  }
}  // namespace

TEST(CompositorIdentityChannelTests, PayloadIsSerializedFieldByFieldWithNoPadding) {
  const auto bytes = payload_for(0x0102030405060708, 0x1112131415161718ull, 1, 0);
  EXPECT_EQ(bytes.size(), 24u);
  std::uint32_t version = 0;
  std::int64_t pid = 0;
  std::memcpy(&version, bytes.data(), 4);
  std::memcpy(&pid, bytes.data() + 8, 8);
  EXPECT_EQ(version, 1u);
  EXPECT_EQ(pid, 0x0102030405060708);
  const auto decoded = compositor_identity::decode_payload(bytes);
  EXPECT_EQ(decoded.version, 1u);
  EXPECT_EQ(decoded.reserved, 0u);
  EXPECT_EQ(decoded.pid, 0x0102030405060708);
  EXPECT_EQ(decoded.starttime, 0x1112131415161718ull);
}

TEST(CompositorIdentityChannelTests, ProcParsersFailClosed) {
  using compositor_identity::parse_fdinfo_pid;
  using compositor_identity::parse_ppid;
  EXPECT_EQ(parse_fdinfo_pid("pos:\t0\nflags:\t02000002\nPid:\t4242\nNSpid:\t4242\n"), 4242);
  EXPECT_EQ(parse_fdinfo_pid("Pid:\t-1\n"), -1) << "an exited pidfd; callers compare, never trust";
  EXPECT_FALSE(parse_fdinfo_pid("pos:\t0\nflags:\t02\n")) << "not a pidfd";
  EXPECT_FALSE(parse_fdinfo_pid("Pid:\t12x\n"));
  EXPECT_FALSE(parse_fdinfo_pid("Pid:\t\n"));
  EXPECT_FALSE(parse_fdinfo_pid("Pid:\t1\nPid:\t2\n"));
  EXPECT_EQ(parse_ppid("77 (labwc) S 76 77 77 0"), 76);
  EXPECT_EQ(parse_ppid("77 (odd) name) (x) R 5 1"), 5) << "fields are read after the last ')'";
  EXPECT_FALSE(parse_ppid("77 labwc S 76"));
  EXPECT_FALSE(parse_ppid("77 (labwc) S"));
  EXPECT_FALSE(parse_ppid("77 (labwc) S x"));
  EXPECT_EQ(compositor_identity::read_ppid(getpid()), getppid());
  EXPECT_FALSE(compositor_identity::read_ppid(0));
  EXPECT_TRUE(compositor_identity::read_starttime(getpid()));
}

TEST(CompositorIdentityChannelTests, ProductionHandoffAcceptsExactlyTheExecdDirectChild) {
  const auto baseline = open_descriptor_count();
  {
    controlled_child_t child;
    ASSERT_GT(child.pid, 0);
    ASSERT_TRUE(child.sent);
    auto result = child.receive();
    ASSERT_TRUE(result.identity) << result.reason;
    EXPECT_EQ(result.reason, "identity_accepted");
    const auto &identity = *result.identity;
    EXPECT_EQ(identity.process.pid, child.pid);
    EXPECT_EQ(identity.process.uid, getuid());
    EXPECT_EQ(identity.process.starttime, compositor_identity::read_starttime(child.pid));
    EXPECT_EQ(compositor_identity::read_pidfd_pid(identity.pidfd.get()), child.pid);
    EXPECT_TRUE(compositor_identity::pidfd_alive(identity.pidfd.get()));
    EXPECT_NE(fcntl(identity.pidfd.get(), F_GETFD) & FD_CLOEXEC, 0) << "never inherited by later spawns";
    // The channel carries exactly one handoff.
    EXPECT_EQ(child.receive(std::chrono::milliseconds(50)).reason, "identity_channel_closed");
  }
  EXPECT_EQ(open_descriptor_count(), baseline);
}

TEST(CompositorIdentityChannelTests, ExecFailureDeathBeforeExecAndSilenceSendNothing) {
  const auto baseline = open_descriptor_count();
  {
    controlled_child_t::options_t options;
    options.program = "/nonexistent/polaris-identity-fixture";
    controlled_child_t child {options};
    ASSERT_GT(child.pid, 0);
    EXPECT_FALSE(child.sent);
    EXPECT_EQ(child.receive(std::chrono::milliseconds(200)).reason, "identity_channel_closed");
  }
  {
    auto options = unsent();
    options.program = "/nonexistent/polaris-identity-fixture";
    controlled_child_t child {options};
    EXPECT_EQ(child.exec_result, exec_e::failed) << "the reported errno is a failure";
  }
  {
    auto options = unsent();
    options.mode = controlled_child_t::mode_e::die_before_exec;
    controlled_child_t child {options};
    EXPECT_EQ(child.exec_result, exec_e::failed) << "EOF from a dead child is not an exec";
  }
  {
    controlled_child_t child {unsent()};
    EXPECT_EQ(child.exec_result, exec_e::succeeded);
    // Sender still open, nothing sent: bounded wait, then nothing.
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(child.receive(std::chrono::milliseconds(100)).reason, "identity_channel_timeout");
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(2));
  }
  EXPECT_EQ(open_descriptor_count(), baseline);
}

TEST(CompositorIdentityChannelTests, TheExecWaitIsBoundedAndHonoursStop) {
  const auto baseline = open_descriptor_count();
  controlled_child_t child {unsent()};  // a live, unreaped direct child to wait on
  ASSERT_GT(child.pid, 0);
  {
    // The writer stays open (a child that never execs nor exits): a finite deadline, not a hang.
    int status[2] {-1, -1};
    ASSERT_EQ(pipe2(status, O_CLOEXEC), 0);
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(compositor_identity::await_exec(child.pid, status[0], nullptr, std::chrono::milliseconds(150)),
              exec_e::timed_out);
    const auto waited = std::chrono::steady_clock::now() - begin;
    EXPECT_GE(waited, std::chrono::milliseconds(140));
    EXPECT_LT(waited, std::chrono::seconds(2));
    // And no handoff follows a timeout.
    EXPECT_FALSE(compositor_identity::send_after_exec(child.sender.get(), child.pid, status[0], nullptr,
                                                      std::chrono::milliseconds(50)));
    EXPECT_EQ(child.receive(std::chrono::milliseconds(50)).reason, "identity_channel_timeout");
    close(status[0]);
    close(status[1]);
  }
  {
    // A stop already requested, writer still open: interrupted at once, without waiting.
    int status[2] {-1, -1};
    ASSERT_EQ(pipe2(status, O_CLOEXEC), 0);
    volatile sig_atomic_t stop = 1;
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(compositor_identity::await_exec(child.pid, status[0], &stop, std::chrono::seconds(5)),
              exec_e::interrupted);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds(500));
    EXPECT_FALSE(compositor_identity::send_after_exec(child.sender.get(), child.pid, status[0], &stop));
    EXPECT_EQ(child.receive(std::chrono::milliseconds(50)).reason, "identity_channel_timeout");
    close(status[0]);
    close(status[1]);
  }
  {
    // The wait never reaps: the child is still ours to supervise afterwards.
    siginfo_t info {};
    ASSERT_EQ(waitid(P_PID, static_cast<id_t>(child.pid), &info, WEXITED | WNOHANG | WNOWAIT), 0);
    EXPECT_EQ(info.si_pid, 0) << "still running and unreaped";
  }
  EXPECT_EQ(open_descriptor_count(), baseline + 3) << "only the live child's own pidfd, receiver and sender";
}

namespace {
  /// A hand-built control buffer, as recvmsg would fill it, for the production ancillary parser.
  struct control_t {
    alignas(cmsghdr) std::array<char, 256> bytes {};
    msghdr message {};
    cmsghdr *next = nullptr;

    control_t() {
      message.msg_control = bytes.data();
      message.msg_controllen = bytes.size();
      next = CMSG_FIRSTHDR(&message);
    }

    void add(int level, int type, const void *data, std::size_t length) {
      next->cmsg_level = level;
      next->cmsg_type = type;
      next->cmsg_len = CMSG_LEN(length);
      std::memcpy(CMSG_DATA(next), data, length);
      const auto used = reinterpret_cast<char *>(next) - bytes.data() + CMSG_SPACE(length);
      message.msg_controllen = used;
      next = reinterpret_cast<cmsghdr *>(bytes.data() + used);
    }
  };

  bool is_closed(int fd) {
    return fcntl(fd, F_GETFD) < 0 && errno == EBADF;
  }

  /// A descriptor this test owns until it is handed to the parser as "received".
  int received_descriptor() {
    return open("/dev/null", O_RDONLY | O_CLOEXEC);
  }
}  // namespace

TEST(CompositorIdentityChannelTests, UnexpectedAncillaryDataIsRefusedAndEveryDescriptorClosed) {
  const ucred credentials {getpid(), getuid(), getgid()};
  {
    control_t control;
    const int fd = received_descriptor();
    control.add(SOL_SOCKET, SCM_CREDENTIALS, &credentials, sizeof(credentials));
    control.add(SOL_SOCKET, SCM_RIGHTS, &fd, sizeof(fd));
    {
      const auto taken = compositor_identity::take_ancillary(control.message);
      EXPECT_FALSE(taken.unexpected);
      EXPECT_EQ(taken.credential_messages, 1u);
      ASSERT_EQ(taken.descriptors.size(), 1u);
      EXPECT_EQ(taken.descriptors.front().get(), fd);
    }
    EXPECT_TRUE(is_closed(fd)) << "owned by the parse result";
  }
  const auto refused = [&](auto &&build) {
    control_t control;
    const int fd = received_descriptor();
    control.add(SOL_SOCKET, SCM_RIGHTS, &fd, sizeof(fd));
    build(control);
    {
      const auto taken = compositor_identity::take_ancillary(control.message);
      EXPECT_TRUE(taken.unexpected);
    }
    EXPECT_TRUE(is_closed(fd)) << "closed although the message was refused";
  };
  // Unknown type at the socket level.
  refused([&](control_t &control) { control.add(SOL_SOCKET, 0x7f, &credentials, sizeof(credentials)); });
  // A known type at another, unrelated level.
  constexpr int k_other_level = 0x7ffe;
  static_assert(k_other_level != SOL_SOCKET);
  refused([&](control_t &control) { control.add(k_other_level, SCM_CREDENTIALS, &credentials, sizeof(credentials)); });
  // A credential that is too short, and one too long.
  refused([&](control_t &control) { control.add(SOL_SOCKET, SCM_CREDENTIALS, &credentials, sizeof(credentials) - 4); });
  refused([&](control_t &control) {
    std::array<char, sizeof(ucred) + 4> longer {};
    std::memcpy(longer.data(), &credentials, sizeof(credentials));
    control.add(SOL_SOCKET, SCM_CREDENTIALS, longer.data(), longer.size());
  });
  // Rights that are not a whole number of descriptors.
  refused([&](control_t &control) {
    std::array<unsigned char, 6> ragged {};
    ragged.fill(0xff);  // the whole int reads as -1, so nothing real is taken from it
    control.add(SOL_SOCKET, SCM_RIGHTS, ragged.data(), ragged.size());
  });
  {
    // A descriptor the kernel installs outside SCM_RIGHTS (SCM_PIDFD) is refused and still closed.
    control_t control;
    const int fd = received_descriptor();
    control.add(SOL_SOCKET, 0x04, &fd, sizeof(fd));
    {
      const auto taken = compositor_identity::take_ancillary(control.message);
      EXPECT_TRUE(taken.unexpected);
      EXPECT_TRUE(taken.descriptors.empty()) << "never counted as the handoff's descriptor";
    }
    EXPECT_TRUE(is_closed(fd));
  }
}

TEST(CompositorIdentityChannelTests, AKernelDeliveredPidfdControlMessageIsRefusedWithoutLeaks) {
  #ifndef SO_PASSPIDFD
    constexpr int SO_PASSPIDFD = 76;
  #endif
  controlled_child_t child;
  ASSERT_TRUE(child.sent);
  const int enabled = 1;
  if (setsockopt(child.receiver.get(), SOL_SOCKET, SO_PASSPIDFD, &enabled, sizeof(enabled)) != 0) {
    GTEST_SKIP() << "kernel without SO_PASSPIDFD (Linux < 6.5)";
  }
  // The kernel attaches SCM_PIDFD at receive time, so the already-queued handoff now carries one.
  const auto baseline = open_descriptor_count();
  {
    const auto result = child.receive();
    EXPECT_FALSE(result.identity);
    EXPECT_EQ(result.reason, "identity_ancillary_unexpected");
  }
  EXPECT_EQ(open_descriptor_count(), baseline) << "both the handed-off pidfd and the kernel's SCM_PIDFD are closed";
}


TEST(CompositorIdentityChannelTests, MalformedMessagesAreRefusedAndEveryDescriptorClosed) {
  const auto baseline = open_descriptor_count();
  {
    controlled_child_t child {unsent()};
    const auto start = *compositor_identity::read_starttime(child.pid);
    const auto good = payload_for(child.pid, start);
    std::vector<unsigned char> shorter(good.begin(), good.end() - 1);
    EXPECT_EQ(refusal(child, shorter, {child.pidfd.get()}), "identity_payload_size");
  }
  {
    controlled_child_t child {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    std::vector<unsigned char> longer(good.begin(), good.end());
    longer.push_back(0);
    EXPECT_EQ(refusal(child, longer, {child.pidfd.get()}), "identity_payload_size");
  }
  {
    controlled_child_t child {unsent()};
    std::vector<unsigned char> oversized(64, 0);
    EXPECT_EQ(refusal(child, oversized, {child.pidfd.get()}), "identity_message_truncated");
  }
  {
    controlled_child_t child {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    EXPECT_EQ(refusal(child, good, {}), "identity_descriptor_count");
  }
  {
    controlled_child_t child {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    EXPECT_EQ(refusal(child, good, {child.pidfd.get(), child.pidfd.get()}), "identity_descriptor_count");
  }
  {
    // More descriptors than the receiver has room for: the kernel truncates, we refuse.
    controlled_child_t child {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    const int p = child.pidfd.get();
    EXPECT_EQ(refusal(child, good, {p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p, p}),
              "identity_message_truncated");
  }
  {
    controlled_child_t child {unsent()};
    int pipe_fds[2];
    ASSERT_EQ(pipe2(pipe_fds, O_CLOEXEC), 0);
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    EXPECT_EQ(refusal(child, good, {pipe_fds[0]}), "identity_descriptor_not_pidfd");
    close(pipe_fds[0]);
    close(pipe_fds[1]);
  }
  {
    controlled_child_t child {unsent()};
    const auto start = *compositor_identity::read_starttime(child.pid);
    EXPECT_EQ(refusal(child, payload_for(child.pid, start, 2), {child.pidfd.get()}), "identity_payload_version");
  }
  {
    controlled_child_t child {unsent()};
    const auto start = *compositor_identity::read_starttime(child.pid);
    EXPECT_EQ(refusal(child, payload_for(child.pid, start, 1, 1), {child.pidfd.get()}), "identity_payload_version");
  }
  {
    controlled_child_t child {unsent()};
    EXPECT_EQ(refusal(child, payload_for(1, 5), {child.pidfd.get()}), "identity_payload_invalid");
  }
  {
    controlled_child_t child {unsent()};
    EXPECT_EQ(refusal(child, payload_for(child.pid, 0), {child.pidfd.get()}), "identity_payload_invalid");
  }
  {
    controlled_child_t child {unsent()};
    const auto start = *compositor_identity::read_starttime(child.pid);
    EXPECT_EQ(refusal(child, payload_for(child.pid, start + 1), {child.pidfd.get()}), "identity_starttime_mismatch");
  }
  EXPECT_EQ(open_descriptor_count(), baseline);
}

TEST(CompositorIdentityChannelTests, TheDescriptorMustBeTheNamedLiveDirectChild) {
  const auto baseline = open_descriptor_count();
  {
    // A pidfd for one child claiming to be another.
    controlled_child_t child {unsent()};
    controlled_child_t other {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    EXPECT_EQ(refusal(child, good, {other.pidfd.get()}), "identity_pidfd_mismatch");
  }
  {
    // A live process that is not the supervisor's direct child: this test's own parent.
    controlled_child_t child {unsent()};
    const auto parent = getppid();
    compositor_identity::unique_fd_t parent_pidfd {open_pidfd(parent)};
    ASSERT_TRUE(parent_pidfd);
    const auto start = compositor_identity::read_starttime(parent);
    ASSERT_TRUE(start);
    EXPECT_EQ(refusal(child, payload_for(parent, *start), {parent_pidfd.get()}), "identity_not_supervisor_child");
  }
  {
    // The compositor exited (not yet reaped) before the host looked.
    controlled_child_t child;
    ASSERT_TRUE(child.sent);
    ASSERT_EQ(kill(child.pid, SIGKILL), 0);
    ASSERT_TRUE(compositor_identity_fixture::wait_for_exit(child.pidfd.get()));
    EXPECT_EQ(child.receive().reason, "identity_compositor_exited");
  }
  EXPECT_EQ(open_descriptor_count(), baseline);
}

TEST(CompositorIdentityChannelTests, SenderCredentialsMustBeTheTrackedSupervisorAndThisUser) {
  const auto baseline = open_descriptor_count();
  {
    // A genuine handoff, but the host tracks a different supervisor.
    controlled_child_t child;
    ASSERT_TRUE(child.sent);
    const auto result = compositor_identity::receive(child.receiver.get(), child.pid, getuid(), std::chrono::seconds(2));
    EXPECT_FALSE(result.identity);
    EXPECT_EQ(result.reason, "identity_sender_mismatch");
  }
  {
    controlled_child_t child;
    ASSERT_TRUE(child.sent);
    const auto result = compositor_identity::receive(child.receiver.get(), getpid(), getuid() + 1, std::chrono::seconds(2));
    EXPECT_FALSE(result.identity);
    EXPECT_EQ(result.reason, "identity_sender_mismatch");
  }
  {
    // Another process (a grandchild of nobody we track) sends an otherwise valid handoff.
    controlled_child_t child {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    const pid_t impostor = fork();
    if (impostor == 0) {
      iovec body {const_cast<unsigned char *>(good.data()), good.size()};
      alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] {};
      msghdr message {};
      message.msg_iov = &body;
      message.msg_iovlen = 1;
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      auto *rights = CMSG_FIRSTHDR(&message);
      rights->cmsg_level = SOL_SOCKET;
      rights->cmsg_type = SCM_RIGHTS;
      rights->cmsg_len = CMSG_LEN(sizeof(int));
      const int fd = child.pidfd.get();
      std::memcpy(CMSG_DATA(rights), &fd, sizeof(int));
      _exit(sendmsg(child.sender.get(), &message, MSG_NOSIGNAL) == static_cast<ssize_t>(good.size()) ? 0 : 1);
    }
    ASSERT_GT(impostor, 0);
    int status = 0;
    ASSERT_EQ(waitpid(impostor, &status, 0), impostor);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    const auto result = child.receive();
    EXPECT_FALSE(result.identity);
    EXPECT_EQ(result.reason, "identity_sender_mismatch");
  }
  {
    // Without SO_PASSCRED no credentials arrive, so nothing can be attributed.
    int channel[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel), 0);
    controlled_child_t child {unsent()};
    const auto good = payload_for(child.pid, *compositor_identity::read_starttime(child.pid));
    ASSERT_TRUE(send_raw(channel[1], good, {child.pidfd.get()}));
    const auto result = compositor_identity::receive(channel[0], getpid(), getuid(), std::chrono::seconds(2));
    EXPECT_EQ(result.reason, "identity_sender_mismatch");
    close(channel[0]);
    close(channel[1]);
  }
  EXPECT_EQ(compositor_identity::receive(-1, getpid(), getuid(), std::chrono::milliseconds(1)).reason,
            "identity_channel_unavailable");
  EXPECT_EQ(compositor_identity::receive(0, 1, getuid(), std::chrono::milliseconds(1)).reason,
            "identity_channel_unavailable");
  EXPECT_EQ(open_descriptor_count(), baseline);
}

TEST(CompositorIdentityChannelTests, PinnedLeaseIsCurrentOnlyWhilePublishedLiveAndUnreplaced) {
  const auto baseline = open_descriptor_count();
  {
    compositor_identity_fixture::scratch_dir_t scratch;
    const auto socket_path = (scratch.path / "wayland-1").string();
    controlled_child_t::options_t options;
    options.listen_path = socket_path;
    controlled_child_t child {options};
    auto received = child.receive();
    ASSERT_TRUE(received.identity) << received.reason;
    compositor_identity::unique_fd_t supervisor {open_pidfd(getpid())};
    auto published = std::make_shared<std::atomic<bool>>(true);
    const vrr_clock::compositor_lease_t *seen = nullptr;
    auto lease = compositor_identity::make_pinned_lease(
      std::move(*received.identity), supervisor.get(), socket_path,
      [published, &seen](const vrr_clock::compositor_lease_t &self) {
        seen = &self;
        return published->load();
      }
    );
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->process.pid, child.pid);
    EXPECT_TRUE(lease->current());
    EXPECT_EQ(seen, lease.get()) << "the publication check is asked about this exact lease";

    published->store(false);
    EXPECT_FALSE(lease->current()) << "retired publication";
    published->store(true);
    EXPECT_TRUE(lease->current());

    // The name now points at a different socket object.
    const auto saved = socket_path + ".saved";
    ASSERT_EQ(rename(socket_path.c_str(), saved.c_str()), 0);
    const int impostor = compositor_identity_fixture::listen_inheritable(socket_path);
    ASSERT_GE(impostor, 0);
    EXPECT_FALSE(lease->current()) << "replaced socket";
    close(impostor);
    ASSERT_EQ(unlink(socket_path.c_str()), 0);
    ASSERT_EQ(rename(saved.c_str(), socket_path.c_str()), 0);
    EXPECT_FALSE(lease->current()) << "an observed replacement revokes for good; restoring the name cannot reauthorize";
    published->store(true);
    EXPECT_FALSE(lease->current());

  }
  {
    // Exit is observed, and latched, the same way.
    compositor_identity_fixture::scratch_dir_t scratch;
    const auto socket_path = (scratch.path / "wayland-4").string();
    controlled_child_t::options_t options;
    options.listen_path = socket_path;
    controlled_child_t child {options};
    auto received = child.receive();
    ASSERT_TRUE(received.identity) << received.reason;
    compositor_identity::unique_fd_t supervisor {open_pidfd(getpid())};
    auto lease = compositor_identity::make_pinned_lease(
      std::move(*received.identity), supervisor.get(), socket_path,
      [](const vrr_clock::compositor_lease_t &) { return true; }
    );
    ASSERT_TRUE(lease);
    EXPECT_TRUE(lease->current());
    ASSERT_EQ(kill(child.pid, SIGKILL), 0);
    ASSERT_TRUE(compositor_identity_fixture::wait_for_exit(child.pidfd.get()));
    EXPECT_FALSE(lease->current()) << "exited compositor";
  }
  {
    // Removing the name (an unavailable endpoint) also revokes, and a later rebind at the same path
    // by anyone cannot restore it.
    compositor_identity_fixture::scratch_dir_t scratch;
    const auto socket_path = (scratch.path / "wayland-5").string();
    controlled_child_t::options_t options;
    options.listen_path = socket_path;
    controlled_child_t child {options};
    auto received = child.receive();
    ASSERT_TRUE(received.identity) << received.reason;
    compositor_identity::unique_fd_t supervisor {open_pidfd(getpid())};
    auto lease = compositor_identity::make_pinned_lease(
      std::move(*received.identity), supervisor.get(), socket_path,
      [](const vrr_clock::compositor_lease_t &) { return true; }
    );
    ASSERT_TRUE(lease);
    const auto saved = socket_path + ".saved";
    ASSERT_EQ(rename(socket_path.c_str(), saved.c_str()), 0);
    EXPECT_FALSE(lease->current()) << "unavailable endpoint";
    ASSERT_EQ(rename(saved.c_str(), socket_path.c_str()), 0);
    EXPECT_FALSE(lease->current()) << "the same object back under its name is still not authority";
  }
  EXPECT_EQ(open_descriptor_count(), baseline) << "a destroyed lease closes every descriptor it pinned";
}

TEST(CompositorIdentityChannelTests, PinnedLeaseNeedsALiveSupervisorAndARealSocket) {
  const auto baseline = open_descriptor_count();
  {
    compositor_identity_fixture::scratch_dir_t scratch;
    const auto socket_path = (scratch.path / "wayland-2").string();
    controlled_child_t::options_t options;
    options.listen_path = socket_path;
    controlled_child_t child {options};
    controlled_child_t stand_in_supervisor;
    auto received = child.receive();
    ASSERT_TRUE(received.identity) << received.reason;
    auto lease = compositor_identity::make_pinned_lease(
      std::move(*received.identity), stand_in_supervisor.pidfd.get(), socket_path,
      [](const vrr_clock::compositor_lease_t &) { return true; }
    );
    ASSERT_TRUE(lease);
    EXPECT_TRUE(lease->current());
    ASSERT_EQ(kill(stand_in_supervisor.pid, SIGKILL), 0);
    ASSERT_TRUE(compositor_identity_fixture::wait_for_exit(stand_in_supervisor.pidfd.get()));
    EXPECT_FALSE(lease->current()) << "exited supervisor";
  }
  {
    compositor_identity_fixture::scratch_dir_t scratch;
    const auto file_path = (scratch.path / "wayland-3").string();
    { std::ofstream file(file_path); file << "not a socket"; }
    controlled_child_t child;
    auto received = child.receive();
    ASSERT_TRUE(received.identity) << received.reason;
    compositor_identity::unique_fd_t supervisor {open_pidfd(getpid())};
    EXPECT_FALSE(compositor_identity::make_pinned_lease(
      std::move(*received.identity), supervisor.get(), file_path,
      [](const vrr_clock::compositor_lease_t &) { return true; }
    ));
  }
  {
    controlled_child_t child;
    auto received = child.receive();
    ASSERT_TRUE(received.identity) << received.reason;
    compositor_identity::unique_fd_t supervisor {open_pidfd(getpid())};
    EXPECT_FALSE(compositor_identity::make_pinned_lease(
      std::move(*received.identity), supervisor.get(), "/nonexistent/polaris-identity/wayland-9",
      [](const vrr_clock::compositor_lease_t &) { return true; }
    ));
    EXPECT_FALSE(compositor_identity::make_pinned_lease(
      compositor_identity::received_t {}, -1, "/nonexistent", [](const vrr_clock::compositor_lease_t &) { return true; }
    ));
  }
  EXPECT_EQ(open_descriptor_count(), baseline);
}

#else
TEST(CompositorIdentityChannelTests, LinuxOnly) {
  GTEST_SKIP() << "the pidfd handoff is Linux-only";
}
#endif
