#include <format>
#include <mutex>
#include <stdexcept>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>

#include "log.hh"
#include "scraper.hh"

namespace fs = std::filesystem;
namespace object = llvm::object;
namespace dwarf = llvm::dwarf;

namespace cheri {

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

std::string AnonymousName(const llvm::DWARFDie &die) {
  using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;
  std::string file = die.getDeclFile(FLIKind::AbsoluteFilePath);
  unsigned long line = die.getDeclLine();
  return std::format("<anon>@{}+{:d}", file, line);
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
  if (obj == nullptr) {
    return -1;
  }
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
  if (obj == nullptr) {
    return -1;
  }
  auto triple = obj->makeTriple();

  if (triple.getArch() == llvm::Triple::aarch64 ||
      triple.getArch() == llvm::Triple::riscv64) {
    return 16;
  } else if (triple.getArch() == llvm::Triple::riscv32) {
    return 8;
  }
  throw std::runtime_error("Unsupported architecture");
}

DwarfScraper::DwarfScraper(StorageManager &sm,
                           std::shared_ptr<const DwarfSource> dwsrc)
    : sm_{sm}, dwsrc_{dwsrc} {}

void DwarfScraper::Extract(std::stop_token stop_tok) {
  auto &dictx = dwsrc_->GetContext();

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
    }
    EndUnit(unit_die);
  }
}

ScrapeResult DwarfScraper::Result() {
  ScrapeResult r;
  r.source = dwsrc_->GetPath();
  r.processed_entries = processed_entries_;
  r.elapsed_time = elapsed_time_;

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
    LOG(kError) << "Failed to resolve type for member " <<
        GetStrAttr(die, dwarf::DW_AT_name).value_or("<anonymous>");
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
        break;
      }
      case dwarf::DW_TAG_structure_type:
      case dwarf::DW_TAG_class_type:
      case dwarf::DW_TAG_union_type: {
        using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;
        info.decl_file = iter_die->getDeclFile(FLIKind::AbsoluteFilePath);
        info.decl_line = iter_die->getDeclLine();
        info.decl_name = GetStrAttr(*iter_die, dwarf::DW_AT_name)
                         .value_or(AnonymousName(*iter_die));

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
        auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
        if (!size) {
          LOG(kError) << "Found aggregate type without size";
          throw std::runtime_error("Aggregate type without size");
        }
        info.byte_size = *size;
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
        break;
      }
      case dwarf::DW_TAG_const_type:
      case dwarf::DW_TAG_volatile_type:
      case dwarf::DW_TAG_typedef: {
        // auto name = GetStrAttr(curr_die, dwarf::DW_AT_name);
        // if (!name) {
        //   LOG(kError) << "Found DW_TAG_typedef without a name";
        //   throw std::runtime_error("Typedef without a name");
        // }
        // // XXX should add an alias table
        // // Reset the naming to reflect the typedef.
        // info.type_name = *name;
        break;
      }
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
        llvm::DWARFDie subrange_die = FindFirstChild(*iter_die, dwarf::DW_TAG_subrange_type);
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
