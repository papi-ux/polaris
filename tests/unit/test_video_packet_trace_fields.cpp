/** @file
 * Historical field dataflow and production writer serialization guards.
 * Public operation observations below do not execute SDK calls or prove devices.
 */
#include "src/video_packet_trace_fields.h"
#include "src/video_bitrate_application.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
  #include <fcntl.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
extern char **environ;
#endif

namespace {
  namespace app = video::bitrate_application;
  namespace fields = video::packet_trace_fields;
  namespace pw = video::pyrowave_work;
  using setting_value = std::optional<const video::bitrate_provenance::record_t>;
  using type_e = vrr_trace::field_t::type_e;

  const vrr_trace::field_t *field(const vrr_trace::event_t &event, std::string_view key) {
    for (std::size_t i = 0; i < event.size; ++i) if (event.fields[i].key == key) return &event.fields[i];
    return nullptr;
  }

  void expect_unsigned(const vrr_trace::event_t &event, std::string_view key, std::uint64_t expected) {
    const auto *value = field(event, key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->type, type_e::unsigned_integer);
    EXPECT_EQ(value->u, expected);
  }
  void expect_signed(const vrr_trace::event_t &event, std::string_view key, std::int64_t expected) {
    const auto *value = field(event, key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->type, type_e::signed_integer);
    EXPECT_EQ(value->i, expected);
  }
  void expect_text(const vrr_trace::event_t &event, std::string_view key, std::string_view expected) {
    const auto *value = field(event, key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->type, type_e::text);
    EXPECT_EQ(value->text, expected);
  }
  void expect_null(const vrr_trace::event_t &event, std::string_view key) {
    const auto *value = field(event, key);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->type, type_e::null);
  }
}

TEST(VideoPacketTraceFields, QueuedR1RemainsR1AfterActualNewR2Observation) {
  app::adapter_t instance {7, app::backend_e::native_nvenc};
  { auto operation = instance.begin_native_update(app::origin_t {11, 10000}); ASSERT_TRUE(operation.nvenc_reconfigure_success(10'000'000)); }
  { auto input = instance.capture_before_submit(5); ASSERT_TRUE(input.accepted(app::submit_stage_e::nvenc_encode_picture_success)); }
  const auto old = instance.take_output(app::output_stage_e::nvenc_output_timestamp, 5);
  ASSERT_TRUE(old);
  const setting_value queued {*old};
  { auto operation = instance.begin_native_update(app::origin_t {12, 6000}); ASSERT_TRUE(operation.nvenc_reconfigure_success(6'000'000)); }
  { auto input = instance.capture_before_submit(6); ASSERT_TRUE(input.accepted(app::submit_stage_e::nvenc_encode_picture_success)); }
  const auto fresh = instance.take_output(app::output_stage_e::nvenc_output_timestamp, 6);
  ASSERT_TRUE(fresh);
  instance.retire();  // Queued values also survive retirement of their producer.
  vrr_trace::event_t event {"packet_dequeue"};
  fields::append(event, queued, {});
  expect_unsigned(event, "frame01_available", 1);
  expect_unsigned(event, "frame01_encoder_lifetime", old->scope().encoder_lifetime);
  expect_unsigned(event, "frame01_stream_generation", 7);
  expect_unsigned(event, "frame01_origin_revision", 11);
  expect_unsigned(event, "frame01_application_sequence", old->application_sequence());
  expect_unsigned(event, "frame01_submission_sequence", old->submission_sequence());
  expect_signed(event, "frame01_configured_encoder_kbps", 10000);
  expect_signed(event, "frame01_input_pts", 5);
  expect_text(event, "frame01_receipt_origin", "backend_reconfiguration");
  EXPECT_NE(field(event, "frame01_origin_revision")->u, fresh->authority_revision());
  expect_unsigned(event, "pyrowork_available", 0);
  expect_null(event, "pyrowork_budget_argument_bytes");
  expect_text(event, "record_stage", "sender_packet_dequeue");
  EXPECT_FALSE(event.overflow);
}

TEST(VideoPacketTraceFields, MissingRecordsRemainNullDespiteExistingTraceIdentities) {
  auto event = vrr_trace::event_t("packet_dequeue").u("packet", 901).u("encoder", 902).u("submission", 903);
  fields::append(event, {}, {});
  expect_unsigned(event, "frame01_available", 0);
  expect_unsigned(event, "pyrowork_available", 0);
  for (const char *key : {"frame01_encoder_lifetime", "frame01_stream_generation", "frame01_application_sequence",
                         "frame01_origin_revision", "frame01_configured_encoder_kbps", "frame01_input_pts",
                         "frame01_submission_sequence", "frame01_receipt_origin", "pyrowork_authority",
                         "pyrowork_provenance", "pyrowork_kind", "pyrowork_sequence", "pyrowork_handoff_frame_index",
                         "pyrowork_budget_argument_bytes", "pyrowork_completed_bitstream_bytes", "pyrowork_packet_copy_bytes"}) {
    expect_null(event, key);
  }
  expect_unsigned(event, "packet", 901);
  expect_unsigned(event, "encoder", 902);
  expect_unsigned(event, "submission", 903);
  EXPECT_FALSE(event.overflow);
}

TEST(VideoPacketTraceFields, QueuedPyroWaveWorkKeepsByteBudgetAndUnknownAuthority) {
  pw::adapter_t instance;
  const std::vector<std::uint8_t> buffer {1, 2, 3};
  const std::vector<std::uint8_t> packet_copy(buffer);
  auto work = instance.begin_work(pw::kind_e::packed, 7001);
  instance.complete_work(std::move(work), true, buffer);
  instance.prepare_cached_buffer(buffer);
  const auto queued = instance.take_for_packet(5, buffer, packet_copy);
  ASSERT_TRUE(queued);
  auto newer_work = instance.begin_work(pw::kind_e::retained, 9007);
  ASSERT_TRUE(instance.complete_retained(std::move(newer_work), true, buffer));
  const auto newer = instance.take_for_packet(6, buffer, packet_copy);
  ASSERT_TRUE(newer);
  // The existing common event has eight fields before the shared extension.
  auto event = vrr_trace::event_t("packet_dequeue").u("packet", 1).u("encoder", 2).u("submission", 3)
    .i("frame_index", 5).n("frame_timestamp_ns").n("encode_done_ns")
    .u("source_clock_timestamp", 0).u("source_time_paced", 0);
  fields::append(event, {}, queued);
  expect_unsigned(event, "frame01_available", 0);
  expect_null(event, "frame01_configured_encoder_kbps");
  expect_unsigned(event, "pyrowork_available", 1);
  expect_unsigned(event, "pyrowork_budget_argument_bytes", 7001);
  EXPECT_NE(field(event, "pyrowork_budget_argument_bytes")->u, newer->effective_budget_argument_bytes());
  expect_unsigned(event, "pyrowork_sequence", queued->local_work_sequence());
  expect_unsigned(event, "pyrowork_completed_bitstream_bytes", buffer.size());
  expect_unsigned(event, "pyrowork_packet_copy_bytes", buffer.size());
  expect_signed(event, "pyrowork_handoff_frame_index", 5);
  expect_text(event, "pyrowork_kind", "packed");
  expect_text(event, "pyrowork_authority", "unknown");
  expect_text(event, "pyrowork_provenance", "wrapper_call_success_nonempty");
  expect_text(event, "record_stage", "sender_packet_dequeue");
  EXPECT_FALSE(event.overflow);
  EXPECT_LE(event.size, vrr_trace::k_fields);
}

TEST(VideoPacketTraceFields, UsableExactAvcodecOpenIsDistinctFromUnattributedNativeOpen) {
  app::adapter_t ffmpeg {7, app::backend_e::avcodec};
  const app::avcodec_rate_t rate {10'000'000, 10'000'000, 10'000'000, false};
  ffmpeg.avcodec_open_success(app::origin_t {11, 10000}, rate, rate);
  ASSERT_TRUE(ffmpeg.publish_usable_avcodec_open());
  { auto input = ffmpeg.capture_before_submit(5); ASSERT_TRUE(input.accepted(app::submit_stage_e::avcodec_send_frame_success)); }
  const auto output = ffmpeg.take_output(app::output_stage_e::avcodec_packet_pts, 5);
  ASSERT_TRUE(output);
  vrr_trace::event_t event {"packet_dequeue"};
  fields::append(event, setting_value {*output}, {});
  expect_text(event, "frame01_receipt_origin", "successful_open");
  expect_signed(event, "frame01_input_pts", 5);
  expect_signed(event, "frame01_configured_encoder_kbps", 10000);

  app::adapter_t native {7, app::backend_e::native_nvenc};
  native.nvenc_initialize_success();
  { auto input = native.capture_before_submit(5); EXPECT_FALSE(input.accepted(app::submit_stage_e::nvenc_encode_picture_success)); }
  EXPECT_FALSE(native.take_output(app::output_stage_e::nvenc_output_timestamp, 5));
  vrr_trace::event_t unknown {"packet_dequeue"};
  fields::append(unknown, {}, {});
  expect_unsigned(unknown, "frame01_available", 0);
  expect_null(unknown, "frame01_receipt_origin");
}

TEST(VideoPacketTraceFields, EveryWorkKindHasStaticTextAndExactlyTwentySevenUniqueSlots) {
  const std::array<std::pair<pw::kind_e, std::string_view>, 5> kinds {{
    {pw::kind_e::imported, "imported"}, {pw::kind_e::blank_primer, "blank_primer"},
    {pw::kind_e::blank_hdr, "blank_hdr"}, {pw::kind_e::packed, "packed"}, {pw::kind_e::retained, "retained"}
  }};
  pw::adapter_t instance;
  const std::vector<std::uint8_t> buffer {1, 2, 3};
  const std::vector<std::uint8_t> copy(buffer);
  std::int64_t pts = 5;
  for (const auto &[kind, name] : kinds) {
    SCOPED_TRACE(name);
    auto work = instance.begin_work(kind, 2);  // Output may exceed an argument; it is not a cap or measured rate.
    if (kind == pw::kind_e::retained) {
      ASSERT_TRUE(instance.complete_retained(std::move(work), true, buffer));
    } else {
      instance.complete_work(std::move(work), true, buffer);
      instance.prepare_cached_buffer(buffer);
    }
    const auto receipt = instance.take_for_packet(pts, buffer, copy);
    ASSERT_TRUE(receipt);
    auto event = vrr_trace::event_t("packet_dequeue").u("packet", 1).u("encoder", 0).u("submission", 0)
      .i("frame_index", pts).n("frame_timestamp_ns").n("encode_done_ns")
      .u("source_clock_timestamp", 0).u("source_time_paced", 0);
    fields::append(event, {}, receipt);
    expect_text(event, "pyrowork_kind", name);
    expect_text(event, "pyrowork_authority", "unknown");
    expect_unsigned(event, "pyrowork_budget_argument_bytes", 2);
    expect_unsigned(event, "pyrowork_completed_bitstream_bytes", 3);
    expect_signed(event, "pyrowork_handoff_frame_index", pts);
    EXPECT_EQ(event.size, 27u);
    EXPECT_FALSE(event.overflow);
    std::set<std::string_view> keys;
    for (std::size_t i = 0; i < event.size; ++i) keys.insert(event.fields[i].key);
    EXPECT_EQ(keys.size(), 27u);
    ++pts;
  }
}

#ifdef __linux__
namespace {
  using json = nlohmann::json;
  struct recording_t {
    std::filesystem::path directory;
    std::filesystem::path path;
    std::atomic<bool> hold {true};
    std::unique_ptr<vrr_trace::recorder_t> recorder;
    recording_t() {
      char scratch[] = "/tmp/polaris-packet-fields-XXXXXX";
      const auto *created = ::mkdtemp(scratch);
      if (!created) throw std::runtime_error("mkdtemp failed");
      directory = created;
      path = directory / "trace.jsonl";
      vrr_trace::limits_t limits;
      limits.test_hold_writer = &hold;
      recorder = vrr_trace::recorder_t::open(path.string(), limits);
    }
    ~recording_t() {
      hold.store(false);
      if (recorder) recorder->stop();
      std::filesystem::remove_all(directory);
    }
    void stop() { hold.store(false); recorder->stop(); }
    std::string bytes() const {
      std::ifstream input(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
  };
  std::string sha256(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) != 1) throw std::runtime_error("sha256 failed");
    std::string output;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned index = 0; index < size; ++index) { output += hex[digest[index] >> 4]; output += hex[digest[index] & 15]; }
    return output;
  }
}
#endif

TEST(VideoPacketTraceFields, ProductionRecorderRoundTripsMinorThreeNullAndHistoricalPositiveFields) {
#ifdef __linux__
  recording_t recording;
  ASSERT_TRUE(recording.recorder);
  ASSERT_EQ(recording.recorder->status(), vrr_trace::status_e::running);
  app::adapter_t ffmpeg {7, app::backend_e::avcodec};
  const app::avcodec_rate_t rate {10'000'000, 10'000'000, 10'000'000, false};
  ffmpeg.avcodec_open_success(app::origin_t {11, 10000}, rate, rate);
  ASSERT_TRUE(ffmpeg.publish_usable_avcodec_open());
  { auto input = ffmpeg.capture_before_submit(5); ASSERT_TRUE(input.accepted(app::submit_stage_e::avcodec_send_frame_success)); }
  const auto setting = ffmpeg.take_output(app::output_stage_e::avcodec_packet_pts, 5);
  ASSERT_TRUE(setting);
  app::adapter_t native {8, app::backend_e::native_nvenc};
  { auto operation = native.begin_native_update(app::origin_t {12, 6000}); ASSERT_TRUE(operation.nvenc_reconfigure_success(6'000'000)); }
  { auto input = native.capture_before_submit(6); ASSERT_TRUE(input.accepted(app::submit_stage_e::nvenc_encode_picture_success)); }
  const auto reconfigured = native.take_output(app::output_stage_e::nvenc_output_timestamp, 6);
  ASSERT_TRUE(reconfigured);
  pw::adapter_t wrapper;
  const std::vector<std::uint8_t> buffer {1, 2, 3};
  const std::vector<std::uint8_t> copy(buffer);
  auto operation = wrapper.begin_work(pw::kind_e::packed, 7001);
  wrapper.complete_work(std::move(operation), true, buffer);
  wrapper.prepare_cached_buffer(buffer);
  const auto work = wrapper.take_for_packet(5, buffer, copy);
  ASSERT_TRUE(work);
  const auto make_event = [](std::uint64_t id, std::int64_t pts) {
    return vrr_trace::event_t("packet_dequeue").u("packet", id).u("encoder", 902).u("submission", 903)
      .i("frame_index", pts).n("frame_timestamp_ns").n("encode_done_ns")
      .u("source_clock_timestamp", 0).u("source_time_paced", 0);
  };
  {
    vrr_trace::test_recorder_scope_t enabled(recording.recorder.get());
    ASSERT_TRUE(vrr_trace::enabled());
    auto absent = make_event(901, std::numeric_limits<std::int64_t>::min()); fields::append(absent, {}, {}); vrr_trace::record(absent);
    auto positive = make_event(904, 5); fields::append(positive, setting_value {*setting}, work); vrr_trace::record(positive);
    auto update = make_event(905, 6); fields::append(update, setting_value {*reconfigured}, {}); vrr_trace::record(update);
  }
  {
    vrr_trace::test_recorder_scope_t disabled(nullptr);
    EXPECT_FALSE(vrr_trace::enabled());
    vrr_trace::record(make_event(999, 7));  // Off scope must not add a fourth row.
  }
  recording.stop();
  const auto bytes = recording.bytes();
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(bytes.back(), '\n');
  std::istringstream input(bytes);
  std::vector<json> rows;
  std::string line;
  while (std::getline(input, line)) rows.push_back(json::parse(line));
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows.front().at("major"), 2);
  EXPECT_EQ(rows.front().at("minor"), 4);
  for (std::size_t index = 1; index <= 3; ++index) {
    EXPECT_EQ(rows[index].size(), 30u);
    EXPECT_EQ(rows[index].at("order"), index);
    EXPECT_EQ(rows[index].at("type"), "packet_dequeue");
  }
  EXPECT_EQ(rows[1].at("encoder"), 902);
  EXPECT_EQ(rows[1].at("frame_index"), std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(rows[1].at("frame01_available"), 0);
  EXPECT_EQ(rows[1].at("pyrowork_available"), 0);
  for (const char *key : {"frame01_encoder_lifetime", "frame01_stream_generation", "frame01_application_sequence",
                         "frame01_origin_revision", "frame01_configured_encoder_kbps", "frame01_input_pts",
                         "frame01_submission_sequence", "frame01_receipt_origin", "pyrowork_authority",
                         "pyrowork_provenance", "pyrowork_kind", "pyrowork_sequence", "pyrowork_handoff_frame_index",
                         "pyrowork_budget_argument_bytes", "pyrowork_completed_bitstream_bytes", "pyrowork_packet_copy_bytes"}) {
    EXPECT_TRUE(rows[1].at(key).is_null());
  }
  const auto &positive = rows[2];
  EXPECT_EQ(positive.at("frame01_available"), 1);
  EXPECT_EQ(positive.at("frame01_receipt_origin"), "successful_open");
  EXPECT_EQ(positive.at("frame01_encoder_lifetime"), setting->scope().encoder_lifetime);
  EXPECT_EQ(positive.at("frame01_stream_generation"), 7);
  EXPECT_EQ(positive.at("frame01_application_sequence"), setting->application_sequence());
  EXPECT_EQ(positive.at("frame01_origin_revision"), 11);
  EXPECT_EQ(positive.at("frame01_configured_encoder_kbps"), 10000);
  EXPECT_EQ(positive.at("frame01_input_pts"), 5);
  EXPECT_EQ(positive.at("frame01_submission_sequence"), setting->submission_sequence());
  EXPECT_TRUE(positive.at("frame01_configured_encoder_kbps").is_number_integer());
  EXPECT_EQ(positive.at("pyrowork_available"), 1);
  EXPECT_EQ(positive.at("pyrowork_authority"), "unknown");
  EXPECT_EQ(positive.at("pyrowork_provenance"), "wrapper_call_success_nonempty");
  EXPECT_EQ(positive.at("pyrowork_kind"), "packed");
  EXPECT_EQ(positive.at("pyrowork_sequence"), work->local_work_sequence());
  EXPECT_EQ(positive.at("pyrowork_handoff_frame_index"), 5);
  EXPECT_EQ(positive.at("pyrowork_budget_argument_bytes"), 7001);
  EXPECT_EQ(positive.at("pyrowork_completed_bitstream_bytes"), 3);
  EXPECT_EQ(positive.at("pyrowork_packet_copy_bytes"), 3);
  EXPECT_EQ(rows[3].at("frame01_receipt_origin"), "backend_reconfiguration");
  EXPECT_EQ(rows[3].at("frame01_origin_revision"), 12);
  const auto &footer = rows.back();
  EXPECT_TRUE(footer.at("producer_complete").get<bool>());
  for (const char *key : {"attempts", "admitted", "event_rows"}) EXPECT_EQ(footer.at(key), 3);
  for (const char *key : {"lost_contention_or_full", "event_cap_loss", "invalid_event_loss", "byte_cap_loss", "instrumentation_refusals"}) EXPECT_EQ(footer.at(key), 0);
  const auto before_footer = footer.at("bytes_before_footer").get<std::size_t>();
  EXPECT_EQ(footer.at("content_sha256"), sha256(std::string_view(bytes).substr(0, before_footer)));
  std::ifstream close_input(recording.path.string() + ".close.json");
  const auto close = json::parse(close_input);
  EXPECT_EQ(close.at("main_bytes"), bytes.size());
  EXPECT_EQ(close.at("main_sha256"), sha256(bytes));
  EXPECT_TRUE(close.at("footer_written").get<bool>());
  EXPECT_EQ(close.at("write_errno"), 0);
  EXPECT_EQ(close.at("main_close_errno"), 0);
  EXPECT_EQ(close.at("durability"), "no_fsync_claim");
  EXPECT_EQ(close.at("receipt_own_close"), "not_observable_in_this_file");
#else
  GTEST_SKIP() << "Production recorder round-trip is Linux-only";
#endif
}

TEST(VideoPacketTraceFields, StrictPythonReaderFixturesRunOnceOnTheSelectedNativeRoute) {
#if defined(__unix__) || defined(__APPLE__)
  const auto script = (std::filesystem::path(POLARIS_SOURCE_DIR) / "scripts/validation/live-tuning/test_inspect_frame_records.py").string();
  // Inherit stdout/stderr so verbose inner cases, counts and failures reach CTest.
  std::array<char *, 5> arguments {{const_cast<char *>("python3"), const_cast<char *>("-B"),
    const_cast<char *>(script.c_str()), const_cast<char *>("-v"), nullptr}};
  pid_t child = -1;
  ASSERT_EQ(::posix_spawnp(&child, "python3", nullptr, nullptr, arguments.data(), environ), 0);
  int wait_status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &wait_status, 0); } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(wait_status));
  EXPECT_EQ(WEXITSTATUS(wait_status), 0);
#else
  GTEST_SKIP() << "Python fixture subprocess route requires Unix";
#endif
}

TEST(VideoPacketTraceFields, ProductionRecorderTwoFourBytesAreRefusedByTheFrozenTwoThreeInspector) {
#ifdef __linux__
  recording_t recording;
  ASSERT_TRUE(recording.recorder);
  ASSERT_EQ(recording.recorder->status(), vrr_trace::status_e::running);
  app::adapter_t ffmpeg {7, app::backend_e::avcodec};
  const app::avcodec_rate_t rate {10'000'000, 10'000'000, 10'000'000, false};
  ffmpeg.avcodec_open_success(app::origin_t {11, 10000}, rate, rate);
  ASSERT_TRUE(ffmpeg.publish_usable_avcodec_open());
  { auto input = ffmpeg.capture_before_submit(5); ASSERT_TRUE(input.accepted(app::submit_stage_e::avcodec_send_frame_success)); }
  const auto setting = ffmpeg.take_output(app::output_stage_e::avcodec_packet_pts, 5);
  ASSERT_TRUE(setting);
  pw::adapter_t wrapper;
  const std::vector<std::uint8_t> buffer {1, 2, 3};
  const std::vector<std::uint8_t> copy(buffer);
  auto operation = wrapper.begin_work(pw::kind_e::packed, 7001);
  wrapper.complete_work(std::move(operation), true, buffer);
  wrapper.prepare_cached_buffer(buffer);
  const auto work = wrapper.take_for_packet(5, buffer, copy);
  ASSERT_TRUE(work);
  auto absent = vrr_trace::event_t("packet_dequeue").u("packet", 901).u("encoder", 902).u("submission", 903)
    .i("frame_index", std::numeric_limits<std::int64_t>::min()).n("frame_timestamp_ns").n("encode_done_ns")
    .u("source_clock_timestamp", 0).u("source_time_paced", 0);
  fields::append(absent, {}, {});
  recording.recorder->record(absent);
  auto positive = vrr_trace::event_t("packet_dequeue").u("packet", 904).u("encoder", 0).u("submission", 0)
    .i("frame_index", 5).n("frame_timestamp_ns").n("encode_done_ns")
    .u("source_clock_timestamp", 0).u("source_time_paced", 0);
  fields::append(positive, setting_value {*setting}, work);
  recording.recorder->record(positive);
  recording.stop();

  // The actual production bytes are a complete 2.4 capture, so the refusal below is the version.
  std::istringstream lines(recording.bytes());
  std::string first, line, last;
  ASSERT_TRUE(std::getline(lines, first));
  while (std::getline(lines, line)) last = line;
  const auto head = json::parse(first);
  const auto footer = json::parse(last);
  EXPECT_EQ(head.at("schema"), "polaris.vrr-host-trace");
  EXPECT_EQ(head.at("major"), 2);
  EXPECT_EQ(head.at("minor"), 4);
  EXPECT_EQ(footer.at("type"), "footer");
  EXPECT_TRUE(footer.at("producer_complete").get<bool>());

  // The frozen 2.3-only inspector, unchanged: argument list, no shell. Only the child's stdout is
  // redirected, to a new private file in this test's own temporary directory (removed with it).
  const auto script = (std::filesystem::path(POLARIS_SOURCE_DIR) / "scripts/validation/live-tuning/inspect_frame_records.py").string();
  const auto raw_path = recording.path.string();
  const auto close_path = raw_path + ".close.json";
  const auto output_path = (recording.directory / "inspector.stdout").string();
  std::array<char *, 6> arguments {{const_cast<char *>("python3"), const_cast<char *>("-B"),
    const_cast<char *>(script.c_str()), const_cast<char *>(raw_path.c_str()), const_cast<char *>(close_path.c_str()), nullptr}};
  posix_spawn_file_actions_t actions;
  ASSERT_EQ(::posix_spawn_file_actions_init(&actions), 0);
  const int opened = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_path.c_str(),
    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  pid_t child = -1;
  const int spawned = opened == 0 ? ::posix_spawnp(&child, "python3", &actions, nullptr, arguments.data(), environ) : opened;
  ::posix_spawn_file_actions_destroy(&actions);
  ASSERT_EQ(opened, 0);
  ASSERT_EQ(spawned, 0);
  int wait_status = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &wait_status, 0); } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(wait_status));
  // The reader's own non-complete exit: a missing interpreter (127) or a crash is a different failure.
  EXPECT_EQ(WEXITSTATUS(wait_status), 1);
  std::ifstream output_input(output_path, std::ios::binary);
  const std::string output {std::istreambuf_iterator<char>(output_input), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(output.empty());
  EXPECT_EQ(output.back(), '\n');
  EXPECT_EQ(std::count(output.begin(), output.end(), '\n'), 1);
  const auto verdict = json::parse(output);
  EXPECT_EQ(verdict.at("schema"), "polaris.packet-historical-receipts-inspection/v1");
  EXPECT_EQ(verdict.at("status"), "INCONCLUSIVE");
  EXPECT_EQ(verdict.at("reason"), "unsupported_version");
  EXPECT_TRUE(verdict.at("records").is_null());
  EXPECT_FALSE(verdict.at("physical_acceptance").get<bool>());
#else
  GTEST_SKIP() << "Actual recorder/inspector interoperability requires Linux";
#endif
}
