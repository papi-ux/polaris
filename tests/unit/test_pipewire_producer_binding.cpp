#include "../tests_common.h"

#include "src/pipewire_producer_binding.h"

#include <map>
#include <string>

namespace {
  namespace pb = pipewire_producer_binding;
  using props_t = std::map<std::string, std::string, std::less<>>;

  constexpr std::int64_t self_pid = 4242;
  constexpr std::uint64_t self_uid = 1000;
  constexpr std::uint32_t self_client = 77;
  constexpr std::uint32_t capture_node = 120;
  constexpr std::uint32_t source_node = 101;
  constexpr std::uint32_t producer_client = 99;

  // A graph like the Deck snapshot: gamescope's Video/Source 101 owned by client 99, our client 77
  // owning capture node 120, and one link 130 from 101 into 120.
  struct fixture_t {
    pb::graph_t graph;

    fixture_t() {
      graph.global(self_client, "PipeWire:Interface:Client", client_props(self_pid, self_uid, 300));
      graph.global(producer_client, "PipeWire:Interface:Client", client_props(75640, self_uid, 267));
      graph.global(source_node, "PipeWire:Interface:Node", node_props(producer_client, 268));
      graph.global(capture_node, "PipeWire:Interface:Node", node_props(self_client, 301));
      graph.global(130, "PipeWire:Interface:Link", link_props(source_node, capture_node, 302));
    }

    static props_t client_props(std::int64_t pid, std::uint64_t uid, std::uint64_t serial) {
      return {
        {"object.serial", std::to_string(serial)},
        {"pipewire.sec.pid", std::to_string(pid)},
        {"pipewire.sec.uid", std::to_string(uid)},
        {"pipewire.sec.gid", std::to_string(uid)},
      };
    }

    static props_t node_props(std::uint32_t owner, std::uint64_t serial) {
      return {{"object.serial", std::to_string(serial)}, {"client.id", std::to_string(owner)}};
    }

    static props_t link_props(std::uint32_t output, std::uint32_t input, std::uint64_t serial) {
      return {
        {"object.serial", std::to_string(serial)},
        {"link.output.node", std::to_string(output)},
        {"link.input.node", std::to_string(input)},
      };
    }

    pb::snapshot_t snapshot() const {
      pb::snapshot_t in;
      in.tracking = true;
      in.running = true;
      in.synced = true;
      in.server_version = "1.6.8";
      in.self_client_id = self_client;
      in.self_pid = self_pid;
      in.self_uid = self_uid;
      in.capture_node_id = capture_node;
      in.graph = &graph;
      return in;
    }

    void set(std::uint32_t id, std::string_view key, std::string value) {
      auto global = graph.globals().at(id);
      global.props[std::string {key}] = std::move(value);
      graph.global(id, type_of(global.kind), global.props);
    }

    void erase(std::uint32_t id, std::string_view key) {
      auto global = graph.globals().at(id);
      global.props.erase(std::string {key});
      graph.global(id, type_of(global.kind), global.props);
    }

    static std::string_view type_of(pb::kind_e kind) {
      switch (kind) {
        case pb::kind_e::client:
          return "PipeWire:Interface:Client";
        case pb::kind_e::node:
          return "PipeWire:Interface:Node";
        case pb::kind_e::link:
          return "PipeWire:Interface:Link";
      }
      return "";
    }
  };
}  // namespace

TEST(PipeWireProducerBindingTests, StrictDecimalRefusesEveryNonCanonicalForm) {
  for (const auto *text : {"", "+1", "-1", " 1", "1 ", "0x10", "01", "1.0", "1e3", "١٢"}) {
    EXPECT_FALSE(pb::parse_decimal(text, 1'000'000)) << text;
  }
  EXPECT_EQ(pb::parse_decimal("0", 10), 0u);
  EXPECT_FALSE(pb::parse_decimal("9", 5));
  EXPECT_FALSE(pb::parse_decimal("1", 0));
  EXPECT_EQ(pb::parse_decimal("18446744073709551615", UINT64_MAX), UINT64_MAX);
  EXPECT_FALSE(pb::parse_decimal("18446744073709551616", UINT64_MAX));
  EXPECT_FALSE(pb::parse_id("4294967295")) << "SPA_ID_INVALID is never a global";
  EXPECT_EQ(pb::parse_id("4294967294"), 4294967294u);
  EXPECT_FALSE(pb::parse_pid("1")) << "init is never a peer here";
  EXPECT_FALSE(pb::parse_pid("0"));
  EXPECT_FALSE(pb::parse_pid("2147483648"));
  EXPECT_EQ(pb::parse_pid("2147483647"), 2147483647);
  EXPECT_FALSE(pb::parse_uid("-1")) << "a large uid printed by %d is refused, not wrapped";
  EXPECT_EQ(pb::parse_uid("0"), 0u);
}

TEST(PipeWireProducerBindingTests, ServerVersionMustParseAndBeAudited) {
  EXPECT_FALSE(pb::server_version_supported("1.0.0"));
  EXPECT_FALSE(pb::server_version_supported("1.2.0"));
  EXPECT_FALSE(pb::server_version_supported("2.0.0"));
  EXPECT_TRUE(pb::server_version_supported("1.0.5"));
  EXPECT_TRUE(pb::server_version_supported("1.6.8"));
  EXPECT_TRUE(pb::server_version_supported("1.6.9"));
  for (const auto *version : {"0.3.81", "", "1.6", "1.6.9.1", "1.6.x", "v1.6.9", "1.06.9"}) {
    EXPECT_FALSE(pb::server_version_supported(version)) << version;
  }
}

TEST(PipeWireProducerBindingTests, BindsTheOneActualSourceToItsServerCredentials) {
  const fixture_t fixture;
  const auto binding = pb::resolve(fixture.snapshot());
  EXPECT_TRUE(binding.verified);
  EXPECT_EQ(binding.reason, "verified");
  EXPECT_EQ(binding.server_version, "1.6.8");
  EXPECT_EQ(binding.capture_client, (capture_producer::object_t {self_client, 300}));
  EXPECT_EQ(binding.capture_node, (capture_producer::object_t {capture_node, 301}));
  EXPECT_EQ(binding.source_node, (capture_producer::object_t {source_node, 268}));
  EXPECT_EQ(binding.producer_client, (capture_producer::object_t {producer_client, 267}));
  ASSERT_EQ(binding.links.size(), 1u);
  EXPECT_EQ(binding.links[0], (capture_producer::link_t {{130, 302}, source_node, capture_node}));
  EXPECT_EQ(binding.producer_pid, 75640);
  EXPECT_EQ(binding.producer_uid, self_uid);
}

TEST(PipeWireProducerBindingTests, NamesRequestedTargetsAndApplicationPidsAreNeverProof) {
  fixture_t fixture;
  // Client-supplied facts that disagree with the server must change nothing.
  fixture.set(source_node, "node.name", "gamescope");
  fixture.set(source_node, "media.class", "Video/Source");
  fixture.set(source_node, "target.object", "999");
  fixture.set(producer_client, "application.process.id", "1");
  fixture.set(producer_client, "application.name", "Polaris");
  // A decoy node named like the producer, with credentials of its own, that is not linked.
  fixture.graph.global(140, "PipeWire:Interface:Node", {{"object.serial", "400"}, {"client.id", "141"}, {"node.name", "gamescope"}});
  fixture.graph.global(141, "PipeWire:Interface:Client", fixture_t::client_props(5555, self_uid, 401));
  const auto binding = pb::resolve(fixture.snapshot());
  EXPECT_TRUE(binding.verified);
  EXPECT_EQ(binding.producer_pid, 75640);
  EXPECT_EQ(binding.source_node.id, source_node);

  // Without the server credential, a client-supplied process id is never used instead.
  fixture.erase(producer_client, "pipewire.sec.pid");
  fixture.set(producer_client, "application.process.id", "75640");
  const auto refused = pb::resolve(fixture.snapshot());
  EXPECT_FALSE(refused.verified);
  EXPECT_EQ(refused.reason, "producer_credentials_absent");
  EXPECT_EQ(refused.producer_pid, 0);
}

TEST(PipeWireProducerBindingTests, TheReadMustHaveCompletedOnTheSameCore) {
  const fixture_t fixture;
  const std::pair<void (*)(pb::snapshot_t &), std::string_view> cases[] = {
    {[](pb::snapshot_t &in) { in.tracking = false; }, "producer_tracking_disabled"},
    {[](pb::snapshot_t &in) { in.cancelled = true; }, "read_cancelled"},
    {[](pb::snapshot_t &in) { in.running = false; }, "capture_not_running"},
    {[](pb::snapshot_t &in) { in.core_error = true; }, "pipewire_core_error"},
    {[](pb::snapshot_t &in) { in.timed_out = true; }, "read_timeout"},
    {[](pb::snapshot_t &in) { in.synced = false; }, "read_timeout"},
    {[](pb::snapshot_t &in) { in.graph = nullptr; }, "read_timeout"},
    {[](pb::snapshot_t &in) { in.generation_changed = true; }, "pipewire_graph_changed"},
    {[](pb::snapshot_t &in) { in.server_version = "0.3.81"; }, "server_version_unsupported"},
    {[](pb::snapshot_t &in) { in.server_version.clear(); }, "server_version_unsupported"},
  };
  for (const auto &[mutate, expected] : cases) {
    auto in = fixture.snapshot();
    mutate(in);
    const auto binding = pb::resolve(in);
    EXPECT_FALSE(binding.verified) << expected;
    EXPECT_EQ(binding.reason, expected);
    EXPECT_EQ(binding.producer_pid, 0);
  }
}

TEST(PipeWireProducerBindingTests, OwnCredentialsMustBeThisProcessInThisNamespace) {
  {
    fixture_t fixture;
    auto in = fixture.snapshot();
    in.self_client_id = pb::invalid_id;
    EXPECT_EQ(pb::resolve(in).reason, "capture_client_unknown") << "proxy not bound yet";
    in.self_client_id = 555;
    EXPECT_EQ(pb::resolve(in).reason, "capture_client_unknown") << "own global not visible";
  }
  {
    fixture_t fixture;
    fixture.erase(self_client, "pipewire.sec.uid");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "capture_credentials_absent");
  }
  {
    // A server in another PID namespace, or a portal-made fd, reports some other pid for us.
    fixture_t fixture;
    fixture.set(self_client, "pipewire.sec.pid", "1635");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "pid_namespace_mismatch");
  }
  {
    fixture_t fixture;
    fixture.set(self_client, "pipewire.sec.uid", "0");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "pid_namespace_mismatch");
  }
}

TEST(PipeWireProducerBindingTests, TheCaptureNodeMustBeOurs) {
  {
    fixture_t fixture;
    auto in = fixture.snapshot();
    in.capture_node_id = pb::invalid_id;
    EXPECT_EQ(pb::resolve(in).reason, "capture_node_unknown");
    in.capture_node_id = 999;
    EXPECT_EQ(pb::resolve(in).reason, "capture_node_unknown");
  }
  {
    fixture_t fixture;
    fixture.set(capture_node, "client.id", std::to_string(producer_client));
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "capture_node_not_owned");
  }
  {
    fixture_t fixture;
    fixture.erase(capture_node, "client.id");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "capture_node_not_owned");
  }
}

TEST(PipeWireProducerBindingTests, LinksMustNameExactlyOneSource) {
  {
    fixture_t fixture;
    fixture.graph.global_remove(130);
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "source_not_linked");
  }
  {
    // A link into some other sink is not ours.
    fixture_t fixture;
    fixture.graph.global_remove(130);
    fixture.graph.global(131, "PipeWire:Interface:Link", fixture_t::link_props(source_node, 555, 303));
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "source_not_linked");
  }
  {
    // Two ports of the same source (for example per-plane) still name one source.
    fixture_t fixture;
    fixture.graph.global(131, "PipeWire:Interface:Link", fixture_t::link_props(source_node, capture_node, 303));
    const auto binding = pb::resolve(fixture.snapshot());
    EXPECT_TRUE(binding.verified);
    EXPECT_EQ(binding.links.size(), 2u);
  }
  {
    fixture_t fixture;
    fixture.graph.global(150, "PipeWire:Interface:Node", fixture_t::node_props(producer_client, 500));
    fixture.graph.global(131, "PipeWire:Interface:Link", fixture_t::link_props(150, capture_node, 303));
    const auto binding = pb::resolve(fixture.snapshot());
    EXPECT_FALSE(binding.verified);
    EXPECT_EQ(binding.reason, "source_ambiguous");
  }
  {
    // An unreadable sink might be ours: the graph is not known well enough to bind.
    fixture_t fixture;
    fixture.graph.global(131, "PipeWire:Interface:Link", {{"object.serial", "303"}, {"link.output.node", "150"}, {"link.input.node", "12x"}});
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "graph_identity_invalid");
  }
}

TEST(PipeWireProducerBindingTests, TheSourceMustBeAttributedToAVisibleCredentialedClient) {
  {
    fixture_t fixture;
    fixture.graph.global_remove(source_node);
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "source_unattributed");
  }
  {
    // A lingering node made by a factory carries no client.id.
    fixture_t fixture;
    fixture.erase(source_node, "client.id");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "source_unattributed");
  }
  {
    fixture_t fixture;
    fixture.set(source_node, "client.id", "99 ");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "source_unattributed");
  }
  {
    fixture_t fixture;
    fixture.graph.global_remove(producer_client);
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "producer_client_not_visible");
  }
  {
    fixture_t fixture;
    fixture.set(source_node, "client.id", std::to_string(capture_node));  // a node, not a client
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "producer_client_not_visible");
  }
  for (const auto *pid : {"", "1", "0", "+75640", "75640 ", "0x1278"}) {
    fixture_t fixture;
    fixture.set(producer_client, "pipewire.sec.pid", pid);
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "producer_credentials_absent") << pid;
  }
  {
    fixture_t fixture;
    fixture.set(producer_client, "pipewire.sec.uid", "1001");
    const auto binding = pb::resolve(fixture.snapshot());
    EXPECT_EQ(binding.reason, "producer_uid_mismatch");
    EXPECT_EQ(binding.producer_pid, 0);
  }
}

TEST(PipeWireProducerBindingTests, EveryBoundObjectNeedsANonzeroSerial) {
  for (const auto id : {self_client, capture_node, source_node, producer_client, 130u}) {
    for (const auto *serial : {"", "0", "-3", "3x"}) {
      fixture_t fixture;
      fixture.set(id, "object.serial", serial);
      EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "graph_identity_invalid") << id << ' ' << serial;
    }
    fixture_t fixture;
    fixture.erase(id, "object.serial");
    EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "graph_identity_invalid") << id;
  }
}

TEST(PipeWireProducerBindingTests, RemovalAndReplacementChangeTheSnapshotAndTheRevision) {
  fixture_t fixture;
  const auto before = pb::resolve(fixture.snapshot());
  ASSERT_TRUE(before.verified);
  const auto revision = fixture.graph.revision();

  // The producer restarts: its client and node go away and come back under reused ids with new
  // serials. The binding resolves again, but it is not the same binding.
  fixture.graph.global_remove(130);
  fixture.graph.global_remove(source_node);
  fixture.graph.global_remove(producer_client);
  EXPECT_EQ(pb::resolve(fixture.snapshot()).reason, "source_not_linked");
  fixture.graph.global(producer_client, "PipeWire:Interface:Client", fixture_t::client_props(80000, self_uid, 900));
  fixture.graph.global(source_node, "PipeWire:Interface:Node", fixture_t::node_props(producer_client, 901));
  fixture.graph.global(130, "PipeWire:Interface:Link", fixture_t::link_props(source_node, capture_node, 902));
  const auto after = pb::resolve(fixture.snapshot());
  ASSERT_TRUE(after.verified);
  EXPECT_NE(after, before);
  EXPECT_NE(after.source_node.serial, before.source_node.serial);
  EXPECT_EQ(after.producer_pid, 80000);
  EXPECT_GT(fixture.graph.revision(), revision);

  // Unrelated object types and unknown removals change nothing.
  const auto settled = fixture.graph.revision();
  fixture.graph.global(7, "PipeWire:Interface:Port", {{"object.serial", "8"}});
  fixture.graph.global_remove(4000);
  fixture.graph.global(pb::invalid_id, "PipeWire:Interface:Node", {});
  EXPECT_EQ(fixture.graph.revision(), settled);
  EXPECT_EQ(pb::resolve(fixture.snapshot()), after);
}
