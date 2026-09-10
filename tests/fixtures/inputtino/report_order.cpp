#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>
#include <linux/input.h>
#include POLARIS_TEST_INPUTTINO_SOURCE

using namespace std::chrono_literals;
namespace inputtino_test {
bool fail_create = false;
struct Report { unsigned buttons; unsigned sequence; };
std::mutex mutex;
std::condition_variable changed;
std::vector<Report> reports;
std::function<void(const uhid_event &, int)> callback;
bool gate = false, waiting = false, release = false, transport_stopped = false;
thread_local bool setter = false;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void opened(const std::function<void(const uhid_event &, int)> &value) {
  std::lock_guard lock(mutex);
  callback = value;
  transport_stopped = false;
}
void stopped() {
  std::lock_guard lock(mutex);
  transport_stopped = true;
  callback = {}; // release the callback's shared pad state, as the real reader does
}
inputtino::Result<bool> accept(const uhid_event &ev) {
  require(ev.type == UHID_INPUT2, "unexpected report type");
  const auto offset = sizeof(uhid::dualsense_input_report_bt_header);
  require(ev.u.input2.size >= offset + sizeof(uhid::dualsense_input_report) + 4 &&
          ev.u.input2.size <= UHID_DATA_MAX, "invalid report length");
  uhid::dualsense_input_report report{};
  std::memcpy(&report, ev.u.input2.data + offset, sizeof report);
  const auto end = ev.u.input2.size - 4;
  uint32_t actual;
  std::memcpy(&actual, ev.u.input2.data + end, 4);
  require(actual == inputtino::sign_crc32(uhid::PS_INPUT_CRC32, ev.u.input2.data, end), "invalid CRC");
  unsigned buttons = 0;
  if (report.buttons[0] & uhid::CROSS) buttons |= inputtino::Joypad::A;
  if (report.buttons[0] & uhid::CIRCLE) buttons |= inputtino::Joypad::B;
  if ((report.buttons[0] & 15) == uhid::HAT_E) buttons |= inputtino::Joypad::DPAD_RIGHT;
  std::unique_lock lock(mutex);
  require(!transport_stopped, "report after transport stopped");
  if (gate && !setter) {
    waiting = true;
    changed.notify_all();
    require(changed.wait_for(lock, 3s, [] { return release; }), "report gate timed out");
  }
  reports.push_back({buttons, report.seq_number});
  changed.notify_all();
  return true;
}
void reset() {
  std::lock_guard lock(mutex);
  reports.clear();
  gate = waiting = release = false;
}
void verify_order() {
  std::lock_guard lock(mutex);
  require(!reports.empty(), "no reports");
  for (size_t i = 1; i < reports.size(); ++i)
    require(reports[i].sequence == (reports[i - 1].sequence + 1) % 255, "report sequence went backwards or skipped");
}
}

int main() {
  using namespace inputtino_test;
  try {
    setter = true;
    fail_create = true;
    require(!inputtino::PS5Joypad::create(), "failed create unexpectedly succeeded");
    fail_create = false;
    // Real create/move/repeat/destruction run each time, without a kernel device.
    for (int iteration = 0; iteration < 5; ++iteration) {
      reset();
      {
        auto result = inputtino::PS5Joypad::create();
        require(bool(result), "fixture create failed");
        inputtino::PS5Joypad first(std::move(*result));
        inputtino::PS5Joypad pad(std::move(first));
        pad.set_pressed_buttons(inputtino::Joypad::A);
        {
          std::unique_lock lock(mutex);
          gate = true;
          require(changed.wait_for(lock, 3s, [] { return waiting; }), "repeat sender did not reach gate");
        }
        std::atomic<bool> started = false, complete = false;
        std::jthread updates([&] {
          setter = true;
          started = true;
          changed.notify_all();
          pad.set_pressed_buttons(inputtino::Joypad::B);
          pad.set_pressed_buttons(inputtino::Joypad::DPAD_RIGHT);
          complete = true;
          changed.notify_all();
        });
        {
          std::unique_lock lock(mutex);
          changed.wait_for(lock, 1s, [&] { return started.load(); });
          // Give the competing setters a bounded window to attempt delivery.
          // Old code completes here; fixed code waits for the held report.
          changed.wait_for(lock, 100ms, [&] { return complete.load(); });
          release = true;
          gate = false;
          changed.notify_all();
        }
        updates.join();
        verify_order();
        {
          std::lock_guard lock(mutex);
          require(reports.back().buttons == inputtino::Joypad::DPAD_RIGHT, "stale buttons after completed setters");
        }
      }
      size_t count;
      { std::lock_guard lock(mutex); count = reports.size(); }
      std::this_thread::sleep_for(20ms);
      { std::lock_guard lock(mutex); require(count == reports.size(), "sender survived pad destruction"); }
    }
    reset();
    {
      auto result = inputtino::PS5Joypad::create();
      require(bool(result), "stress create failed");
      inputtino::PS5Joypad pad(std::move(*result));
      std::function<void(const uhid_event &, int)> reader;
      { std::lock_guard lock(mutex); reader = callback; }
      std::barrier start(2);
      std::atomic<unsigned> feedback{0};
      pad.set_on_rumble([&](int, int) { ++feedback; });
      std::jthread output([&] {
        start.arrive_and_wait();
        uhid_event ev{};
        ev.type = UHID_OUTPUT;
        auto *report = reinterpret_cast<uhid::dualsense_output_report_usb *>(ev.u.output.data);
        ev.u.output.data[0] = uhid::DS_OUTPUT_REPORT_USB;
        report->common.valid_flag0 = uhid::MOTOR_OR_COMPATIBLE_VIBRATION;
        for (int i = 0; i < 10000; ++i) reader(ev, -1);
      });
      start.arrive_and_wait();
      for (int i = 0; i < 10000; ++i) {
        pad.set_pressed_buttons(i % 2 ? inputtino::Joypad::A : inputtino::Joypad::B);
        pad.set_triggers(i % 256, (i + 1) % 256);
        pad.set_stick(inputtino::Joypad::LS, i % 30000, i % 30000);
        pad.set_motion(inputtino::PS5Joypad::ACCELERATION, 1, 2, 3);
        pad.set_battery(inputtino::PS5Joypad::BATTERY_FULL, 100);
        pad.place_finger(0, i % 1920, i % 1080);
        pad.release_finger(0);
        pad.set_on_rumble([&](int, int) {
          ++feedback;
          // Reentrant registration must not deadlock under the callback lock.
          pad.set_on_led([](int, int, int) {});
        });
      }
      output.join();
      require(feedback > 0, "feedback callback never exercised");
      verify_order();
    }
    std::cout << "PASS: report order, CRC, concurrent setters/feedback, failed create, move and repeat teardown\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
