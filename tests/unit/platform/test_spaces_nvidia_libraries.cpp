/**
 * @file tests/unit/platform/test_spaces_nvidia_libraries.cpp
 * @brief The NVIDIA userspace a host-driver Space borrows from the machine.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include "src/platform/linux/spaces_nvidia_libraries.h"
  #include <cstring>
  #include <functional>
  #include <filesystem>
  #include <fstream>
  #include <map>
  #include <nlohmann/json.hpp>
  #include <set>
  #include <iostream>
  #include "src/platform/linux/multiseat_container_host.h"

using namespace multiseat;
using json = nlohmann::json;

namespace {
  void put(std::string &bytes, std::size_t at, std::uint64_t value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index)
      bytes.at(at + index) = static_cast<char>((value >> (8 * index)) & 0xff);
  }

  /** A shared object with just enough of an ELF in it to be identified. */
  std::string shared_object(std::string_view soname, bool thirty_two_bit) {
    const std::size_t header = thirty_two_bit ? 52 : 64;
    const std::size_t entry = thirty_two_bit ? 32 : 56;
    const std::size_t pair = thirty_two_bit ? 8 : 16;
    const std::size_t programs = header;
    const std::size_t dynamic = programs + 2 * entry;
    const std::size_t dynamic_size = 3 * pair;
    const std::size_t strings = dynamic + dynamic_size;
    std::string bytes(strings + soname.size() + 2, '\0');

    std::memcpy(bytes.data(), "\x7f" "ELF", 4);
    bytes.at(4) = thirty_two_bit ? 1 : 2;
    bytes.at(5) = 1;
    bytes.at(6) = 1;
    put(bytes, 16, 3, 2);  // ET_DYN
    put(bytes, 18, thirty_two_bit ? 3 : 62, 2);  // EM_386 or EM_X86_64
    if (thirty_two_bit) {
      put(bytes, 28, programs, 4);
      put(bytes, 42, entry, 2);
      put(bytes, 44, 2, 2);
    } else {
      put(bytes, 32, programs, 8);
      put(bytes, 54, entry, 2);
      put(bytes, 56, 2, 2);
    }

    // One PT_LOAD mapping the whole file at address zero, so an address is an
    // offset, then PT_DYNAMIC.
    if (thirty_two_bit) {
      put(bytes, programs, 1, 4);
      put(bytes, programs + 4, 0, 4);
      put(bytes, programs + 8, 0, 4);
      put(bytes, programs + 16, bytes.size(), 4);
      put(bytes, programs + entry, 2, 4);
      put(bytes, programs + entry + 4, dynamic, 4);
      put(bytes, programs + entry + 8, dynamic, 4);
      put(bytes, programs + entry + 16, dynamic_size, 4);
    } else {
      put(bytes, programs, 1, 4);
      put(bytes, programs + 8, 0, 8);
      put(bytes, programs + 16, 0, 8);
      put(bytes, programs + 32, bytes.size(), 8);
      put(bytes, programs + entry, 2, 4);
      put(bytes, programs + entry + 8, dynamic, 8);
      put(bytes, programs + entry + 16, dynamic, 8);
      put(bytes, programs + entry + 32, dynamic_size, 8);
    }

    const std::size_t width = thirty_two_bit ? 4 : 8;
    put(bytes, dynamic, 5, width);  // DT_STRTAB
    put(bytes, dynamic + width, strings, width);
    put(bytes, dynamic + pair, 14, width);  // DT_SONAME
    put(bytes, dynamic + pair + width, 1, width);
    put(bytes, dynamic + 2 * pair, 0, width);  // DT_NULL
    std::memcpy(bytes.data() + strings + 1, soname.data(), soname.size());
    return bytes;
  }

  class fake_host_t : public container::host_t {
  public:
    std::set<std::string> untrusted;
    std::map<std::string, std::string> files;
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return true; }
    bool trusted_system_file(const std::filesystem::path &path) const override {
      return !untrusted.contains(path.string());
    }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return {}; }
    bool readable_directory(const std::filesystem::path &) const override { return true; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return true; }
    bool private_readable_file(const std::filesystem::path &) const override { return true; }
    std::optional<container::character_device_identity_t> read_write_character_device(
      const std::filesystem::path &) const override {
      ADD_FAILURE() << "Resolving driver files cannot touch devices";
      return {};
    }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override {
      ADD_FAILURE() << "Driver files belong to root, not to Polaris";
      return {};
    }
    std::optional<std::string> read_trusted_system_file(const std::filesystem::path &path, std::size_t) const override {
      const auto found = files.find(path.string());
      if (found == files.end() || untrusted.contains(path.string())) return {};
      return found->second;
    }
    container::command_result_t run(const std::vector<std::string> &, std::chrono::milliseconds, std::size_t) override {
      ADD_FAILURE() << "Resolving driver files cannot run a process";
      return {.exit_status = 1};
    }
  };

  struct fixture_t {
    std::filesystem::path root;
    fake_host_t host;
    spaces::nvidia_contract_t contract;

    explicit fixture_t(std::string name) {
      root = std::filesystem::temp_directory_path() / ("polaris-nvidia-" + name + "-" + std::to_string(::getpid()));
      std::filesystem::remove_all(root);
      contract = *spaces::decode_nvidia_contract(contract_json().dump());
    }
    ~fixture_t() { std::filesystem::remove_all(root); }

    static json contract_json() {
      return {{"schema", 1}, {"contract", 1}, {"minimum_driver", "570.00"},
        {"library_prefixes", json::array({"libcuda.so.", "libEGL_nvidia.so.", "libGLX_nvidia.so.",
          "libnvidia-encode.so.", "libnvidia-ml.so.", "libnvidia-glcore.so."})},
        {"architectures", json::array({
          {{"name", "amd64"}, {"directory", "/usr/lib/x86_64-linux-gnu"},
           {"search", json::array({"/usr/lib/x86_64-linux-gnu", "/usr/lib64"})},
           {"required", json::array({"libcuda.so.1", "libEGL_nvidia.so.0", "libGLX_nvidia.so.0",
             "libnvidia-encode.so.1", "libnvidia-ml.so.1"})}},
          {{"name", "i386"}, {"directory", "/usr/lib/i386-linux-gnu"},
           {"search", json::array({"/usr/lib/i386-linux-gnu", "/usr/lib"})},
           {"required", json::array({"libcuda.so.1", "libEGL_nvidia.so.0", "libGLX_nvidia.so.0"})}}})},
        {"vendor_files", json::array({
          {{"destination", "/usr/share/vulkan/icd.d/nvidia_icd.json"},
           {"sources", json::array({"/usr/share/vulkan/icd.d/nvidia_icd.json",
             "/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json"})}}})}};
    }

    void write(const std::filesystem::path &path, std::string_view contents) {
      const auto full = root / path.relative_path();
      std::filesystem::create_directories(full.parent_path());
      std::ofstream out(full, std::ios::binary);
      out << contents;
    }

    void library(const std::filesystem::path &directory, std::string_view file, std::string_view soname, bool thirty_two_bit) {
      write(directory / file, shared_object(soname, thirty_two_bit));
    }

    /** A host laid out the way Fedora lays one out, driver 615.71.09. */
    void complete_host(std::string_view version = "615.71.09") {
      const std::string suffix {version};
      for (const auto &[file, soname] : std::vector<std::pair<std::string, std::string>> {
             {"libcuda.so." + suffix, "libcuda.so.1"},
             {"libEGL_nvidia.so." + suffix, "libEGL_nvidia.so.0"},
             {"libGLX_nvidia.so." + suffix, "libGLX_nvidia.so.0"},
             {"libnvidia-encode.so." + suffix, "libnvidia-encode.so.1"},
             {"libnvidia-ml.so." + suffix, "libnvidia-ml.so.1"}})
        library("/usr/lib64", file, soname, false);
      for (const auto &[file, soname] : std::vector<std::pair<std::string, std::string>> {
             {"libcuda.so." + suffix, "libcuda.so.1"},
             {"libEGL_nvidia.so." + suffix, "libEGL_nvidia.so.0"},
             {"libGLX_nvidia.so." + suffix, "libGLX_nvidia.so.0"}})
        library("/usr/lib", file, soname, true);
      // Fedora writes an absolute host path here, which does not exist inside a
      // container; the resolver has to rewrite it.
      host.files[(root / "usr/share/vulkan/icd.d/nvidia_icd.x86_64.json").string()] =
        R"({"file_format_version":"1.0.1","ICD":{"library_path":"/usr/lib64/libGLX_nvidia.so.0","api_version":"1.4.351"}})";
    }

    spaces::host_driver_facts_t resolve(std::string_view version = "615.71.09") {
      return spaces::resolve_host_driver_libraries(contract, host, version, contract.minimum_driver, root);
    }
  };

  std::string soname_of(const spaces::host_driver_facts_t &facts, std::string_view architecture, std::string_view soname) {
    for (const auto &library : facts.libraries)
      if (library.architecture == architecture && library.soname == soname) return library.host_path.string();
    return {};
  }
}  // namespace

TEST(SpacesNvidiaContract, DecodesOnlyTheReviewedShape) {
  EXPECT_TRUE(spaces::decode_nvidia_contract(fixture_t::contract_json().dump()));
  EXPECT_TRUE(spaces::trusted_nvidia_contract()) << "the compiled contract must parse";

  for (const auto &mutate : std::vector<std::function<void(json &)>> {
         [](json &c) { c["schema"] = 2; },
         [](json &c) { c["contract"] = 0; },
         [](json &c) { c["minimum_driver"] = "570"; },
         [](json &c) { c["library_prefixes"] = json::array({"cuda"}); },
         [](json &c) { c["architectures"][0]["name"] = "arm64"; },
         [](json &c) { c["architectures"][0]["directory"] = "usr/lib"; },
         [](json &c) { c["architectures"][1]["required"] = json::array(); },
         [](json &c) { c["vendor_files"][0]["destination"] = "/etc/passwd"; },
         [](json &c) { c["vendor_files"][0]["sources"] = json::array(); },
         [](json &c) { c["architectures"].erase(1); },
         [](json &c) { c["extra"] = 1; }}) {
    auto document = fixture_t::contract_json();
    mutate(document);
    EXPECT_FALSE(spaces::decode_nvidia_contract(document.dump())) << document.dump();
  }
}

TEST(SpacesNvidiaLibraries, ResolvesBothAbisAtTheirSonamesFromTheLoadedDriverDirectory) {
  fixture_t fixture {"both-abis"};
  fixture.complete_host();

  const auto facts = fixture.resolve();

  ASSERT_TRUE(facts.ready()) << facts.code;
  EXPECT_EQ(facts.driver_version, "615.71.09");
  EXPECT_EQ(soname_of(facts, "amd64", "libcuda.so.1"), (fixture.root / "usr/lib64/libcuda.so.615.71.09").string());
  EXPECT_EQ(soname_of(facts, "i386", "libGLX_nvidia.so.0"), (fixture.root / "usr/lib/libGLX_nvidia.so.615.71.09").string());
  EXPECT_EQ(facts.libraries.size(), 8U);

  const auto mounts = spaces::host_driver_mounts(fixture.contract, facts, "/var/lib/polaris/spaces-graphics");
  ASSERT_FALSE(mounts.empty());
  EXPECT_TRUE(std::any_of(mounts.begin(), mounts.end(), [](const auto &mount) {
    return mount.destination == "/usr/lib/x86_64-linux-gnu/libcuda.so.1";
  }));
  EXPECT_TRUE(std::any_of(mounts.begin(), mounts.end(), [](const auto &mount) {
    return mount.destination == "/usr/lib/i386-linux-gnu/libEGL_nvidia.so.0";
  }));
}

TEST(SpacesNvidiaLibraries, RewritesAVendorPathThatOnlyExistsOnTheHost) {
  fixture_t fixture {"vendor-rewrite"};
  fixture.complete_host();

  const auto facts = fixture.resolve();

  ASSERT_TRUE(facts.ready()) << facts.code;
  ASSERT_EQ(facts.vendor_files.size(), 1U);
  EXPECT_EQ(facts.vendor_files.front().destination, "/usr/share/vulkan/icd.d/nvidia_icd.json");
  const auto document = json::parse(facts.vendor_files.front().contents);
  EXPECT_EQ(document.at("ICD").at("library_path"), "libGLX_nvidia.so.0");
  EXPECT_EQ(document.at("ICD").at("api_version"), "1.4.351");
}

TEST(SpacesNvidiaLibraries, RejectsAVendorFileNamingALibraryThatWasNotResolved) {
  fixture_t fixture {"vendor-unknown"};
  fixture.complete_host();
  fixture.host.files[(fixture.root / "usr/share/vulkan/icd.d/nvidia_icd.x86_64.json").string()] =
    R"({"file_format_version":"1.0.1","ICD":{"library_path":"/opt/evil/libGLX_nvidia.so.9"}})";

  const auto facts = fixture.resolve();

  EXPECT_EQ(facts.code, "driver_configuration_missing");
  EXPECT_TRUE(facts.libraries.empty());
}

TEST(SpacesNvidiaLibraries, ReportsMissingThirtyTwoBitLibrariesSeparately) {
  fixture_t fixture {"no-32bit"};
  fixture.complete_host();
  std::filesystem::remove_all(fixture.root / "usr/lib");

  const auto facts = fixture.resolve();

  EXPECT_EQ(facts.code, "driver_libraries_32bit_missing");
  EXPECT_FALSE(facts.missing.empty());
  EXPECT_TRUE(facts.libraries.empty()) << "a partial set must not be mounted";
}

TEST(SpacesNvidiaLibraries, RejectsALibraryWhoseElfClassDisagreesWithItsDirectory) {
  fixture_t fixture {"wrong-class"};
  fixture.complete_host();
  fixture.library("/usr/lib64", "libnvidia-ml.so.615.71.09", "libnvidia-ml.so.1", true);

  const auto facts = fixture.resolve();

  EXPECT_EQ(facts.code, "driver_libraries_missing");
  EXPECT_TRUE(std::any_of(facts.missing.begin(), facts.missing.end(),
    [](const auto &entry) { return entry == "amd64:libnvidia-ml.so.1"; }));
}

TEST(SpacesNvidiaLibraries, RejectsALibraryTheHostWillNotVouchFor) {
  fixture_t fixture {"untrusted"};
  fixture.complete_host();
  fixture.host.untrusted.insert((fixture.root / "usr/lib64/libcuda.so.615.71.09").string());

  const auto facts = fixture.resolve();

  EXPECT_EQ(facts.code, "driver_libraries_untrusted");
  EXPECT_TRUE(facts.libraries.empty());
}

TEST(SpacesNvidiaLibraries, RejectsADirectoryThatCarriesAnotherDriverVersion) {
  fixture_t fixture {"stale-copy"};
  fixture.complete_host("610.57.04");

  const auto facts = fixture.resolve("615.71.09");

  EXPECT_EQ(facts.code, "driver_libraries_missing");
  EXPECT_TRUE(std::any_of(facts.missing.begin(), facts.missing.end(),
    [](const auto &entry) { return entry == "615.71.09"; }));
}

TEST(SpacesNvidiaLibraries, RejectsTwoFilesClaimingTheSameSoname) {
  fixture_t fixture {"duplicate"};
  fixture.complete_host();
  fixture.library("/usr/lib64", "libnvidia-glcore.so.615.71.09", "libcuda.so.1", false);

  const auto facts = fixture.resolve();

  EXPECT_EQ(facts.code, "driver_libraries_untrusted");
}

TEST(SpacesNvidiaLibraries, RefusesADriverBelowTheContractMinimum) {
  fixture_t fixture {"too-old"};
  fixture.complete_host("560.35.03");

  const auto facts = fixture.resolve("560.35.03");

  EXPECT_EQ(facts.code, "driver_below_minimum");
  EXPECT_TRUE(spaces::host_driver_mounts(fixture.contract, facts, "/var/lib/polaris/spaces-graphics").empty());
}

/**
 * Opt in with POLARIS_NVIDIA_HOST_PHYSICAL=1 on a machine with the NVIDIA
 * driver loaded. Fixtures prove the rules; this proves the rules describe a
 * real driver install, which is the part that varies by distribution.
 */
TEST(SpacesNvidiaLibrariesPhysical, ResolvesThisMachinesDriver) {
  const auto *opt_in = std::getenv("POLARIS_NVIDIA_HOST_PHYSICAL");
  if (!opt_in || std::string_view {opt_in} != "1") GTEST_SKIP() << "opt-in physical check";
  std::ifstream module("/sys/module/nvidia/version");
  std::string loaded;
  ASSERT_TRUE(std::getline(module, loaded)) << "no NVIDIA driver is loaded";

  const auto &contract = spaces::trusted_nvidia_contract();
  ASSERT_TRUE(contract);
  container::local_host_t host;
  const auto facts = spaces::resolve_host_driver_libraries(*contract, host, loaded, contract->minimum_driver);

  for (const auto &entry : facts.missing) std::cerr << "missing: " << entry << "\n";
  for (const auto &library : facts.libraries)
    std::cerr << library.architecture << " " << library.soname << " -> " << library.host_path << "\n";
  for (const auto &file : facts.vendor_files)
    std::cerr << file.destination << "\n" << file.contents << "\n";
  EXPECT_EQ(facts.code, "") << "this machine's driver files did not resolve";
  EXPECT_FALSE(facts.libraries.empty());
}

TEST(SpacesNvidiaLibraries, ComparesDriverVersionsByNumberNotText) {
  EXPECT_TRUE(spaces::driver_at_least("615.71.09", "570.00"));
  EXPECT_TRUE(spaces::driver_at_least("570.00", "570.00"));
  EXPECT_TRUE(spaces::driver_at_least("570.0.1", "570.00"));
  EXPECT_FALSE(spaces::driver_at_least("9.99", "570.00"));
  EXPECT_FALSE(spaces::driver_at_least("560.35.03", "570.00"));
  EXPECT_FALSE(spaces::driver_at_least("", "570.00"));
  EXPECT_FALSE(spaces::driver_at_least("570.00", "not-a-version"));
}
#endif
