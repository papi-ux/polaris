/**
 * @file tests/unit/platform/test_ei_virtual_input.cpp
 * @brief The gamescope EIS route against a socket that is missing, or answers and then says nothing.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_EI_VIRTUAL_INPUT)
  #include <cerrno>
  #include <chrono>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <string>
  #include <thread>

  #include <sys/resource.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>

  #include "src/config.h"
  #include "src/platform/linux/input/inputtino_ei_virtual_input.h"

namespace {
  using namespace std::chrono_literals;

  /// Just past the route's one second wait between connection attempts.
  constexpr auto k_past_the_reconnect_wait = 1100ms;
  /// Just past the two seconds a connection may go without offering a device.
  constexpr auto k_past_the_handshake_deadline = 2200ms;

  std::chrono::microseconds process_cpu_time() {
    rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    const auto micros = [](const timeval &value) {
      return std::chrono::seconds {value.tv_sec} + std::chrono::microseconds {value.tv_usec};
    };
    return micros(usage.ru_utime) + micros(usage.ru_stime);
  }

  class EiVirtualInputTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_stream_mode = config::video.linux_display.stream_mode;
      saved_private_runtime = config::video.linux_display.private_runtime;
      config::video.linux_display.stream_mode = "gamescope_stream";

      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      directory = std::filesystem::temp_directory_path() /
                  ("polaris-ei-" + std::to_string(getpid()) + "-" + info->name());
      std::filesystem::remove_all(directory);
      std::filesystem::create_directories(directory);
      socket_path = directory / "ei";
      ASSERT_LT(socket_path.native().size(), sizeof(sockaddr_un::sun_path));
      // An absolute LIBEI_SOCKET is what the route connects to, ahead of the
      // name it would derive from gamescope's display.
      setenv("LIBEI_SOCKET", socket_path.c_str(), 1);
    }

    void TearDown() override {
      if (listener >= 0) {
        close(listener);
      }
      unsetenv("LIBEI_SOCKET");
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
      config::video.linux_display.stream_mode = saved_stream_mode;
      config::video.linux_display.private_runtime = saved_private_runtime;
    }

    /// A listening socket nobody accepts on. connect() succeeds from the
    /// backlog and no EIS handshake ever comes back, which is what a wedged
    /// compositor looks like from here.
    void listen_without_answering() {
      listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ASSERT_GE(listener, 0);
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
      ASSERT_EQ(0, bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address))) << strerror(errno);
      ASSERT_EQ(0, listen(listener, 8)) << strerror(errno);
    }

    std::filesystem::path directory;
    std::filesystem::path socket_path;
    int listener = -1;
    std::string saved_stream_mode;
    std::string saved_private_runtime;
  };
}  // namespace

TEST_F(EiVirtualInputTests, AMissingSocketLeavesInputWithUinput) {
  platf::ei_virtual_input_t input;

  EXPECT_FALSE(input.move(1, 1));
  EXPECT_FALSE(input.should_block_host_fallback())
    << "a host whose gamescope never started must keep its mouse and keyboard";
}

TEST_F(EiVirtualInputTests, AStalledSocketDoesNotTakeInputBack) {
  listen_without_answering();
  platf::ei_virtual_input_t input;

  EXPECT_FALSE(input.move(1, 1));
  EXPECT_TRUE(input.should_block_host_fallback())
    << "a first connection still in its handshake keeps the event off the host desktop";

  std::this_thread::sleep_for(k_past_the_handshake_deadline);
  EXPECT_FALSE(input.move(1, 1));
  EXPECT_FALSE(input.should_block_host_fallback())
    << "a connection that never offered a device must hand input back to uinput";

  // The reconnect wait has passed, and the socket still answers. Retrying is
  // fine; swallowing input again for the length of every retry is not.
  std::this_thread::sleep_for(k_past_the_reconnect_wait);
  EXPECT_FALSE(input.move(1, 1));
  EXPECT_FALSE(input.should_block_host_fallback())
    << "a stalled socket took input back on the next retry";
}

TEST_F(EiVirtualInputTests, ASocketThatComesBackClaimsNothingUntilADeviceEmulates) {
  platf::ei_virtual_input_t input;
  EXPECT_FALSE(input.move(1, 1));
  ASSERT_FALSE(input.should_block_host_fallback());

  listen_without_answering();
  std::this_thread::sleep_for(k_past_the_reconnect_wait);

  EXPECT_FALSE(input.move(1, 1));
  EXPECT_FALSE(input.should_block_host_fallback())
    << "after a failure, a socket that merely answers must not take input back";
}

TEST_F(EiVirtualInputTests, AWakeByteLeftByResetDoesNotSpinTheNextReader) {
  platf::ei_virtual_input_t input;
  // The first attempt opens the wake pipe and fails to connect, so no reader
  // runs. reset() then writes a wake byte with nobody to drain it afterwards.
  EXPECT_FALSE(input.move(1, 1));
  input.reset();

  listen_without_answering();
  std::this_thread::sleep_for(k_past_the_reconnect_wait);
  EXPECT_FALSE(input.move(1, 1));  // Connects, and starts a reader.

  constexpr auto window = 400ms;
  const auto before = process_cpu_time();
  std::this_thread::sleep_for(window);
  const auto spent = process_cpu_time() - before;

  // This thread slept and the reader has nothing to read. A reader that finds
  // the stale byte readable on every poll() burns the whole window instead.
  EXPECT_LT(spent, window / 2) << "the reader spun for "
                               << std::chrono::duration_cast<std::chrono::milliseconds>(spent).count() << " ms of CPU";
}
#endif
