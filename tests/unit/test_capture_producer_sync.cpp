#include "../tests_common.h"

#ifdef __linux__
  #include <array>
  #include <cerrno>
  #include <fcntl.h>
  #include <unistd.h>
  #include <vector>
  #include "src/platform/linux/capture_producer_sync.h"

namespace {
  using namespace std::chrono_literals;
  using capture_producer_sync::status_e;

  struct wait_fixture_t {
    std::array<int, 2> first {-1, -1};
    std::array<int, 2> second {-1, -1};
    std::array<int, 4> capture {-1, -1, -1, -1};
    std::vector<int> exported;
    std::vector<int> closed;
    std::vector<int> timeouts;
    int exports = 0;
    int export_failure_at = 0;
    int export_error = ENOMEM;
    bool interrupt_export_once = false;
    bool interrupt_poll_to_deadline = false;
    bool advance_after_first_poll = false;
    bool fence_error = false;
    std::chrono::milliseconds elapsed {0};

    wait_fixture_t() {
      EXPECT_EQ(pipe2(first.data(), O_CLOEXEC | O_NONBLOCK), 0);
      EXPECT_EQ(pipe2(second.data(), O_CLOEXEC | O_NONBLOCK), 0);
      capture[0] = first[0];
    }

    ~wait_fixture_t() {
      for (const auto fd : {first[0], first[1], second[0], second[1]}) {
        if (fd >= 0) {
          close(fd);
        }
      }
    }

    void ready(int writer) {
      const char byte = 1;
      ASSERT_EQ(write(writer, &byte, 1), 1);
    }

    void expect_capture_owned_and_fences_closed() const {
      for (const auto fd : capture) {
        if (fd >= 0) {
          EXPECT_NE(fcntl(fd, F_GETFD), -1);
        }
      }
      EXPECT_EQ(closed.size(), exported.size());
      for (const auto fd : exported) {
        EXPECT_EQ(fcntl(fd, F_GETFD), -1);
        EXPECT_EQ(errno, EBADF);
      }
    }

    capture_producer_sync::detail::syscalls_t calls {
      this,
      [](void *context, int source, int &fence) {
        auto &self = *static_cast<wait_fixture_t *>(context);
        ++self.exports;
        if (self.interrupt_export_once && self.exports == 1) {
          errno = EINTR;
          return -1;
        }
        if (self.exports == self.export_failure_at) {
          errno = self.export_error;
          return -1;
        }
        fence = fcntl(source, F_DUPFD_CLOEXEC, 0);
        if (fence < 0) {
          return -1;
        }
        self.exported.push_back(fence);
        return 0;
      },
      [](void *context, pollfd *fds, nfds_t count, int timeout) {
        auto &self = *static_cast<wait_fixture_t *>(context);
        self.timeouts.push_back(timeout);
        if (self.interrupt_poll_to_deadline) {
          self.elapsed += 550ms;
          errno = EINTR;
          return -1;
        }
        if (self.fence_error) {
          fds[0].revents = POLLIN | POLLERR;
          return 1;
        }
        if (self.advance_after_first_poll) {
          if (self.timeouts.size() > 1) {
            self.elapsed += std::chrono::milliseconds {timeout};
            return 0;
          }
          self.elapsed += 100ms;
        }
        return poll(fds, count, 0);
      },
      [](void *context, int fd) {
        auto &self = *static_cast<wait_fixture_t *>(context);
        self.closed.push_back(fd);
        (void) close(fd);
      },
      [](void *context) {
        return std::chrono::steady_clock::time_point {} + static_cast<wait_fixture_t *>(context)->elapsed;
      },
    };
  };
}

TEST(CaptureProducerSyncTests, ReadyWriterFenceNeedsNoWaitAndPreservesCaptureOwnership) {
  wait_fixture_t fixture;
  fixture.ready(fixture.first[1]);
  const auto result = capture_producer_sync::wait_for_tests(fixture.capture, 0ms, fixture.calls);
  EXPECT_EQ(result.status, status_e::ready);
  EXPECT_EQ(result.error, 0);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, EveryPlaneMustBeReady) {
  wait_fixture_t fixture;
  fixture.capture[1] = fixture.second[0];
  fixture.ready(fixture.first[1]);
  const auto result = capture_producer_sync::wait_for_tests(fixture.capture, 0ms, fixture.calls);
  EXPECT_EQ(result.status, status_e::timeout);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, AllReadyPlanesCloseOnlyTheirExportedFences) {
  wait_fixture_t fixture;
  fixture.capture[1] = fixture.second[0];
  fixture.ready(fixture.first[1]);
  fixture.ready(fixture.second[1]);
  EXPECT_EQ(capture_producer_sync::wait_for_tests(fixture.capture, 0ms, fixture.calls).status, status_e::ready);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, LaterExportFailureClosesEarlierFences) {
  wait_fixture_t fixture;
  fixture.capture[1] = fixture.second[0];
  fixture.export_failure_at = 2;
  const auto result = capture_producer_sync::wait_for_tests(fixture.capture, 500ms, fixture.calls);
  EXPECT_EQ(result.status, status_e::failed);
  EXPECT_EQ(result.error, ENOMEM);
  EXPECT_EQ(fixture.exported.size(), 1);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, UnsupportedExportIsNotReportedReady) {
  wait_fixture_t fixture;
  fixture.export_failure_at = 1;
  fixture.export_error = ENOTTY;
  const auto result = capture_producer_sync::wait_for_tests(fixture.capture, 500ms, fixture.calls);
  EXPECT_EQ(result.status, status_e::unsupported);
  EXPECT_TRUE(fixture.timeouts.empty());
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, InterruptedExportCanFinishBeforeTheDeadline) {
  wait_fixture_t fixture;
  fixture.interrupt_export_once = true;
  fixture.ready(fixture.first[1]);
  EXPECT_EQ(capture_producer_sync::wait_for_tests(fixture.capture, 500ms, fixture.calls).status, status_e::ready);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, InterruptedPollDoesNotRestartTheDeadline) {
  wait_fixture_t fixture;
  fixture.interrupt_poll_to_deadline = true;
  const auto result = capture_producer_sync::wait_for_tests(fixture.capture, 500ms, fixture.calls);
  EXPECT_EQ(result.status, status_e::timeout);
  EXPECT_EQ(fixture.timeouts.size(), 1);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, LaterPlanesShareTheOriginalWaitBudget) {
  wait_fixture_t fixture;
  fixture.capture[1] = fixture.second[0];
  fixture.ready(fixture.first[1]);
  fixture.advance_after_first_poll = true;
  EXPECT_EQ(capture_producer_sync::wait_for_tests(fixture.capture, 500ms, fixture.calls).status, status_e::timeout);
  ASSERT_EQ(fixture.timeouts.size(), 2);
  EXPECT_LE(fixture.timeouts.back(), 400);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, SignaledFenceWithAProducerErrorIsRefused) {
  wait_fixture_t fixture;
  fixture.fence_error = true;
  const auto result = capture_producer_sync::wait_for_tests(fixture.capture, 500ms, fixture.calls);
  EXPECT_EQ(result.status, status_e::failed);
  EXPECT_EQ(result.error, EIO);
  fixture.expect_capture_owned_and_fences_closed();
}

TEST(CaptureProducerSyncTests, RealNonDmabufDescriptorHasNoSupportedProducerFence) {
  const auto fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  const std::array<int, 1> fds {fd};
  const auto result = capture_producer_sync::wait(fds, 0ms);
  EXPECT_EQ(result.status, status_e::unsupported);
  EXPECT_NE(fcntl(fd, F_GETFD), -1);
  close(fd);
}
#endif
