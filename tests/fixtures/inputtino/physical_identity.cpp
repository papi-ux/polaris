#include "uinput_boundary.hpp"
#include POLARIS_TEST_MOUSE_SOURCE
#include <inputtino/input.h>
#undef open
#undef ioctl
#undef close
#undef libevdev_uinput_create_from_device
#undef libevdev_uinput_destroy
#include <cstdarg>
#include <sys/wait.h>

namespace inputtino {
Result<libevdev_uinput_ptr> create_keyboard(const DeviceDefinition &);
Result<libevdev_uinput_ptr> create_touch_screen(const DeviceDefinition &);
Result<libevdev_uinput_ptr> create_tablet(const DeviceDefinition &);
Result<libevdev_uinput_ptr> create_trackpad(const DeviceDefinition &);
Result<libevdev_uinput_ptr> create_xbox_controller(const DeviceDefinition &);
Result<libevdev_uinput_ptr> create_nintendo_controller(const DeviceDefinition &);
Result<libevdev_uinput_ptr> create_ps_controller(const DeviceDefinition &);
}
namespace fixture {
struct Device { int fd; };
struct Event { std::string kind; int fd; std::string value; };
std::vector<Event> events;
std::map<int, std::string> identities;
std::string failure;
int attempts = 0, fail_at = 1;
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void reset(std::string fail = {}, int at = 1) {
  require(identities.empty(), "descriptor survived previous test");
  events.clear(); failure = std::move(fail); attempts = 0; fail_at = at;
}
void check_closed() {
  require(identities.empty(), "input descriptor leaked");
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].kind != "close") continue;
    // Every successful creation must be destroyed before that close.
    bool created = false, destroyed = false;
    for (size_t j = i; j-- > 0;) {
      if (events[j].fd != events[i].fd) continue;
      if (events[j].kind == "open") break;
      if (events[j].kind == "create") created = true;
      if (events[j].kind == "destroy") destroyed = true;
    }
    require(!created || destroyed, "descriptor closed before device destruction");
  }
}
void check_exec(int fd) {
  const int unrelated = ::open("/dev/null", O_RDONLY);
  require(unrelated >= 0, "unrelated fixture fd failed");
  const auto fd_arg = std::to_string(fd), unrelated_arg = std::to_string(unrelated);
  const auto child = fork();
  if (child == 0) {
    execl("/proc/self/exe", "test_inputtino_phys", "--check-fds", fd_arg.c_str(), unrelated_arg.c_str(), nullptr);
    _exit(127);
  }
  require(child > 0, "fork failed");
  int status;
  require(waitpid(child, &status, 0) == child, "wait failed");
  ::close(unrelated);
  require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "input descriptor survived exec or unrelated fd was closed");
}
}

int inputtino_test_open(const char *name, int flags, ...) {
  using namespace fixture;
  require(std::string(name) == "/dev/uinput", "unexpected open boundary");
  ++attempts;
  if (failure == "open" && attempts == fail_at) { errno = EACCES; return -1; }
  require((flags & (O_CLOEXEC | O_NONBLOCK | O_ACCMODE)) == (O_CLOEXEC | O_NONBLOCK | O_RDWR), "incorrect creating fd flags");
  int fd = ::open("/dev/null", flags);
  require(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "creating fd is not close-on-exec");
  identities[fd] = {};
  events.push_back({"open", fd, {}});
  return fd;
}
int inputtino_test_ioctl(int fd, unsigned long request, ...) {
  using namespace fixture;
  require(request == UI_SET_PHYS && identities.contains(fd), "unexpected ioctl boundary");
  if (failure == "phys" && attempts == fail_at) { errno = EIO; return -1; }
  va_list args;
  va_start(args, request);
  identities[fd] = va_arg(args, const char *);
  va_end(args);
  events.push_back({"phys", fd, identities[fd]});
  return 0;
}
int inputtino_test_create(const libevdev *definition, int fd, libevdev_uinput **output) {
  using namespace fixture;
  if (fd >= 0) require(identities.contains(fd) && !identities[fd].empty(), "create ran before physical identity");
  else require(fd == LIBEVDEV_UINPUT_OPEN_MANAGED, "unexpected managed identity");
  if (failure == "create" && attempts == fail_at) return -EIO;
  events.push_back({"create", fd, libevdev_get_name(definition)});
  *output = reinterpret_cast<libevdev_uinput *>(new Device{fd});
  return 0;
}
void inputtino_test_destroy(libevdev_uinput *pointer) {
  auto *device = reinterpret_cast<fixture::Device *>(pointer);
  fixture::events.push_back({"destroy", device->fd, {}});
  delete device;
}
int inputtino_test_close(int fd) {
  using namespace fixture;
  require(identities.erase(fd) == 1, "descriptor closed twice or not owned");
  events.push_back({"close", fd, {}});
  return ::close(fd);
}

int main(int argc, char **argv) {
  if (argc == 4 && std::string(argv[1]) == "--check-fds") {
    return fcntl(std::stoi(argv[2]), F_GETFD) == -1 && errno == EBADF &&
           fcntl(std::stoi(argv[3]), F_GETFD) >= 0 ? 0 : 1;
  }
  using namespace inputtino;
  using namespace fixture;
  try {
    const DeviceDefinition definition{.name="Polaris fixture", .vendor_id=0x1234, .product_id=2, .version=1,
        .device_phys="polaris/seat-fixture/mouse"};
    using Factory = Result<libevdev_uinput_ptr> (*)(const DeviceDefinition &);
    const Factory factories[] = {create_keyboard, create_mouse, create_mouse_abs, create_touch_screen,
      create_tablet, create_trackpad, create_xbox_controller, create_nintendo_controller, create_ps_controller};
    for (const auto factory : factories) {
      reset();
      {
        auto result = factory(definition);
        require(bool(result), "constructor failed");
        require(identities.size() == 1 && identities.begin()->second == definition.device_phys, "identity changed");
        check_exec(identities.begin()->first);
        auto shared = *result;
        result = libevdev_uinput_ptr{};
        require(identities.size() == 1, "shared handle closed early");
      }
      check_closed();
      for (const auto &step : {"open", "phys", "create"}) {
        reset(step);
        require(!factory(definition), "failed identity fell back to unmanaged creation");
        check_closed();
      }
      reset();
      auto empty = definition;
      empty.device_phys.clear();
      { require(bool(factory(empty)), "empty-phys managed behavior changed"); }
      require(attempts == 0 && identities.empty(), "empty phys opened explicit descriptor");
    }
    reset();
    auto invalid = definition;
    invalid.device_phys = std::string("bad\0identity", 12);
    require(!create_keyboard(invalid) && attempts == 0, "NUL identity not rejected before open");
    reset("create", 2);
    require(!Mouse::create(definition), "second mouse failure not propagated");
    check_closed();
    for (const char *value : {static_cast<const char *>(nullptr), "", "polaris/c-api/mouse"}) {
      reset();
      const InputtinoDeviceDefinition def{.name="C API fixture", .vendor_id=1, .product_id=2, .version=1, .device_phys=value};
      const InputtinoErrorHandler errors{.eh=[](const char *, void *) {}, .user_data=nullptr};
      auto *mouse = inputtino_mouse_create(&def, &errors);
      require(mouse != nullptr, "C API create failed");
      for (const auto &[fd, identity] : identities)
        require(identity == (value ? value : "00:11:22:33:44:55"), "C API default identity changed");
      require(attempts == (value && !*value ? 0 : 2), "C API empty/null behavior changed");
      inputtino_mouse_destroy(mouse);
      check_closed();
    }
    std::cout << "PASS: nine creation paths, identity failures, shared lifetime, C API and exec inheritance\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
