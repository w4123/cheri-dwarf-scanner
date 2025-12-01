/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023-2025 Alfredo Mazzinghi
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

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
