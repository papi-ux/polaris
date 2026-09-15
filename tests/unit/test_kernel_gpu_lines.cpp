/**
 * @file tests/unit/test_kernel_gpu_lines.cpp
 * @brief The bundle keeps the kernel's GPU lines and says when the journal is closed to it.
 */
#include <src/kernel_gpu_lines.h>

#include <gtest/gtest.h>

namespace kg = kernel_gpu_lines;

TEST(KernelGpuLines, KeepsDriverAndHangLinesAndDropsTheRest) {
  const std::string journal =
    "2026-09-14T19:58:19-0400 cachyos kernel: ACPI: bus type drm_connector registered\n"
    "2026-09-14T19:58:19-0400 cachyos kernel: r8169 0000:3b:00.0 eth0: RTL8168h\n"
    "2026-09-14T19:58:20-0400 cachyos kernel: NVRM: loading NVIDIA UNIX Open Kernel Module for x86_64  615.71.09\n"
    "2026-09-14T19:58:20-0400 cachyos kernel: i915 0000:00:02.0: [drm] VT-d active for gfx access\n"
    "2026-09-14T19:59:35-0400 Farttop kernel: [drm] Initialized hermes-kms 0.4.0 for hermes-kms on minor 2\n"
    "2026-09-14T20:30:27-0400 Farttop kernel: NVRM: Xid (PCI:0000:01:00): 79, pid=0, GPU has fallen off the bus.\n"
    "2026-09-14T20:30:40-0400 Farttop kernel: sof-audio-pci-intel-cnl 0000:00:1f.3: bound 0000:00:02.0\n";

  // The ACPI bus-type line names drm_connector without being about a GPU; the filter
  // wants [drm] or a driver, not the substring.
  const auto lines = kg::filter(journal);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_NE(lines[0].find("NVRM"), std::string::npos);
  EXPECT_NE(lines[1].find("i915"), std::string::npos);
  EXPECT_NE(lines[2].find("hermes-kms"), std::string::npos);
  EXPECT_NE(lines[3].find("Xid"), std::string::npos);
}

TEST(KernelGpuLines, KeepsTheLastLinesWhenThereAreTooMany) {
  std::string journal;
  for (int i = 0; i < 400; ++i) {
    journal += "kernel: amdgpu line " + std::to_string(i) + "\n";
  }
  const auto lines = kg::filter(journal, 10);
  ASSERT_EQ(lines.size(), 10u);
  EXPECT_NE(lines.front().find("line 390"), std::string::npos);
  EXPECT_NE(lines.back().find("line 399"), std::string::npos);
}

TEST(KernelGpuLines, TrimsAbsurdlyLongLines) {
  const std::string journal = "kernel: nvidia " + std::string(2000, 'x') + "\n";
  const auto lines = kg::filter(journal);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].size(), kg::k_max_line_length);
}

TEST(KernelGpuLines, RecognisesAJournalItMayNotRead) {
  EXPECT_TRUE(kg::journal_unreadable("No journal files were found.\n"));
  EXPECT_TRUE(kg::journal_unreadable("Hint: You are currently not seeing messages from other users and the system.\n"));
  EXPECT_TRUE(kg::journal_unreadable("Failed to open journal: Permission denied\n"));
  EXPECT_FALSE(kg::journal_unreadable("2026-09-14T19:58:20-0400 host kernel: i915 0000:00:02.0: [drm] fb0\n"));
  EXPECT_FALSE(kg::journal_unreadable(""));
}
