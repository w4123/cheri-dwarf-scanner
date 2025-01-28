#pragma once

#include <chrono>
#include <concepts>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>

#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/Binary.h>

#include <QDebug>

#include "storage.hh"

namespace cheri {

class DwarfScraper;

namespace impl {

/**
 * Internal method used to dispatch a DIE to the corresponding visitor method
 */
template <typename S>
bool visitDispatch(S &scraper, llvm::DWARFDie &die)
  requires std::is_base_of_v<DwarfScraper, S>
{

#define HANDLE_DW_TAG(ID, NAME, VERSION, VENDOR, KIND)                         \
  case ID: {                                                                   \
    constexpr bool has_visit = requires(S &s_ref, llvm::DWARFDie &die_ref) {   \
      { s_ref.visit_##NAME(die_ref) } -> std::same_as<bool>;                   \
    };                                                                         \
    if constexpr (has_visit)                                                   \
      return scraper.visit_##NAME(die);                                        \
    break;                                                                     \
  }

  switch (die.getTag()) {
#include <llvm/BinaryFormat/Dwarf.def>
  }
#undef HANDLE_DW_TAG
  return false;
}

} /* namespace impl */

/**
 * Scraper execution result.
 */
struct ScraperResult {
  ScraperResult() : dup_structs(0), dup_members(0) {}
  virtual ~ScraperResult() = default;

  // TimingScope Timing(std::string_view name);

  std::filesystem::path source;
  // std::unordered_map<std::string, TimingInfo> profile;
  std::vector<std::string> errors;

  unsigned long dup_structs;
  unsigned long dup_members;
};

QDebug operator<<(QDebug dbg, const ScraperResult &sr);

struct ScraperError : std::runtime_error {
  template <typename T>
  ScraperError(llvm::Expected<T> &exp)
      : std::runtime_error(llvm::toString(exp.takeError())) {}

  template <typename T>
  ScraperError(const std::string &prefix, llvm::Expected<T> &exp)
      : std::runtime_error(prefix + " " + llvm::toString(exp.takeError())) {}

  ScraperError(const std::string &msg) : std::runtime_error(msg) {}
};

std::optional<unsigned long> getULongAttr(const llvm::DWARFDie &die,
                                          llvm::dwarf::Attribute attr);
std::optional<std::string> getStrAttr(const llvm::DWARFDie &die,
                                      llvm::dwarf::Attribute attr);

/**
 * A shared DWARF object, possibly between multiple scrapers.
 */
class DwarfSource {
public:
  DwarfSource(std::filesystem::path path);

  std::filesystem::path getPath() const;
  llvm::DWARFContext &getContext() const;
  int getABIPointerSize() const;
  int getABICapabilitySize() const;
  std::pair<uint64_t, uint64_t> findRepresentableRange(uint64_t base,
                                                       uint64_t length) const;
  /**
   * This should produce the mantissa size required to precisely
   * represent a (base, length) pair in the Cheri compressed capability
   * format.
   * For example, a capability with base=0x0, top=0x100000 requires 1 bit
   * of precision (considering the exponent as a separate field).
   * A capability with base=0x04, top=0x1004 requires 11 bits of precision,
   * with the exponent = 2, leaving a range of 11 bits between msb and lsb
   * of the cursor range.
   *
   * Note that this says nothing about the number of bits in the mantissa or
   * the encoding, it just estimates the number of bits of information that
   * the capability format requires. In particular, the mantissa width will
   * be larger because we require a non-dereferenceable representable region
   * around the capability.
   *
   * Place it here because it may be arch-specific.
   */
  short findRequiredPrecision(uint64_t base, uint64_t length) const;

private:
  std::filesystem::path path_;
  std::unique_ptr<llvm::DWARFContext> dictx_;
  llvm::object::OwningBinary<llvm::object::Binary> owned_binary_;
};

/**
 * Main scraper interface.
 * Different scrapers collect set of information.
 *
 * This is designed to work within a thread. Parse dwarf information within an
 * object file and extract a specific set of information in the given storage.
 */
class DwarfScraper {
public:
  DwarfScraper(StorageManager &sm, std::unique_ptr<const DwarfSource> dwsrc);
  DwarfScraper(const DwarfScraper &other) = delete;

  /**
   * Public name of the scraper
   */
  virtual std::string name() { return "unknown"; }

  /**
   * Data source for the scraper
   */
  const DwarfSource &source() { return *dwsrc_; }

  /**
   * Main data extraction loop.
   */
  void run(std::stop_token stop_tok);

  /**
   * Produce a summary for the extraction process.
   */
  ScraperResult result();

  /**
   * Set prefix path to strip from file names before committing to storage.
   */
  void setStripPrefix(std::optional<std::string> prefix) {
    strip_prefix_ = prefix;
  }

  /**
   * Hook to initialize the storage schema.
   * This should be called when the scraper is initialized. The schema may
   * already exist.
   */
  virtual void initSchema() = 0;

protected:
  /* Subclasses must implement this to properly invoke VisitDispatch */
  virtual bool doVisit(llvm::DWARFDie &die) = 0;

  /**
   * Hook that is called when beginning to scan a new compilation unit.
   */
  virtual void beginUnit(llvm::DWARFDie &unit_die) = 0;
  virtual void endUnit(llvm::DWARFDie &unit_die) = 0;

  /**
   * Given an absolute path from the DWARF information, apply
   * transformations to normalize it for the database.
   */
  std::filesystem::path normalizePath(std::filesystem::path path);

  /**
   * Reference to the shared storage manager that provides per-thread
   * database connections.
   */
  StorageManager &sm_;
  /**
   * Owned dwarf source that abstracts access to an ELF file
   * containing DWARF information.
   */
  std::unique_ptr<const DwarfSource> dwsrc_;
  /**
   * Path prefix to strip from any file path we emit to storage.
   */
  std::optional<std::filesystem::path> strip_prefix_;

  /* Statistics */
  ScraperResult stats_;
};

} /* namespace cheri */
