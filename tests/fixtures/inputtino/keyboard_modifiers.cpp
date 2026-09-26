#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include "uinput_boundary.hpp"

namespace fixture {
using namespace std::chrono_literals;
struct Device { unsigned id; };
struct Event { unsigned device, code; int value; };
std::mutex mutex;
std::condition_variable changed;
std::vector<Event> events;
unsigned next_device = 0;
bool fail_key_write = false;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int write_event(const libevdev_uinput *device, unsigned type, unsigned code, int value) {
  std::lock_guard lock{mutex};
  if (type == EV_KEY) {
    events.push_back({reinterpret_cast<const Device *>(device)->id, code, value});
    changed.notify_all();
    if (fail_key_write && code == KEY_A && value == 1) return -EIO;
  }
  return 0;
}
auto snapshot() {
  std::lock_guard lock{mutex};
  return events;
}
unsigned key_presses() {
  std::lock_guard lock{mutex};
  return std::count_if(events.begin(), events.end(), [](auto e) { return e.code == KEY_A && e.value == 1; });
}
void await_presses(unsigned count) {
  std::unique_lock lock{mutex};
  require(changed.wait_for(lock, 3s, [count] {
    return std::count_if(events.begin(), events.end(), [](auto e) { return e.code == KEY_A && e.value == 1; }) >= count;
  }), "key did not repeat");
}
void expect_keys(std::vector<std::pair<unsigned, int>> expected, std::size_t offset = 0) {
  const auto actual = snapshot();
  require(actual.size() == offset + expected.size(), "unexpected key event count");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    require(std::pair{actual[offset + i].code, actual[offset + i].value} == expected[i], "unexpected modifier/key order");
  }
}
}

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
  *output = reinterpret_cast<libevdev_uinput *>(new fixture::Device{++fixture::next_device});
  return 0;
}
void inputtino_test_destroy(libevdev_uinput *device) { delete reinterpret_cast<fixture::Device *>(device); }

int main(int argc, char **argv) {
  using namespace fixture;
  using Keyboard = inputtino::Keyboard;
  try {
    require(argc == 2, "missing scenario");
    const std::string scenario{argv[1]};
    const bool repeating = scenario == "repeat" || scenario == "physical-release";
    auto result = Keyboard::create({}, repeating ? 50 : 60000);
    require(bool(result), "create failed");
    Keyboard keyboard(std::move(*result));
    if (scenario == "chord" || scenario == "write-error") {
      fail_key_write = scenario == "write-error";
      keyboard.press(0x41, Keyboard::SHIFT | Keyboard::CTRL | Keyboard::ALT | Keyboard::META);
      keyboard.release(0x41);
      expect_keys({{KEY_LEFTSHIFT, 1}, {KEY_LEFTCTRL, 1}, {KEY_LEFTALT, 1}, {KEY_LEFTMETA, 1},
        {KEY_A, 1}, {KEY_LEFTMETA, 0}, {KEY_LEFTALT, 0}, {KEY_LEFTCTRL, 0}, {KEY_LEFTSHIFT, 0}, {KEY_A, 0}});
    } else if (scenario == "held") {
      // Every generic/left/right spelling must retain physical ownership.
      for (const auto &[code, linux_code, modifier] : std::vector<std::tuple<short, unsigned, uint8_t>>{
        {0x10, KEY_LEFTSHIFT, Keyboard::SHIFT}, {0xA0, KEY_LEFTSHIFT, Keyboard::SHIFT}, {0xA1, KEY_RIGHTSHIFT, Keyboard::SHIFT},
        {0x11, KEY_LEFTCTRL, Keyboard::CTRL}, {0xA2, KEY_LEFTCTRL, Keyboard::CTRL}, {0xA3, KEY_RIGHTCTRL, Keyboard::CTRL},
        {0x12, KEY_LEFTALT, Keyboard::ALT}, {0xA4, KEY_LEFTALT, Keyboard::ALT}, {0xA5, KEY_RIGHTALT, Keyboard::ALT},
        {0x5B, KEY_LEFTMETA, Keyboard::META}, {0x5C, KEY_RIGHTMETA, Keyboard::META}}) {
        const auto offset = snapshot().size();
        keyboard.press(code);
        keyboard.press(0x41, modifier);
        keyboard.release(0x41);
        expect_keys({{linux_code, 1}, {KEY_A, 1}, {KEY_A, 0}}, offset);
        keyboard.release(code);
      }
    } else if (scenario == "modifier-key") {
      keyboard.press(0xA1, Keyboard::SHIFT | Keyboard::CTRL | Keyboard::ALT | Keyboard::META);
      keyboard.release(0xA1);
      expect_keys({{KEY_RIGHTSHIFT, 1}, {KEY_RIGHTSHIFT, 0}});
    } else if (scenario == "isolation") {
      auto other_result = Keyboard::create({}, 60000);
      require(bool(other_result), "second create failed");
      Keyboard other(std::move(*other_result));
      keyboard.press(0xA1);
      other.press(0x41, Keyboard::SHIFT);
      other.release(0x41);
      keyboard.release(0xA1);
      const auto seen = snapshot();
      require(seen.size() == 6, "cross-keyboard state suppressed a modifier");
      require(seen.front().device == 1 && seen.back().device == 1, "physical modifier changed device");
      for (std::size_t i = 1; i < 5; ++i) require(seen[i].device == 2, "synthetic chord reached another keyboard");
    } else if (scenario == "repeat") {
      keyboard.press(0x41, Keyboard::SHIFT);
      await_presses(2);
      keyboard.press(0xA1);
      await_presses(key_presses() + 1);
      keyboard.release(0xA1);
      await_presses(key_presses() + 1);
      keyboard.release(0x41);
      const auto seen = snapshot();
      bool physically_held = false;
      unsigned synthetic = 0, physical = 0;
      for (std::size_t i = 0; i < seen.size(); ++i) {
        if (seen[i].code == KEY_RIGHTSHIFT) physically_held = seen[i].value != 0;
        if (seen[i].code == KEY_LEFTSHIFT) require(!physically_held, "repeat touched a physically held modifier family");
        if (seen[i].code == KEY_A && seen[i].value == 1) {
          if (physically_held) ++physical;
          else {
            ++synthetic;
            require(i > 0 && i + 1 < seen.size(), "incomplete repeated chord");
            require(seen[i - 1].code == KEY_LEFTSHIFT && seen[i - 1].value == 1 &&
              seen[i + 1].code == KEY_LEFTSHIFT && seen[i + 1].value == 0, "repeat lost synthetic modifier");
          }
        }
      }
      require(synthetic >= 3 && physical >= 1, "missing repeat ownership transitions");
    } else if (scenario == "physical-release") {
      keyboard.press(0xA1);
      keyboard.press(0x41, Keyboard::SHIFT);
      await_presses(2);
      keyboard.release(0xA1);
      await_presses(key_presses() + 1);
      keyboard.release(0x41);
      for (auto e : snapshot()) require(e.code != KEY_LEFTSHIFT, "physical release became a synthetic hold");
    } else {
      throw std::runtime_error("unknown scenario");
    }
    std::cout << "keyboard modifiers " << scenario << " passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
