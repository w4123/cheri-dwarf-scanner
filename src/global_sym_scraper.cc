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
#include <QVariant>

#include "llvm/DebugInfo/DWARF/DWARFExpression.h"
#include "llvm/Support/DataExtractor.h"

#include "global_sym_scraper.hh"

namespace dwarf = llvm::dwarf;
using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;

namespace cheri {

void GlobalSymScraper::initSchema() {
  // clang-format off
  /* Initialize tables */
  sm_.query("CREATE TABLE IF NOT EXISTS global_sym ("
            "id INTEGER PRIMARY KEY,"
            // File where the symbol is defined
            "file TEXT NOT NULL,"
            // Line where the symbol is defined
            "line INTEGER NOT NULL,"
            // Name of the symbol.
            "name TEXT NOT NULL,"
            // Size in bytes
            "size INTEGER NOT NULL,"
            // If not NULL, a sized array with the given number of items
            "array_items INTEGER,"
            // Required capability alignment
            "cap_alignment INTEGER NOT NULL,"
            // Representable symbol length
            "cap_length INTEGER NOT NULL,"
            // Whether the symbol size is representable
            "is_imprecise INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_imprecise >= 0 AND is_imprecise <= 1),"
            "UNIQUE(name, file, line))");
  // clang-format on
}

void GlobalSymScraper::beginUnit(llvm::DWARFDie &unit_die) {
  auto at_name = unit_die.find(dwarf::DW_AT_name);
  if (at_name) {
    llvm::Expected name = at_name->getAsCString();
    if (name) {
      current_unit_ = *name;
    } else {
      qCritical() << "Invalid compilation unit, can't extract AT_name";
      throw ScraperError("Invalid compilation unit:", name);
    }
  } else {
    qCritical() << "Invalid compliation unit, missing AT_name";
    throw ScraperError("Invalid compliation unit");
  }
  qDebug() << "Enter compilation unit" << current_unit_;
}

void GlobalSymScraper::endUnit(llvm::DWARFDie &unit_die) {
  qDebug() << "Done compilation unit" << current_unit_;

  for (auto i = globals_.begin(); i != globals_.end(); i++) {
    GlobalSymInfo info;
    std::swap(i->second, info);
    recordInfo(std::move(info));
  }

  globals_.clear();
}

std::optional<uint64_t> GlobalSymScraper::getGlobalAddr(llvm::DWARFDie &die) {
  // Verify that this is a global variable
  // We match the exact expression DW_OP_addr(x) [DW_OP_plus_uconst]
  uint64_t global_addr;

  if (!die.find(dwarf::DW_AT_location)) {
    // Nothing to do
    return std::nullopt;
  }

  auto loc_vec = die.getLocations(dwarf::DW_AT_location);
  if (auto err = loc_vec.takeError()) {
    qCritical() << std::format(
        "Can not extract DW_AT_location data for DIE {:#x} in {}",
        die.getOffset(), current_unit_);
    throw ScraperError(llvm::toString(std::move(err)));
  }
  auto &unit = *die.getDwarfUnit();
  auto addr_size = unit.getAddressByteSize();
  for (auto &loc : *loc_vec) {
    llvm::DataExtractor data(loc.Expr, source().getContext().isLittleEndian(),
                             addr_size);
    llvm::DWARFExpression expr(data, addr_size);
    auto it = expr.begin();
    if (it->getCode() == dwarf::DW_OP_addr) {
      global_addr = it->getRawOperand(0);
    } else if (it->getCode() == dwarf::DW_OP_addrx) {
      uint64_t index = it->getRawOperand(0);
      if (auto section_addr = unit.getAddrOffsetSectionItem(index)) {
        global_addr = section_addr->Address;
      }
    } else {
      // Unexpected first operand, try next location
      continue;
    }

    // Optional DW_OP_plus_uconst operand
    if (++it != expr.end()) {
      if (it->getCode() != dwarf::DW_OP_plus_uconst) {
        // Unexpected second operand, try next location
        continue;
      }
      global_addr += it->getRawOperand(0);
      // There shouldn't be a third operand
      if (++it != expr.end()) {
        continue;
      }
    }
    return global_addr;
  }

  return std::nullopt;
}

bool GlobalSymScraper::visit_variable(llvm::DWARFDie &die) {
  GlobalSymInfo info;

  // Ignore declarations
  if (die.find(dwarf::DW_AT_declaration)) {
    return false;
  }

  auto maybe_addr = getGlobalAddr(die);
  if (!maybe_addr) {
    // Not a global variable, bail
    return false;
  }
  info.addr = *maybe_addr;
  auto at_name = getStrAttr(die, dwarf::DW_AT_name);
  if (!at_name) {
    // Sometimes we may find unnamed globals. These seem to be emitted for
    // global constant strings.
    // We don't have enough information in DWARF to link the string buffer to
    // a char * variable initialized to the string buffer, but we may attempt
    // to match them based on the (file, line) tuple.
    // For bounds precision, there is little use for this, so just generate an
    // anonymous name.
    info.name = anonymousName(die);
  } else {
    info.name = *at_name;
  }
  info.file = die.getDeclFile(FLIKind::AbsoluteFilePath);
  info.line = die.getDeclLine();

  llvm::DWARFDie def = die;
  constexpr int kMaxFollow = 8;
  for (int i = 0; i < kMaxFollow && def.isValid(); i++) {
    if (def.find(dwarf::DW_AT_type)) break;
    if (auto spec = def.getAttributeValueAsReferencedDie(dwarf::DW_AT_specification);
        spec.isValid()) { def = spec; continue; }
    if (auto ao = def.getAttributeValueAsReferencedDie(dwarf::DW_AT_abstract_origin);
        ao.isValid()) { def = ao; continue; }
    break;
  }

  auto type_die = def.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                      .resolveTypeUnitReference();
  TypeDesc desc = resolveTypeDie(type_die);
  info.size = desc.byte_size;
  info.array_items = desc.array_count;
  info.cap_alignment = source().findRepresentableAlign(info.size);
  auto [_, length] = source().findRepresentableRange(0, info.size);
  info.cap_length = length;

  qDebug() << "Found global sym "
           << std::format("{}:{} {} @{:#x} size={:#x} align={:#x} clen={:#x}",
                          info.file, info.line, desc.name, info.addr, info.size,
                          info.cap_alignment, info.cap_length);
  auto [pos, inserted] =
      globals_.emplace(std::make_pair(info.id(), std::move(info)));
  assert(inserted && "Could not insert duplicate global");

  return false;
}

void GlobalSymScraper::recordInfo(GlobalSymInfo &&info) {
  sm_.transaction([&](StorageManager &sm) {
    qDebug() << "Transaction for" << info.name;

    // clang-format off
    auto insert_info = sm.prepare(
        "INSERT INTO global_sym (file, line, name, size, array_items, "
        "cap_alignment, cap_length, is_imprecise) "
        "VALUES (:file, :line, :name, :size, :array_items, :cap_align, "
        ":cap_len, :is_imprecise) "
        "ON CONFLICT DO NOTHING RETURNING id");
    // clang-format on

    insert_info.bindValue(":file", QString::fromStdString(info.file));
    insert_info.bindValue(":line", info.line);
    insert_info.bindValue(":name", QString::fromStdString(info.name));
    insert_info.bindValue(":size", info.size);
    if (info.array_items) {
      insert_info.bindValue(":array_items", *info.array_items);
    } else {
      insert_info.bindValue(":array_items", QVariant::fromValue(nullptr));
    }
    insert_info.bindValue(":cap_align",
                          static_cast<unsigned long long>(info.cap_alignment));
    insert_info.bindValue(":cap_len",
                          static_cast<unsigned long long>(info.cap_length));
    insert_info.bindValue(":is_imprecise", info.size != info.cap_length);
    if (!insert_info.exec()) {
      // Failed, abort the transaction
      qCritical() << "Failed to insert global info:" << insert_info.lastQuery();
      throw DBError(insert_info.lastError());
    }
    insert_info.finish();

    qDebug() << "Transaction for" << info.name << "Done";
  });
}

} /* namespace cheri */
