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

#pragma once

#include <cstdint>
#include <unordered_map>
#include <variant>

#include "scraper.hh"

namespace cheri {

using SymbolId = std::tuple<std::string, std::string, size_t>;

struct GlobalSymInfo {
  GlobalSymInfo()
      : line(0), addr(0), size(0), cap_alignment(0), cap_length(0) {}
  SymbolId id() const { return std::make_tuple(name, file, line); }

  // Source file where the symbol is defined
  std::string file;
  // Line where the symbol is defined
  unsigned long long line;
  // Global variable address (not relocated)
  unsigned long long addr;
  // Name of the symbol
  std::string name;
  // Symbol size (requested size)
  unsigned long long size;
  // Symbol array items, if any
  std::optional<unsigned long long> array_items;
  // Capability alignment required
  uint64_t cap_alignment;
  // Capability size required
  uint64_t cap_length;
};

struct SymbolHash {
  std::size_t operator()(const SymbolId &k) const noexcept {
    std::size_t h0 = std::hash<std::string>{}(std::get<0>(k));
    std::size_t h1 = std::hash<std::string>{}(std::get<1>(k));
    std::size_t h2 = std::hash<std::size_t>{}(std::get<2>(k));

    return llvm::hash_combine(h0, h1, h2);
  }
};

/**
 * Scraper to extract global variables information from DWARF.
 *
 * The scraper annotates imprecise global variables that need
 * padding for CHERI representability.
 */
class GlobalSymScraper : public DwarfScraper {
public:
  GlobalSymScraper(StorageManager &sm, std::unique_ptr<const DwarfSource> dwsrc)
      : DwarfScraper(sm, std::move(dwsrc)) {}

  std::string name() override { return "global-var"; }

  bool visit_variable(llvm::DWARFDie &die);

protected:
  void initSchema() override;
  void beginUnit(llvm::DWARFDie &unit_die) override;
  void endUnit(llvm::DWARFDie &unit_die) override;
  bool doVisit(llvm::DWARFDie &die) override {
    return impl::visitDispatch(*this, die);
  }

  /**
   * Attempt to extract the address of a global variable.
   * If nullopt is returned, the DIE does not describe a global variable.
   */
  std::optional<uint64_t> getGlobalAddr(llvm::DWARFDie &die);

  /**
   * Write a global variable info descriptor to the database.
   */
  void recordInfo(GlobalSymInfo &&info);

  /**
   * Globals information.
   * Associate a (file, line) tuple to each flattened layout.
   * It is assumed that the (file, line) tuple is uniquely indentifying a
   * structure layout. Associate a (file, line) tuple to a set of entries
   * defined at that source location.
   */
  std::unordered_map<SymbolId, GlobalSymInfo, SymbolHash> globals_;

  /**
   * Compilation unit currently processed
   */
  std::string current_unit_;
};

} /* namespace cheri */
