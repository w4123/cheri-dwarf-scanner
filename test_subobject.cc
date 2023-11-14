#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

#include "scraper.hh"

namespace fs = std::filesystem;

fs::path GetAssetPath(std::string_view v) {
  const char *env_value = std::getenv("ASSET_DIR");
  assert(env_value != nullptr && "Must specify ASSET_DIR env var");
  fs::path base(env_value);
  return base.append(v);
}

TEST(ComputePrecision, Subobject) {
  cheri::DwarfSource dwsrc(
      GetAssetPath("riscv_purecap_test_unrepresentable_subobject"));

  auto Check = [&dwsrc](uint64_t base, uint64_t top) {
    return dwsrc.FindRequiredPrecision(base, top - base);
  };

  ASSERT_EQ(Check(0x00000000, 0x00100000), 1);
  ASSERT_EQ(Check(0x00000004, 0x00001004), 11);
  ASSERT_EQ(Check(0x0FFFFFFF, 0x10000000), 1);
  ASSERT_EQ(Check(0x00000FFF, 0x00002001), 13);
}
