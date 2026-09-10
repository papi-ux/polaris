#pragma once
#include <linux/uhid.h>
#include <functional>
#include <inputtino/result.hpp>
#include <string>
#include <vector>

// Only the kernel transport is replaced. The test compiles the same patched
// serializer, setters, repeat loop and pad move/destructor as the host build.
namespace inputtino_test {
extern bool fail_create;
void opened(const std::function<void(const uhid_event &, int)> &callback);
void stopped();
inputtino::Result<bool> accept(const uhid_event &);
}
namespace uhid {
struct DeviceDefinition {
  std::string name, phys, uniq;
  uint16_t bus;
  uint32_t vendor, product, version, country;
  std::vector<unsigned char> report_description;
};
inline inputtino::Result<bool> uhid_write(int, const uhid_event *ev) {
  return inputtino_test::accept(*ev);
}
class Device {
public:
  static inputtino::Result<Device> create(const DeviceDefinition &,
      const std::function<void(const uhid_event &, int)> &callback) {
    if (inputtino_test::fail_create) return inputtino::Error("fixture creation failure");
    inputtino_test::opened(callback);
    return Device{};
  }
  inputtino::Result<bool> send(const uhid_event &ev) { return inputtino_test::accept(ev); }
  void stop_thread() { inputtino_test::stopped(); }
};
}
