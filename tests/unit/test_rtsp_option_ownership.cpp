#include <gtest/gtest.h>

extern "C" {
int polaris_rtsp_normal_control(void);
int polaris_rtsp_duplicate_control(void);
int polaris_rtsp_malformed_control(void);
int polaris_rtsp_oom_control(void);
}

TEST(RtspOptionOwnershipTests, NormalHeadersReleaseTheirParserAllocations) {
    EXPECT_EQ(polaris_rtsp_normal_control(), 0);
}

TEST(RtspOptionOwnershipTests, DuplicatesKeepFirstOrderLastValueAndCaseSensitiveOwnership) {
    EXPECT_EQ(polaris_rtsp_duplicate_control(), 0);
}

TEST(RtspOptionOwnershipTests, MalformedTailReleasesHeadersIncludingDuplicates) {
    EXPECT_EQ(polaris_rtsp_malformed_control(), 0);
}

TEST(RtspOptionOwnershipTests, EveryAllocationFailureKeepsTheOriginalFailureCleanup) {
    EXPECT_EQ(polaris_rtsp_oom_control(), 0);
}
