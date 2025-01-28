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
