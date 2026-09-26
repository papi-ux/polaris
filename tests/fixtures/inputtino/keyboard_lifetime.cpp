#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include "uinput_boundary.hpp"

namespace fixture {
using namespace std::chrono_literals;
std::mutex mutex;
std::condition_variable changed;
const auto main_thread = std::this_thread::get_id();
bool gate_repeat = false, repeat_entered = false, release_repeat = false;
unsigned created = 0, destroyed = 0, releases = 0, repeats = 0;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int write_event(const libevdev_uinput *, unsigned type, unsigned code, int value) {
  std::unique_lock lock{mutex};
  if (type == EV_KEY && code == KEY_A) {
    if (value == 0) ++releases;
    if (value == 1 && gate_repeat && std::this_thread::get_id() != main_thread) {
      ++repeats;
      repeat_entered = true;
      changed.notify_all();
      changed.wait(lock, [] { return release_repeat; });
    }
  }
  return 0;
}
void await_repeat() {
  std::unique_lock lock{mutex};
  require(changed.wait_for(lock, 2s, [] { return repeat_entered; }), "repeat did not reach the transport");
}
void unblock_repeat() {
  std::lock_guard lock{mutex};
  release_repeat = true;
  changed.notify_all();
}
void await_destroyed() {
  std::unique_lock lock{mutex};
  require(changed.wait_for(lock, 2s, [] { return destroyed == created; }), "keyboard device survived teardown");
}
}

// Run the actual pinned implementation. Only uinput creation and event writes
// are replaced; the repeat thread, key state, moves, and destruction are real.
#define libevdev_uinput_write_event fixture::write_event
#include POLARIS_TEST_KEYBOARD_SOURCE
#undef libevdev_uinput_write_event
#undef open
#undef ioctl
#undef close
#undef libevdev_uinput_create_from_device
#undef libevdev_uinput_destroy

int inputtino_test_open(const char *, int, ...) { throw std::runtime_error("unexpected device open"); }
int inputtino_test_ioctl(int, unsigned long, ...) { throw std::runtime_error("unexpected ioctl"); }
int inputtino_test_close(int) { throw std::runtime_error("unexpected close"); }
int inputtino_test_create(const libevdev *, int fd, libevdev_uinput **output) {
  fixture::require(fd == LIBEVDEV_UINPUT_OPEN_MANAGED, "unexpected descriptor");
  std::lock_guard lock{fixture::mutex};
  ++fixture::created;
  *output = reinterpret_cast<libevdev_uinput *>(new int{0});
  return 0;
}
void inputtino_test_destroy(libevdev_uinput *device) {
  delete reinterpret_cast<int *>(device);
  std::lock_guard lock{fixture::mutex};
  ++fixture::destroyed;
  fixture::changed.notify_all();
}

int main(int argc, char **argv) {
  using namespace fixture;
  try {
    require(argc == 2, "missing scenario");
    const std::string scenario{argv[1]};
    if (scenario == "idle") {
      // Closing an idle keyboard must wake a long repeat delay immediately.
      auto result = inputtino::Keyboard::create({}, 60000);
      require(bool(result), "create failed");
      auto keyboard = std::make_unique<inputtino::Keyboard>(std::move(*result));
      std::this_thread::sleep_for(20ms);
      const auto start = std::chrono::steady_clock::now();
      keyboard.reset();
      {
        std::lock_guard lock{mutex};
        require(created == destroyed, "destructor returned before destroying its device");
      }
      require(std::chrono::steady_clock::now() - start < 1s, "stop waited for the repeat period");
    } else if (scenario == "move") {
      auto result = inputtino::Keyboard::create({}, 60000);
      require(bool(result), "create failed");
      auto first = std::make_unique<inputtino::Keyboard>(std::move(*result));
      auto second = std::make_unique<inputtino::Keyboard>(std::move(*first));
      first.reset();
      second->press(0x41);
      second->release(0x41);
      second.reset();
      std::lock_guard lock{mutex};
      require(releases == 1, "move lost keyboard input");
      require(created == destroyed, "move lost repeat-thread ownership");
    } else {
      require(scenario == "release" || scenario == "teardown" || scenario == "duplicate", "unknown scenario");
      gate_repeat = true;
      auto result = inputtino::Keyboard::create({}, scenario == "duplicate" ? 200 : 5);
      require(bool(result), "create failed");
      auto keyboard = std::make_unique<inputtino::Keyboard>(std::move(*result));
      keyboard->press(0x41);
      if (scenario == "duplicate") {
        for (int i = 0; i < 16; ++i) keyboard->press(0x41);
      }
      await_repeat();
      std::promise<void> started;
      auto started_signal = started.get_future();
      auto operation = std::async(std::launch::async, [&] {
        started.set_value();
        if (scenario == "teardown") keyboard.reset();
        else keyboard->release(0x41);
      });
      started_signal.wait();
      const bool completed_early = operation.wait_for(100ms) == std::future_status::ready;
      unblock_repeat();
      operation.get();
      keyboard.reset();
      await_destroyed();
      require(!completed_early, scenario == "teardown" ?
        "destructor returned with a repeat still running" : "release overtook an in-flight repeat");
      if (scenario == "duplicate") {
        std::lock_guard lock{mutex};
        require(repeats == 1, "repeated press calls multiplied held-key repeat ownership");
      }
    }
    std::cout << "keyboard " << scenario << " passed\n";
    return 0;
  } catch (const std::exception &error) {
    unblock_repeat();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
