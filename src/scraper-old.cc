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

#include "cheri_compressed_cap.h"

#include "log.hh"
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
std::pair<uint64_t, uint64_t> FindRepresentableRangeImpl(uint64_t base,
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

} // namespace

namespace cheri {

TimingScope ScrapeResult::Timing(std::string_view name) {
  auto [entry, _] = profile.emplace(name, TimingInfo());
  return TimingScope(entry->second);
}

std::ostream &operator<<(std::ostream &os, const ScrapeResult &sr) {
  os << "Result for " << sr.source << ":";
  for (auto &prof : sr.profile) {
    os << " |" << prof.first << "> #" << prof.second.events
       << " avg:" << prof.second.avg;
  }
  if (sr.errors.size()) {
    os << " (WITH ERRORS)";
  }
  return os;
}

std::optional<unsigned long> GetULongAttr(const llvm::DWARFDie &die,
                                          dwarf::Attribute attr) {
  if (auto opt = dwarf::toUnsigned(die.find(attr))) {
    return *opt;
  }
  return std::nullopt;
}

std::optional<std::string> GetStrAttr(const llvm::DWARFDie &die,
                                      dwarf::Attribute attr) {
  if (auto opt = dwarf::toString(die.find(attr))) {
    return *opt;
  }
  return std::nullopt;
}

llvm::DWARFDie FindFirstChild(const llvm::DWARFDie &die, dwarf::Tag tag) {
  for (auto &child : die) {
    if (child.getTag() == tag)
      return child;
  }
  return llvm::DWARFDie();
}

std::string AnonymousName(const llvm::DWARFDie &die,
                          const std::optional<fs::path> &strip,
                          const std::string prefix) {
  using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;
  std::string file = die.getDeclFile(FLIKind::AbsoluteFilePath);
  if (strip) {
    file = fs::relative(file, *strip);
  }
  unsigned long line = die.getDeclLine();
  return std::format("<anon@{}{}+{:d}>", prefix, file, line);
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

fs::path DwarfSource::GetPath() const { return path_; }

llvm::DWARFContext &DwarfSource::GetContext() const { return *dictx_; }

int DwarfSource::GetABIPointerSize() const {
  auto *obj = dictx_->getDWARFObj().getFile();
  assert(obj != nullptr && "Invalid DWARF source");
  auto triple = obj->makeTriple();

  if (triple.getEnvironment() == llvm::Triple::CheriPurecap) {
    return GetABICapabilitySize();
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

int DwarfSource::GetABICapabilitySize() const {
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
DwarfSource::FindRepresentableRange(uint64_t base, uint64_t length) const {
  auto *obj = dictx_->getDWARFObj().getFile();
  assert(obj != nullptr && "Invalid DWARF source");
  auto triple = obj->makeTriple();

  if (triple.getArch() == llvm::Triple::aarch64) {
    return FindRepresentableRangeImpl<CompressedCap128m>(base, length);
  } else if (triple.getArch() == llvm::Triple::riscv64) {
    return FindRepresentableRangeImpl<CompressedCap128>(base, length);
  } else if (triple.getArch() == llvm::Triple::riscv32) {
    return FindRepresentableRangeImpl<CompressedCap64>(base, length);
  }

  throw std::runtime_error("Unsupported architecture");
}

short DwarfSource::FindRequiredPrecision(uint64_t base, uint64_t length) const {
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
                           std::shared_ptr<const DwarfSource> dwsrc)
    : sm_(sm), dwsrc_(dwsrc) {}

void DwarfScraper::Extract(std::stop_token stop_tok) {
  auto &dictx = dwsrc_->GetContext();

  auto timing = stats_.Timing("elapsed_time");
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
    BeginUnit(unit_die);
    try {
      /* Iterate over DIEs in the unit */
      llvm::DWARFDie child_die = unit_die.getFirstChild();
      bool stop = false;
      while (child_die && !stop && !stop_tok.stop_requested()) {
        // stop = impl::VisitDispatch(*this, child_die);
        stop = this->DoVisit(child_die);
        child_die = child_die.getSibling();
      }
    } catch (std::exception &ex) {
      LOG(kError) << "Failed to scan compilation unit "
                  << unit_die.getName(llvm::DINameKind::LinkageName)
                  << " reason: " << ex.what();
      stats_.errors.push_back(ex.what());
    }
    EndUnit(unit_die);
  }
}

ScrapeResult DwarfScraper::Result() {
  ScrapeResult r(stats_);
  r.source = dwsrc_->GetPath();

  return r;
}

void DwarfScraper::GetTypeInfo(const llvm::DWARFDie &die, TypeInfo &info) {
  llvm::raw_string_ostream type_name_stream(info.type_name);
  llvm::dumpTypeUnqualifiedName(die, type_name_stream);

  /* Go through the DIE chain */
  std::vector<llvm::DWARFDie> chain;
  llvm::DWARFDie next = die;
  while (next) {
    chain.push_back(next);
    next = next.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
               .resolveTypeUnitReference();
  }

  if (chain.size() == 0) {
    LOG(kError) << "Failed to resolve type for member "
                << GetStrAttr(die, dwarf::DW_AT_name).value_or("<anonymous>");
    throw std::runtime_error("Could not resolve type");
  }

  for (auto iter_die = chain.rbegin(); iter_die != chain.rend(); ++iter_die) {
    switch (iter_die->getTag()) {
    case dwarf::DW_TAG_base_type: {
      auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        LOG(kError) << "Found DW_TAG_base_type without a size";
        throw std::runtime_error("Base type without a size");
      }
      info.byte_size = *size;
      info.type_die = *iter_die;
      break;
    }
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
    case dwarf::DW_TAG_union_type: {
      using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;
      info.decl_file = iter_die->getDeclFile(FLIKind::AbsoluteFilePath);
      if (strip_prefix_) {
        info.decl_file = fs::relative(*info.decl_file, *strip_prefix_);
      }
      info.decl_line = iter_die->getDeclLine();
      info.decl_name = GetStrAttr(*iter_die, dwarf::DW_AT_name)
                           .value_or(AnonymousName(*iter_die, strip_prefix_));

      if (iter_die->getTag() == dwarf::DW_TAG_structure_type) {
        info.flags |= TypeInfoFlags::kTypeIsStruct;
      } else if (iter_die->getTag() == dwarf::DW_TAG_class_type) {
        info.flags |= TypeInfoFlags::kTypeIsClass;
      } else {
        info.flags |= TypeInfoFlags::kTypeIsUnion;
      }
      if (iter_die->find(dwarf::DW_AT_declaration)) {
        /*
         * This only happens when we have a pointer to something,
         * it is safe to ignore the size here, as the pointer DIE
         * will set it.
         */
        break;
      }
      if (!iter_die->find(dwarf::DW_AT_name)) {
        info.flags |= TypeInfoFlags::kTypeIsAnon;
        info.type_name = AnonymousName(*iter_die, strip_prefix_);
      }
      auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        LOG(kError) << "Found aggregate type without size";
        throw std::runtime_error("Aggregate type without size");
      }
      info.byte_size = *size;
      info.type_die = *iter_die;
      break;
    }
    case dwarf::DW_TAG_enumeration_type: {
      if (iter_die->find(dwarf::DW_AT_declaration)) {
        LOG(kWarn) << "TODO support enum member type declarations";
        break;
      }
      auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        LOG(kError) << "Found enum type without a size";
        throw std::runtime_error("Enum type without a size");
      }
      info.byte_size = *size;
      info.type_die = *iter_die;

      // If the name exists, it is parsed correctly above
      // otherwise use our own pattern.
      if (!iter_die->find(dwarf::DW_AT_name)) {
        info.flags |= TypeInfoFlags::kTypeIsAnon;
        info.type_name = AnonymousName(*iter_die, strip_prefix_);
      }
      break;
    }
    case dwarf::DW_TAG_const_type:
    case dwarf::DW_TAG_volatile_type:
      break;
    case dwarf::DW_TAG_typedef:
      info.alias_name = GetStrAttr(*iter_die, dwarf::DW_AT_name);
      if (!info.alias_name) {
        LOG(kError) << "Invalid typedef, missing type name";
        throw std::runtime_error("Typedef without name");
      }
      break;
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type:
    case dwarf::DW_TAG_pointer_type: {
      info.byte_size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size)
                           .value_or(dwsrc_->GetABIPointerSize());
      if ((info.flags & TypeInfoFlags::kTypeIsFn) == TypeInfoFlags::kTypeNone)
        info.flags = TypeInfoFlags::kTypeIsPtr;
      // Reset # of array items, as this is a pointer to an array
      info.array_items = std::nullopt;
      break;
    }
    case dwarf::DW_TAG_array_type: {
      llvm::DWARFDie subrange_die =
          FindFirstChild(*iter_die, dwarf::DW_TAG_subrange_type);
      if (!subrange_die) {
        LOG(kError) << "Found DW_TAG_array_type without a subrange DIE";
        throw std::runtime_error("Array without subrange");
      }
      auto count = GetULongAttr(subrange_die, dwarf::DW_AT_count);
      auto upper_bound = GetULongAttr(subrange_die, dwarf::DW_AT_upper_bound);
      unsigned long array_items = 0;
      if (count) {
        array_items = *count;
      } else if (upper_bound) {
        array_items = *upper_bound + 1;
      }
      info.array_items = array_items;
      info.byte_size = info.byte_size * array_items;
      info.flags = TypeInfoFlags::kTypeIsArray;
      break;
    }
    case dwarf::DW_TAG_subroutine_type: {
      info.flags |= TypeInfoFlags::kTypeIsFn;
      info.type_die = *iter_die;
      break;
    }
    default:
      auto tag_name = dwarf::TagString(iter_die->getTag());
      LOG(kError) << "Unhandled DIE " << tag_name.str();
      throw std::runtime_error("Unhandled DIE");
    }
  }
}

} /* namespace cheri */
