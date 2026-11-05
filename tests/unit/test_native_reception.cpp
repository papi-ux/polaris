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
  // Genuine capacity alias: frame84/seq834 is overwritten by frame94/seq(834+16384).
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
    for(unsigned n=0;n<10;++n) packet(f,static_cast<std::uint16_t>(f==94 && n==6 ? 834+nr::ledger_capacity : (f-1)*10+n));
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
    for(unsigned n=0;n<10;++n) packet(f,static_cast<std::uint16_t>(f==94 && n==6 ? 834+nr::ledger_capacity : (f-1)*10+n));
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
  // Frame84 has two data/five parity packets, subsequent frames ten data.
  // Only parity84/834 is evicted by data94/(834+16384). All102 data remain, so103 is impossible.
  std::uint16_t sequence=830;
  for(unsigned f=84;f<=94;++f) {
    frame(f);
    for(unsigned n=0;n<(f==84 ? 7u : 10u);++n) {
      packet(f,sequence==936 ? static_cast<std::uint16_t>(834+nr::ledger_capacity) : sequence,f==84 && n>=2);
      ++sequence;
    }
  }
  auto report=reading();report.first_frame=84;report.last_frame=94;
  report.samples[0]={84,830,50000};report.data_packets=103;report.data_bytes=103000;
  EXPECT_EQ(observer->accept(wire(report),true,now),nr::reason_e::sample_counts);
  auto snapshot=observer->readback(now);ASSERT_TRUE(snapshot);EXPECT_FALSE(snapshot->has_report);
}

TEST_F(NativeReception, EvictionCompletenessMetadataIsSerializedBetweenSenderAndControl) {
  for(unsigned f=84;f<=94;++f) {
    frame(f);
    for(unsigned n=0;n<10;++n) packet(f,static_cast<std::uint16_t>(f==94 && n==6 ? 834+nr::ledger_capacity : (f-1)*10+n));
  }
  std::atomic_bool start {false};
  std::thread sender([&] {
    while(!start.load()) std::this_thread::yield();
    for(unsigned n=0;n<128;++n) {
      packet(84,834);packet(94,static_cast<std::uint16_t>(834+nr::ledger_capacity)); // alternate a genuine capacity alias.
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
  frame(1);frame(3);both(1,0);both(3,1);
  std::vector<std::string> inputs;
  auto r=reading();inputs.push_back(wire(r));inputs.push_back(wire(r));inputs.push_back("x");
  r.sequence=2;r.end_us=200000;r.first_frame=1;r.last_frame=2;inputs.push_back(wire(r));  // frame 2 absent
  auto contradiction=reading();contradiction.sequence=3;contradiction.end_us=300000;contradiction.data_packets=0;
  contradiction.data_bytes=0;contradiction.parity_packets=1;contradiction.parity_bytes=1000;inputs.push_back(wire(contradiction));
  r=reading();r.sequence=3;r.end_us=300000;r.first_frame=r.last_frame=3;r.samples[0]={3,1,50000};inputs.push_back(wire(r));
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

namespace {
  namespace nvs = native_video_signal;
  nvs::context_t signal_context(std::int64_t now_ns, int rate = 18000) {
    nvs::context_t c;
    c.reason=nvs::context_reason_e::ready;c.observed=true;c.generation_matches=c.app_matches=c.exclusive=true;
    c.configured_enabled=c.enabled=c.runtime_supported=c.in_place=c.applied_known=true;
    c.base_kbps=c.requested_kbps=c.applied_kbps=rate;c.floor_kbps=2000;
    c.operator_revision=c.action_revision=c.applied_revision=80;c.inspected_steady_ns=now_ns;c.applied_age_ns=1;
    return c;
  }
  nvs::caller_t signal_caller() { return {615,true,false,true,true,false}; }
}

TEST_F(NativeReception, SignalUsesSignedApiBoundsAndCancelsHostAndClientClockOffsets) {
  frame(1);packet();observer->record(1,1,1200,true,now-90ms,now-88ms,true);
  auto r=reading(2);r.parity_packets=1;r.parity_bytes=1200;r.data_packets=1;r.data_bytes=1000;
  r.samples[0].age_us=30000;r.samples[1]={1,1,15000};r.end_us=1'000'000'000'000ULL;
  nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(r),true,now,&a),nr::reason_e::joined);
  auto c=signal_context(a.received_steady_ns);
  const auto s=nvs::project(a,signal_caller(),c);
  ASSERT_TRUE(s.inspection_ready);ASSERT_TRUE(s.measurement_available);ASSERT_TRUE(s.setting);
  EXPECT_EQ(s.samples[0].elapsed_lower_ns,69'000'000);EXPECT_EQ(s.samples[0].elapsed_upper_ns,70'000'000);
  EXPECT_EQ(s.samples[1].elapsed_lower_ns,73'000'000);EXPECT_EQ(s.samples[1].elapsed_upper_ns,75'000'000);
  EXPECT_FALSE(s.samples[0].parity);EXPECT_TRUE(s.samples[1].parity);
  EXPECT_EQ(s.data_bytes,1000u);EXPECT_EQ(s.parity_bytes,1200u);
  for(unsigned i=0;i<2;++i) EXPECT_EQ(*s.samples[i].elapsed_upper_ns-*s.samples[i].elapsed_lower_ns,
    a.samples[i].sender_api_end_ns-a.samples[i].sender_api_begin_ns);
  a.received_steady_ns+=1'000'000'000'000LL;c.inspected_steady_ns=a.received_steady_ns;
  for(unsigned i=0;i<2;++i) { a.samples[i].sender_api_begin_ns+=1'000'000'000'000LL;a.samples[i].sender_api_end_ns+=1'000'000'000'000LL; }
  a.report.end_us+=2'000'000'000'000ULL;
  const auto shifted=nvs::project(a,signal_caller(),c);
  EXPECT_EQ(shifted.samples[0].elapsed_lower_ns,s.samples[0].elapsed_lower_ns);
  EXPECT_EQ(shifted.samples[1].elapsed_upper_ns,s.samples[1].elapsed_upper_ns);
}

TEST_F(NativeReception, SignalRetainsNegativeIntervalsWithoutClampingOrReadiness) {
  frame(1);packet();auto r=reading();r.end_us=1'000'000;r.samples[0].age_us=200000;
  nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(r),true,now,&a),nr::reason_e::joined);
  auto s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));
  EXPECT_EQ(s.reason,nvs::reason_e::negative_interval);EXPECT_FALSE(s.measurement_available);EXPECT_FALSE(s.inspection_ready);
  EXPECT_EQ(s.samples[0].elapsed_lower_ns,-101'000'000);EXPECT_EQ(s.samples[0].elapsed_upper_ns,-100'000'000);
  a.samples[0].sender_api_end_ns=a.received_steady_ns+1;
  s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));
  EXPECT_EQ(s.reason,nvs::reason_e::invalid_time);EXPECT_FALSE(s.samples[0].elapsed_lower_ns);
  EXPECT_FALSE(s.samples[0].elapsed_upper_ns);
}

TEST_F(NativeReception, SignalIsPerReportAndSparseSamplesDoNotMultiplyCountersOrBorrowRevision) {
  frame(1);for(unsigned i=0;i<30;++i) packet(1,static_cast<std::uint16_t>(i));
  auto r=reading(16);r.flags|=nr::timing_sample_limit;r.data_packets=30;r.data_bytes=30000;
  nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(r),true,now,&a),nr::reason_e::joined);
  auto c=signal_context(a.received_steady_ns);c.operator_revision=9999;c.applied_revision=9999;
  auto s=nvs::project(a,signal_caller(),c);
  ASSERT_TRUE(s.inspection_ready);EXPECT_TRUE(s.sparse);EXPECT_EQ(s.sample_count,16u);EXPECT_EQ(s.data_packets,30u);
  EXPECT_EQ(s.setting->origin_revision,8u);EXPECT_EQ(s.context.operator_revision,9999u);
  c.applied_kbps=17000;s=nvs::project(a,signal_caller(),c);
  EXPECT_TRUE(s.measurement_available);EXPECT_FALSE(s.inspection_ready);EXPECT_EQ(s.reason,nvs::reason_e::source_rate_mismatch);
  a.sample_count=17;s=nvs::project(a,signal_caller(),c);EXPECT_EQ(s.reason,nvs::reason_e::invalid_samples);EXPECT_EQ(s.sample_count,0u);
}

TEST_F(NativeReception, SignalUnavailableAndCallerOrContextExclusionsNeverReemitOldDerivedFacts) {
  frame(1);packet();nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(reading()),true,now,&a),nr::reason_e::joined);
  auto c=signal_context(a.received_steady_ns);
  for(auto reason:{nr::reason_e::incomplete,nr::reason_e::no_accepted,nr::reason_e::no_samples,nr::reason_e::packet_stale,
      nr::reason_e::packet_missing,nr::reason_e::packet_ambiguous,nr::reason_e::retention_limited,nr::reason_e::frame_unavailable,
      nr::reason_e::report_stale,nr::reason_e::clock_unavailable}) {
    auto unavailable=a;unavailable.availability=reason;
    const auto s=nvs::project(unavailable,signal_caller(),c);EXPECT_FALSE(s.inspection_ready);EXPECT_FALSE(s.measurement_available);
    EXPECT_FALSE(s.setting);EXPECT_EQ(s.sample_count,0u);EXPECT_FALSE(s.samples[0].elapsed_lower_ns);
  }
  for(unsigned i=0;i<4;++i) {
    auto caller=signal_caller();if(i==0)caller.running=false;if(i==1)caller.watch_or_input_only=true;
    if(i==2)caller.negotiated=false;if(i==3)caller.control_authenticated=false;
    const auto s=nvs::project(a,caller,c);EXPECT_TRUE(s.measurement_available);EXPECT_FALSE(s.inspection_ready);
    EXPECT_EQ(s.reason,nvs::reason_e::caller_unready);
  }
  c.reason=nvs::context_reason_e::busy;c.observed=false;
  const auto s=nvs::project(a,signal_caller(),c);EXPECT_TRUE(s.measurement_available);EXPECT_FALSE(s.inspection_ready);
  EXPECT_EQ(s.reason,nvs::reason_e::context_unready);
}

TEST_F(NativeReception, SignalProductionReadbackWorksWithTraceOffAndNeverRecomputesAtPollTime) {
  vrr_trace::test_recorder_scope_t off(nullptr);EXPECT_FALSE(vrr_trace::enabled());
  frame(1);packet();nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(reading()),true,now,&a),nr::reason_e::joined);
  const auto s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));ASSERT_TRUE(observer->publish_signal(s,now));
  const auto current=observer->readback(now+50ms);ASSERT_TRUE(current && current->signal);
  EXPECT_TRUE(current->signal->inspection_ready);EXPECT_EQ(current->signal->samples[0].elapsed_lower_ns,s.samples[0].elapsed_lower_ns);
  EXPECT_EQ(current->host_report_age_ms,50u);
  const auto j=nr::readback_json(*current).dump();EXPECT_EQ(j.find("inspection"),std::string::npos);EXPECT_EQ(j.find("elapsed_lower"),std::string::npos);
  const auto stale=observer->readback(now+2001ms);ASSERT_TRUE(stale);EXPECT_FALSE(stale->signal);EXPECT_FALSE(stale->available);
}

TEST_F(NativeReception, SignalPublicationRechecksCurrentReportAndTerminalRetirement) {
  frame(1);packet();nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(reading()),true,now,&a),nr::reason_e::joined);
  const auto old=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));ASSERT_TRUE(observer->publish_signal(old,now));
  auto r=reading();r.sequence=2;r.end_us+=100000;now+=100ms;
  nr::admission_trace_t b;ASSERT_EQ(observer->accept(wire(r),true,now,&b),nr::reason_e::joined);
  EXPECT_FALSE(observer->readback(now)->signal);EXPECT_FALSE(observer->publish_signal(old,now));
  const auto newer=nvs::project(b,signal_caller(),signal_context(b.received_steady_ns));ASSERT_TRUE(observer->publish_signal(newer,now));
  EXPECT_FALSE(observer->publish_signal(nvs::signal_t{},now)); // An undispatched callback has no scope copy.
  EXPECT_FALSE(observer->readback(now)->signal);ASSERT_TRUE(observer->publish_signal(newer,now));
  nr::admission_trace_t duplicate;EXPECT_EQ(observer->accept(wire(r),true,now,&duplicate),nr::reason_e::report_order);
  EXPECT_FALSE(observer->publish_signal(nvs::project(duplicate,signal_caller(),signal_context(duplicate.received_steady_ns)),now));
  EXPECT_FALSE(observer->readback(now)->signal);EXPECT_EQ(observer->readback(now)->report.sequence,2u);
  observer->retire();EXPECT_FALSE(observer->publish_signal(newer,now));EXPECT_FALSE(observer->readback(now));
}

TEST_F(NativeReception, SignalDelayedInspectionAndPublicationReuseExistingReportSenderAndFrameFreshness) {
  frame(1);packet();nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(reading()),true,now,&a),nr::reason_e::joined);
  auto c=signal_context(a.received_steady_ns+2'000'000'001LL);
  auto s=nvs::project(a,signal_caller(),c);EXPECT_TRUE(s.measurement_available);EXPECT_FALSE(s.inspection_ready);
  EXPECT_EQ(s.reason,nvs::reason_e::report_stale);
  c=signal_context(a.received_steady_ns+1'950'000'000LL);
  s=nvs::project(a,signal_caller(),c);EXPECT_FALSE(s.inspection_ready);EXPECT_EQ(s.reason,nvs::reason_e::sender_stale);
  s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));ASSERT_TRUE(s.inspection_ready);
  auto why=nvs::reason_e::ready;
  EXPECT_FALSE(observer->publish_signal(s,now+2001ms,&why));EXPECT_EQ(why,nvs::reason_e::report_stale);
  EXPECT_FALSE(observer->readback(now)->signal);
  EXPECT_FALSE(observer->publish_signal(s,now+1950ms,&why));EXPECT_EQ(why,nvs::reason_e::sender_stale);
  frames->retire();EXPECT_FALSE(observer->publish_signal(s,now,&why));EXPECT_EQ(why,nvs::reason_e::frame_unavailable);
}

TEST_F(NativeReception, SignalMixedSameKbpsNewApplicationAndForeignGenerationCannotBorrowReadiness) {
  frame(1);packet();auto next=setting;++next.application_sequence;frame(2,ff::send_disposition_e::sent,next);
  auto r=reading();r.last_frame=2;nr::admission_trace_t a;
  ASSERT_EQ(observer->accept(wire(r),true,now,&a),nr::reason_e::joined);EXPECT_EQ(a.availability,nr::reason_e::frame_unavailable);
  auto s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));EXPECT_FALSE(s.setting);EXPECT_FALSE(s.inspection_ready);
  a.admitted=true;a.availability=nr::reason_e::joined;a.setting=setting;a.generation=616;
  s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));EXPECT_EQ(s.reason,nvs::reason_e::generation_mismatch);
  EXPECT_FALSE(s.setting);EXPECT_EQ(s.sample_count,0u);EXPECT_FALSE(s.admitted);EXPECT_EQ(s.sequence,0u);
  EXPECT_FALSE(s.context.observed);EXPECT_EQ(s.context.operator_revision,0u);
}

TEST_F(NativeReception, SignalCompanionRowsKeepFortyFieldBoundAndCannotRelabelAReport) {
  frame(1);for(unsigned i=0;i<16;++i) packet(1,static_cast<std::uint16_t>(i));
  nr::admission_trace_t a;ASSERT_EQ(observer->accept(wire(reading(16)),true,now,&a),nr::reason_e::joined);
  const auto s=nvs::project(a,signal_caller(),signal_context(a.received_steady_ns));
  rows_t rows;EXPECT_EQ(nrt::emit_signal(s,true,facts(),rows),17u);ASSERT_EQ(rows.rows.size(),17u);
  EXPECT_EQ(rows.rows[0].size,40u);EXPECT_FALSE(rows.rows[0].overflow);EXPECT_EQ(u(rows.rows[0],"inspection_ready"),1u);
  for(std::size_t i=1;i<rows.rows.size();++i) { EXPECT_EQ(rows.rows[i].size,12u);EXPECT_FALSE(rows.rows[i].overflow); }
  for(const auto &row:rows.rows) for(std::size_t i=0;i<row.size;++i) {
    const std::string key=row.fields[i].key;
    for(const auto *secret:{"token","address","uuid","certificate","key_material"}) EXPECT_EQ(key.find(secret),std::string::npos);
  }
  auto wrong=facts(616);rows_t denied;EXPECT_EQ(nrt::emit_signal(s,true,wrong,denied),1u);
  EXPECT_TRUE(null(denied.rows[0],"report_sequence"));EXPECT_EQ(u(denied.rows[0],"inspection_ready"),0u);
  EXPECT_EQ(u(denied.rows[0],"published"),0u);EXPECT_TRUE(null(denied.rows[0],"applied_age_ns"));
  auto refusal=a;refusal.admitted=false;
  const auto not_admitted=nvs::project(refusal,signal_caller(),signal_context(a.received_steady_ns));
  EXPECT_FALSE(not_admitted.context.observed);EXPECT_EQ(not_admitted.sequence,0u);
  rows_t refused;EXPECT_EQ(nrt::emit_signal(not_admitted,true,facts(),refused),1u);
  EXPECT_TRUE(null(refused.rows[0],"report_sequence"));EXPECT_EQ(u(refused.rows[0],"context_observed"),0u);
  EXPECT_EQ(u(refused.rows[0],"operator_revision"),0u);EXPECT_EQ(u(refused.rows[0],"sample_count"),0u);
  EXPECT_LT(sizeof(nr::scope_t),540000u);EXPECT_EQ(nr::ledger_capacity,16384u);EXPECT_EQ(nvs::max_samples,16u);
}

#include "src/live_tuning_native.h"
namespace {
  native_video_signal::signal_t native_trial_signal(int ms, int lower_us, int upper_us,
                                                   std::uint32_t sequence = 0) {
    native_video_signal::signal_t s;
    s.admitted = s.measurement_available = s.inspection_ready = true;
    s.sequence = sequence ? sequence : static_cast<std::uint32_t>(ms + 1);
    s.received_steady_ns = static_cast<std::int64_t>(ms) * 1'000'000;
    s.setting = ff::encoder_setting_t {9, 615, 1, 8, 20000, bp::receipt_origin_e::successful_open};
    s.context.action_revision = 10; s.context.floor_kbps = 2000;
    s.sample_count = 1;
    s.samples[0].reason = native_video_signal::reason_e::ready;
    s.samples[0].elapsed_lower_ns = static_cast<std::int64_t>(lower_us) * 1000;
    s.samples[0].elapsed_upper_ns = static_cast<std::int64_t>(upper_us) * 1000;
    return s;
  }
  template<class Metric>
  std::vector<int> native_trial_feed(live_tuning_native::episode_t &episode, int seconds, Metric metric) {
    std::vector<int> requests;
    for (int ms = 0; ms <= seconds * 1000; ms += 100) {
      const auto lower = metric(ms / 1000);
      if (const auto next = episode.observe(native_trial_signal(ms, lower, lower + 100))) requests.push_back(*next);
    }
    return requests;
  }
}

TEST(NativeDownwardEpisode, NoisyStandingPlateauPermitsExactlyOneFifteenPercentTrial) {
  live_tuning_native::episode_t episode;
  const auto requests = native_trial_feed(episode, 30, [](int second) {
    return second < 4 ? 3000 : 30000 + (second % 3 - 1) * 150;
  });
  ASSERT_EQ(requests.size(), 1u); EXPECT_EQ(requests.front(), 17000);
  EXPECT_EQ(episode.evidence().source.origin_revision, 8u);
  EXPECT_EQ(episode.evidence().reference_upper_ns, 3'100'000);
}

TEST(NativeDownwardEpisode, GenuineFallSpikeAndUnderpopulatedBucketsHaveNoAuthority) {
  for (const int mode : {0, 1, 2}) {
    live_tuning_native::episode_t episode;
    std::vector<int> requests;
    for (int ms = 0; ms < 24000; ms += mode == 2 ? 400 : 100) {
      const int second = ms / 1000;
      const int lower = second < 4 ? 3000 : mode == 0 ? std::max(14000, 60000 - 3000 * second) :
        mode == 1 && second > 8 ? 3000 : 30000;
      if (const auto next = episode.observe(native_trial_signal(ms, lower, lower + 100))) requests.push_back(*next);
    }
    EXPECT_TRUE(requests.empty());
  }
}

TEST(NativeDownwardEpisode, MixedTupleChangedAuthorityGapParityAndInvalidEndpointsResetReference) {
  for (const int refusal : {0, 1, 2, 3, 4}) {
    live_tuning_native::episode_t episode;
    for (int ms = 0; ms < 12000; ms += 100) {
      auto s = native_trial_signal(ms, ms < 4000 ? 3000 : 30000, ms < 4000 ? 3100 : 30100);
      if (ms == 10000) {
        if (refusal == 0) ++s.setting->application_sequence;
        if (refusal == 1) ++s.context.action_revision;
        if (refusal == 2) s.received_steady_ns += 600'000'000;
        if (refusal == 3) s.samples[0].parity = true;
        if (refusal == 4) s.samples[0].elapsed_lower_ns = -1;
      }
      EXPECT_FALSE(episode.observe(s));
    }
  }
}

TEST(NativeDownwardEpisode, SeparateBackendProofAndResponseWindowsCannotRestoreOrRecut) {
  live_tuning_native::episode_t episode;
  const auto requests = native_trial_feed(episode, 12, [](int s) { return s < 4 ? 3000 : 30000; });
  ASSERT_EQ(requests, std::vector<int> {17000});
  episode.accepted_application(41);
  for (int ms = 12100; ms <= 16200; ms += 100) {
    auto s = native_trial_signal(ms, 3500, 3600);
    s.setting->application_sequence = 2; s.setting->origin_revision = 41;
    s.setting->configured_encoder_kbps = 17000; s.setting->receipt_origin = bp::receipt_origin_e::backend_reconfiguration;
    EXPECT_FALSE(episode.observe(s));
  }
  EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::responded);
  episode.unavailable();
  EXPECT_TRUE(episode.spent()); EXPECT_EQ(episode.evidence().requested_kbps, 17000);
}

TEST(NativeDownwardEpisode, WrongBackendEpochHistoricalRevisionAndMissingProofAreNotSuccess) {
  for (const int refusal : {0, 1, 2, 3}) {
    live_tuning_native::episode_t episode;
    ASSERT_EQ(native_trial_feed(episode, 12, [](int s) { return s < 4 ? 3000 : 30000; }),
              std::vector<int> {17000});
    if (refusal != 3) episode.accepted_application(41);
    auto s = native_trial_signal(refusal == 2 ? 18101 : 12100, 3500, 3600);
    s.setting->application_sequence = 2; s.setting->origin_revision = refusal == 1 ? 8 : 41;
    s.setting->encoder_lifetime = refusal == 0 ? 10 : 9;
    s.setting->configured_encoder_kbps = 17000; s.setting->receipt_origin = bp::receipt_origin_e::backend_reconfiguration;
    EXPECT_FALSE(episode.observe(s));
    EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::unassessed);
  }
}

TEST(NativeDownwardEpisode, RealMixedAdmissionCanWaitForCurrentDWithoutExtendingProofDeadline) {
  live_tuning_native::episode_t episode;
  ASSERT_EQ(native_trial_feed(episode, 12, [](int s) { return s < 4 ? 3000 : 30000; }),
            std::vector<int> {17000});
  const auto now = test_clock::now();
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  // The two client reports end 100ms apart but arrive together: the mixed report was delayed.
  // Dispatch precedes both D sends. These fixture timestamps do not change either 6s deadline.
  const auto dispatched_ns = now_ns - 110'000'000;
  episode.dispatched_at(dispatched_ns); episode.accepted_application(41);
  auto frames = std::make_shared<ff::sender_ledger_t>(615);
  nr::scope_t native {615, frames, true};
  bp::ledger_t producer {{9, 615}};
  ASSERT_TRUE(producer.observe_receipt({{9, 615}, 1, 8, 20000, bp::receipt_origin_e::successful_open}));
  const auto c_input = producer.capture(1); ASSERT_TRUE(c_input);
  ASSERT_TRUE(producer.complete(*c_input, bp::submission_result_e::submitted_under_captured_setting));
  const auto c_output = producer.take({9, 615}, 1); ASSERT_TRUE(c_output);
  const auto c = *ff::encoder_setting_of(std::optional<const bp::record_t> {*c_output});
  ASSERT_EQ(episode.evidence().source, c);
  ASSERT_TRUE(producer.observe_receipt({{9, 615}, 2, 41, 17000, bp::receipt_origin_e::backend_reconfiguration}));
  const auto d_input = producer.capture(2); ASSERT_TRUE(d_input);
  ASSERT_TRUE(producer.complete(*d_input, bp::submission_result_e::submitted_under_captured_setting));
  const auto d_output = producer.take({9, 615}, 2); ASSERT_TRUE(d_output);
  const auto d = *ff::encoder_setting_of(std::optional<const bp::record_t> {*d_output});
  for (const auto &[id, setting] : std::vector<std::pair<unsigned, ff::encoder_setting_t>> {{1, c}, {2, d}, {3, d}}) {
    const auto sent = id < 3 ? now - 105ms : now - 5ms;
    const auto ticket = frames->begin(id, setting, sent - 1ms); ASSERT_TRUE(ticket);
    frames->complete(*ticket, ff::send_disposition_e::sent, sent + 1ms);
    native.record(id, static_cast<std::uint16_t>(id), 1000, false, sent, sent + 1ms, true);
  }
  auto r = reading(2); r.sequence = 20001; r.first_frame = 1; r.last_frame = 2;
  r.last_accepted_age_us = 1000;
  r.samples[0] = {1, 1, 1000}; r.samples[1] = {2, 2, 1000};
  nr::admission_trace_t mixed;
  native.accept(wire(r), true, now, &mixed);
  ASSERT_TRUE(mixed.admitted); EXPECT_EQ(mixed.availability, nr::reason_e::frame_unavailable);
  native_video_signal::caller_t caller {615, true, false, true, true, false};
  native_video_signal::context_t context;
  context.observed = true; context.reason = native_video_signal::context_reason_e::ready;
  context.applied_kbps = 17000; context.inspected_steady_ns = now_ns;
  auto transition = native_video_signal::project(mixed, caller, context);
  ASSERT_FALSE(transition.measurement_available);
  ASSERT_TRUE(native.publish_signal(transition));
  EXPECT_FALSE(native.with_current_signal(transition, [] {}));
  ASSERT_TRUE(native.with_current_signal(transition, [&] { episode.wait_for_application(now_ns); }, false));
  EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::awaiting_application);
  EXPECT_EQ(episode.evidence().dispatched_ns, dispatched_ns);
  r.count = 1; r.sequence = 20002; r.end_us = 200000; r.loop_tick = 20;
  r.data_packets = 1; r.data_bytes = 1000;
  r.first_frame = r.last_frame = 3; r.samples[0] = {3, 3, 1000};
  nr::admission_trace_t proven;
  native.accept(wire(r), true, now, &proven);
  ASSERT_EQ(proven.availability, nr::reason_e::joined);
  auto signal = native_video_signal::project(proven, caller, context);
  ASSERT_TRUE(signal.measurement_available); ASSERT_TRUE(native.publish_signal(signal));
  ASSERT_TRUE(signal.setting); EXPECT_EQ(*signal.setting, d);
  ASSERT_TRUE(native.with_current_signal(signal, [&] { EXPECT_FALSE(episode.observe(signal)); }));
  EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::response);
  native.retire();
  EXPECT_FALSE(native.with_current_signal(signal, [] {}, false));
}

TEST(NativeDownwardEpisode, ExpiredReferenceAndDelayedConsumptionCannotExtendEitherWindow) {
  live_tuning_native::episode_t reference;
  EXPECT_TRUE(native_trial_feed(reference, 72, [](int s) { return s < 57 ? 3000 : 30000; }).empty());
  live_tuning_native::episode_t pending;
  ASSERT_EQ(native_trial_feed(pending, 12, [](int s) { return s < 4 ? 3000 : 30000; }),
            std::vector<int> {17000});
  pending.accepted_application(41);
  pending.check_deadline(18'000'000'001);
  EXPECT_EQ(pending.phase(), live_tuning_native::phase_e::unassessed);
  auto old = native_trial_signal(12100, 3500, 3600);
  old.setting->application_sequence = 2; old.setting->origin_revision = 41;
  old.setting->configured_encoder_kbps = 17000; old.setting->receipt_origin = bp::receipt_origin_e::backend_reconfiguration;
  EXPECT_FALSE(pending.observe(old)); EXPECT_EQ(pending.phase(), live_tuning_native::phase_e::unassessed);
}

namespace {
  // A report of the accepted 17000 kbps backend application at revision 41, in the trial fixture's units.
  native_video_signal::signal_t native_d_signal(int ms, int lower_us, int upper_us, std::uint32_t sequence = 0) {
    auto s = native_trial_signal(ms, lower_us, upper_us, sequence);
    s.setting->application_sequence = 2; s.setting->origin_revision = 41;
    s.setting->configured_encoder_kbps = 17000; s.setting->receipt_origin = bp::receipt_origin_e::backend_reconfiguration;
    return s;
  }
  // The existing standing fixture: one request at 12000 ms (trigger lower 30 ms), then backend acceptance at 41.
  void native_d_pending(live_tuning_native::episode_t &episode) {
    ASSERT_EQ(native_trial_feed(episode, 12, [](int s) { return s < 4 ? 3000 : 30000; }), std::vector<int> {17000});
    episode.accepted_application(41);
  }
}

TEST(NativeDownwardEpisode, LateResponseStartDiscardsOnlyItsPartialFirstSecondWithoutCredit) {
  // The retained N87E3 HISTORY-R1 shape: the first D report lands 602 ms into an absolute steady second, so that
  // second can hold only four response reports spanning 300 ms. That is not telemetry loss, and it earns nothing.
  live_tuning_native::episode_t episode;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(episode));
  for (int ms = 12602; ms < 17000; ms += 100) {
    EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(episode.phase(), live_tuning_native::phase_e::response) << ms;
  }
  // Only the four full sub-trigger seconds 13..16 count. A credited partial second would have responded at 16002 ms.
  EXPECT_FALSE(episode.observe(native_d_signal(17002, 3500, 3600)));
  EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::responded);
  EXPECT_EQ(episode.evidence().requested_kbps, 17000);
}

TEST(NativeDownwardEpisode, JitterBoundaryFirstBucketIsDiscardedWhileACoveredFirstBucketStillCounts) {
  // 402 ms into the second, ordinary report jitter leaves six reports spanning 499 ms: one millisecond short.
  live_tuning_native::episode_t jitter;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(jitter));
  for (const int ms : {12402, 12498, 12599, 12699, 12799, 12901, 13001}) {
    EXPECT_FALSE(jitter.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(jitter.phase(), live_tuning_native::phase_e::response) << ms;
  }
  for (int ms = 13101; ms < 17000; ms += 100) {
    EXPECT_FALSE(jitter.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(jitter.phase(), live_tuning_native::phase_e::response) << ms;
  }
  EXPECT_FALSE(jitter.observe(native_d_signal(17001, 3500, 3600)));
  EXPECT_EQ(jitter.phase(), live_tuning_native::phase_e::responded);
  // A first bucket that already holds four reports over 500 ms keeps its original credit: responded one second earlier.
  live_tuning_native::episode_t covered;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(covered));
  for (int ms = 12450; ms < 16000; ms += 100) {
    EXPECT_FALSE(covered.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(covered.phase(), live_tuning_native::phase_e::response) << ms;
  }
  EXPECT_FALSE(covered.observe(native_d_signal(16050, 3500, 3600)));
  EXPECT_EQ(covered.phase(), live_tuning_native::phase_e::responded);
}

TEST(NativeDownwardEpisode, SlowDrainAfterALateStartStillEndsIneffectiveAtTheOriginalSixSeconds) {
  // The retained case's geometry: sub-trigger seconds begin too late for four of them to close inside six seconds.
  live_tuning_native::episode_t episode;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(episode));
  for (int ms = 12602; ms <= 18602; ms += 100) {
    const int lower = ms < 15000 ? 50000 : 3500;
    EXPECT_FALSE(episode.observe(native_d_signal(ms, lower, lower + 100)));
    ASSERT_EQ(episode.phase(), live_tuning_native::phase_e::response) << ms;
  }
  EXPECT_FALSE(episode.observe(native_d_signal(18702, 3500, 3600)));
  EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::ineffective);
  for (int ms = 18802; ms < 40000; ms += 100) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
  EXPECT_EQ(episode.phase(), live_tuning_native::phase_e::ineffective);
  EXPECT_EQ(episode.evidence().requested_kbps, 17000);
}

TEST(NativeDownwardEpisode, TypedFactsShowTheDiscardedPartialAndEveryQualifyingResponseBucket) {
  namespace ltn = live_tuning_native;
  ltn::episode_t episode;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(episode));
  EXPECT_EQ(episode.diagnostic().reason, ltn::transition_reason_e::standing_trigger);
  for (int ms = 12602; ms < 13000; ms += 100) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
  EXPECT_EQ(episode.diagnostic().reason, ltn::transition_reason_e::backend_tuple_confirmed);
  EXPECT_EQ(episode.diagnostic().buckets, 0u);
  EXPECT_FALSE(episode.observe(native_d_signal(13002, 3500, 3600)));
  auto fact = episode.diagnostic().bucket;
  EXPECT_EQ(episode.diagnostic().buckets, 1u);
  EXPECT_EQ(fact.disposition, ltn::bucket_disposition_e::initial_partial_discarded);
  EXPECT_EQ(fact.count, 4u); EXPECT_EQ(fact.span_ns, 300'000'000); EXPECT_EQ(fact.consecutive_below, 0);
  EXPECT_EQ(fact.trigger_lower_ns, 30'000'000);
  for (int ms = 13102; ms <= 14002; ms += 100) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
  fact = episode.diagnostic().bucket;
  EXPECT_EQ(episode.diagnostic().buckets, 2u);
  EXPECT_EQ(fact.disposition, ltn::bucket_disposition_e::below_trigger);
  EXPECT_EQ(fact.count, 10u); EXPECT_EQ(fact.span_ns, 900'000'000); EXPECT_EQ(fact.consecutive_below, 1);
  EXPECT_EQ(fact.median_lower_ns, 3'500'000); EXPECT_EQ(fact.median_upper_ns, 3'600'000);
  for (int ms = 14102; ms <= 17002; ms += 100) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
  EXPECT_EQ(episode.phase(), ltn::phase_e::responded);
  EXPECT_EQ(episode.diagnostic().reason, ltn::transition_reason_e::four_buckets_below_trigger);
  EXPECT_EQ(episode.diagnostic().bucket.consecutive_below, 4);
  EXPECT_EQ(episode.diagnostic().buckets, 5u);
}

TEST(NativeDownwardEpisode, LaterBucketsKeepEveryOriginalRefusalAfterTheDiscardedPartial) {
  namespace ltn = live_tuning_native;
  using reason = ltn::transition_reason_e;
  struct refusal_t { const char *name; ltn::phase_e phase; reason why; };
  const std::vector<refusal_t> refusals {
    {"short later bucket", ltn::phase_e::unassessed, reason::bucket_coverage},
    {"gap", ltn::phase_e::unassessed, reason::gap},
    {"order", ltn::phase_e::unassessed, reason::order},
    {"wrong setting", ltn::phase_e::ineffective, reason::setting_changed},
    {"expired deadline", ltn::phase_e::ineffective, reason::response_deadline},
    {"data unavailable", ltn::phase_e::unassessed, reason::median_unavailable},
    {"bucket overflow", ltn::phase_e::unassessed, reason::bucket_overflow},
  };
  for (std::size_t i = 0; i < refusals.size(); ++i) {
    SCOPED_TRACE(refusals[i].name);
    ltn::episode_t episode;
    ASSERT_NO_FATAL_FAILURE(native_d_pending(episode));
    for (int ms = 12602; ms <= 13002; ms += 100) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(episode.phase(), ltn::phase_e::response);
    ASSERT_EQ(episode.diagnostic().bucket.disposition, ltn::bucket_disposition_e::initial_partial_discarded);
    if (i == 0) {
      for (const int ms : {13402, 13802, 14002}) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
      EXPECT_EQ(episode.diagnostic().bucket.disposition, ltn::bucket_disposition_e::coverage_short);
      EXPECT_EQ(episode.diagnostic().bucket.count, 3u);
    } else if (i == 1) {
      EXPECT_FALSE(episode.observe(native_d_signal(13603, 3500, 3600)));
    } else if (i == 2) {
      EXPECT_FALSE(episode.observe(native_d_signal(13102, 3500, 3600, 13003)));
    } else if (i == 3) {
      auto s = native_d_signal(13102, 3500, 3600); s.setting->origin_revision = 42;
      EXPECT_FALSE(episode.observe(s));
    } else if (i == 4) {
      episode.check_deadline(18'602'000'001);
    } else if (i == 5) {
      auto s = native_d_signal(13102, 3500, 3600); s.samples[0].reason = native_video_signal::reason_e::invalid_time;
      EXPECT_FALSE(episode.observe(s));
    } else {
      for (int ms = 13012; ms < 13400; ms += 10) EXPECT_FALSE(episode.observe(native_d_signal(ms, 3500, 3600)));
      EXPECT_EQ(episode.diagnostic().bucket.disposition, ltn::bucket_disposition_e::overflow);
      EXPECT_EQ(episode.diagnostic().bucket.count, 32u);
    }
    EXPECT_EQ(episode.phase(), refusals[i].phase);
    EXPECT_EQ(episode.diagnostic().reason, refusals[i].why);
    // Terminal outcomes stay put: no response credit, restore or second cut from any later report.
    const auto terminal = episode.diagnostic();
    for (int ms = 20000; ms < 50000; ms += 100) {
      EXPECT_FALSE(episode.observe(native_d_signal(ms, ms < 35000 ? 3500 : 30000, ms < 35000 ? 3600 : 30100)));
      EXPECT_FALSE(episode.observe(native_trial_signal(ms + 50, 30000, 30100)));
    }
    episode.unavailable(reason::permission);
    EXPECT_EQ(episode.phase(), refusals[i].phase);
    EXPECT_EQ(episode.diagnostic().reason, terminal.reason);
    EXPECT_EQ(episode.diagnostic().buckets, terminal.buckets);
    EXPECT_EQ(episode.evidence().requested_kbps, 17000);
  }
}

TEST(NativeDownwardEpisode, PresentlySilentRefusalsCarryTypedReasonsWithoutChangingTheirOutcome) {
  namespace ltn = live_tuning_native;
  using reason = ltn::transition_reason_e;
  ltn::episode_t reference;
  reference.unavailable(reason::permission);
  EXPECT_EQ(reference.phase(), ltn::phase_e::reference);
  EXPECT_EQ(reference.diagnostic().reason, reason::none);
  for (const auto why : {reason::permission, reason::not_consumed_receipt, reason::not_consumed_measurement,
                         reason::not_consumed_setting, reason::not_consumed_current, reason::policy_refused}) {
    ltn::episode_t pending;
    ASSERT_NO_FATAL_FAILURE(native_d_pending(pending));
    pending.unavailable(why);
    EXPECT_EQ(pending.phase(), ltn::phase_e::unassessed);
    EXPECT_EQ(pending.diagnostic().reason, why);
    ltn::episode_t response;
    ASSERT_NO_FATAL_FAILURE(native_d_pending(response));
    EXPECT_FALSE(response.observe(native_d_signal(12602, 3500, 3600)));
    ASSERT_EQ(response.phase(), ltn::phase_e::response);
    response.unavailable(why);
    EXPECT_EQ(response.phase(), ltn::phase_e::unassessed);
    EXPECT_EQ(response.diagnostic().reason, why);
  }
  ltn::episode_t legacy;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(legacy));
  legacy.unavailable();
  EXPECT_EQ(legacy.diagnostic().reason, reason::unavailable);
  ltn::episode_t wrong;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(wrong));
  auto other = native_d_signal(12602, 3500, 3600); other.setting->origin_revision = 8;
  EXPECT_FALSE(wrong.observe(other));
  EXPECT_EQ(wrong.phase(), ltn::phase_e::unassessed);
  EXPECT_EQ(wrong.diagnostic().reason, reason::wrong_application);
  ltn::episode_t late;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(late));
  late.wait_for_application(18'000'000'001);
  EXPECT_EQ(late.phase(), ltn::phase_e::unassessed);
  EXPECT_EQ(late.diagnostic().reason, reason::proof_deadline);
}

TEST(NativeDownwardEpisode, TransitionAndBucketNamesAreStaticDistinctIdentifiers) {
  namespace ltn = live_tuning_native;
  std::set<std::string> names;
  for (int i = 0; i <= static_cast<int>(ltn::transition_reason_e::unavailable); ++i) {
    const std::string n = ltn::name(static_cast<ltn::transition_reason_e>(i));
    EXPECT_TRUE(names.insert(n).second) << n;
    EXPECT_EQ(n.find_first_not_of("abcdefghijklmnopqrstuvwxyz_"), std::string::npos) << n;
  }
  EXPECT_EQ(names.size(), 22u);
  std::set<std::string> dispositions;
  for (int i = 0; i <= static_cast<int>(ltn::bucket_disposition_e::overflow); ++i) {
    const std::string n = ltn::name(static_cast<ltn::bucket_disposition_e>(i));
    EXPECT_TRUE(dispositions.insert(n).second) << n;
    EXPECT_EQ(n.find_first_not_of("abcdefghijklmnopqrstuvwxyz_"), std::string::npos) << n;
  }
  EXPECT_EQ(dispositions.size(), 7u);
}

TEST(NativeDownwardEpisode, ResponseStartingExactlyOnASecondBoundaryKeepsTheFourReportMinimum) {
  // The first D report lands exactly on a steady-second boundary, so the first bucket spans the whole second:
  // three reports 450 ms apart are a genuine undercount, not a leading partial second, and must still refuse.
  namespace ltn = live_tuning_native;
  ltn::episode_t aligned;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(aligned));
  for (const int ms : {13000, 13450, 13900}) {
    EXPECT_FALSE(aligned.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(aligned.phase(), ltn::phase_e::response) << ms;
  }
  EXPECT_FALSE(aligned.observe(native_d_signal(14350, 3500, 3600)));
  EXPECT_EQ(aligned.phase(), ltn::phase_e::unassessed);
  EXPECT_EQ(aligned.diagnostic().reason, ltn::transition_reason_e::bucket_coverage);
  EXPECT_EQ(aligned.diagnostic().bucket.disposition, ltn::bucket_disposition_e::coverage_short);
  EXPECT_EQ(aligned.diagnostic().bucket.count, 3u);
  EXPECT_EQ(aligned.diagnostic().bucket.span_ns, 900'000'000);
  for (int ms = 14450; ms <= 18050; ms += 100) EXPECT_FALSE(aligned.observe(native_d_signal(ms, 3500, 3600)));
  EXPECT_EQ(aligned.phase(), ltn::phase_e::unassessed);
  // One millisecond into the second the same reports cover only the rest of it: that entry keeps the no-credit
  // discard, and only four later full sub-trigger buckets can respond.
  ltn::episode_t late;
  ASSERT_NO_FATAL_FAILURE(native_d_pending(late));
  for (const int ms : {13001, 13451, 13901, 14351}) {
    EXPECT_FALSE(late.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(late.phase(), ltn::phase_e::response) << ms;
  }
  EXPECT_EQ(late.diagnostic().bucket.disposition, ltn::bucket_disposition_e::initial_partial_discarded);
  for (int ms = 14451; ms < 18000; ms += 100) {
    EXPECT_FALSE(late.observe(native_d_signal(ms, 3500, 3600)));
    ASSERT_EQ(late.phase(), ltn::phase_e::response) << ms;
  }
  EXPECT_FALSE(late.observe(native_d_signal(18051, 3500, 3600)));
  EXPECT_EQ(late.phase(), ltn::phase_e::responded);
}

namespace {
  // The standing fixture (3 ms for seconds 0-3, then 30 ms) fed to end_ms, with mutate() applied to every report
  // from 10000 ms and the reports in (skip_from, skip_to] left out. Returns every request the episode made.
  template<class Mutate>
  std::vector<int> native_interrupted_feed(live_tuning_native::episode_t &episode, int end_ms, Mutate mutate,
                                           int skip_from = -1, int skip_to = -1) {
    std::vector<int> requests;
    for (int ms = 0; ms < end_ms; ms += 100) {
      if (ms > skip_from && ms <= skip_to) continue;
      auto s = native_trial_signal(ms, ms < 4000 ? 3000 : 30000, ms < 4000 ? 3100 : 30100);
      if (ms >= 10000) mutate(ms, s);
      if (const auto next = episode.observe(s)) requests.push_back(*next);
    }
    return requests;
  }
}

TEST(NativeDownwardEpisode, SilentReferenceClearsAreNamedWithWhatTheyDiscardedAndDecideNothing) {
  namespace ltn = live_tuning_native;
  using clear = ltn::reference_clear_e;
  struct case_t { clear why; int closed, standing; };
  // Every clear is decided by the 10000 ms report before it closes second 9: buckets 0-8 closed, 4-8 standing.
  const auto run = [](int variant, ltn::episode_t &episode) {
    return native_interrupted_feed(episode, 12000, [variant](int ms, native_video_signal::signal_t &s) {
      if (variant == 0) ++s.setting->application_sequence;
      if (variant == 1) ++s.context.action_revision;
      if (variant == 2) ++s.context.floor_kbps;
      if (variant == 3 && ms == 10000) s.samples[0].parity = true;
      if (variant == 4 && ms == 10000) s.samples[0].elapsed_lower_ns = -1;
    }, variant == 5 ? 9400 : -1, variant == 5 ? 9900 : -1);
  };
  const std::array<case_t, 6> cases {{{clear::source_changed, 9, 5}, {clear::action_revision_changed, 9, 5},
    {clear::floor_changed, 9, 5}, {clear::median_unavailable, 9, 5}, {clear::median_unavailable, 9, 5},
    {clear::gap, 9, 5}}};
  for (int variant = 0; variant < static_cast<int>(cases.size()); ++variant) {
    SCOPED_TRACE(variant);
    ltn::episode_t episode;
    EXPECT_TRUE(run(variant, episode).empty()); // The decisions these interruptions already caused: no request.
    EXPECT_EQ(episode.phase(), ltn::phase_e::reference);
    EXPECT_EQ(episode.diagnostic().reason, ltn::transition_reason_e::none); // Still no phase transition.
    const auto &r = episode.reference_interruptions();
    EXPECT_EQ(r.total, 1u);
    EXPECT_EQ(r.first, cases[variant].why); EXPECT_EQ(r.last, cases[variant].why);
    EXPECT_EQ(r.clears[static_cast<std::size_t>(cases[variant].why)], 1u);
    EXPECT_EQ(r.last_lost_closed, cases[variant].closed);
    EXPECT_EQ(r.last_lost_reference, 4);
    EXPECT_EQ(r.last_lost_standing, cases[variant].standing);
    EXPECT_EQ(r.max_lost_standing, cases[variant].standing);
    EXPECT_TRUE(episode.reference_window()); // The next report started a fresh window.
  }
  // A gap ending inside a second restarts the window there, so its short first bucket is honestly a second clear.
  ltn::episode_t midsecond;
  EXPECT_TRUE(native_interrupted_feed(midsecond, 12000, [](int, native_video_signal::signal_t &) {}, 10000, 10500)
                .empty());
  const auto &m = midsecond.reference_interruptions();
  EXPECT_EQ(m.total, 2u); EXPECT_EQ(m.first, clear::gap); EXPECT_EQ(m.last, clear::bucket_coverage);
  EXPECT_EQ(m.clears[static_cast<std::size_t>(clear::gap)], 1u);
  EXPECT_EQ(m.clears[static_cast<std::size_t>(clear::bucket_coverage)], 1u);
  EXPECT_EQ(m.last_lost_closed, 0); EXPECT_EQ(m.last_lost_standing, 0); EXPECT_EQ(m.max_lost_standing, 6);
  // A caller's refusal while standing is named with the caller's reason.
  ltn::episode_t refused;
  EXPECT_TRUE(native_interrupted_feed(refused, 10100, [](int, native_video_signal::signal_t &) {}).empty());
  refused.unavailable(ltn::transition_reason_e::not_consumed_current);
  EXPECT_EQ(refused.reference_interruptions().last, clear::not_consumed_current);
  EXPECT_EQ(refused.reference_interruptions().last_lost_standing, 6);
  EXPECT_FALSE(refused.reference_window());
  // An underpopulated second: three reports 450 ms apart, then the next second closes it short.
  ltn::episode_t sparse;
  for (const int ms : {0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1500, 1600,
                       1700, 1800, 1900, 2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700, 2800, 2900, 3000, 3100,
                       3200, 3300, 3400, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200, 4300, 4400, 4500, 4600,
                       4700, 4800, 4900, 5000, 5450, 5900, 6000}) {
    EXPECT_FALSE(sparse.observe(native_trial_signal(ms, ms < 4000 ? 3000 : 30000, ms < 4000 ? 3100 : 30100)));
  }
  EXPECT_EQ(sparse.reference_interruptions().last, clear::bucket_coverage);
  EXPECT_EQ(sparse.reference_interruptions().last_lost_closed, 5);
  EXPECT_EQ(sparse.reference_interruptions().last_lost_standing, 1);
  // The 60 s reference age is a routine restart, named separately.
  ltn::episode_t aged;
  EXPECT_TRUE(native_trial_feed(aged, 61, [](int) { return 3000; }).empty());
  EXPECT_EQ(aged.reference_interruptions().total, 1u);
  EXPECT_EQ(aged.reference_interruptions().last, clear::reference_expired);
  EXPECT_EQ(aged.reference_interruptions().last_lost_closed, 60);
  EXPECT_EQ(aged.reference_interruptions().last_lost_standing, 0);
}

TEST(NativeDownwardEpisode, UninterruptedTrialAndPostCutRefusalsCountNoReferenceClear) {
  namespace ltn = live_tuning_native;
  ltn::episode_t empty;
  empty.unavailable(ltn::transition_reason_e::permission); // No window yet: nothing was discarded.
  EXPECT_EQ(empty.reference_interruptions().total, 0u);
  EXPECT_FALSE(empty.reference_window());
  ltn::episode_t clean;
  EXPECT_EQ(native_trial_feed(clean, 12, [](int s) { return s < 4 ? 3000 : 30000; }), std::vector<int> {17000});
  EXPECT_EQ(clean.reference_interruptions().total, 0u);
  EXPECT_FALSE(clean.reference_window()); // Spent: the reference window is over.
  clean.unavailable(ltn::transition_reason_e::permission); // A post-cut refusal is a transition, not a clear.
  EXPECT_EQ(clean.phase(), ltn::phase_e::unassessed);
  EXPECT_EQ(clean.reference_interruptions().total, 0u);
  clean.note_writer_or_scope_clear(); // A writer replacing a spent episode discards no reference window.
  EXPECT_EQ(clean.reference_interruptions().total, 0u);
}

TEST(NativeDownwardEpisode, WriterOrScopeReplacementNamesTheWindowItDiscardsAndChangesNoDecisionField) {
  namespace ltn = live_tuning_native;
  ltn::episode_t standing, control;
  const auto fixture = [](int, native_video_signal::signal_t &) {};
  EXPECT_TRUE(native_interrupted_feed(standing, 10100, fixture).empty());
  EXPECT_TRUE(native_interrupted_feed(control, 10100, fixture).empty());
  standing.note_writer_or_scope_clear(); // The owner replaces the episode right after this.
  const auto &r = standing.reference_interruptions();
  EXPECT_EQ(r.total, 1u); EXPECT_EQ(r.last, ltn::reference_clear_e::writer_or_scope);
  EXPECT_EQ(r.last_lost_closed, 10); EXPECT_EQ(r.last_lost_reference, 4); EXPECT_EQ(r.last_lost_standing, 6);
  // Diagnostic only: the noted episode still decides exactly as an unnoted one would.
  EXPECT_TRUE(standing.reference_window());
  for (int ms = 10100; ms <= 12000; ms += 100) {
    const auto s = native_trial_signal(ms, 30000, 30100);
    EXPECT_EQ(standing.observe(s), control.observe(s)) << ms;
  }
  EXPECT_EQ(standing.phase(), control.phase()); EXPECT_EQ(standing.phase(), ltn::phase_e::awaiting_application);
  EXPECT_EQ(standing.evidence().requested_kbps, 17000);
  EXPECT_EQ(standing.evidence().trigger_lower_ns, control.evidence().trigger_lower_ns);
}

TEST(NativeDownwardEpisode, ReferenceClearNamesAreStaticAndCountsSaturate) {
  namespace ltn = live_tuning_native;
  std::set<std::string> names;
  for (std::size_t i = 0; i < ltn::reference_clear_count; ++i) {
    const std::string n = ltn::name(static_cast<ltn::reference_clear_e>(i));
    EXPECT_TRUE(names.insert(n).second) << n;
    EXPECT_EQ(n.find_first_not_of("abcdefghijklmnopqrstuvwxyz_"), std::string::npos) << n;
  }
  EXPECT_EQ(names.size(), 19u);
  std::uint32_t count = UINT32_MAX - 1;
  ltn::saturating_increment(count); EXPECT_EQ(count, UINT32_MAX);
  ltn::saturating_increment(count); EXPECT_EQ(count, UINT32_MAX);
}

TEST_F(NativeReception, RecentDifferentFramesKeepBothActualReport35CollisionKeys) {
  // These genuine sender keys collided before the actual trace had sent 6333 datagrams.
  frame(297);frame(298);frame(299);
  packet(297,6134);packet(298,6150);packet(299,6164);
  auto report=reading(2);report.first_frame=297;report.last_frame=299;
  report.data_packets=3;report.data_bytes=3000;
  report.samples[0]={297,6134,50000};report.samples[1]={299,6164,40000};
  auto current=accept(report);ASSERT_TRUE(current.available);ASSERT_TRUE(current.setting);
  EXPECT_EQ(*current.setting,setting);
  EXPECT_TRUE(current.samples[0].matched);EXPECT_TRUE(current.samples[1].matched);
  EXPECT_EQ(current.samples[0].udp_bytes,1000u);EXPECT_EQ(current.samples[1].udp_bytes,1000u);
}

TEST_F(NativeReception, RecentReport35RangeRetainsUnsampledDataAndAuthenticClassBounds) {
  // Retained sender ranges: frames297-306, 191 DATA/20 parity, full sequences6122-6332.
  // The client claimed191 DATA/10 parity; the two selected keys here were retained by both indices.
  std::uint16_t sequence=6122;
  for(unsigned f=297;f<=306;++f) {
    frame(f);const auto data=f==299 ? 20u : 19u;
    for(unsigned n=0;n<data+2;++n) {
      observer->record(f,sequence++,1408,n>=data,now-100ms,now-99ms,true);
    }
  }
  ASSERT_EQ(sequence,6333u);
  auto report=reading(2);report.first_frame=297;report.last_frame=306;
  report.data_packets=191;report.data_bytes=268928;report.parity_packets=10;report.parity_bytes=14080;
  report.samples[0]={297,6122,50000};report.samples[1]={306,6312,40000};
  auto current=accept(report);ASSERT_TRUE(current.available);ASSERT_TRUE(current.setting);
  EXPECT_EQ(*current.setting,setting);
  report.sequence=2;report.end_us+=100000;report.data_packets=192;report.data_bytes=192*1408;
  EXPECT_EQ(observer->accept(wire(report),true,now),nr::reason_e::sample_counts);
  current=*observer->readback(now);EXPECT_EQ(current.report.sequence,1u);
}

TEST_F(NativeReception, ConsecutiveDataAndParityAcrossFramesKeepRealSixteenBitRtpWrap) {
  frame(297);frame(298);
  packet(297,65534);packet(297,65535);packet(298,0,true);packet(298,1,true);
  auto report=reading(4);report.first_frame=297;report.last_frame=298;
  report.data_packets=report.parity_packets=2;report.data_bytes=report.parity_bytes=2000;
  report.samples[0]={297,65534,50000};report.samples[1]={297,65535,40000};
  report.samples[2]={298,0,30000};report.samples[3]={298,1,20000};
  auto current=accept(report);ASSERT_TRUE(current.available);
  EXPECT_FALSE(current.samples[0].parity);EXPECT_FALSE(current.samples[1].parity);
  EXPECT_TRUE(current.samples[2].parity);EXPECT_TRUE(current.samples[3].parity);
  for(unsigned i=0;i<4;++i) EXPECT_TRUE(current.samples[i].matched);
}

TEST_F(NativeReception, CapacityAliasAndReusedRangeCannotBorrowAnotherFramesKeyOrHideDuplicate) {
  // Cursor reuse after cancellation/exception is not assumed to be monotonic.
  for(auto replacement:{std::uint16_t(6134+nr::ledger_capacity),std::uint16_t(6134)}) {
    frames=std::make_shared<ff::sender_ledger_t>(615);observer=std::make_unique<nr::scope_t>(615,frames,true);
    frame(297);frame(298);frame(299);packet(297,6134);packet(299,replacement);
    auto old=reading();old.first_frame=old.last_frame=297;old.samples[0]={297,6134,50000};
    EXPECT_EQ(accept(old).reason,nr::reason_e::packet_missing);
    auto fresh=reading();fresh.sequence=2;fresh.end_us=200000;
    fresh.first_frame=fresh.last_frame=299;fresh.samples[0]={299,replacement,50000};
    ASSERT_TRUE(accept(fresh).available);
    packet(299,replacement);fresh.sequence=3;fresh.end_us=300000;
    EXPECT_EQ(accept(fresh).reason,nr::reason_e::packet_ambiguous);
  }
}

TEST_F(NativeReception, SameWireKeyInNewGenerationCannotInheritRetiredAmbiguity) {
  frame(297);packet(297,6134);packet(297,6134);
  auto report=reading();report.first_frame=report.last_frame=297;report.samples[0]={297,6134,50000};
  EXPECT_EQ(accept(report).reason,nr::reason_e::packet_ambiguous);
  observer->retire();EXPECT_FALSE(observer->readback(now));
  frames=std::make_shared<ff::sender_ledger_t>(616);observer=std::make_unique<nr::scope_t>(616,frames,true);
  setting=actual_setting(616);frame(297);packet(297,6134);
  auto fresh=accept(report);ASSERT_TRUE(fresh.available);ASSERT_TRUE(fresh.setting);
  EXPECT_EQ(fresh.setting->stream_generation,616u);EXPECT_EQ(*fresh.setting,setting);
}
