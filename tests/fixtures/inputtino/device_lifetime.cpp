#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <linux/uhid.h>
#include <memory>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <inputtino/result.hpp>

namespace fixture {
std::mutex mutex;
std::condition_variable changed;
int peer = -1;
std::atomic<unsigned> closes{0};
std::atomic<bool> callback_running{false};
std::atomic<bool> early_close{false};
bool reject_create = false, release_callback = false;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int open(const char *, int) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return -1;
  peer = pair[1];
  return pair[0];
}
ssize_t write(int, const void *, size_t bytes) {
  if (reject_create) { errno = EIO; return -1; }
  return bytes;
}
int close(int fd) {
  if (callback_running) early_close = true;
  ++closes;
  return ::close(fd);
}
struct ThrowOnCopy {
  std::shared_ptr<bool> armed;
  explicit ThrowOnCopy(std::shared_ptr<bool> value) : armed(std::move(value)) {}
  ThrowOnCopy(const ThrowOnCopy &value) : armed(value.armed) {
    if (*armed) throw std::bad_alloc();
  }
  void operator()(const uhid_event &, int) const {}
};
}
// Compile the actual UHID owner, replacing only open/write/close. Its real poll,
// read, thread and stop/join code runs against a private socket pair.
#define open fixture::open
#define write fixture::write
#define close fixture::close
#include POLARIS_TEST_UHID_SOURCE
#undef open
#undef write
#undef close

using namespace std::chrono_literals;
int main() {
  using namespace fixture;
  try {
    const uhid::DeviceDefinition definition{};
    reject_create = true;
    require(!uhid::Device::create(definition, [](const uhid_event &, int) {}), "create write failure was ignored");
    require(closes == 1, "failed creation leaked descriptor");
    ::close(peer);
    reject_create = false;
    auto armed = std::make_shared<bool>(false);
    const std::function<void(const uhid_event &, int)> throwing = ThrowOnCopy(armed);
    *armed = true;
    bool caught = false;
    try { auto unused = uhid::Device::create(definition, throwing); }
    catch (const std::bad_alloc &) { caught = true; }
    require(caught && closes == 2, "exception before reader ownership leaked descriptor");
    ::close(peer);
    {
      auto result = uhid::Device::create(definition, [](const uhid_event &, int) {
        std::unique_lock lock(mutex);
        callback_running = true;
        changed.notify_all();
        require(changed.wait_for(lock, 3s, [] { return release_callback; }), "callback timed out");
        callback_running = false;
      });
      require(bool(result), "private transport create failed");
      auto owned = std::make_unique<uhid::Device>(std::move(*result));
      const uhid_event event{};
      require(::write(peer, &event, sizeof event) == sizeof event, "private event injection failed");
      {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, 3s, [] { return callback_running.load(); }), "reader callback not entered");
      }
      std::atomic<bool> started{false}, destroyed{false};
      std::jthread destroy([device = std::move(owned), &started, &destroyed]() mutable {
        started = true;
        changed.notify_all();
        device.reset();
        destroyed = true;
        changed.notify_all();
      });
      bool premature;
      {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, 1s, [&] { return started.load(); });
        changed.wait_for(lock, 100ms, [&] { return destroyed.load(); });
        premature = destroyed;
        release_callback = true;
        changed.notify_all();
      }
      destroy.join();
      require(!premature && !early_close, "device destroyed while callback was active");
      require(closes == 3 && destroyed, "joined teardown did not close descriptor");
    }
    ::close(peer);
    std::cout << "PASS: real UHID reader move, in-flight callback join and creation failure cleanup\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
