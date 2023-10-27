#include <format>
#include <mutex>
#include <stdexcept>

#include <llvm/DebugInfo/DIContext.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/TargetSelect.h>

#include "log.hh"
#include "scraper.hh"

namespace cheri {

namespace fs = std::filesystem;
namespace object = llvm::object;

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

} /* namespace cheri */
