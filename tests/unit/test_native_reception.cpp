/** @file Real native reception parser/correlation tests; no network, probe or controller. The trace
 * tests inspect pure private-trace row values; no recorder writes a file here. */
#include "src/stream_native_reception.h"
#include "src/stream_native_reception_json.h"
#include "src/stream_native_reception_trace.h"
#include <gtest/gtest.h>
#include <chrono>
#include <atomic>
#include <thread>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>
using namespace std::chrono_literals;
namespace {
  namespace nr = stream_native_reception;
  namespace ff = stream_frame_feedback;
  namespace bp = video::bitrate_provenance;
  using test_clock = nr::clock;
  void be(std::string &out, std::size_t at, std::uint64_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) out[at + bytes - i - 1] = static_cast<char>(value >> (i * 8));
  }
  std::string wire(nr::report_t r) {
    std::string out(52 + 10 * r.count, '\0');
    out[0] = 1; out[1] = r.flags; out[2] = r.count;
    be(out,4,r.sequence,4); be(out,8,r.interval_us,4); be(out,12,r.end_us,8); be(out,20,r.loop_tick,4);
    be(out,24,r.data_packets,4); be(out,28,r.parity_packets,4); be(out,32,r.data_bytes,4); be(out,36,r.parity_bytes,4);
    be(out,40,r.last_accepted_age_us,4); be(out,44,r.first_frame,4); be(out,48,r.last_frame,4);
    for (unsigned i = 0; i < r.count; ++i) {
      be(out,52 + i*10,r.samples[i].frame,4); be(out,56+i*10,r.samples[i].sequence,2); be(out,58+i*10,r.samples[i].age_us,4);
    }
    return out;
  }
  nr::report_t reading(unsigned count = 1) {
    nr::report_t r; r.sequence=1; r.interval_us=100'000; r.end_us=100'000; r.loop_tick=10;
    r.flags=count ? nr::has_accepted : 0; r.count=count; r.data_packets=count; r.data_bytes=count*1000;
    r.last_accepted_age_us=count ? 10'000 : UINT32_MAX; r.first_frame=r.last_frame=count ? 1 : 0;
    for (unsigned i=0;i<count;++i) r.samples[i]={1,static_cast<std::uint16_t>(i),static_cast<std::uint32_t>(50'000-i*1000)};
    return r;
  }
  ff::encoder_setting_t actual_setting(std::uint64_t generation=615) {
    bp::scope_t id {9,generation}; bp::ledger_t ledger {id};
    EXPECT_TRUE(ledger.observe_receipt({id,1,8,18000,bp::receipt_origin_e::successful_open}));
    auto captured=ledger.capture(1); EXPECT_TRUE(captured);
    EXPECT_TRUE(ledger.complete(*captured,bp::submission_result_e::submitted_under_captured_setting));
    auto record=ledger.take(id,1); EXPECT_TRUE(record);
    return *ff::encoder_setting_of(std::optional<const bp::record_t>{*record});
  }
  struct NativeReception : ::testing::Test {
    test_clock::time_point now {10s};
    std::shared_ptr<ff::sender_ledger_t> frames=std::make_shared<ff::sender_ledger_t>(615);
    std::unique_ptr<nr::scope_t> observer=std::make_unique<nr::scope_t>(615,frames,true);
    ff::encoder_setting_t setting=actual_setting();
    void frame(std::uint32_t id, ff::send_disposition_e disposition=ff::send_disposition_e::sent,
               std::optional<ff::encoder_setting_t> s=std::nullopt) {
      auto ticket=frames->begin(id,s ? s : std::optional{setting},now-110ms); ASSERT_TRUE(ticket);
      if(disposition!=ff::send_disposition_e::pending) frames->complete(*ticket,disposition,now-20ms);
    }
    void packet(std::uint32_t frame=1,std::uint16_t seq=0,bool parity=false,bool ok=true) {
      observer->record(frame,seq,1000,parity,now-100ms,now-99ms,ok);
    }
    nr::readback_t accept(nr::report_t r) {
      EXPECT_EQ(observer->accept(wire(r),true,now),nr::reason_e::joined);
      auto result=observer->readback(now); EXPECT_TRUE(result); return *result;
    }
  };
}
TEST(NativeReceptionWire, ExactZeroThroughSixteenAndByteOrder) {
  for(unsigned n=0;n<=16;++n) {
    auto r=reading(n); auto bytes=wire(r); auto p=nr::parse(bytes); ASSERT_TRUE(p.report) << n;
    EXPECT_EQ(bytes.size(),52+10*n); EXPECT_EQ(p.report->data_bytes,n*1000); EXPECT_EQ(p.report->end_us,100'000u);
    EXPECT_EQ(p.report->count,n);
  }
  auto bytes=wire(reading(16)); bytes[2]=17; bytes+=std::string(10,'\0'); EXPECT_FALSE(nr::parse(bytes).report);
}
TEST(NativeReceptionWire, ExactLengthRejectsEveryTruncationAndTrailingByte) {
  auto bytes=wire(reading(16));
  for(unsigned n=0;n<bytes.size();++n) EXPECT_FALSE(nr::parse(bytes.substr(0,n)).report) << n;
  EXPECT_FALSE(nr::parse(bytes+"x").report);
}
TEST(NativeReceptionWire, UnknownVersionFlagsReservedAndSequenceZeroRefused) {
  for(auto [offset,value]:std::vector<std::pair<int,int>>{{0,0},{0,2},{1,0x11},{1,0x81},{3,1}}) {
    auto bytes=wire(reading()); bytes[offset]=value; EXPECT_FALSE(nr::parse(bytes).report);
  }
  auto r=reading(); r.sequence=0; EXPECT_FALSE(nr::parse(wire(r)).report);
}
TEST(NativeReceptionWire, CountsBytesAndRangesMustAgreeWithoutArithmeticWrap) {
  std::vector<nr::report_t> bad;
  auto r=reading(); r.flags=0; bad.push_back(r);
  r=reading();r.data_bytes=0;bad.push_back(r);
  r=reading();r.parity_bytes=1;bad.push_back(r);
  r=reading();r.first_frame=2;bad.push_back(r);
  r=reading(0);r.first_frame=1;bad.push_back(r);
  r=reading();r.data_packets=0;r.data_bytes=0;bad.push_back(r);
  r=reading();r.data_bytes=65536;bad.push_back(r);
  r=reading();r.count=2;r.samples[1]={1,2,20000};bad.push_back(r);
  for(auto &b:bad) EXPECT_FALSE(nr::parse(wire(b)).report);
  r=reading(0);r.flags=nr::has_accepted|nr::incomplete;r.first_frame=r.last_frame=1;
  r.data_packets=r.parity_packets=r.data_bytes=r.parity_bytes=UINT32_MAX;
  EXPECT_TRUE(nr::parse(wire(r)).report); // explicit incomplete facts, no u32 total overflow/join.
}
TEST(NativeReceptionWire, DuplicateKeysSampleBoundsAndDelayedCommit) {
  auto r=reading(2);r.samples[1]=r.samples[0];EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading();r.samples[0].frame=2;EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading();r.samples[0].age_us=2'000'001;EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading();r.samples[0].age_us=9000;EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading();r.end_us=1'000'000;r.samples[0].age_us=900'000;
  EXPECT_TRUE(nr::parse(wire(r)).report); // socket-read before commit interval remains explicit.
}
TEST(NativeReceptionWire, ZeroOldAgeLongIncompleteAndClockSentinel) {
  auto r=reading(0);r.last_accepted_age_us=99;EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading();r.interval_us=0;EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading();r.interval_us=2'000'001;r.end_us=3'000'000;EXPECT_FALSE(nr::parse(wire(r)).report);
  r.flags|=nr::incomplete;r.count=0;EXPECT_TRUE(nr::parse(wire(r)).report);
  r.count=1;EXPECT_FALSE(nr::parse(wire(r)).report);
  r=reading(0);r.flags=nr::incomplete|nr::clock_unavailable;r.interval_us=0;r.end_us=0;
  EXPECT_TRUE(nr::parse(wire(r)).report);r.flags=nr::clock_unavailable;EXPECT_FALSE(nr::parse(wire(r)).report);
}
TEST(NativeReceptionNegotiation, EveryIndependentGateAndExactWhitespaceToken) {
  EXPECT_TRUE(nr::negotiated(true,true," \t1\r\n",1,2,13,3));
  EXPECT_FALSE(nr::negotiated(false,true,"1",1,2,13,3));
  EXPECT_FALSE(nr::negotiated(true,false,"1",1,2,13,3));
  for(auto value:{"","0","2","01","1.0","1 1","1junk"}) EXPECT_FALSE(nr::negotiated(true,true,value,1,2,13,3));
  for(unsigned count:{0,2,3}) EXPECT_FALSE(nr::negotiated(true,true,"1",count,2,13,3));
  EXPECT_FALSE(nr::negotiated(true,true,"1",1,1,13,3));
  EXPECT_FALSE(nr::negotiated(true,true,"1",1,2,1,3));
  EXPECT_TRUE(nr::negotiated(true,true,"1",1,2,13,1));
  for(unsigned flags:{0,2,4,6}) EXPECT_FALSE(nr::negotiated(true,true,"1",1,2,13,flags));
}
TEST_F(NativeReception, ExactImmutableTupleAndDifferenceBoundsNotAbsoluteClocks) {
  frame(1);packet();observer->record(1,1,1000,false,now-90ms,now-88ms,true);
  auto r=reading(2);r.samples[0].age_us=30000;r.samples[1].age_us=15000;r.end_us=1'000'000'000'000ULL;
  auto s=accept(r);ASSERT_TRUE(s.available);ASSERT_TRUE(s.setting);EXPECT_EQ(*s.setting,setting);
  EXPECT_EQ(s.samples[1].dispersion_lower_ns,3'000'000);EXPECT_EQ(s.samples[1].dispersion_upper_ns,6'000'000);
  auto j=nr::readback_json(s);EXPECT_FALSE(j.contains("frame_submission_receipt"));
  EXPECT_EQ(j.dump().find("begin_ns"),std::string::npos);EXPECT_EQ(j.dump().find("owner_uuid"),std::string::npos);
}
TEST_F(NativeReception, BatchShardsShareHandoffIntervalAndSparseFlagIsNotFailure) {
  frame(1);for(unsigned n=0;n<30;++n) packet(1,static_cast<std::uint16_t>(n));
  auto r=reading(2);r.flags|=nr::timing_sample_limit;r.data_packets=30;r.data_bytes=30000;
  auto s=accept(r);ASSERT_TRUE(s.available);EXPECT_EQ(s.samples[0].handoff_duration_ns,s.samples[1].handoff_duration_ns);
  EXPECT_TRUE(s.samples[1].dispersion_available);
}
TEST_F(NativeReception, FrameZeroIsInclusiveWithoutChangingHistoricalExclusiveJoin) {
  frame(0);packet(0);auto r=reading();r.first_frame=r.last_frame=0;r.samples[0].frame=0;
  EXPECT_TRUE(accept(r).available);EXPECT_FALSE(frames->join(0,0,now,2s).available);
  EXPECT_TRUE(frames->join_inclusive(0,0,now,2s).available);
}
TEST_F(NativeReception, MissingUnsampledFrameRefusesTheWholeRange) {
  frame(1);frame(3);packet();auto r=reading();r.last_frame=3;
  auto s=accept(r);EXPECT_FALSE(s.available);EXPECT_STREQ(s.frame_reason,"host_frame_missing");
}
TEST_F(NativeReception, SameKbpsDifferentApplicationAndOriginAreMixed) {
  frame(1);auto changed=setting;++changed.application_sequence;frame(2,ff::send_disposition_e::sent,changed);packet();
  auto r=reading();r.last_frame=2;auto s=accept(r);EXPECT_FALSE(s.available);EXPECT_STREQ(s.frame_reason,"mixed_encoder_settings");
}
TEST_F(NativeReception, WrongGenerationAndEncoderRecreationCannotJoin) {
  auto changed=setting;++changed.stream_generation;frame(1,ff::send_disposition_e::sent,changed);packet();
  auto s=accept(reading());EXPECT_FALSE(s.available);EXPECT_STREQ(s.frame_reason,"encoder_setting_generation_mismatch");
  auto other=std::make_unique<nr::scope_t>(616,frames,true);EXPECT_FALSE(other->readback(now));
}
TEST_F(NativeReception, EveryAmbiguousFrameDispositionRefusesEvenMatchingPackets) {
  for(auto d:{ff::send_disposition_e::pending,ff::send_disposition_e::batch_fallback,ff::send_disposition_e::send_failed,
      ff::send_disposition_e::cancelled,ff::send_disposition_e::exception,ff::send_disposition_e::incomplete,ff::send_disposition_e::not_sent}) {
    frames=std::make_shared<ff::sender_ledger_t>(615);observer=std::make_unique<nr::scope_t>(615,frames,true);
    frame(1,d);packet();EXPECT_FALSE(accept(reading()).available) << static_cast<int>(d);
  }
}
TEST_F(NativeReception, PartialBatchThenSuccessfulFallbackIsNeverRelabeledSuccess) {
  frame(1);packet(1,0,false,false);packet(1,0,false,true);
  auto s=accept(reading());EXPECT_FALSE(s.available);EXPECT_EQ(s.reason,nr::reason_e::packet_ambiguous);
}
TEST_F(NativeReception, DuplicateKeyAndMissingSampleAreNotInvented) {
  frame(1);packet();packet();EXPECT_EQ(accept(reading()).reason,nr::reason_e::packet_ambiguous);
  observer=std::make_unique<nr::scope_t>(615,frames,true); // independent missing-key scenario, no ambiguous slot.
  auto r=reading();r.samples[0].sequence=1;
  EXPECT_EQ(accept(r).reason,nr::reason_e::packet_missing);
}
TEST_F(NativeReception, ExactRtpWrapAndReorderedApiPairStayExplicit) {
  frame(1);packet(1,UINT16_MAX);observer->record(1,0,1000,false,now-105ms,now-104ms,true);
  auto r=reading(2);r.samples[0].sequence=UINT16_MAX;r.samples[1].sequence=0;
  auto s=accept(r);ASSERT_TRUE(s.available);EXPECT_FALSE(s.samples[1].dispersion_available);
}
TEST_F(NativeReception, PacketHashCollisionAndBoundedEvictionLoseOnlyOldProvenance) {
  frame(1);packet(1,0);packet(1,16384);
  EXPECT_EQ(accept(reading()).reason,nr::reason_e::packet_missing);
  auto r=reading();r.sequence=2;r.end_us+=100000;r.samples[0].sequence=16384;now+=100ms;
  EXPECT_TRUE(accept(r).available);
}
TEST_F(NativeReception, CapacityIsFixedAndEvictedPacketCannotJoin) {
  frame(1);for(unsigned n=0;n<=nr::ledger_capacity;++n) packet(1,static_cast<std::uint16_t>(n));
  EXPECT_EQ(accept(reading()).reason,nr::reason_e::packet_missing);
  EXPECT_LT(sizeof(nr::scope_t),540000u);
}
TEST_F(NativeReception, FrameLedgerEvictionAndIndexPoisonAreTerminalForJoins) {
  for(unsigned n=0;n<=ff::k_ledger_capacity;++n) frame(n);
  packet(0);auto r=reading();r.first_frame=r.last_frame=0;r.samples[0].frame=0;
  EXPECT_STREQ(accept(r).frame_reason,"ledger_evicted");
  EXPECT_FALSE(frames->begin(-1,setting,now));EXPECT_FALSE(frames->join_inclusive(4096,4096,now,2s).available);
}
TEST_F(NativeReception, CounterClassAndByteBoundsUseActualSampleIdentity) {
  frame(1);packet(1,0,true);
  EXPECT_EQ(observer->accept(wire(reading()),true,now),nr::reason_e::sample_counts);
  auto r=reading();r.sequence=2;r.end_us+=100000;r.data_packets=r.data_bytes=0;r.parity_packets=1;r.parity_bytes=999;now+=100ms;
  EXPECT_EQ(observer->accept(wire(r),true,now),nr::reason_e::sample_counts);
  auto snapshot=observer->readback(now);ASSERT_TRUE(snapshot);EXPECT_FALSE(snapshot->has_report);
}
TEST_F(NativeReception, ExpiryInvalidatesMetricsWithoutRefreshingOrHidingRawAge) {
  frame(1);packet();ASSERT_TRUE(accept(reading()).available);
  auto s=observer->readback(now+2001ms);ASSERT_TRUE(s);EXPECT_FALSE(s->available);EXPECT_FALSE(s->setting);
  EXPECT_FALSE(s->samples[0].matched);EXPECT_EQ(s->host_report_age_ms,2001u);EXPECT_TRUE(s->has_report);
}
TEST_F(NativeReception, SenderBoundariesExpireBeforeFreshReportAndIncludeUnsampledFrames) {
  frame(1);packet();now+=1950ms;EXPECT_FALSE(accept(reading()).available);
}
TEST_F(NativeReception, SenderFutureAndInvalidApiBoundsAreUnavailable) {
  frame(1);observer->record(1,0,1000,false,now+1ms,now+2ms,true);
  EXPECT_EQ(accept(reading()).reason,nr::reason_e::packet_stale);
}
TEST_F(NativeReception, RejectionsNeverAdvanceSequenceAgeOrLoop) {
  frame(1);packet();ASSERT_TRUE(accept(reading()).available);
  auto r=reading();r.sequence=2;r.end_us+=100000;r.loop_tick=11;
  EXPECT_EQ(observer->accept(wire(r),false,now+100ms),nr::reason_e::unauthenticated);
  auto bad=wire(r);bad[3]=1;EXPECT_EQ(observer->accept(bad,true,now+100ms),nr::reason_e::malformed_flags);
  r.sequence=1;EXPECT_EQ(observer->accept(wire(r),true,now+100ms),nr::reason_e::report_order);
  auto s=observer->readback(now+100ms);ASSERT_TRUE(s);EXPECT_EQ(s->report.sequence,1u);EXPECT_EQ(s->host_report_age_ms,100u);
}
TEST_F(NativeReception, GapsLoopStallAndZeroMediaClearAllPriorDerivedValues) {
  frame(1);packet();ASSERT_TRUE(accept(reading()).available);
  auto r=reading(0);r.sequence=3;r.end_us=300000;r.last_accepted_age_us=210000;now+=200ms;
  auto s=accept(r);EXPECT_EQ(s.reason,nr::reason_e::no_accepted);EXPECT_EQ(s.missing_reports,1u);
  EXPECT_EQ(s.receive_loop,nr::loop_e::unchanged);EXPECT_FALSE(s.available);EXPECT_FALSE(s.setting);
  auto json=nr::readback_json(s);EXPECT_EQ(json["samples"].size(),0u);EXPECT_FALSE(json.contains("setting"));
}
TEST_F(NativeReception, LoopCounterWrapIsProgressButRegressionAndClockOverlapRefuse) {
  frame(1);packet();auto r=reading();r.loop_tick=UINT32_MAX;accept(r);
  r.sequence=2;r.end_us+=100000;r.loop_tick=0;now+=100ms;EXPECT_EQ(accept(r).receive_loop,nr::loop_e::advancing);
  r.sequence=3;r.end_us+=100000;r.loop_tick=UINT32_MAX;
  EXPECT_EQ(observer->accept(wire(r),true,now+100ms),nr::reason_e::loop_regression);
  r.loop_tick=1;r.end_us=250000;EXPECT_EQ(observer->accept(wire(r),true,now+100ms),nr::reason_e::malformed_time);
}
TEST_F(NativeReception, LongIncompleteClearsMetricsThenNormalObservationCanResume) {
  frame(1);packet();ASSERT_TRUE(accept(reading()).available);
  auto r=reading();r.sequence=2;r.interval_us=2100000;r.end_us=2200000;r.flags|=nr::incomplete;r.count=0;r.loop_tick=11;now+=2100ms;
  EXPECT_EQ(accept(r).reason,nr::reason_e::incomplete);
  now+=100ms;frame(2);packet(2);
  r=reading();r.sequence=3;r.end_us=2300000;r.first_frame=r.last_frame=2;r.samples[0].frame=2;r.loop_tick=12;
  EXPECT_TRUE(accept(r).available);
}
TEST_F(NativeReception, ClockSentinelDisablesUntilNewConnectionAndRetirementCannotRevive) {
  frame(1);packet();accept(reading());
  auto r=reading(0);r.flags=nr::incomplete|nr::clock_unavailable;r.interval_us=0;r.end_us=0;r.sequence=2;r.loop_tick=11;
  EXPECT_EQ(accept(r).reason,nr::reason_e::clock_unavailable);
  r=reading();r.sequence=3;EXPECT_EQ(observer->accept(wire(r),true,now+100ms),nr::reason_e::clock_unavailable);
  observer->retire();EXPECT_FALSE(observer->readback(now));EXPECT_EQ(observer->accept(wire(r),true,now),nr::reason_e::retired);
  auto fresh=std::make_unique<nr::scope_t>(616,std::make_shared<ff::sender_ledger_t>(616),true);
  auto state=fresh->readback(now);ASSERT_TRUE(state);EXPECT_FALSE(state->has_report);EXPECT_EQ(state->reason,nr::reason_e::waiting);
}
TEST_F(NativeReception, NoSamplesAndDisabledOrMissingProvenanceNeverSynthesizeArrival) {
  frame(1);auto r=reading();r.count=0;EXPECT_EQ(accept(r).reason,nr::reason_e::no_samples);
  auto disabled=std::make_unique<nr::scope_t>(615,frames,false);EXPECT_FALSE(disabled->readback(now));
  EXPECT_EQ(disabled->accept(wire(reading()),true,now),nr::reason_e::not_negotiated);
  auto absent=std::make_unique<nr::scope_t>(615,nullptr,true);EXPECT_FALSE(absent->readback(now));
}

TEST(NativeReceptionProvenance, BothVideoModesKeepHostMediaAndVerifiedControlFactsSeparate) {
  for(bool encrypted:{false,true}) {
    auto frames=std::make_shared<ff::sender_ledger_t>(615);
    auto scope=std::make_unique<nr::scope_t>(615,frames,true,encrypted);
    auto before=scope->readback(test_clock::time_point{10s});ASSERT_TRUE(before);
    EXPECT_EQ(before->media_authenticated,encrypted);EXPECT_FALSE(before->control_authenticated);
    auto r=reading(0);EXPECT_EQ(scope->accept(wire(r),false,test_clock::time_point{10s}),nr::reason_e::unauthenticated);
    EXPECT_EQ(scope->accept(wire(r),true,test_clock::time_point{10s}),nr::reason_e::joined);
    auto after=scope->readback(test_clock::time_point{10s});ASSERT_TRUE(after);
    auto json=nr::readback_json(*after);EXPECT_EQ(json["media_authenticated"],encrypted);
    EXPECT_EQ(json["control_authenticated"],true);EXPECT_EQ(json["coverage"],"queue_accepted_original");
    EXPECT_FALSE(json["available"].get<bool>());
  }
}

TEST_F(NativeReception, CounterContradictionCannotRefreshAnEarlierObservation) {
  frame(1);packet(1,0);packet(1,1,true);ASSERT_TRUE(accept(reading()).available);
  auto r=reading();r.sequence=2;r.end_us+=100000;r.loop_tick=11;r.samples[0].sequence=1;
  EXPECT_EQ(observer->accept(wire(r),true,now+100ms),nr::reason_e::sample_counts);
  auto snapshot=observer->readback(now+100ms);ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->report.sequence,1u);EXPECT_EQ(snapshot->host_report_age_ms,100u);
}

TEST(NativeReceptionWire, ActualNovaCSerializerGoldenBytesWithoutHostRegeneration) {
  // Frozen from the peer production C serializer, not wire() above or a host encoder.
  const char *hex[] = {
    "0100000000000001000186a000000000000186a00000000100000000000000000000000000000000ffffffff0000000000000000",
    "01010200000000020001879a0000000000030e3a0000003900000028000000040000a28000001040000004b0000000110000001200000011123400017ed000000012124000002ee0",
    "0103000000000003002625a000000000002933da00000052000000030000000000000c300000000000249f000000001300000013",
    "01060000000000040000000000000000000000000000005a00000000000000000000000000000000ffffffff0000000000000000",
  };
  auto frames=std::make_shared<ff::sender_ledger_t>(615);
  auto scope=std::make_unique<nr::scope_t>(615,frames,true);
  auto now=test_clock::time_point{10s};
  for(unsigned v=0;v<4;++v) {
    const std::string text=hex[v];std::string bytes;
    for(std::size_t n=0;n<text.size();n+=2) bytes.push_back(static_cast<char>(std::stoul(text.substr(n,2),nullptr,16)));
    auto parsed=nr::parse(bytes);ASSERT_TRUE(parsed.report) << v;EXPECT_EQ(parsed.report->sequence,v+1);
    EXPECT_EQ(scope->accept(bytes,true,now),nr::reason_e::joined) << v;
    auto snapshot=scope->readback(now);ASSERT_TRUE(snapshot);EXPECT_FALSE(snapshot->available);
    if(v==1) { EXPECT_EQ(parsed.report->data_packets,40u);EXPECT_EQ(parsed.report->parity_packets,4u);EXPECT_EQ(parsed.report->count,2u); }
    if(v==2) EXPECT_EQ(snapshot->reason,nr::reason_e::incomplete);
    if(v==3) EXPECT_EQ(snapshot->reason,nr::reason_e::clock_unavailable);
    now+=100ms;
  }
}
TEST_F(NativeReception, AcceptedCountersCannotExceedRetainedOfferedPacketsOrBytes) {
  frame(1);packet();ASSERT_TRUE(accept(reading()).available);
  auto r=reading();r.sequence=2;r.end_us+=100000;r.data_packets=2;r.data_bytes=2000;
  EXPECT_EQ(observer->accept(wire(r),true,now+100ms),nr::reason_e::sample_counts);
  auto snapshot=observer->readback(now+100ms);ASSERT_TRUE(snapshot);EXPECT_EQ(snapshot->report.sequence,1u);
}
TEST_F(NativeReception, EveryTupleComponentAndOriginRemainExactAtTheSameKbps) {
  for(unsigned field=0;field<4;++field) {
    frames=std::make_shared<ff::sender_ledger_t>(615);observer=std::make_unique<nr::scope_t>(615,frames,true);
    frame(1);auto next=setting;
    switch(field) { case 0:++next.encoder_lifetime;break;case 1:++next.origin_revision;break;
      case 2:++next.application_sequence;break;case 3:next.receipt_origin=bp::receipt_origin_e::backend_reconfiguration;break; }
    frame(2,ff::send_disposition_e::sent,next);packet();auto r=reading();r.last_frame=2;
    auto observed=accept(r);EXPECT_FALSE(observed.available);EXPECT_STREQ(observed.frame_reason,"mixed_encoder_settings");
  }
}

TEST_F(NativeReception, ConcurrentSenderReadbackAndRetirementHaveNoLockInversionOrRevival) {
  frame(1);packet();ASSERT_TRUE(accept(reading()).available);
  std::atomic_bool started {false};
  std::thread sender([&] {
    for(unsigned n=2;n<102;++n) {
      auto ticket=frames->begin(n,setting,now-100ms);
      if(ticket) frames->complete(*ticket,ff::send_disposition_e::sent,now-90ms);
      observer->record(n,static_cast<std::uint16_t>(n),1000,false,now-100ms,now-99ms,true);
      started.store(true);
    }
  });
  std::thread consumer([&] {
    while(!started.load()) std::this_thread::yield();
    for(unsigned n=0;n<100;++n) (void)observer->readback(now);
  });
  while(!started.load()) std::this_thread::yield();observer->retire();
  sender.join();consumer.join();EXPECT_FALSE(observer->readback(now));
  EXPECT_EQ(observer->accept(wire(reading()),true,now),nr::reason_e::retired);
}

TEST_F(NativeReception, UnsampledSequentialFrameCollisionPublishesFreshUnavailableObservation) {
  // Production scope, ledger and index: frame84/seq834 is overwritten by frame94/seq936.
  // The report samples830 and930, so neither sample itself is missing. This is retention,
  // not a proved contradiction of the client's legitimate110 accepted packets/110000 bytes.
  frame(83);
  for(unsigned n=0;n<10;++n) packet(83,static_cast<std::uint16_t>(820+n));
  auto previous=reading();previous.first_frame=previous.last_frame=83;
  previous.data_packets=10;previous.data_bytes=10000;previous.samples[0]={83,820,50000};
  ASSERT_TRUE(accept(previous).available);
  now+=100ms;
  for(unsigned f=84;f<=94;++f) {
    frame(f);
    for(unsigned n=0;n<10;++n) packet(f,static_cast<std::uint16_t>((f-1)*10+n));
  }
  auto report=reading(2);report.sequence=2;report.end_us=200000;report.loop_tick=11;
  report.first_frame=84;report.last_frame=94;report.data_packets=110;report.data_bytes=110000;
  report.samples[0]={84,830,50000};report.samples[1]={94,930,40000};
  EXPECT_EQ(observer->accept(wire(report),true,now),nr::reason_e::joined);
  auto current=observer->readback(now);ASSERT_TRUE(current);
  EXPECT_EQ(current->report.sequence,2u);EXPECT_EQ(current->host_report_age_ms,0u);
  EXPECT_FALSE(current->available);EXPECT_STREQ(nr::name(current->reason),"retention_limited");
  EXPECT_FALSE(current->setting);EXPECT_FALSE(current->samples[0].matched);
  EXPECT_EQ(current->report.data_packets,110u);EXPECT_EQ(current->report.data_bytes,110000u);
}

TEST_F(NativeReception, RetentionLossDoesNotHideMatchedSampleContradictionOrPoisonLaterCompleteRange) {
  for(unsigned f=84;f<=94;++f) {
    frame(f);
    for(unsigned n=0;n<10;++n) packet(f,static_cast<std::uint16_t>((f-1)*10+n));
  }
  auto report=reading(2);report.first_frame=84;report.last_frame=94;
  report.data_packets=110;report.data_bytes=110000;
  report.samples[0]={84,830,50000};report.samples[1]={94,930,40000};
  auto retained=accept(report);ASSERT_FALSE(retained.available);
  ASSERT_STREQ(nr::name(retained.reason),"retention_limited");
  now+=100ms;
  auto bad=reading();bad.sequence=2;bad.end_us=200000;bad.loop_tick=11;
  bad.first_frame=84;bad.last_frame=94;bad.samples[0]={84,830,50000};
  bad.data_packets=bad.data_bytes=0;bad.parity_packets=1;bad.parity_bytes=1000;
  EXPECT_EQ(observer->accept(wire(bad),true,now),nr::reason_e::sample_counts);
  auto unchanged=observer->readback(now);ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->report.sequence,1u);EXPECT_EQ(unchanged->host_report_age_ms,100u);
  // A later complete range is not permanently poisoned by older retention loss.
  frame(95);packet(95,940);
  auto fresh=reading();fresh.sequence=3;fresh.end_us=300000;fresh.loop_tick=12;
  fresh.first_frame=fresh.last_frame=95;fresh.samples[0]={95,940,50000};
  ASSERT_TRUE(accept(fresh).available);
  now+=100ms;fresh.sequence=4;fresh.end_us=400000;fresh.loop_tick=13;
  fresh.data_packets=2;fresh.data_bytes=2000;
  EXPECT_EQ(observer->accept(wire(fresh),true,now),nr::reason_e::sample_counts);
  unchanged=observer->readback(now);ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->report.sequence,3u);EXPECT_EQ(unchanged->host_report_age_ms,100u);
}
TEST_F(NativeReception, ParityEvictionCannotExcuseAProvedCompleteDataClassContradiction) {
  // Sequential RTP: frame84 has two data/five parity packets, subsequent frames ten data.
  // Only parity84/834 is evicted by data94/936. All102 data records remain, so103 is impossible.
  std::uint16_t sequence=830;
  for(unsigned f=84;f<=94;++f) {
    frame(f);
    for(unsigned n=0;n<(f==84 ? 7u : 10u);++n) packet(f,sequence++,f==84 && n>=2);
  }
  auto report=reading();report.first_frame=84;report.last_frame=94;
  report.samples[0]={84,830,50000};report.data_packets=103;report.data_bytes=103000;
  EXPECT_EQ(observer->accept(wire(report),true,now),nr::reason_e::sample_counts);
  auto snapshot=observer->readback(now);ASSERT_TRUE(snapshot);EXPECT_FALSE(snapshot->has_report);
}

TEST_F(NativeReception, EvictionCompletenessMetadataIsSerializedBetweenSenderAndControl) {
  for(unsigned f=84;f<=94;++f) {
    frame(f);
    for(unsigned n=0;n<10;++n) packet(f,static_cast<std::uint16_t>((f-1)*10+n));
  }
  std::atomic_bool start {false};
  std::thread sender([&] {
    while(!start.load()) std::this_thread::yield();
    for(unsigned n=0;n<128;++n) {
      packet(84,834);packet(94,936); // alternate the same known different-key collision.
      std::this_thread::yield();
    }
  });
  start.store(true);
  for(unsigned n=1;n<=32;++n) {
    auto report=reading(2);report.sequence=n;report.end_us=n*100000;report.loop_tick=10+n;
    report.first_frame=84;report.last_frame=94;report.data_packets=110;report.data_bytes=110000;
    report.samples[0]={84,830,50000};report.samples[1]={94,930,40000};
    EXPECT_EQ(observer->accept(wire(report),true,now),nr::reason_e::joined);
    auto current=observer->readback(now);EXPECT_TRUE(current);
    if(current) { EXPECT_FALSE(current->available);EXPECT_STREQ(nr::name(current->reason),"retention_limited"); }
  }
  sender.join();observer->retire();EXPECT_FALSE(observer->readback(now));
}

namespace {
  namespace nrt = stream_native_reception::trace;
  using row_t = vrr_trace::event_t;
  using type_e = vrr_trace::field_t::type_e;
  struct rows_t {
    std::vector<row_t> rows;
    void operator()(const row_t &row) { rows.push_back(row); }
  };
  const vrr_trace::field_t *field(const row_t &row, std::string_view key) {
    for(std::size_t i=0;i<row.size;++i) if(key==row.fields[i].key) return &row.fields[i];
    return nullptr;
  }
  std::uint64_t u(const row_t &row, std::string_view key) {
    const auto *f=field(row,key); EXPECT_TRUE(f && f->type==type_e::unsigned_integer) << key; return f ? f->u : 0;
  }
  std::int64_t i64(const row_t &row, std::string_view key) {
    const auto *f=field(row,key); EXPECT_TRUE(f && f->type==type_e::signed_integer) << key; return f ? f->i : 0;
  }
  std::string_view text(const row_t &row, std::string_view key) {
    const auto *f=field(row,key); EXPECT_TRUE(f && f->type==type_e::text) << key; return f ? f->text : std::string_view {};
  }
  bool null(const row_t &row, std::string_view key) {
    const auto *f=field(row,key); return f && f->type==type_e::null;
  }
  std::int64_t steady_ns(test_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
  }
  nrt::callback_t facts(std::uint64_t generation=615) {
    nrt::callback_t c; c.dispatched=true; c.generation=generation; c.trace_scope=77; c.callback=78; c.enet_smoothed_rtt_ms=4;
    return c;
  }
  constexpr const char *k_report_keys[] {
    "report_sequence","report_end_us","report_interval_us","report_flags","report_receive_loop_tick","receive_loop",
    "missing_reports","report_data_packets","report_parity_packets","report_data_udp_bytes","report_parity_udp_bytes",
    "report_last_accepted_age_us","report_first_frame","report_last_frame","report_sample_count","availability","frame_reason"};
  constexpr const char *k_tuple_keys[] {
    "encoder_lifetime","encoder_stream_generation","encoder_application_sequence","encoder_origin_revision",
    "configured_encoder_kbps","encoder_receipt_origin"};
  void expect_refusal_row(const row_t &row, std::string_view admission) {
    EXPECT_STREQ(row.kind,nrt::admission_kind); EXPECT_EQ(row.size,nrt::admission_fields); EXPECT_FALSE(row.overflow);
    EXPECT_EQ(text(row,"admission"),admission); EXPECT_EQ(u(row,"admitted"),0u); EXPECT_EQ(u(row,"sample_rows"),0u);
    EXPECT_EQ(text(row,"report_attribution"),"none");
    for(const auto *key:k_report_keys) EXPECT_TRUE(null(row,key)) << key;
    for(const auto *key:k_tuple_keys) EXPECT_TRUE(null(row,key)) << key;
  }
}
TEST_F(NativeReception, TraceCopiesExactlyThisAdmittedJoinedPayloadAndItsRetainedApiBounds) {
  frame(1);packet(1,0);observer->record(1,1,1200,true,now-90ms,now-88ms,true);
  auto r=reading(2);r.data_packets=1;r.data_bytes=1000;r.parity_packets=1;r.parity_bytes=1200;
  r.samples[0].age_us=30000;r.samples[1]={1,1,15000};r.end_us=1'000'000'000'000ULL;
  nr::admission_trace_t t;
  ASSERT_EQ(observer->accept(wire(r),true,now,&t),nr::reason_e::joined);
  rows_t out;ASSERT_EQ(nrt::emit(t,facts(),out),3u);ASSERT_EQ(out.rows.size(),3u);
  const auto &h=out.rows[0];
  EXPECT_STREQ(h.kind,"native_admission");EXPECT_EQ(h.size,34u);EXPECT_FALSE(h.overflow);
  EXPECT_EQ(u(h,"trace_scope"),77u);EXPECT_EQ(u(h,"session_generation"),615u);EXPECT_EQ(u(h,"callback"),78u);
  EXPECT_EQ(u(h,"scope_generation"),615u);EXPECT_EQ(text(h,"report_attribution"),"admitted_payload");
  EXPECT_EQ(text(h,"admission"),"joined");EXPECT_EQ(u(h,"admitted"),1u);
  EXPECT_EQ(i64(h,"received_steady_ns"),steady_ns(now));EXPECT_EQ(u(h,"enet_smoothed_rtt_ms"),4u);
  EXPECT_EQ(u(h,"report_sequence"),1u);EXPECT_EQ(u(h,"report_end_us"),1'000'000'000'000ULL);
  EXPECT_EQ(u(h,"report_interval_us"),100000u);EXPECT_EQ(u(h,"report_flags"),nr::has_accepted);
  EXPECT_EQ(u(h,"report_receive_loop_tick"),10u);EXPECT_EQ(text(h,"receive_loop"),"baseline");
  EXPECT_EQ(u(h,"missing_reports"),0u);EXPECT_EQ(u(h,"report_data_packets"),1u);EXPECT_EQ(u(h,"report_parity_packets"),1u);
  EXPECT_EQ(u(h,"report_data_udp_bytes"),1000u);EXPECT_EQ(u(h,"report_parity_udp_bytes"),1200u);
  EXPECT_EQ(u(h,"report_last_accepted_age_us"),10000u);EXPECT_EQ(u(h,"report_first_frame"),1u);
  EXPECT_EQ(u(h,"report_last_frame"),1u);EXPECT_EQ(u(h,"report_sample_count"),2u);
  EXPECT_EQ(text(h,"availability"),"joined");EXPECT_EQ(text(h,"frame_reason"),"joined");
  EXPECT_EQ(u(h,"encoder_lifetime"),setting.encoder_lifetime);EXPECT_EQ(u(h,"encoder_stream_generation"),setting.stream_generation);
  EXPECT_EQ(u(h,"encoder_application_sequence"),setting.application_sequence);
  EXPECT_EQ(u(h,"encoder_origin_revision"),setting.origin_revision);
  EXPECT_EQ(i64(h,"configured_encoder_kbps"),setting.configured_encoder_kbps);
  EXPECT_EQ(text(h,"encoder_receipt_origin"),"successful_open");EXPECT_EQ(u(h,"sample_rows"),2u);
  EXPECT_EQ(text(h,"clock_domains"),"host_steady_ns;client_connection_us");
  // Raw copies in their own clock domains; no subtraction, no derived delay field.
  const struct { std::uint16_t seq; std::uint32_t age; test_clock::time_point begin, end; const char *kind; std::uint64_t bytes; } want[] {
    {0,30000,now-100ms,now-99ms,"data",1000},{1,15000,now-90ms,now-88ms,"parity",1200}};
  for(std::size_t n=0;n<2;++n) {
    const auto &s=out.rows[n+1];
    EXPECT_STREQ(s.kind,"native_admission_sample");EXPECT_EQ(s.size,12u);EXPECT_FALSE(s.overflow);
    EXPECT_EQ(u(s,"trace_scope"),77u);EXPECT_EQ(u(s,"session_generation"),615u);EXPECT_EQ(u(s,"callback"),78u);
    EXPECT_EQ(u(s,"report_sequence"),1u);EXPECT_EQ(u(s,"sample_index"),n);EXPECT_EQ(u(s,"frame_index"),1u);
    EXPECT_EQ(u(s,"rtp_sequence"),want[n].seq);EXPECT_EQ(u(s,"socket_read_age_us"),want[n].age);
    EXPECT_EQ(i64(s,"sender_api_begin_ns"),steady_ns(want[n].begin));EXPECT_EQ(i64(s,"sender_api_end_ns"),steady_ns(want[n].end));
    EXPECT_EQ(text(s,"packet_kind"),want[n].kind);EXPECT_EQ(u(s,"udp_bytes"),want[n].bytes);
    for(const auto *derived:{"dispersion_lower_ns","dispersion_upper_ns","handoff_duration_ns","queue_delay","one_way_delay"})
      EXPECT_FALSE(field(s,derived)) << derived;
  }
}
TEST_F(NativeReception, TraceOfAnAdmittedButUnavailableReportCarriesNoTupleOrSamples) {
  frame(1);frame(3);packet();auto r=reading();r.last_frame=3;
  nr::admission_trace_t t;ASSERT_EQ(observer->accept(wire(r),true,now,&t),nr::reason_e::joined);
  EXPECT_TRUE(t.admitted);EXPECT_EQ(t.availability,nr::reason_e::frame_unavailable);EXPECT_FALSE(t.setting);EXPECT_EQ(t.sample_count,0u);
  rows_t out;ASSERT_EQ(nrt::emit(t,facts(),out),1u);
  const auto &h=out.rows[0];EXPECT_EQ(h.size,34u);
  EXPECT_EQ(text(h,"admission"),"joined");EXPECT_EQ(u(h,"admitted"),1u);  // admission is not availability
  EXPECT_EQ(text(h,"report_attribution"),"admitted_payload");
  EXPECT_EQ(u(h,"report_sequence"),1u);EXPECT_EQ(u(h,"report_last_frame"),3u);
  EXPECT_EQ(text(h,"availability"),"frame_unavailable");EXPECT_EQ(text(h,"frame_reason"),"host_frame_missing");
  for(const auto *key:k_tuple_keys) EXPECT_TRUE(null(h,key)) << key;
  EXPECT_EQ(u(h,"sample_rows"),0u);
}
TEST_F(NativeReception, TraceRefusalAfterValidAndDuplicateNeverReemitsOrRefreshes) {
  frame(1);packet();
  const auto valid=wire(reading());
  nr::admission_trace_t t;ASSERT_EQ(observer->accept(valid,true,now,&t),nr::reason_e::joined);
  rows_t first;ASSERT_EQ(nrt::emit(t,facts(),first),2u);
  now+=50ms;
  // The same bytes again, malformed bytes, unauthenticated bytes and a retained-identity contradiction.
  auto contradiction=reading();contradiction.sequence=2;contradiction.end_us=200000;
  contradiction.data_packets=0;contradiction.data_bytes=0;contradiction.parity_packets=1;contradiction.parity_bytes=1000;
  const struct { std::string payload; bool authenticated; const char *admission; } refused[] {
    {valid,true,"report_order"},{"x",true,"malformed_size"},{wire(contradiction),false,"unauthenticated"},
    {wire(contradiction),true,"sample_counts"}};
  for(const auto &input:refused) {
    nr::admission_trace_t again;
    EXPECT_STREQ(nr::name(observer->accept(input.payload,input.authenticated,now,&again)),input.admission);
    EXPECT_FALSE(again.admitted);EXPECT_EQ(again.generation,615u);EXPECT_EQ(again.received_steady_ns,steady_ns(now));
    EXPECT_EQ(again.report.sequence,0u);EXPECT_EQ(again.sample_count,0u);EXPECT_FALSE(again.setting);
    rows_t out;ASSERT_EQ(nrt::emit(again,facts(),out),1u) << input.admission;
    expect_refusal_row(out.rows[0],input.admission);
  }
  // The earlier observation is neither re-emitted nor refreshed by any refusal.
  auto current=observer->readback(now);ASSERT_TRUE(current);
  EXPECT_EQ(current->report.sequence,1u);EXPECT_EQ(current->host_report_age_ms,50u);EXPECT_TRUE(current->available);
}
TEST_F(NativeReception, TraceRetirementAndANewGenerationStartFresh) {
  frame(1);packet();
  nr::admission_trace_t t;ASSERT_EQ(observer->accept(wire(reading()),true,now,&t),nr::reason_e::joined);
  ASSERT_EQ(t.sample_count,1u);
  observer->retire();
  auto next=reading();next.sequence=2;next.end_us=200000;
  nr::admission_trace_t retired;EXPECT_EQ(observer->accept(wire(next),true,now,&retired),nr::reason_e::retired);
  rows_t out;ASSERT_EQ(nrt::emit(retired,facts(),out),1u);expect_refusal_row(out.rows[0],"retired");
  // A new generation has its own ledger, scope and tuple; its first report starts again at sequence 1.
  auto frames616=std::make_shared<ff::sender_ledger_t>(616);nr::scope_t fresh(616,frames616,true);
  const auto setting616=actual_setting(616);
  auto ticket=frames616->begin(1,setting616,now-110ms);ASSERT_TRUE(ticket);
  frames616->complete(*ticket,ff::send_disposition_e::sent,now-20ms);
  fresh.record(1,0,1000,false,now-100ms,now-99ms,true);
  nr::admission_trace_t first;ASSERT_EQ(fresh.accept(wire(reading()),true,now,&first),nr::reason_e::joined);
  rows_t rows;ASSERT_EQ(nrt::emit(first,facts(616),rows),2u);
  EXPECT_EQ(first.generation,616u);EXPECT_EQ(u(rows.rows[0],"session_generation"),616u);
  EXPECT_EQ(u(rows.rows[0],"report_sequence"),1u);EXPECT_EQ(u(rows.rows[0],"encoder_stream_generation"),616u);
  EXPECT_EQ(text(rows.rows[0],"receive_loop"),"baseline");EXPECT_EQ(u(rows.rows[0],"missing_reports"),0u);
}
TEST_F(NativeReception, TraceRowsAreBoundedFixedStaticAndCarryNoIdentity) {
  frame(1);for(unsigned n=0;n<16;++n) packet(1,static_cast<std::uint16_t>(n));
  const auto payload=wire(reading(16));
  nr::admission_trace_t t;ASSERT_EQ(observer->accept(payload,true,now,&t),nr::reason_e::joined);
  rows_t out;ASSERT_EQ(nrt::emit(t,facts(),out),17u);
  ASSERT_EQ(out.rows.size(),1u+nr::max_samples);
  static_assert(nrt::admission_fields<=vrr_trace::k_fields && nrt::sample_fields<=vrr_trace::k_fields);
  for(std::size_t n=0;n<out.rows.size();++n) {
    const auto &row=out.rows[n];
    EXPECT_FALSE(row.overflow);EXPECT_EQ(row.size,n ? nrt::sample_fields : nrt::admission_fields);
    if(n) EXPECT_EQ(u(row,"sample_index"),n-1);
    std::set<std::string_view> keys;
    for(std::size_t k=0;k<row.size;++k) {
      const std::string_view key=row.fields[k].key;
      EXPECT_TRUE(keys.insert(key).second) << key;
      for(const auto *forbidden:{"token","uuid","addr","cert","nonce","owner","device","client","name","payload","key","app_"})
        EXPECT_EQ(key.find(forbidden),std::string_view::npos) << key;
      // Text never borrows the received payload's bytes.
      const auto &f=row.fields[k];
      if(f.type==type_e::text) EXPECT_TRUE(f.text.data()<payload.data() || f.text.data()>=payload.data()+payload.size()) << key;
    }
  }
  // A refusal row has the same fixed shape; a corrupted count can never exceed sixteen rows.
  nr::admission_trace_t refused;EXPECT_EQ(observer->accept(payload,true,now,&refused),nr::reason_e::report_order);
  rows_t one;ASSERT_EQ(nrt::emit(refused,facts(),one),1u);EXPECT_EQ(one.rows[0].size,nrt::admission_fields);
  auto forced=t;forced.sample_count=200;
  rows_t capped;EXPECT_EQ(nrt::emit(forced,facts(),capped),1u+nr::max_samples);
}
TEST_F(NativeReception, TraceMarksAnUndispatchedCallbackWithoutAnyReport) {
  frame(1);packet();
  nr::admission_trace_t t;ASSERT_EQ(observer->accept(wire(reading()),true,now,&t),nr::reason_e::joined);
  auto c=facts();c.dispatched=false;c.trace_scope=0;c.enet_smoothed_rtt_ms.reset();
  nr::admission_trace_t undispatched;undispatched.received_steady_ns=5;
  for(const auto *a:{&undispatched,&t}) {  // even facts left in a reused value cannot leak rows
    rows_t out;ASSERT_EQ(nrt::emit(*a,c,out),1u);
    expect_refusal_row(out.rows[0],"not_dispatched");
    EXPECT_TRUE(null(out.rows[0],"trace_scope"));EXPECT_TRUE(null(out.rows[0],"enet_smoothed_rtt_ms"));
  }
}
TEST_F(NativeReception, UntracedAndTracedAcceptMakeIdenticalDecisionsAndRecorderStateCannotChangeThem) {
  nr::scope_t twin(615,frames,true);
  const auto both=[&](std::uint32_t f,std::uint16_t s) { observer->record(f,s,1000,false,now-100ms,now-99ms,true);
    twin.record(f,s,1000,false,now-100ms,now-99ms,true); };
  frame(1);frame(3);both(1,0);both(3,0);
  std::vector<std::string> inputs;
  auto r=reading();inputs.push_back(wire(r));inputs.push_back(wire(r));inputs.push_back("x");
  r.sequence=2;r.end_us=200000;r.first_frame=1;r.last_frame=2;inputs.push_back(wire(r));  // frame 2 absent
  auto contradiction=reading();contradiction.sequence=3;contradiction.end_us=300000;contradiction.data_packets=0;
  contradiction.data_bytes=0;contradiction.parity_packets=1;contradiction.parity_bytes=1000;inputs.push_back(wire(contradiction));
  r=reading();r.sequence=3;r.end_us=300000;r.first_frame=r.last_frame=3;r.samples[0]={3,0,50000};inputs.push_back(wire(r));
  // A refused recorder is installed while traced rows are emitted: admission never depends on it.
  auto refused=vrr_trace::recorder_t::open("relative-path-refused");ASSERT_TRUE(refused);
  EXPECT_NE(refused->status(),vrr_trace::status_e::running);
  vrr_trace::test_recorder_scope_t active(refused.get());
  for(const auto &input:inputs) {
    nr::admission_trace_t t;
    const auto plain=observer->accept(input,true,now);
    const auto traced=twin.accept(input,true,now,&t);
    EXPECT_EQ(plain,traced);EXPECT_EQ(nrt::emit(t,facts(),[](const row_t &row) { vrr_trace::record(row); }) >= 1u,true);
    auto a=observer->readback(now),b=twin.readback(now);ASSERT_TRUE(a && b);
    EXPECT_EQ(nr::readback_json(*a).dump(),nr::readback_json(*b).dump());
    now+=10ms;
  }
  auto last=twin.readback(now);ASSERT_TRUE(last);EXPECT_EQ(last->report.sequence,3u);EXPECT_TRUE(last->available);
}
TEST_F(NativeReception, TraceNeverRelabelsAnotherGenerationsOrAnOldSnapshot) {
  frame(1);packet();
  nr::admission_trace_t t;ASSERT_EQ(observer->accept(wire(reading()),true,now,&t),nr::reason_e::joined);
  ASSERT_EQ(t.generation,615u);ASSERT_EQ(t.sample_count,1u);
  rows_t own;ASSERT_EQ(nrt::emit(t,facts(615),own),2u);  // the discriminating control: same snapshot, own callback
  // The identical retained snapshot under another generation's callback, after this scope retired,
  // or with no scope generation at all, must neither relabel its report nor emit its samples.
  observer->retire();
  auto zero=t;zero.generation=0;
  const struct { const nr::admission_trace_t *a; std::uint64_t callback_generation; } cases[] {{&t,616},{&t,0},{&zero,0},{&zero,615}};
  for(const auto &c:cases) {
    rows_t out;ASSERT_EQ(nrt::emit(*c.a,facts(c.callback_generation),out),1u) << c.callback_generation;
    const auto &h=out.rows[0];EXPECT_EQ(h.size,nrt::admission_fields);
    EXPECT_EQ(text(h,"admission"),"joined");EXPECT_EQ(u(h,"admitted"),1u);
    EXPECT_EQ(text(h,"report_attribution"),"generation_mismatch");
    EXPECT_EQ(u(h,"session_generation"),c.callback_generation);
    if(c.a->generation) EXPECT_EQ(u(h,"scope_generation"),615u); else EXPECT_TRUE(null(h,"scope_generation"));
    EXPECT_EQ(u(h,"sample_rows"),0u);
    for(const auto *key:k_report_keys) EXPECT_TRUE(null(h,key)) << key;
    for(const auto *key:k_tuple_keys) EXPECT_TRUE(null(h,key)) << key;
  }
}
