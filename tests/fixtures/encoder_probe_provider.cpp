// A separately loaded ELF object exercises real loader ownership and replacement.
extern "C" int polaris_probe_provider_fixture() {
  return 42;
}
