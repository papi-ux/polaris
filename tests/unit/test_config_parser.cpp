/**
 * @file tests/unit/test_config_parser.cpp
 * @brief Test configuration value parsers.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/configuration_store.h>
#include <src/nvenc/nvenc_config.h>
#include <src/private_state_file.h>
#include <src/utility.h>

#include <filesystem>
#include <fstream>
#ifndef _WIN32
  #include <fcntl.h>
  #include <sys/file.h>
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif
#include <cstring>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

TEST(ConfigParserTests, ProtocolDecimalsUseDotAndRequireTheWholeValue) {
  const auto fps = util::parse_decimal<double>("60.0");
  ASSERT_TRUE(fps.has_value());
  EXPECT_DOUBLE_EQ(*fps, 60.0);

  const auto fractional = util::parse_decimal<double>("59.94");
  ASSERT_TRUE(fractional.has_value());
  EXPECT_DOUBLE_EQ(*fractional, 59.94);

  EXPECT_FALSE(util::parse_decimal<double>("60,0").has_value());
  EXPECT_FALSE(util::parse_decimal<double>("60.0fps").has_value());
  EXPECT_FALSE(util::parse_decimal<double>(" 60.0").has_value());
  EXPECT_FALSE(util::parse_decimal<double>("nan").has_value());
  EXPECT_FALSE(util::parse_decimal<double>("inf").has_value());
}

TEST(ConfigParserTests, ProtocolDecimalFormattingNeverUsesAComma) {
  EXPECT_EQ(util::format_decimal(59.94), "59.94");
  EXPECT_EQ(util::format_decimal(60.0), "60");
}

TEST(ConfigParserTests, ParsesNvencSplitEncodeModeValues) {
  EXPECT_EQ(config::nv::split_encode_mode_from_view("disabled"), nvenc::nvenc_split_encode_mode::disabled);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("auto"), nvenc::nvenc_split_encode_mode::auto_mode);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("forced"), nvenc::nvenc_split_encode_mode::forced);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("2"), nvenc::nvenc_split_encode_mode::two_way);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("3"), nvenc::nvenc_split_encode_mode::three_way);
}

TEST(ConfigParserTests, UnknownNvencSplitEncodeModeFallsBackToDisabled) {
  EXPECT_EQ(config::nv::split_encode_mode_from_view("not-a-real-mode"), nvenc::nvenc_split_encode_mode::disabled);
}

namespace {
  bool contains(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
  }
}  // namespace

TEST(ConfigParserTests, MoonlightMultiseatInputDefaultsOff) {
  EXPECT_FALSE(config::input.multiseat_moonlight_input);
}

TEST(ConfigParserTests, BooleanValuesParseRegardlessOfCase) {
  // The lowercasing in to_bool() was a no-op, so every capitalized spelling in a
  // hand-edited polaris.conf read as false without saying anything.
  EXPECT_EQ(config::parse_bool("Enabled"), std::optional<bool> {true});
  EXPECT_EQ(config::parse_bool("TRUE"), std::optional<bool> {true});
  EXPECT_EQ(config::parse_bool("On"), std::optional<bool> {true});
  EXPECT_EQ(config::parse_bool("Disabled"), std::optional<bool> {false});
  EXPECT_EQ(config::parse_bool("OFF"), std::optional<bool> {false});
}

TEST(ConfigParserTests, DocumentedBooleanSpellingsParseBothWays) {
  for (const auto *value : {"true", "yes", "enable", "enabled", "on", "1"}) {
    EXPECT_EQ(config::parse_bool(value), std::optional<bool> {true}) << value;
  }

  for (const auto *value : {"false", "no", "disable", "disabled", "off", "0"}) {
    EXPECT_EQ(config::parse_bool(value), std::optional<bool> {false}) << value;
  }
}

TEST(ConfigParserTests, AValueThatIsNotABooleanIsReportedRatherThanReadAsFalse) {
  // #517, found via #409: linux_capture_profile = gpu_native is a mode name on a
  // boolean key. It read as false, capture telemetry stayed off, and nothing in
  // the log said the value had been rejected.
  EXPECT_FALSE(config::parse_bool("gpu_native").has_value());
  EXPECT_FALSE(config::parse_bool("sometimes").has_value());
  EXPECT_FALSE(config::parse_bool("").has_value());
}

TEST(ConfigParserTests, PlausibleBackButtonTimeoutsAreNotWarnedAbout) {
  // -1 is the documented way to disable Home emulation, and any other negative
  // value disables it too, so neither is a mistake.
  EXPECT_TRUE(config::back_button_timeout_warning(-1).empty());
  EXPECT_TRUE(config::back_button_timeout_warning(-500).empty());

  // At and above the emulated press length the setting behaves as described.
  EXPECT_TRUE(config::back_button_timeout_warning(config::back_button_emulated_press_ms).empty());
  EXPECT_TRUE(config::back_button_timeout_warning(2000).empty());
}

TEST(ConfigParserTests, BackButtonTimeoutShorterThanTheEmulatedPressIsWarnedAbout) {
  // The value from #222: intended as two seconds, applied as two milliseconds,
  // which turned every Select press into Home.
  const auto advice = config::back_button_timeout_warning(2);
  ASSERT_FALSE(advice.empty());

  EXPECT_TRUE(contains(advice, "2 milliseconds"));
  // Naming the replacement value is the whole point; a warning that only says
  // "too small" leaves the reader exactly where they started.
  EXPECT_TRUE(contains(advice, "use 2000"));
  EXPECT_TRUE(contains(advice, "-1 to disable"));
}

TEST(ConfigParserTests, BackButtonTimeoutWarningPluralizesTheSecondsGuess) {
  EXPECT_TRUE(contains(config::back_button_timeout_warning(1), "1 second, use 1000"));
  EXPECT_TRUE(contains(config::back_button_timeout_warning(5), "5 seconds, use 5000"));
}

TEST(ConfigParserTests, ZeroBackButtonTimeoutWarnsWithoutASecondsGuess) {
  const auto advice = config::back_button_timeout_warning(0);
  ASSERT_FALSE(advice.empty());

  // Zero fires Home immediately, so it is worth warning about, but "0 seconds"
  // is not a guess at intent worth printing. Match the guess clause rather than
  // the word "second", which also occurs inside "milliseconds".
  EXPECT_FALSE(contains(advice, "If you meant"));
  EXPECT_TRUE(contains(advice, "0 milliseconds"));
  EXPECT_TRUE(contains(advice, "-1 to disable"));
}

TEST(ConfigParserTests, ASettingsFileThatStillSetsTheAdaptiveCeilingIsToldItCapsNothing) {
  // adaptive_bitrate_max no longer caps a stream, and the settings page no
  // longer shows it. A host that set it to 30000 as a cap would see its streams
  // run above 30 Mbps with nothing saying why, so parsing names max_bitrate.
  EXPECT_TRUE(config::retired_adaptive_bitrate_max_warning({}).empty());
  EXPECT_TRUE(config::retired_adaptive_bitrate_max_warning({{"adaptive_bitrate_min", "4000"}}).empty());

  const auto advice = config::retired_adaptive_bitrate_max_warning({{"adaptive_bitrate_max", "30000"}});
  ASSERT_FALSE(advice.empty());
  EXPECT_TRUE(contains(advice, "adaptive_bitrate_max = 30000"));
  EXPECT_TRUE(contains(advice, "no longer limits"));
  EXPECT_TRUE(contains(advice, "set max_bitrate"));
  // Two things run a stream above its request: a request below adaptive_bitrate_min starts at that
  // floor, and Doctor may raise a starved PyroWave stream. The warning names both rather than
  // saying nothing does, or that Doctor alone does.
  EXPECT_FALSE(contains(advice, "only lower"));
  EXPECT_FALSE(contains(advice, "only Doctor"));
  EXPECT_TRUE(contains(advice, "a request below adaptive_bitrate_min starts at that floor"));
  EXPECT_TRUE(contains(advice, "PyroWave"));

  // Startup has to ask before the key is parsed, because parsing consumes it.
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/config.cpp");
  ASSERT_TRUE(in);
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto warned = source.find("retired_adaptive_bitrate_max_warning(vars)");
  const auto parsed = source.find("int_between_f(vars, \"adaptive_bitrate_max\"");
  ASSERT_NE(warned, std::string::npos) << "startup no longer warns about adaptive_bitrate_max";
  ASSERT_NE(parsed, std::string::npos);
  EXPECT_LT(warned, parsed) << "the warning must run before parsing consumes the key";
}

TEST(ConfigParserTests, VaapiOptionsParseAndCanReturnToAutomatic) {
  const auto initial = config::parse_vaapi_settings({
    {"vaapi_quality", "balanced"}, {"vaapi_rc", "qvbr"}, {"vaapi_blbrc", "enabled"}, {"vaapi_strict_rc_buffer", "true"}
  });
  EXPECT_EQ(initial.quality, config::vaapi::quality_e::balanced);
  EXPECT_EQ(initial.rc, config::vaapi::rc_e::qvbr);
  EXPECT_EQ(initial.blbrc, true);
  EXPECT_TRUE(initial.strict_rc_buffer);
  const auto restored = config::parse_vaapi_settings({
    {"vaapi_quality", "auto"}, {"vaapi_rc", "auto"}, {"vaapi_blbrc", "auto"}, {"vaapi_strict_rc_buffer", "false"}
  }, initial);
  EXPECT_EQ(restored.quality, config::vaapi::quality_e::automatic);
  EXPECT_EQ(restored.rc, config::vaapi::rc_e::automatic);
  EXPECT_FALSE(restored.blbrc.has_value());
  EXPECT_FALSE(restored.strict_rc_buffer);
  const auto invalid = config::parse_vaapi_settings({
    {"vaapi_quality", "ultra"}, {"vaapi_rc", "unknown"}, {"vaapi_blbrc", "sometimes"}, {"vaapi_strict_rc_buffer", "unknown"}
  }, initial);
  EXPECT_EQ(invalid.quality, initial.quality);
  EXPECT_EQ(invalid.rc, initial.rc);
  EXPECT_EQ(invalid.blbrc, initial.blbrc);
  EXPECT_EQ(invalid.strict_rc_buffer, initial.strict_rc_buffer);
}

TEST(ConfigParserTests, ConcurrentVaapiSnapshotsNeverMixSavedSettings) {
  const auto saved = config::vaapi::snapshot();
  auto restore = util::fail_guard([&] { config::vaapi::publish(saved); });
  const config::vaapi::settings_t manual {
    .strict_rc_buffer = true, .quality = config::vaapi::quality_e::quality,
    .rc = config::vaapi::rc_e::qvbr, .blbrc = true
  };
  config::vaapi::publish({});
  std::thread writer([&] {
    for (int i = 0; i < 20000; ++i) config::vaapi::publish(i % 2 ? manual : config::vaapi::settings_t {});
  });
  for (int i = 0; i < 20000; ++i) {
    const auto value = config::vaapi::snapshot();
    const bool automatic = value.quality == config::vaapi::quality_e::automatic;
    EXPECT_EQ(value.rc, automatic ? config::vaapi::rc_e::automatic : manual.rc);
    EXPECT_EQ(value.strict_rc_buffer, !automatic);
    EXPECT_EQ(value.blbrc, automatic ? std::optional<bool> {} : manual.blbrc);
  }
  writer.join();
}

#ifdef __linux__
TEST(ConfigParserTests, FailedConfigWriteDoesNotPublishVaapiSettings) {
  const auto saved = config::vaapi::snapshot();
  const auto directory = std::filesystem::temp_directory_path() / ("polaris-vaapi-write-failure-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  auto restore = util::fail_guard([&] {
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    config::vaapi::publish(saved);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  });
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  const auto path = (directory / "polaris.conf").string();
  config::vaapi::publish({});
  ASSERT_EQ(config::write_config_with_vaapi_settings(path, ""), 0);
  // The protected writer rejects device nodes before writing. Exercise failures
  // after admission instead, and verify both the saved file and published state.
  for (const auto fault : {private_state_file::write_fault_e::short_write,
         private_state_file::write_fault_e::flush, private_state_file::write_fault_e::sync,
         private_state_file::write_fault_e::rename}) {
    SCOPED_TRACE(static_cast<int>(fault));
    private_state_file::set_write_fault_for_tests(fault);
    ASSERT_NE(config::write_config_with_vaapi_settings(path,
      "vaapi_quality = quality\nvaapi_rc = vbr\nvaapi_blbrc = enabled\nvaapi_strict_rc_buffer = enabled\n"), 0);
    const auto after = config::vaapi::snapshot();
    EXPECT_EQ(after.quality, config::vaapi::quality_e::automatic);
    EXPECT_EQ(after.rc, config::vaapi::rc_e::automatic);
    EXPECT_FALSE(after.blbrc.has_value());
    EXPECT_FALSE(after.strict_rc_buffer);
    const auto persisted = private_state_file::read_secure(path, 4096);
    ASSERT_TRUE(persisted);
    EXPECT_TRUE(persisted.payload.empty());
  }
}
#endif

TEST(ConfigParserTests, SuccessfulConfigWritePublishesCompleteVaapiSettingsAndClearRestoresDefaults) {
  const auto saved = config::vaapi::snapshot();
  const auto directory = std::filesystem::temp_directory_path() / ("polaris-vaapi-config-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  auto restore = util::fail_guard([&] {
    config::vaapi::publish(saved);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  });
  // The atomic writer requires an owned parent that others cannot modify.
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  const auto path = directory / "polaris.conf";
  ASSERT_EQ(config::write_config_with_vaapi_settings(path.string(),
    "vaapi_quality = balanced\nvaapi_rc = qvbr\nvaapi_blbrc = enabled\nvaapi_strict_rc_buffer = enabled\n"), 0);
  const auto after = config::vaapi::snapshot();
  EXPECT_EQ(after.quality, config::vaapi::quality_e::balanced);
  EXPECT_EQ(after.rc, config::vaapi::rc_e::qvbr);
  EXPECT_EQ(after.blbrc, true);
  EXPECT_TRUE(after.strict_rc_buffer);
  ASSERT_EQ(config::write_config_with_vaapi_settings(path.string(), ""), 0);
  const auto cleared = config::vaapi::snapshot();
  EXPECT_EQ(cleared.quality, config::vaapi::quality_e::automatic);
  EXPECT_EQ(cleared.rc, config::vaapi::rc_e::automatic);
  EXPECT_FALSE(cleared.blbrc.has_value());
  EXPECT_FALSE(cleared.strict_rc_buffer);
}

TEST(ConfigLiveApplyTests, AiSettingsFromSavedVariablesStartFromTheBuiltInDefaults) {
  std::unordered_map<std::string, std::string> vars {{"ai_enabled", "enabled"}, {"ai_model", "gpt-5.6-luna"}, {"port", "47989"}};
  const auto settings = config::ai_optimizer_settings(vars);
  EXPECT_TRUE(settings.enabled);
  EXPECT_EQ(settings.provider, "anthropic");
  EXPECT_EQ(settings.model, "gpt-5.6-luna");
  EXPECT_EQ(settings.timeout_ms, 5000);
  EXPECT_EQ(settings.cache_ttl_hours, 168);
  EXPECT_EQ(vars.size(), 1u);
  EXPECT_EQ(vars.count("port"), 1u);
}

TEST(ConfigParserTests, CaptureAutoLoadsAsUnset) {
  // Every backend chooser reads an empty capture as auto. The word itself reached the capture
  // evaluation as a backend it did not know, which then substituted one and recorded a silent
  // failure for a host that was doing exactly what it was told.
  std::unordered_map<std::string, std::string> vars {{"capture", "auto"}, {"port", "47989"}};
  EXPECT_EQ(config::capture_setting(vars, "kms"), "");
  EXPECT_EQ(vars.count("capture"), 0u) << "the key is consumed, as apply_config consumes it";
  EXPECT_EQ(vars.count("port"), 1u);

  std::unordered_map<std::string, std::string> named {{"capture", "kms"}};
  EXPECT_EQ(config::capture_setting(named, ""), "kms");
  std::unordered_map<std::string, std::string> absent;
  EXPECT_EQ(config::capture_setting(absent, "portal"), "portal") << "an absent key leaves the setting alone";

  // Startup has to parse the key through it, or none of this reaches a running host.
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/config.cpp");
  ASSERT_TRUE(in);
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_NE(source.find("video.capture = capture_setting(vars, "), std::string::npos);
  EXPECT_EQ(source.find("string_f(vars, \"capture\", video.capture);"), std::string::npos)
    << "capture is parsed around capture_setting again";
}

TEST(ConfigLiveApplyTests, SteamGridDbKeyAccessorRoundTrips) {
  const auto previous = config::steamgriddb_api_key();
  config::set_steamgriddb_api_key("round-trip-key");
  EXPECT_EQ(config::steamgriddb_api_key(), "round-trip-key");
  config::set_steamgriddb_api_key(previous);
  EXPECT_EQ(config::steamgriddb_api_key(), previous);
}

TEST(ConfigLiveApplyTests, TrustedNetworkAppliesFromSavedVariables) {
  const auto previous_subnets = config::trusted_subnets();
  const auto previous_auto_pairing = config::trusted_subnet_auto_pairing();
  auto restore = util::fail_guard([&] {
    config::set_trusted_network(previous_subnets, previous_auto_pairing);
  });

  config::apply_trusted_network({{"trusted_subnets", "10.0.0.0/24,192.168.1.0/24"}, {"trusted_subnet_auto_pairing", "enabled"}});
  EXPECT_EQ(config::trusted_subnets(), (std::vector<std::string> {"10.0.0.0/24", "192.168.1.0/24"}));
  EXPECT_TRUE(config::trusted_subnet_auto_pairing());

  // Hand-written files use the bracketed list form; startup's parser reads both.
  config::apply_trusted_network({{"trusted_subnets", "[10.0.0.0/24, fd00::/64]"}, {"trusted_subnet_auto_pairing", "disabled"}});
  EXPECT_EQ(config::trusted_subnets(), (std::vector<std::string> {"10.0.0.0/24", "fd00::/64"}));
  EXPECT_FALSE(config::trusted_subnet_auto_pairing());

  // A key removed from the file turns its setting off.
  config::apply_trusted_network({});
  EXPECT_TRUE(config::trusted_subnets().empty());
  EXPECT_FALSE(config::trusted_subnet_auto_pairing());
}

TEST(ConfigNewInstallTests, ANewInstallStartsInPrivateStreamWhenItCanRun) {
  EXPECT_EQ(config::new_install_config(true), "linux_stream_mode = headless_stream\n");
  EXPECT_EQ(config::new_install_config(false), "");
  const auto vars = config::parse_config(config::new_install_config(true));
  ASSERT_EQ(vars.count("linux_stream_mode"), 1u);
  EXPECT_EQ(vars.at("linux_stream_mode"), "headless_stream");
}

TEST(ConfigLoadedFileTests, ParseKeepsTheFileItReadBeforeCommandLineOverrides) {
  // Settings saves judge restart_required against these variables, so they must be the file
  // itself: command line overrides are not in the file a save writes.
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/config.cpp");
  ASSERT_TRUE(in);
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto read = source.find("auto vars = parse_config(file_handler::read_file(sunshine.config_file.c_str()));");
  ASSERT_NE(read, std::string::npos);
  const auto keep = source.find("loaded_config_file = vars;", read);
  ASSERT_NE(keep, std::string::npos);
  const auto overrides = source.find("for (auto &[name, value] : cmd_vars)", read);
  ASSERT_NE(overrides, std::string::npos);
  EXPECT_LT(keep, overrides);
}

TEST(ConfigNewInstallTests, OnlyTheFileCreationWritesTheNewInstallDefault) {
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/config.cpp");
  ASSERT_TRUE(in);
  const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto create = source.find("if (!fs::exists(sunshine.config_file)) {\n        auto cfg_file = std::ofstream {sunshine.config_file};");
  ASSERT_NE(create, std::string::npos);
  const auto use = source.find("cfg_file << new_install_config(");
  ASSERT_NE(use, std::string::npos);
  EXPECT_GT(use, create);
  EXPECT_LT(use - create, 900u);
  const std::string_view call = "cfg_file << new_install_config(";
  EXPECT_EQ(source.find("new_install_config(", use + call.size()), std::string::npos);
}

#ifndef _WIN32
namespace {
  // A configuration file in a directory of its own, removed with the directory.
  struct config_file_mode_fixture_t {
    std::filesystem::path directory;
    std::filesystem::path file;

    explicit config_file_mode_fixture_t(mode_t mode) {
      auto pattern = (std::filesystem::temp_directory_path() / "polaris-config-mode-XXXXXX").string();
      directory = ::mkdtemp(pattern.data());
      file = directory / "polaris.conf";
      std::ofstream {file} << "encoder = vaapi\n";
      std::filesystem::permissions(file, static_cast<std::filesystem::perms>(mode));
    }

    ~config_file_mode_fixture_t() {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }

    mode_t mode() const {
      struct stat metadata {};
      ::lstat(file.c_str(), &metadata);
      return metadata.st_mode & 07777;
    }
  };
}  // namespace

TEST(ConfigFileModeTests, GroupWritableFileLosesOnlyItsWriteBitsAndBecomesSavable) {
  // umask 002 creates the file 0664 (#769); the settings store refuses it until then.
  config_file_mode_fixture_t fixture {0664};
  EXPECT_EQ(private_state_file::read_secure(fixture.file, 4096, true, false).status,
            private_state_file::read_status_e::rejected);

  EXPECT_TRUE(config::restrict_config_file_mode(fixture.file));

  EXPECT_EQ(fixture.mode(), 0644u);
  const auto read = private_state_file::read_secure(fixture.file, 4096, true, false);
  EXPECT_EQ(read.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(read.payload, "encoder = vaapi\n");
}

TEST(ConfigFileModeTests, OtherWritableFileLosesBothWriteBits) {
  config_file_mode_fixture_t fixture {0666};
  EXPECT_TRUE(config::restrict_config_file_mode(fixture.file));
  EXPECT_EQ(fixture.mode(), 0644u);
}

TEST(ConfigFileModeTests, PrivateFileIsLeftAlone) {
  config_file_mode_fixture_t fixture {0600};
  EXPECT_TRUE(config::restrict_config_file_mode(fixture.file));
  EXPECT_EQ(fixture.mode(), 0600u);
}

TEST(ConfigFileModeTests, MissingFileIsNotAFailure) {
  config_file_mode_fixture_t fixture {0600};
  EXPECT_TRUE(config::restrict_config_file_mode(fixture.directory / "absent.conf"));
}

TEST(ConfigFileModeTests, SymlinkIsNotFollowed) {
  config_file_mode_fixture_t fixture {0664};
  const auto link = fixture.directory / "linked.conf";
  std::filesystem::create_symlink(fixture.file, link);

  EXPECT_TRUE(config::restrict_config_file_mode(link));
  EXPECT_EQ(fixture.mode(), 0664u);
}

// #782: the settings store refused a 0664 polaris.conf on read, and the console
// got a bare 503 with nothing in the log, while the write side printed a chmod.
// Every refusal now carries a reason and a fix, and the read side says them.
namespace {
  // What an operator would read, severity included.
  class settings_log_capture_t {
  public:
    settings_log_capture_t():
        stream_ {boost::make_shared<std::ostringstream>()} {
      auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
      backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
      backend->auto_flush(true);
      sink_ = boost::make_shared<sink_t>(backend);
      sink_->set_formatter(&logging::formatter);
      boost::log::core::get()->add_sink(sink_);
    }

    ~settings_log_capture_t() {
      boost::log::core::get()->remove_sink(sink_);
    }

    settings_log_capture_t(const settings_log_capture_t &) = delete;
    settings_log_capture_t &operator=(const settings_log_capture_t &) = delete;

    [[nodiscard]] std::string text() const {
      return stream_->str();
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    boost::shared_ptr<std::ostringstream> stream_;
    boost::shared_ptr<sink_t> sink_;
  };

  std::size_t occurrences(const std::string &text, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
      ++count;
    }
    return count;
  }

  // What GET /api/config serves and the log says for a file the store refuses.
  configuration_store::refusal_t refused_read(const std::filesystem::path &file) {
    configuration_store::refusal_t refusal;
    EXPECT_FALSE(configuration_store::read(file.string(), &refusal).has_value())
      << "the settings store read a file it has to refuse: " << file;
    EXPECT_EQ(refusal.path, file.string());
    return refusal;
  }

  std::string warning_for(const std::filesystem::path &file) {
    return "Warning: Refusing to use the settings file [" + file.string() + "]: ";
  }

  // A UNIX socket bound at a path. open(2) refuses one with ENXIO.
  class bound_socket_t {
  public:
    explicit bound_socket_t(const std::filesystem::path &path):
        descriptor_ {::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)} {
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto text = path.string();
      if (descriptor_ < 0 || text.size() >= sizeof(address.sun_path)) return;
      std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
      bound_ = ::bind(descriptor_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
    }

    ~bound_socket_t() {
      if (descriptor_ >= 0) ::close(descriptor_);
    }

    bound_socket_t(const bound_socket_t &) = delete;
    bound_socket_t &operator=(const bound_socket_t &) = delete;

    [[nodiscard]] bool bound() const {
      return bound_;
    }

  private:
    int descriptor_;
    bool bound_ = false;
  };

  bool fits_a_socket_address(const std::filesystem::path &path) {
    return path.string().size() < sizeof(sockaddr_un {}.sun_path);
  }
}  // namespace

TEST(ConfigFileRefusalTests, GroupWritableFileNamesItsModeAndTheChmod) {
  config_file_mode_fixture_t fixture {0664};
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::group_writable);
  EXPECT_EQ(refusal.reason,
            "It is writable by its group (mode 0664), and the settings store refuses a file another user can change.");
  EXPECT_EQ(refusal.fix, "Restrict it with \"chmod go-w " + fixture.file.string() + "\".");
}

TEST(ConfigFileRefusalTests, OtherWritableFileNamesItsModeAndTheChmod) {
  config_file_mode_fixture_t fixture {0666};
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::other_writable);
  EXPECT_EQ(refusal.reason,
            "It is writable by every user on this machine (mode 0666), and the settings store refuses a file another user can change.");
  EXPECT_EQ(refusal.fix, "Restrict it with \"chmod go-w " + fixture.file.string() + "\".");
}

TEST(ConfigFileRefusalTests, SymlinkIsNamedAndReplacedWithWhatItPointsTo) {
  config_file_mode_fixture_t fixture {0600};
  const auto link = fixture.directory / "linked.conf";
  std::filesystem::create_symlink(fixture.file, link);
  const auto refusal = refused_read(link);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::symlink);
  EXPECT_EQ(refusal.reason, "It is a symbolic link, and the settings store only reads a real file.");
  EXPECT_EQ(refusal.fix, "Replace the link with a copy of what it points to: \"cp --remove-destination \"$(readlink -f " +
                           link.string() + ")\" " + link.string() + "\".");
}

TEST(ConfigFileRefusalTests, HardLinkedFileIsNamedAndGivenOneLink) {
  config_file_mode_fixture_t fixture {0600};
  std::filesystem::create_hard_link(fixture.file, fixture.directory / "second-name.conf");
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::hard_linked);
  EXPECT_EQ(refusal.reason,
            "It has 2 hard links. The settings store only reads a file with one, since any other name for it can change it too.");
  const auto file = fixture.file.string();
  EXPECT_EQ(refusal.fix, "Give it a single link with \"cp -p " + file + ' ' + file + ".new && mv " + file + ".new " + file + "\".");
}

TEST(ConfigFileRefusalTests, OversizedFileNamesItsSizeAndTheLimit) {
  config_file_mode_fixture_t fixture {0600};
  std::ofstream {fixture.file, std::ios::trunc} << std::string(4 * 1024 * 1024 + 1, '#');
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::oversize);
  EXPECT_EQ(refusal.reason, "It is 4194305 bytes, more than the 4 MiB the settings store reads.");
  const auto file = fixture.file.string();
  EXPECT_EQ(refusal.fix, "Move it aside with \"mv " + file + ' ' + file +
                           ".old\", restart Polaris to write a new one, then copy back the settings you need.");
}

TEST(ConfigFileRefusalTests, MissingFileSaysSoAndHowToGetOneBack) {
  config_file_mode_fixture_t fixture {0600};
  const auto absent = fixture.directory / "absent.conf";
  const auto refusal = refused_read(absent);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::missing);
  EXPECT_EQ(refusal.reason, "The settings file does not exist.");
  EXPECT_EQ(refusal.fix, "Restart Polaris to write a new one with the defaults, or put your copy back at " +
                           absent.string() + ".");
}

TEST(ConfigFileRefusalTests, MissingFolderIsNamed) {
  config_file_mode_fixture_t fixture {0600};
  const auto gone = fixture.directory / "gone";
  const auto refusal = refused_read(gone / "polaris.conf");
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::directory_missing);
  EXPECT_EQ(refusal.reason, "The folder that holds it, " + gone.string() + ", does not exist.");
  EXPECT_TRUE(contains(refusal.fix, "Restart Polaris to create it")) << refusal.fix;
}

TEST(ConfigFileRefusalTests, DirectoryInPlaceOfTheFileIsNamed) {
  config_file_mode_fixture_t fixture {0600};
  const auto folder = fixture.directory / "folder.conf";
  ASSERT_TRUE(std::filesystem::create_directory(folder));
  const auto refusal = refused_read(folder);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::not_regular);
  EXPECT_EQ(refusal.reason, "It is not a regular file.");
  EXPECT_EQ(refusal.fix, "Move it out of the way with \"mv " + folder.string() + ' ' + folder.string() +
                           ".old\" and restart Polaris to write a new settings file.");
}

TEST(ConfigFileRefusalTests, FileThisUserCannotOpenIsNamed) {
  if (::geteuid() == 0) GTEST_SKIP() << "root opens a 0200 file anyway";
  config_file_mode_fixture_t fixture {0200};
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::permission_denied);
  EXPECT_EQ(refusal.reason, "Your user is not allowed to open it: Permission denied.");
  const auto file = fixture.file.string();
  EXPECT_EQ(refusal.fix, "Take it back with \"sudo chown \"$USER\": " + file + "\" and \"chmod 600 " + file +
                           "\", then start Polaris without sudo.");
}

TEST(ConfigFileRefusalTests, UnsafeLockSidecarIsNamedAndMovedAside) {
  config_file_mode_fixture_t fixture {0600};
  const auto lock = fixture.directory / "polaris.conf.lock";
  std::ofstream {fixture.directory / "elsewhere"} << "";
  std::filesystem::create_hard_link(fixture.directory / "elsewhere", lock);
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::lock_unsafe);
  EXPECT_EQ(refusal.reason, "Its lock file, " + lock.string() +
                              ", is not a regular file that your user owns with a single link.");
  EXPECT_EQ(refusal.fix, "Move it out of the way with \"mv " + lock.string() + ' ' + lock.string() +
                           ".old\"; Polaris creates a new one.");
}

TEST(ConfigFileRefusalTests, HeldLockIsNamedAndTheReadWorksOnceItIsReleased) {
  config_file_mode_fixture_t fixture {0600};
  const auto lock = fixture.directory / "polaris.conf.lock";
  const int holder = ::open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  ASSERT_GE(holder, 0);
  ASSERT_EQ(::flock(holder, LOCK_EX), 0);
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::lock_busy);
  EXPECT_EQ(refusal.reason, "Another process is holding its lock file, " + lock.string() + ".");
  EXPECT_TRUE(contains(refusal.fix, "pgrep -a polaris")) << refusal.fix;
  ::close(holder);
  configuration_store::refusal_t after;
  EXPECT_TRUE(configuration_store::read(fixture.file.string(), &after).has_value());
  EXPECT_FALSE(after);
}

TEST(ConfigFileRefusalTests, GroupWritableFolderIsNamedOncePerChangeInOneVoice) {
  config_file_mode_fixture_t fixture {0600};
  ASSERT_EQ(::chmod(fixture.directory.c_str(), 0775), 0);
  settings_log_capture_t capture;
  const auto refusal = refused_read(fixture.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::directory_writable);
  EXPECT_EQ(refusal.reason, "The folder that holds it, " + fixture.directory.string() +
                              ", is writable by group or other users (mode 0775), so Polaris will not keep private state there.");
  EXPECT_EQ(refusal.fix, "Restrict it with \"chmod 700 " + fixture.directory.string() +
                           "\"; a umask of 002 is enough to leave it group writable.");
  // Settings asks again every few seconds while it is open. The store says it
  // once, and the folder walk leaves it to the store rather than repeating it.
  EXPECT_FALSE(configuration_store::read(fixture.file.string()).has_value());
  EXPECT_FALSE(configuration_store::read(fixture.file.string()).has_value());
  const auto log = capture.text();
  EXPECT_EQ(occurrences(log, warning_for(fixture.file) + refusal.reason + ' ' + refusal.fix), 1u) << log;
  EXPECT_EQ(occurrences(log, "Refusing to keep private state in ["), 0u) << log;
}

TEST(ConfigFileRefusalTests, FolderAboveTheFileIsNamedAsOneOnTheWayToIt) {
  // ~/.config left 0775 by a 002 umask refuses ~/.config/polaris/polaris.conf.
  // Calling ~/.config the folder that holds the file sends people to the wrong one.
  config_file_mode_fixture_t fixture {0600};
  const auto inner = fixture.directory / "polaris";
  ASSERT_TRUE(std::filesystem::create_directory(inner));
  ASSERT_EQ(::chmod(inner.c_str(), 0700), 0);
  const auto file = inner / "polaris.conf";
  std::ofstream {file} << "encoder = vaapi\n";
  ASSERT_EQ(::chmod(file.c_str(), 0600), 0);
  ASSERT_EQ(::chmod(fixture.directory.c_str(), 0775), 0);
  const auto refusal = refused_read(file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::directory_writable);
  EXPECT_EQ(refusal.reason, "A folder on the way to it, " + fixture.directory.string() +
                              ", is writable by group or other users (mode 0775), so Polaris will not keep private state there.");
  EXPECT_EQ(refusal.fix, "Restrict it with \"chmod 700 " + fixture.directory.string() +
                           "\"; a umask of 002 is enough to leave it group writable.");
}

TEST(ConfigFileRefusalTests, SymlinkedFolderIsNamedAsALink) {
  // A config folder linked into a dotfiles repository. The walk never follows a
  // link, and opening one as a folder fails with ENOTDIR rather than ELOOP.
  config_file_mode_fixture_t fixture {0600};
  const auto real = fixture.directory / "real";
  ASSERT_TRUE(std::filesystem::create_directory(real));
  ASSERT_EQ(::chmod(real.c_str(), 0700), 0);
  std::ofstream {real / "polaris.conf"} << "encoder = vaapi\n";
  const auto link = fixture.directory / "linked";
  std::filesystem::create_directory_symlink(real, link);
  const auto refusal = refused_read(link / "polaris.conf");
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::directory_symlink);
  EXPECT_EQ(refusal.reason, "The folder that holds it, " + link.string() +
                              ", is a symbolic link, and Polaris only walks real folders to private state.");
  EXPECT_EQ(refusal.fix, "Replace the link with the folder it points to, which \"readlink -f " + link.string() +
                           "\" prints.");
}

TEST(ConfigFileRefusalTests, LockFileInTheWayIsMovedAside) {
  // A folder there fails to open with EISDIR and a link with ELOOP. Both are in
  // the way, and the fix is to move them, not to chown a lock file.
  for (const bool as_link : {false, true}) {
    config_file_mode_fixture_t fixture {0600};
    const auto lock = fixture.directory / "polaris.conf.lock";
    if (as_link) {
      std::ofstream {fixture.directory / "elsewhere"} << "";
      std::filesystem::create_symlink(fixture.directory / "elsewhere", lock);
    } else {
      ASSERT_TRUE(std::filesystem::create_directory(lock));
    }
    const auto refusal = refused_read(fixture.file);
    EXPECT_EQ(refusal.kind, private_state_file::refusal_e::lock_unsafe) << "link " << as_link;
    EXPECT_EQ(refusal.reason, "Its lock file, " + lock.string() +
                                ", is not a regular file that your user owns with a single link.") << "link " << as_link;
    EXPECT_EQ(refusal.fix, "Move it out of the way with \"mv " + lock.string() + ' ' + lock.string() +
                             ".old\"; Polaris creates a new one.") << "link " << as_link;
  }

  // Nothing in the way, but the folder will not take a new lock file.
  if (::geteuid() == 0) return;
  config_file_mode_fixture_t fixture {0600};
  ASSERT_EQ(::chmod(fixture.directory.c_str(), 0500), 0);
  const auto refusal = refused_read(fixture.file);
  ASSERT_EQ(::chmod(fixture.directory.c_str(), 0700), 0);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::lock_unavailable);
  const auto lock = fixture.directory.string() + "/polaris.conf.lock";
  EXPECT_EQ(refusal.reason, "Polaris could not open or lock its lock file, " + lock + ": Permission denied.");
  EXPECT_EQ(refusal.fix, "Take it back with \"sudo chown \"$USER\": " + lock +
                           "\" if another user owns it, and make sure your user can write to the folder that holds it.");
}

TEST(ConfigFileRefusalTests, SocketInPlaceOfTheFileOrItsLockIsNamed) {
  // open(2) refuses a socket with ENXIO, which read as "Reading it failed" and a disk check.
  config_file_mode_fixture_t fixture {0600};
  if (!fits_a_socket_address(fixture.directory / "polaris.conf.lock")) {
    GTEST_SKIP() << "the temporary directory is too deep for a socket address";
  }
  ASSERT_TRUE(std::filesystem::remove(fixture.file));
  {
    bound_socket_t socket {fixture.file};
    ASSERT_TRUE(socket.bound()) << std::strerror(errno);
    const auto refusal = refused_read(fixture.file);
    EXPECT_EQ(refusal.kind, private_state_file::refusal_e::not_regular);
    EXPECT_EQ(refusal.reason, "It is not a regular file.");
    EXPECT_EQ(refusal.fix, "Move it out of the way with \"mv " + fixture.file.string() + ' ' + fixture.file.string() +
                             ".old\" and restart Polaris to write a new settings file.");
  }

  config_file_mode_fixture_t locked {0600};
  const auto lock = locked.directory / "polaris.conf.lock";
  bound_socket_t socket {lock};
  ASSERT_TRUE(socket.bound()) << std::strerror(errno);
  const auto refusal = refused_read(locked.file);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::lock_unsafe);
  EXPECT_EQ(refusal.fix, "Move it out of the way with \"mv " + lock.string() + ' ' + lock.string() +
                           ".old\"; Polaris creates a new one.");
}

TEST(ConfigFileRefusalTests, FileDirectlyUnderTheRootIsNeverGivenAChownOfTheRoot) {
  if (::geteuid() == 0) GTEST_SKIP() << "root owns / and may keep private state there";
  // Only the walk opens /, and it refuses / before anything is created there.
  const auto path = "/polaris-refusal-test-" + std::to_string(::getpid()) + ".conf";
  const auto refusal = refused_read(path);
  EXPECT_EQ(refusal.kind, private_state_file::refusal_e::directory_foreign_owner);
  EXPECT_EQ(refusal.reason, "The folder that holds it, /, is owned by uid 0, and Polaris runs as uid " +
                              std::to_string(::geteuid()) + ", so Polaris will not keep private state there.");
  EXPECT_EQ(refusal.fix, "Keep the settings file in a folder your user owns, such as ~/.config/polaris, "
                         "and start Polaris with that path instead of " + path + ".");

  // A relative path is walked from the folder Polaris runs in. A command that
  // says "." would run against whatever folder the operator's shell is in.
  const auto here = std::filesystem::current_path().string();
  const auto relative = configuration_store::describe_refusal(
    "polaris.conf",
    {.kind = private_state_file::refusal_e::directory_foreign_owner, .directory = ".", .holds_file = true}
  );
  EXPECT_TRUE(contains(relative.reason, "The folder that holds it, " + here + ", ")) << relative.reason;
  EXPECT_TRUE(contains(relative.fix, "sudo chown -R \"$USER\": ")) << relative.fix;
  EXPECT_TRUE(contains(relative.fix, here)) << relative.fix;
  EXPECT_FALSE(contains(relative.fix, ": .\"")) << relative.fix;
}

TEST(ConfigFileRefusalTests, SaveAgainstAMissingFileLeavesItMissing) {
  // A save carries the revision it read, so against a missing file it
  // conflicts. The file did not read, so the log must not say it reads again,
  // and the next read must not warn about it a second time.
  config_file_mode_fixture_t fixture {0600};
  const auto absent = fixture.directory / "absent.conf";
  settings_log_capture_t capture;
  EXPECT_EQ(configuration_store::revision(absent.string(), true), "");
  EXPECT_EQ(configuration_store::replace(absent.string(), "encoder = nvenc\n", ""), configuration_store::result::conflict);
  EXPECT_EQ(configuration_store::patch(absent.string(), {{"encoder", "nvenc"}}, std::string(64, 'a')),
            configuration_store::result::conflict);
  EXPECT_FALSE(configuration_store::read(absent.string()).has_value());
  const auto log = capture.text();
  EXPECT_EQ(occurrences(log, warning_for(absent) + "The settings file does not exist."), 1u) << log;
  EXPECT_EQ(occurrences(log, "can be read again"), 0u) << log;
  EXPECT_FALSE(std::filesystem::exists(absent));
}

TEST(ConfigFileRefusalTests, ReadableFileCarriesNoRefusal) {
  config_file_mode_fixture_t fixture {0644};
  configuration_store::refusal_t refusal {.kind = private_state_file::refusal_e::group_writable, .reason = "stale"};
  const auto snapshot = configuration_store::read(fixture.file.string(), &refusal);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->contents, "encoder = vaapi\n");
  EXPECT_FALSE(refusal);
  EXPECT_EQ(refusal.path, fixture.file.string());
  EXPECT_TRUE(refusal.reason.empty());
  EXPECT_TRUE(refusal.fix.empty());
}

TEST(ConfigFileRefusalTests, EveryRefusalHasAReasonAndAFixInPlainWords) {
  using private_state_file::refusal_e;
  const std::string path = "/srv/polaris/polaris.conf";
  for (auto kind = static_cast<int>(refusal_e::missing); kind <= static_cast<int>(refusal_e::read_failed); ++kind) {
    private_state_file::refusal_t status {
      .kind = static_cast<refusal_e>(kind),
      .error_number = EIO,
      .directory = "/srv/polaris",
      .inspected = true,
      .mode = 0664,
      .owner = 4242,
      .links = 1,
      .size = 12,
    };
    const auto refusal = configuration_store::describe_refusal(path, status);
    EXPECT_EQ(refusal.kind, status.kind) << kind;
    EXPECT_EQ(refusal.path, path) << kind;
    EXPECT_FALSE(refusal.reason.empty()) << kind;
    EXPECT_FALSE(refusal.fix.empty()) << kind;
    for (const std::string_view dash : {"\u2014", "\u2013", " - "}) {
      EXPECT_FALSE(contains(refusal.reason, dash)) << kind << ": " << refusal.reason;
      EXPECT_FALSE(contains(refusal.fix, dash)) << kind << ": " << refusal.fix;
    }
  }

  // The branches a test cannot provoke without root or a racing writer.
  const auto foreign = configuration_store::describe_refusal(path, {.kind = refusal_e::foreign_owner, .inspected = true, .owner = 0});
  EXPECT_TRUE(contains(foreign.reason, "It is owned by uid 0, and Polaris runs as uid " + std::to_string(::geteuid()))) << foreign.reason;
  EXPECT_TRUE(contains(foreign.fix, "sudo chown \"$USER\": " + path)) << foreign.fix;
  const auto shrinking = configuration_store::describe_refusal(path, {.kind = refusal_e::size_changed});
  EXPECT_EQ(shrinking.reason, "It changed size while Polaris was reading it, so something else is writing to it.");
  const auto failing = configuration_store::describe_refusal(path, {.kind = refusal_e::read_failed, .error_number = EIO});
  EXPECT_EQ(failing.reason, "Reading it failed: Input/output error.");
  const auto unlockable = configuration_store::describe_refusal(path, {.kind = refusal_e::lock_unavailable, .error_number = EACCES});
  EXPECT_EQ(unlockable.reason, "Polaris could not open or lock its lock file, " + path + ".lock: Permission denied.");
  const auto owned = configuration_store::describe_refusal(path, {.kind = refusal_e::directory_foreign_owner, .directory = "/srv/polaris"});
  EXPECT_TRUE(contains(owned.fix, "sudo chown -R \"$USER\": /srv/polaris")) << owned.fix;

  // A path a shell would split is quoted in the command and left alone in prose.
  const auto spaced = configuration_store::describe_refusal("/srv/a b/polaris.conf", {.kind = refusal_e::group_writable, .mode = 0664});
  EXPECT_EQ(spaced.fix, "Restrict it with \"chmod go-w '/srv/a b/polaris.conf'\".");
}

TEST(ConfigFileRefusalTests, LogsOneWarningPerChangeOfRefusalNotPerRead) {
  config_file_mode_fixture_t fixture {0664};
  settings_log_capture_t capture;
  for (int request = 0; request < 3; ++request) {
    EXPECT_FALSE(configuration_store::read(fixture.file.string()).has_value());
  }
  auto log = capture.text();
  EXPECT_EQ(occurrences(log, warning_for(fixture.file)), 1u) << log;
  EXPECT_TRUE(contains(log, warning_for(fixture.file) +
                              "It is writable by its group (mode 0664), and the settings store refuses a file another user can change. "
                              "Restrict it with \"chmod go-w " + fixture.file.string() + "\".")) << log;

  ASSERT_EQ(::chmod(fixture.file.c_str(), 0644), 0);
  EXPECT_TRUE(configuration_store::read(fixture.file.string()).has_value());
  EXPECT_TRUE(configuration_store::read(fixture.file.string()).has_value());
  log = capture.text();
  EXPECT_EQ(occurrences(log, "Info: The settings file [" + fixture.file.string() + "] can be read again."), 1u) << log;

  ASSERT_EQ(::chmod(fixture.file.c_str(), 0666), 0);
  EXPECT_FALSE(configuration_store::read(fixture.file.string()).has_value());
  EXPECT_FALSE(configuration_store::read(fixture.file.string()).has_value());
  log = capture.text();
  EXPECT_EQ(occurrences(log, warning_for(fixture.file)), 2u) << log;
  EXPECT_TRUE(contains(log, "writable by every user on this machine (mode 0666)")) << log;
}

TEST(ConfigFileRefusalTests, WriteSideSharesTheReadSidesWordsAndWarning) {
  config_file_mode_fixture_t fixture {0664};
  settings_log_capture_t capture;
  EXPECT_EQ(configuration_store::replace(fixture.file.string(), "encoder = nvenc\n"), configuration_store::result::failed);
  auto log = capture.text();
  EXPECT_EQ(occurrences(log, warning_for(fixture.file)), 1u) << log;
  EXPECT_TRUE(contains(log, "Restrict it with \"chmod go-w " + fixture.file.string() + "\".")) << log;

  // The console reads next; the status has not changed, so the log does not repeat it.
  EXPECT_FALSE(configuration_store::read(fixture.file.string()).has_value());
  EXPECT_EQ(configuration_store::patch(fixture.file.string(), {{"encoder", "nvenc"}}), configuration_store::result::failed);
  log = capture.text();
  EXPECT_EQ(occurrences(log, warning_for(fixture.file)), 1u) << log;

  ASSERT_EQ(::chmod(fixture.file.c_str(), 0644), 0);
  EXPECT_EQ(configuration_store::replace(fixture.file.string(), "encoder = nvenc\n"), configuration_store::result::committed);
  log = capture.text();
  EXPECT_EQ(occurrences(log, "Info: The settings file [" + fixture.file.string() + "] can be read again."), 1u) << log;
}
#endif
