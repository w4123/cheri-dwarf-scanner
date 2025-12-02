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
#include <format>
#include <limits>
#include <mutex>
#include <stdexcept>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <QDebug>
#include <QtLogging>

#include "cheri_compressed_cap.h"

#include "scraper.hh"

namespace fs = std::filesystem;
namespace object = llvm::object;
namespace dwarf = llvm::dwarf;

namespace {

template <typename CC> struct CapTraits {};

template <> struct CapTraits<CompressedCap128m> {
  using CapType = typename CompressedCap128m::cap_t;
  using AddrType = typename CompressedCap128m::addr_t;
  static CapType &SetAddr(CapType &cap, AddrType cursor) {
    cc128m_set_addr(&cap, cursor);
    return cap;
  }
};

template <> struct CapTraits<CompressedCap128> {
  using CapType = typename CompressedCap128::cap_t;
  using AddrType = typename CompressedCap128::addr_t;
  static CapType &SetAddr(CapType &cap, AddrType cursor) {
    cc128_set_addr(&cap, cursor);
    return cap;
  }
};

template <> struct CapTraits<CompressedCap64> {
  using CapType = typename CompressedCap64::cap_t;
  using AddrType = typename CompressedCap64::addr_t;
  static CapType &SetAddr(CapType &cap, AddrType cursor) {
    cc64_set_addr(&cap, cursor);
    return cap;
  }
};

template <typename CC>
std::pair<uint64_t, uint64_t> findRepresentableRangeImpl(uint64_t base,
                                                         uint64_t length) {
  using AddrT = typename CC::addr_t;
  using CheriCap = typename CC::cap_t;

  CheriCap cap =
      CC::make_max_perms_cap(0, 0, std::numeric_limits<AddrT>::max() + 1);
  CapTraits<CC>::SetAddr(cap, base);
  bool exact = CC::setbounds(&cap, length);
  if (exact) {
    assert(cap.base() == base && "Invalid exact base?");
    assert(cap.length64() == length && "Invalid exact length?");
  }
  return std::make_pair(cap.base(), cap.length64());
}

template <typename CC> uint64_t findRepresentableAlignImpl(uint64_t length) {
  return CC::representable_mask(length);
}

} // namespace

namespace cheri {

QDebug operator<<(QDebug debug, const std::filesystem::path &p) {
  QDebugStateSaver saver(debug);
  debug.nospace() << p.string();
  return debug;
}

QDebug operator<<(QDebug debug, const ScraperResult &sr) {
  QDebugStateSaver saver(debug);
  QDebug stream = debug.nospace();
  stream << "Result for " << sr.source << ":";
  if (sr.errors.size()) {
    stream << " (WITH ERRORS)";
  }
  return debug;
}

std::string anonymousName(const llvm::DWARFDie &die) {
  return std::format("<anon@{:#x}>", die.getOffset());
}

std::optional<unsigned long> getULongAttr(const llvm::DWARFDie &die,
                                          dwarf::Attribute attr) {
  if (auto opt = dwarf::toUnsigned(die.find(attr))) {
    return *opt;
  }
  return std::nullopt;
}

std::optional<std::string> getStrAttr(const llvm::DWARFDie &die,
                                      dwarf::Attribute attr) {
  if (auto opt = dwarf::toString(die.find(attr))) {
    return *opt;
  }
  return std::nullopt;
}

DwarfSource::DwarfSource(fs::path path) : path_{path} {
  static std::once_flag llvm_init_flag;
  std::call_once(llvm_init_flag, []() {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargetMCs();
  });

  llvm::Expected<object::OwningBinary<object::Binary>> bin_or_err =
      object::createBinary(path.string());
  if (auto err = bin_or_err.takeError()) {
    throw std::runtime_error(llvm::toString(std::move(err)));
  }

  owned_binary_ = std::move(*bin_or_err);

  auto *obj = llvm::dyn_cast<object::ObjectFile>(owned_binary_.getBinary());
  if (obj == nullptr) {
    throw std::runtime_error(
        std::format("Invalid binary at %s, not an object", path.string()));
  }

  dictx_ = llvm::DWARFContext::create(
      *obj, llvm::DWARFContext::ProcessDebugRelocations::Process, nullptr, "",
      nullptr);

  // Check DWARF version
  if (dictx_->getMaxVersion() < 4) {
    throw std::runtime_error("Unsupported DWARF version, use 4 or above");
  }
}

fs::path DwarfSource::getPath() const { return path_; }

llvm::DWARFContext &DwarfSource::getContext() const { return *dictx_; }

int DwarfSource::getABIPointerSize() const {
  auto *obj = dictx_->getDWARFObj().getFile();
  assert(obj != nullptr && "Invalid DWARF source");
  auto triple = obj->makeTriple();

  if (triple.getEnvironment() == llvm::Triple::CheriPurecap) {
    return getABICapabilitySize();
  } else {
    if (triple.getArch() == llvm::Triple::aarch64 ||
        triple.getArch() == llvm::Triple::riscv64) {
      return 8;
    } else if (triple.getArch() == llvm::Triple::riscv32) {
      return 4;
    }
  }
  throw std::runtime_error("Unsupported architecture");
}

int DwarfSource::getABICapabilitySize() const {
  auto *obj = dictx_->getDWARFObj().getFile();
  assert(obj != nullptr && "Invalid DWARF source");
  auto triple = obj->makeTriple();

  if (triple.getArch() == llvm::Triple::aarch64 ||
      triple.getArch() == llvm::Triple::riscv64) {
    return 16;
  } else if (triple.getArch() == llvm::Triple::riscv32) {
    return 8;
  }
  throw std::runtime_error("Unsupported architecture");
}

std::pair<uint64_t, uint64_t>
DwarfSource::findRepresentableRange(uint64_t base, uint64_t length) const {
  auto *obj = dictx_->getDWARFObj().getFile();
  assert(obj != nullptr && "Invalid DWARF source");
  auto triple = obj->makeTriple();

  if (triple.getArch() == llvm::Triple::aarch64) {
    return findRepresentableRangeImpl<CompressedCap128m>(base, length);
  } else if (triple.getArch() == llvm::Triple::riscv64) {
    return findRepresentableRangeImpl<CompressedCap128>(base, length);
  } else if (triple.getArch() == llvm::Triple::riscv32) {
    return findRepresentableRangeImpl<CompressedCap64>(base, length);
  }

  throw std::runtime_error("Unsupported architecture");
}

uint64_t DwarfSource::findRepresentableAlign(uint64_t length) const {
  auto *obj = dictx_->getDWARFObj().getFile();
  assert(obj != nullptr && "Invalid DWARF source");
  auto triple = obj->makeTriple();

  if (triple.getArch() == llvm::Triple::aarch64) {
    return findRepresentableAlignImpl<CompressedCap128m>(length);
  } else if (triple.getArch() == llvm::Triple::riscv64) {
    return findRepresentableAlignImpl<CompressedCap128>(length);
  } else if (triple.getArch() == llvm::Triple::riscv32) {
    return findRepresentableAlignImpl<CompressedCap64>(length);
  }

  throw std::runtime_error("Unsupported architecture");
}

short DwarfSource::findRequiredPrecision(uint64_t base, uint64_t length) const {
#if !__has_builtin(__builtin_clzll) || !__has_builtin(__builtin_ffsll)
#error "__builtin_clzll and ffsll are required!"
#endif
  if (length == 0)
    return 0;

  uint64_t top = base + length;
  int len_msb = 64 - __builtin_clzll(length);
  int exp = 0;
  if (base == 0) {
    exp = __builtin_ffsll(top);
  } else {
    exp = std::min(__builtin_ffsll(base), __builtin_ffsll(top));
  }
  return len_msb - exp + 1;
}

DwarfScraper::DwarfScraper(StorageManager &sm,
                           std::unique_ptr<const DwarfSource> dwsrc)
    : sm_(sm), dwsrc_(std::move(dwsrc)) {}

void DwarfScraper::run(std::stop_token stop_tok) {
  auto &dictx = dwsrc_->getContext();

  // auto timing = stats_.Timing("elapsed_time");
  for (auto &unit : dictx.info_section_units()) {
    if (stop_tok.stop_requested()) {
      break;
    }
    if (!llvm::isCompileUnit(unit)) {
      continue;
    }
    if (unit->getVersion() < 4) {
      throw std::runtime_error("Unsupported DWARF version");
    }

    llvm::DWARFDie unit_die = unit->getUnitDIE(false);
    beginUnit(unit_die);
    try {
      /* Iterate over DIEs in the unit */
      llvm::DWARFDie child_die = unit_die.getFirstChild();
      bool stop = false;
      while (child_die && !stop && !stop_tok.stop_requested()) {
        // stop = impl::VisitDispatch(*this, child_die);
        stop = this->doVisit(child_die);
        child_die = child_die.getSibling();
      }
    } catch (std::exception &ex) {
      qCritical() << "Failed to scan compilation unit "
                  << unit_die.getName(llvm::DINameKind::LinkageName)
                  << " reason: " << ex.what();
      stats_.errors.push_back(ex.what());
    }
    endUnit(unit_die);
  }
}

TypeDesc DwarfScraper::resolveTypeDie(const llvm::DWARFDie &die) {
  assert(die.isValid() && "Invalid DIE");
  TypeDesc desc(die);

  // Resolve the type name in a readable form.
  llvm::raw_string_ostream type_name_stream(desc.name);
  llvm::dumpTypeUnqualifiedName(die, type_name_stream);

  bool has_typedef = false;
  llvm::DWARFDie iter_die = die;
  std::optional<uint64_t> byte_size;
  uint64_t size_multiplier = 1;
  while (iter_die) {
    switch (iter_die.getTag()) {
    case dwarf::DW_TAG_const_type:
      desc.is_const = true;
      break;
    case dwarf::DW_TAG_volatile_type:
      desc.is_volatile = true;
      break;
    case dwarf::DW_TAG_base_type: {
      auto size = getULongAttr(iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        qCritical() << "Found DW_TAG_base_type without a size";
        throw ScraperError("Base type without a size");
      }
      if (!byte_size)
        byte_size = *size;
      break;
    }
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type:
      desc.pointer = PointerKind::Reference;
      /* fallthrough */
    case dwarf::DW_TAG_pointer_type: {
      auto size = getULongAttr(iter_die, dwarf::DW_AT_byte_size)
                      .value_or(source().getABIPointerSize());
      if (!byte_size)
        byte_size = size;
      if (!desc.pointer)
        desc.pointer = PointerKind::Pointer;
      break;
    }
    case dwarf::DW_TAG_subroutine_type: {
      desc.pointer = PointerKind::Function;
      break;
    }
    case dwarf::DW_TAG_array_type: {
      // Find the first child DIE with TAG subrange_type;
      // this contains the number of items in the array.
      if (iter_die.find(dwarf::DW_AT_byte_stride) ||
          iter_die.find(dwarf::DW_AT_bit_stride)) {
        qCritical()
            << "Unsupported attributes DW_AT_{byte, bit}_stride on arrays";
        throw ScraperError("Not implemented");
      }
      if (iter_die.find(dwarf::DW_AT_bit_size)) {
        qCritical() << "Unsupported attribute DW_AT_bit_size on arrays";
        throw ScraperError("Not implemented");
      }

      // If this is a pointer type to something, we don't care about
      // the array information because it is erased by the pointerness.
      assert(!desc.pointer &&
             "Unexpectedly reached DW_TAG_array_type with a pointer");

      // Every subrange die describes an array dimension.
      // Note that only the last one may be without a size in C.
      std::optional<uint64_t> array_count;
      for (auto &child : iter_die) {
        if (child.getTag() != dwarf::DW_TAG_subrange_type) {
          break;
        }
        auto count = getULongAttr(child, dwarf::DW_AT_count);
        auto upper_bound = getULongAttr(child, dwarf::DW_AT_upper_bound);
        if (count) {
          array_count = array_count.value_or(1) * (*count);
        } else if (upper_bound) {
          array_count = array_count.value_or(1) * (*upper_bound + 1);
        } else {
          // No size given, assume 0
          array_count = 0;
        }
      }

      if (!array_count) {
        qCritical() << "Found DW_TAG_array_type without a subrange DIE";
        throw ScraperError("Array without subrange");
      }
      desc.array_count = array_count;

      auto arr_bytes = getULongAttr(iter_die, dwarf::DW_AT_byte_size);
      if (arr_bytes && !byte_size) {
        // Override type size calculation as we already know the size
        byte_size = *arr_bytes;
      } else {
        size_multiplier = array_count.value();
      }
      break;
    }
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
    case dwarf::DW_TAG_enumeration_type:
    case dwarf::DW_TAG_union_type: {
      using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;
      TypeDecl decl(iter_die);
      decl.file =
          normalizePath(iter_die.getDeclFile(FLIKind::AbsoluteFilePath));
      decl.line = iter_die.getDeclLine();
      decl.name = getStrAttr(iter_die, dwarf::DW_AT_name);
      desc.decl = decl;

      /*
       * This only happens when we have a pointer to something,
       * it is safe to ignore the size here, as the pointer DIE
       * will set it.
       * Note that in this case this should not be reached.
       */
      assert(!iter_die.find(dwarf::DW_AT_declaration));

      auto size = getULongAttr(iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        auto tag_name = dwarf::TagString(iter_die.getTag());
        qCritical() << "Found aggregate type without size at DIE"
                    << tag_name.str();
        throw ScraperError("Aggregate type without size");
      }
      if (!byte_size)
        byte_size = *size;
      break;
    }
    case dwarf::DW_TAG_typedef:
      // Signal that the declaration will not be anonymous.
      has_typedef = true;
      break;
    default:
      auto tag_name = dwarf::TagString(iter_die.getTag());
      qCritical() << "Unhandled DIE " << tag_name.str();
      throw ScraperError("Unhandled DIE");
    }

    // If we reached a pointer type, stop scanning.
    // We don't care about the rest, as the pointer forces the size to be
    // a pointer size.
    if (desc.pointer) {
      break;
    }

    iter_die = iter_die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                   .resolveTypeUnitReference();
  }

  if (!byte_size) {
    qCritical() << "Failed to resolve type description for DIE @"
                << die.getOffset();
    throw ScraperError("Failed to resolve type description");
  }

  desc.byte_size = byte_size.value() * size_multiplier;
  /*
   * If this is an anonymous definition, dumpTypeUnqualified name will be of
   * the form "struct ". Therefore, we generate a unique name based on the
   * location of the typedef.
   */
  if (desc.decl && !desc.decl->name && !has_typedef) {
    desc.name += anonymousName(desc.decl->type_die);
    desc.is_anonymous = true;
  }
  return desc;
}

fs::path DwarfScraper::normalizePath(fs::path path) {
  if (strip_prefix_) {
    path = fs::relative(path, *strip_prefix_);
  }
  return path;
}

ScraperResult DwarfScraper::result() {
  ScraperResult r(stats_);
  r.source = dwsrc_->getPath();

  return r;
}

} /* namespace cheri */
