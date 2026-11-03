/** @file Real native reception parser/correlation tests; no network, tracing, probe or controller. */
#include "src/stream_native_reception.h"
#include "src/stream_native_reception_json.h"
#include <gtest/gtest.h>
#include <chrono>
#include <atomic>
#include <thread>
#include <memory>
#include <string>
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
