/** @file tests/unit/platform/test_audio_process_id.cpp
 * Audio ownership metadata and opt-in private server routing regressions.
 */
#include "src/platform/linux/audio_process_id.h"
#include "src/platform/common.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  using namespace platf::audio_process_id;
  using properties_t = std::unique_ptr<pa_proplist, decltype(&pa_proplist_free)>;

  properties_t properties() {
    return {pa_proplist_new(), pa_proplist_free};
  }

  property_t stream_property(const char *value) {
    auto props = properties();
    if (value) pa_proplist_sets(props.get(), PA_PROP_APPLICATION_PROCESS_ID, value);
    return property(props.get(), PA_PROP_APPLICATION_PROCESS_ID);
  }

  std::optional<pid_t> client_pid(const char *application, const char *security, const char *api = nullptr) {
    auto props = properties();
    if (application) pa_proplist_sets(props.get(), PA_PROP_APPLICATION_PROCESS_ID, application);
    if (security) pa_proplist_sets(props.get(), "pipewire.sec.pid", security);
    if (api) pa_proplist_sets(props.get(), "client.api", api);
    return client(props.get());
  }
}

TEST(AudioProcessId, ParsesOnlyCompletePositiveProcessIds) {
  EXPECT_EQ(parse("42"), 42);
  EXPECT_EQ(parse(std::to_string(std::numeric_limits<pid_t>::max())), std::numeric_limits<pid_t>::max());
  for (const auto text : {"", "0", "1", "-2", "+42", " 42", "42 ", "42junk", "999999999999999999999"}) {
    EXPECT_FALSE(parse(text)) << text;
  }
  EXPECT_FALSE(parse(std::string_view {"42\0extra", 8}));
}

TEST(AudioProcessId, ExplicitStreamPidTakesPrecedenceAndNeverQueriesClient) {
  int lookups = 0;
  const client_lookup_t lookup = [&](std::uint32_t) { ++lookups; return 900; };
  EXPECT_EQ(sink_input(stream_property("42"), 7, lookup), 42);
  for (const auto text : {"42junk", "1", ""}) {
    EXPECT_FALSE(sink_input(stream_property(text), 7, lookup));
  }
  EXPECT_EQ(lookups, 0);
}

TEST(AudioProcessId, MissingStreamPidResolvesOnlyItsOwningClient) {
  std::vector<std::uint32_t> requested;
  const client_lookup_t lookup = [&](std::uint32_t index) -> std::optional<pid_t> {
    requested.push_back(index);
    if (index == 7) return client_pid("42", nullptr);
    if (index == 8) return client_pid(nullptr, "43");
    return std::nullopt;
  };
  EXPECT_EQ(sink_input(stream_property(nullptr), 7, lookup), 42);
  EXPECT_EQ(sink_input(stream_property(nullptr), 8, lookup), 43);
  EXPECT_FALSE(sink_input(stream_property(nullptr), 9, lookup));
  EXPECT_EQ(requested, (std::vector<std::uint32_t> {7, 8, 9}));
}

TEST(AudioProcessId, MissingClientAndMalformedClientPidFailClosed) {
  const client_lookup_t lookup = [](std::uint32_t) { ADD_FAILURE() << "Invalid client queried"; return 42; };
  EXPECT_FALSE(sink_input({}, std::numeric_limits<std::uint32_t>::max(), lookup));
  EXPECT_FALSE(sink_input({}, 7, {}));
  EXPECT_FALSE(client_pid(nullptr, nullptr));
  EXPECT_FALSE(client_pid("42junk", nullptr));
  EXPECT_FALSE(client_pid("42", "43junk"));
  EXPECT_FALSE(client_pid("42", ""));
}

TEST(AudioProcessId, NativeProtocolHostPidPrecedesClientNamespacePid) {
  EXPECT_EQ(client_pid("2", "4300"), 4300);
  EXPECT_EQ(client_pid("42", nullptr), 42);
  EXPECT_EQ(client_pid("2", "4300", "pipewire"), 4300);
}

TEST(AudioProcessId, PulseProxyUsesApplicationPidAndNeverDaemonPid) {
  EXPECT_EQ(client_pid("42", "4300", "pipewire-pulse"), 42);
  EXPECT_FALSE(client_pid(nullptr, "4300", "pipewire-pulse"));
  EXPECT_FALSE(client_pid("", "4300", "pipewire-pulse"));
  EXPECT_FALSE(client_pid("42junk", "4300", "pipewire-pulse"));
}

TEST(AudioProcessId, PresentBinaryPropertiesCannotBecomeMissingAndEnableFallback) {
  const client_lookup_t lookup = [](std::uint32_t) { ADD_FAILURE() << "Malformed stream queried client"; return 42; };
  for (const auto &raw : {std::string {}, std::string {"42"}, std::string {"42\0extra\0", 9}, std::string {"\xff\0", 2}}) {
    auto props = properties();
    ASSERT_EQ(pa_proplist_set(props.get(), PA_PROP_APPLICATION_PROCESS_ID, raw.data(), raw.size()), 0);
    EXPECT_FALSE(sink_input(property(props.get(), PA_PROP_APPLICATION_PROCESS_ID), 7, lookup));
    EXPECT_FALSE(client(props.get()));
    ASSERT_EQ(pa_proplist_sets(props.get(), PA_PROP_APPLICATION_PROCESS_ID, "42"), 0);
    ASSERT_EQ(pa_proplist_set(props.get(), "pipewire.sec.pid", raw.data(), raw.size()), 0);
    EXPECT_FALSE(client(props.get()));
    ASSERT_EQ(pa_proplist_sets(props.get(), "pipewire.sec.pid", "43"), 0);
    ASSERT_EQ(pa_proplist_set(props.get(), "client.api", raw.data(), raw.size()), 0);
    EXPECT_FALSE(client(props.get()));
  }
}

class AudioProcessRouting: public testing::Test {
protected:
  std::string runtime;
  void SetUp() override {
    if (!std::getenv("POLARIS_TEST_PRIVATE_AUDIO_ROUTING")) {
      GTEST_SKIP() << "Requires the isolated PipeWire routing harness";
    }
    const auto *directory = std::getenv("XDG_RUNTIME_DIR");
    const auto *server = std::getenv("PULSE_SERVER");
    ASSERT_NE(directory, nullptr);
    ASSERT_NE(server, nullptr);
    runtime = directory;
    ASSERT_TRUE(runtime.starts_with("/tmp/polaris-audio629-"));
    ASSERT_EQ(std::string(server), "unix:" + runtime + "/pulse/native");
    struct stat info {};
    ASSERT_EQ(::lstat(runtime.c_str(), &info), 0);
    ASSERT_TRUE(S_ISDIR(info.st_mode));
    ASSERT_EQ(info.st_uid, ::geteuid());
    ASSERT_EQ(info.st_mode & 0777, 0700);
  }
};

TEST_F(AudioProcessRouting, PrivatePipeWireFixture) {
  auto control = platf::audio_control();
  ASSERT_NE(control, nullptr);
  control->route_process_audio_to_sink("polaris-routing-session");
  control->route_process_audio_to_sink("polaris-routing-session");
  // The outer harness verifies real streams and the unchanged default.
}
