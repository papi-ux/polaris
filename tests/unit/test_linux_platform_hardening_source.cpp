/**
 * @file tests/unit/test_linux_platform_hardening_source.cpp
 * @brief Source contracts for Linux backends omitted by some build variants.
 */
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

  std::string read_source(const std::filesystem::path &relative_path) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path);
    EXPECT_TRUE(input.good()) << relative_path;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

}  // namespace

TEST(LinuxPlatformHardeningSource, CageCapturePublishesMetadataAtomicallyAndJoins) {
  const auto source = read_source("src/platform/linux/cage_screencopy.cpp");
  EXPECT_NE(source.find("published_frame_t"), std::string::npos);
  const auto frame_timestamp = source.find("img_out->frame_timestamp = std::chrono::steady_clock::now();");
  const auto frame_metadata = source.find("img_out->frame_metadata = {", frame_timestamp);
  const auto metadata_update = source.find("stream_stats::update_capture_metadata(img_out->frame_metadata);", frame_metadata);
  const auto frame_publish = source.find("push_cb(std::move(img_out), true)", metadata_update);
  ASSERT_NE(frame_timestamp, std::string::npos);
  ASSERT_NE(frame_metadata, std::string::npos);
  ASSERT_NE(metadata_update, std::string::npos);
  ASSERT_NE(frame_publish, std::string::npos);
  EXPECT_LT(frame_timestamp, frame_metadata);
  EXPECT_LT(frame_metadata, metadata_update);
  EXPECT_LT(metadata_update, frame_publish);
  EXPECT_NE(source.find(".transport = platf::frame_transport_e::shm", frame_metadata), std::string::npos);
  EXPECT_NE(source.find(".residency = platf::frame_residency_e::cpu", frame_metadata), std::string::npos);
  EXPECT_NE(source.find(".format = platf::frame_format_e::bgra8", frame_metadata), std::string::npos);
  EXPECT_NE(source.find("::poll(&display_poll"), std::string::npos);
  EXPECT_NE(source.find("sc_thread.join()"), std::string::npos);
  EXPECT_EQ(source.find("sc_thread.detach()"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, TimedOutWlrCaptureCancelsItsRequest) {
  const auto header = read_source("src/platform/linux/wayland.h");
  const auto capture = read_source("src/platform/linux/wlgrab.cpp");
  EXPECT_NE(header.find("pending_frame"), std::string::npos);
  EXPECT_NE(header.find("pending_buffer_create"), std::string::npos);
  EXPECT_NE(capture.find("dmabuf.cancel()"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, HeadlessVaapiFailsClosedBeforeDmabufInitialization) {
  const auto policy = read_source("src/platform/linux/wlgrab_capture_policy.h");
  const auto capture = read_source("src/platform/linux/wlgrab.cpp");
  const auto process = read_source("src/process.cpp");

  EXPECT_NE(policy.find("hwdevice_type == platf::mem_type_e::cuda"), std::string::npos);
  EXPECT_NE(policy.find("hwdevice_type == platf::mem_type_e::vulkan"), std::string::npos);
  EXPECT_EQ(policy.find("hwdevice_type == platf::mem_type_e::vaapi"), std::string::npos);
  EXPECT_NE(capture.find("vaapi_headless_dmabuf_disabled_for_stability"), std::string::npos);
  EXPECT_NE(capture.find("true-headless ext-image-copy-capture DMA-BUF is disabled for VAAPI stability"), std::string::npos);
  EXPECT_NE(process.find("if (!headless_attempt.failure_reason.empty())"), std::string::npos);
  EXPECT_NE(process.find("Retaining headless extcopy failure reason"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, PipeWireVaapiDefaultsToShmWithExplicitOptInAndForcedCpuPrecedence) {
  const auto policy = read_source("src/platform/linux/pipewire_capture.cpp");
  const auto capture = read_source("src/platform/linux/portal_grab.cpp");

  EXPECT_NE(policy.find("override == dmabuf_override_e::allow_vaapi && eligibility.mem_type == platf::mem_type_e::vaapi"), std::string::npos);
  EXPECT_NE(policy.find("dmabuf_override_from_env(std::getenv(\"POLARIS_PORTAL_DMABUF\"))"), std::string::npos);
  EXPECT_NE(capture.find("portal_dmabuf_override"), std::string::npos);
  EXPECT_NE(capture.find("vaapi_pipewire_dmabuf_disabled_for_stability"), std::string::npos);
  EXPECT_NE(capture.find("vaapi_pipewire_dmabuf_explicitly_enabled"), std::string::npos);

  const std::string offer_call = "may_offer_dmabuf(eligibility, dmabuf_override)";
  const auto local_offer = capture.find(offer_call);
  ASSERT_NE(local_offer, std::string::npos);
  EXPECT_NE(capture.find(offer_call, local_offer + offer_call.size()), std::string::npos)
    << "both local-graph and portal-remote capture must apply the same explicit policy";

  EXPECT_NE(capture.find("query_egl_dmabuf_import_formats(*encoder_render_node)"), std::string::npos)
    << "the explicit VAAPI opt-in must retain the existing EGL modifier capability gate";
}

TEST(LinuxPlatformHardeningSource, HeadlessModifierGuardRemainsBehindVaapiContainment) {
  const auto header = read_source("src/platform/linux/wayland.h");
  const auto capture = read_source("src/platform/linux/wlgrab.cpp");

  EXPECT_NE(header.find("std::optional<std::uint64_t> capture_modifier() const"), std::string::npos);

  const auto headless_probe = capture.find("if (try_headless_extcopy_dmabuf && gpu_native_capture_supported)");
  ASSERT_NE(headless_probe, std::string::npos);
  const auto modifier = capture.find("wlr->extcopy.capture_modifier()", headless_probe);
  const auto policy = capture.find("gpu_native_dmabuf_is_safe(", modifier);
  const auto accepted = capture.find("update_headless_extcopy_dmabuf_probe_result(true)", policy);
  ASSERT_NE(modifier, std::string::npos);
  ASSERT_NE(policy, std::string::npos);
  ASSERT_NE(accepted, std::string::npos);
  EXPECT_LT(modifier, policy);
  EXPECT_LT(policy, accepted);
  EXPECT_NE(capture.find("vaapi_headless_modifier_not_linear", policy), std::string::npos);
  EXPECT_NE(capture.find("vaapi_headless_modifier_unavailable", policy), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, CudaFormatTransitionReleasesCompleteDestination) {
  const auto source = read_source("src/platform/linux/cuda.cpp");
  const auto transition = source.find("source_fourcc != descriptor.sd.fourcc");
  ASSERT_NE(transition, std::string::npos);
  EXPECT_NE(source.find("release_destination();", transition), std::string::npos);

  const auto release = source.find("void release_destination()");
  ASSERT_NE(release, std::string::npos);
  EXPECT_NE(source.find("vkDestroyBuffer", release), std::string::npos);
  EXPECT_NE(source.find("vkFreeMemory", release), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, NetlinkReadsAlwaysStartAtBufferBase) {
  const auto source = read_source("src/platform/linux/misc.cpp");
  EXPECT_NE(source.find("::recv(fd, buffer, sizeof(buffer), 0)"), std::string::npos);
  EXPECT_EQ(source.find("recv(fd, nlMsg"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, KmsCardRejectsAnUnusableRenderDescriptor) {
  const auto source = read_source("src/platform/linux/kmsgrab.cpp");
  const auto dup_fallback = source.find("render_fd.el = dup(fd.el);");
  ASSERT_NE(dup_fallback, std::string::npos);

  // init() must fail rather than publish render_fd == -1 to va::validate().
  const auto guard = source.find("if (render_fd.el < 0) {", dup_fallback);
  ASSERT_NE(guard, std::string::npos);
  EXPECT_NE(source.find("return -1;", guard), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, KmsCapturePublishesDirectAndReadbackMetadata) {
  const auto source = read_source("src/platform/linux/kmsgrab.cpp");
  const auto publisher = source.find("void publish_capture_metadata(platf::img_t &img, bool gpu_resident)");
  ASSERT_NE(publisher, std::string::npos);
  EXPECT_NE(source.find("stream_stats::update_capture_metadata(img.frame_metadata);", publisher), std::string::npos);
  EXPECT_NE(source.find("publish_capture_metadata(*img_out, false);", publisher), std::string::npos);
  EXPECT_NE(source.find("publish_capture_metadata(*img, true);", publisher), std::string::npos);
  EXPECT_NE(source.find("render_node = rendernode_path;"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, VulkanVideoPreservesRadvOptionsAndInitializesProbeFrames) {
  const auto platform = read_source("src/platform/linux/misc.cpp");
  EXPECT_NE(platform.find("append_environment_token(\"RADV_PERFTEST\", \"video_encode\")"), std::string::npos);
  EXPECT_NE(platform.find("append_environment_token(\"RADV_EXPERIMENTAL\", \"video_encode\")"), std::string::npos);
  EXPECT_NE(platform.find("current + ',' + std::string {token}"), std::string::npos)
    << "existing RADV options must be preserved";

  const auto encoder = read_source("src/platform/linux/vulkan_encode.cpp");
  const auto target_views = encoder.find("if (!target.views_created)");
  const auto dummy_frame = encoder.find("descriptor->sequence == 0", target_views);
  const auto blank_dispatch = encoder.find("return dispatch_compute(true);", dummy_frame);
  ASSERT_NE(target_views, std::string::npos);
  ASSERT_NE(dummy_frame, std::string::npos);
  ASSERT_NE(blank_dispatch, std::string::npos);
  EXPECT_LT(target_views, dummy_frame);
  EXPECT_LT(dummy_frame, blank_dispatch);
  EXPECT_NE(encoder.find("const std::array<uint8_t, 4> transparent_pixel = {}"), std::string::npos);
  EXPECT_NE(encoder.find("if (!blank) {", encoder.find("int dispatch_compute(bool blank,")), std::string::npos);
  EXPECT_NE(encoder.find("create_ram_upload_resources()"), std::string::npos);
  EXPECT_NE(encoder.find("stage_ram_frame(slot, *ram_img)"), std::string::npos);
  EXPECT_NE(encoder.find("VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT"), std::string::npos);
  EXPECT_NE(encoder.find("vk_frame->access[i] = static_cast<VkAccessFlagBits>(0);"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, VulkanTeardownClosesCodecBeforeConverterResources) {
  const auto video = read_source("src/video.cpp");
  const auto destructor = video.find("~avcodec_encode_session_t()");
  const auto codec_close = video.find("avcodec_ctx.reset();", destructor);
  const auto converter_close = video.find("converter.reset();", codec_close);
  ASSERT_NE(destructor, std::string::npos);
  ASSERT_NE(codec_close, std::string::npos);
  ASSERT_NE(converter_close, std::string::npos);
  EXPECT_LT(codec_close, converter_close)
    << "FFmpeg must release codec-owned Vulkan picture views before converter resources";
  EXPECT_EQ(video.find("release_encode_resources", destructor), std::string::npos);

  const auto platform = read_source("src/platform/common.h");
  EXPECT_EQ(platform.find("release_encode_resources"), std::string::npos);

  const auto encoder = read_source("src/platform/linux/vulkan_encode.cpp");
  const auto vulkan_destructor = encoder.find("~vk_vram_t() override");
  const auto cleanup = encoder.find("cleanup_pipeline();", vulkan_destructor);
  ASSERT_NE(vulkan_destructor, std::string::npos);
  ASSERT_NE(cleanup, std::string::npos);
  EXPECT_NE(encoder.find("Vulkan converter teardown: device idle status=", cleanup), std::string::npos);
  EXPECT_NE(encoder.find("Vulkan converter teardown: complete", cleanup), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, KscreenConfigurationNeverPassesThroughShell) {
  const auto topology = read_source("src/platform/linux/display_topology.cpp");
  EXPECT_EQ(topology.find("std::system"), std::string::npos);
  EXPECT_NE(topology.find("platf::run_process_argv(argv)"), std::string::npos);

  const auto virtual_display = read_source("src/platform/linux/virtual_display.cpp");
  const auto kscreen = virtual_display.find("namespace kscreen");
  ASSERT_NE(kscreen, std::string::npos);
  EXPECT_NE(virtual_display.find("platf::run_process_argv(args)", kscreen), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, VaapiValidatesExportedDescriptorBeforeIndexing) {
  const auto source = read_source("src/platform/linux/vaapi.cpp");
  EXPECT_NE(source.find("DRMPRIMESurfaceDescriptor prime {}"), std::string::npos);
  EXPECT_NE(source.find("prime.num_objects > max_objects"), std::string::npos);
  EXPECT_NE(source.find("layer.num_planes > max_planes"), std::string::npos);
  EXPECT_NE(source.find("layer.object_index[x] >= prime.num_objects"), std::string::npos);
  EXPECT_NE(source.find("vaSetInfoCallback"), std::string::npos);
  EXPECT_NE(source.find("bytes > 0"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, X11ShmRefreshStaysOnCaptureThread) {
  const auto source = read_source("src/platform/linux/x11grab.cpp");
  EXPECT_EQ(source.find("task_pool.pushDelayed"), std::string::npos);
  EXPECT_EQ(source.find("task_pool.cancel"), std::string::npos);
  EXPECT_NE(source.find("now >= next_refresh"), std::string::npos);
  EXPECT_NE(source.find("if (!x_img)"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, EglRejectsMissingDriverMetadata) {
  const auto source = read_source("src/platform/linux/graphics.cpp");
  EXPECT_NE(source.find("if (!extension_st)"), std::string::npos);
  EXPECT_NE(source.find("count == 0 || !conf"), std::string::npos);
}

TEST(LinuxPlatformHardeningSource, NvfbcOutputsStartInitialized) {
  const auto source = read_source("src/platform/linux/cuda.cpp");
  EXPECT_NE(source.find("NVFBC_SESSION_HANDLE handle {0}"), std::string::npos);
  EXPECT_NE(source.find("CUdeviceptr device_ptr {}"), std::string::npos);
  EXPECT_NE(source.find("NVFBC_FRAME_GRAB_INFO info {}"), std::string::npos);
}
