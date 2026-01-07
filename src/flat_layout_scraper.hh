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

#include "scraper.hh"

namespace cheri {

enum class LayoutKind {
  Struct = 1,
  Class = 2,
  Union = 3,
};

/**
 * In-memory representation of a member in a flattened layout
 */
struct LayoutMember {
  LayoutMember()
      : byte_size(0), bit_size(0), byte_offset(0), bit_offset(0), alignment(0), depth(0),
        is_pointer(false), is_function(false), is_anon(false), is_union(false),
        is_imprecise(false), base(0), top(0), required_precision(0) {}

  // Qualified flattened member name using :: as separator
  std::string name;
  // Name of the member type
  std::string type_name;
  // Size, in bytes, of the member
  unsigned long long byte_size;
  // Fractional bit remainder of the size
  uint8_t bit_size;
  // Offset from the start of the layout
  unsigned long long byte_offset;
  // Fractional bit remainder of the size
  // This can be >= 2**8
  unsigned long long bit_offset;
  // Number of array items, if the field is an array
  // this will be 0 if the array is a flexible array.
  std::optional<unsigned long long> array_items;
  // Alignment in bytes
  uint64_t alignment;
  // Depth of member
  uint64_t depth;
  // Flags used to mark member properties
  bool is_pointer : 1;
  bool is_function : 1;
  bool is_anon : 1;
  bool is_union : 1;
  bool is_imprecise : 1;
  // Capability base for this sub-object
  uint64_t base;
  // Capability top for this sub-object
  uint64_t top;
  // Number of precision bits required to represent this field exactly
  short required_precision;
  // If this member is a VLA, the maximum size given the current alignment.
  std::optional<unsigned long long> max_vla_size;
};

using LayoutId = std::tuple<std::string, size_t>;

struct LayoutHash {
  std::size_t operator()(const LayoutId &k) const noexcept {
    std::size_t h0 = std::hash<std::string>{}(std::get<0>(k));
    std::size_t h1 = std::hash<std::size_t>{}(std::get<1>(k));

    return llvm::hash_combine(h0, h1);
  }
};

/**
 * In-memory representation of a flattened structure layout
 */
struct FlattenedLayout {
  FlattenedLayout() : line(0), size(0), die_offset(0), has_vla(false) {}
  FlattenedLayout(const TypeDesc &desc);
  LayoutId id() const { return std::make_tuple(file, line); }

  // Source file where the structure is defined
  std::string file;
  // Line where the structure is defined
  unsigned long long line;
  // Name of the structure, without considering any typedef
  std::string name;
  // Total size of the structure
  unsigned long long size;
  // Differentiate struct, union and class types.
  LayoutKind kind;

  // Offset of the related type DIE
  uint64_t die_offset;
  // Members as part of the flattened layout
  // Direct member information
  std::vector<std::unique_ptr<LayoutMember>> members;
  // Does the layout include a VLA?
  bool has_vla;
};

/**
 * Scraper to extract flattened structure layout information from DWARF.
 *
 * This creates a simplified schema with only two tables.
 * - The 'struct_types' table contains a record for each structure found,
 *   uniquely identified by the tuple (name, file, line, size).
 * - The 'members' table contains the flattened layout for all structures.
 *   Each member is associated with one and only one entry in the 'types' table.
 */
class FlatLayoutScraper : public DwarfScraper {
public:
  FlatLayoutScraper(StorageManager &sm,
                    std::unique_ptr<const DwarfSource> dwsrc)
      : DwarfScraper(sm, std::move(dwsrc)) {}

  std::string name() override { return "flat-layout"; }

  bool visit_structure_type(llvm::DWARFDie &die);
  bool visit_class_type(llvm::DWARFDie &die);
  bool visit_union_type(llvm::DWARFDie &die);
  bool visit_typedef(llvm::DWARFDie &die);

protected:
  void initSchema() override;
  void beginUnit(llvm::DWARFDie &unit_die) override;
  void endUnit(llvm::DWARFDie &unit_die) override;
  bool doVisit(llvm::DWARFDie &die) override {
    return impl::visitDispatch(*this, die);
  }

  /**
   * Common visitor logic for all aggregate types.
   * Returns a reference to the in-memory flattend layout.
   */
  std::optional<FlattenedLayout *>
  visitCommon(const llvm::DWARFDie &die,
              std::optional<std::string> typedef_name = std::nullopt);

  /**
   * Visit a structure/union/class member DIE
   */
  LayoutMember *visitNested(const llvm::DWARFDie &die, FlattenedLayout *layout,
                            std::string prefix, long mindex,
                            unsigned long offset, uint64_t depth);

  /**
   * Check whether the given member is a VLA and mark the layout accordingly.
   */
  void checkVLAMember(FlattenedLayout *layout, LayoutMember *member);

  /**
   * Insert a flattened layout into the database.
   */
  void recordLayout(std::unique_ptr<FlattenedLayout> layout);

  /**
   * Compilation unit currently being scanned
   */
  std::string current_unit_;

  /**
   * Flattened layouts.
   * Associate a (file, line) tuple to each flattened layout.
   * It is assumed that the (file, line) tuple is uniquely indentifying a
   * structure layout. Associate a (file, line) tuple to a set of entries
   * defined at that source location.
   */
  std::unordered_map<LayoutId, std::unique_ptr<FlattenedLayout>, LayoutHash>
      layouts_;
};

} /* namespace cheri */
