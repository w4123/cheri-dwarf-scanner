#pragma once

#include <chrono>
#include <concepts>
#include <filesystem>
#include <memory>
#include <thread>
#include <type_traits>

#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/Binary.h>

#include "storage.hh"

namespace cheri {

class DwarfScraper;

namespace impl {

/*
 * Internal method used to dispatch a DIE to the corresponding visitor method
 */
template <typename S>
bool VisitDispatch(S &scraper, llvm::DWARFDie &die)
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
 * A shared DWARF object, possibly between multiple scrapers.
 */
class DwarfSource {
public:
  DwarfSource(std::filesystem::path path);

  std::filesystem::path GetPath() const;
  llvm::DWARFContext &GetContext() const;
  int GetABIPointerSize() const;
  int GetABICapabilitySize() const;

private:
  std::filesystem::path path_;
  std::unique_ptr<llvm::DWARFContext> dictx_;
  llvm::object::OwningBinary<llvm::object::Binary> owned_binary_;
};

/**
 * Scraper execution result.
 *
 * The result should contain statistics and metadata that can be used to
 * query the StorageManager.
 * This should not be used to pass heavy data, use the StorageManager instead.
 */
struct ScrapeResult {
  ScrapeResult() = default;
  virtual ~ScrapeResult() = default;

  std::filesystem::path source;
  unsigned long processed_entries;
  std::chrono::milliseconds elapsed_time;
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
  DwarfScraper(StorageManager &sm, std::shared_ptr<const DwarfSource> dwsrc);
  DwarfScraper(const DwarfScraper &other) = delete;
  virtual ~DwarfScraper() = default;

  /**
   * Main data extraction loop.
   */
  void Extract(std::stop_token stop_tok);

  /**
   * Produce a summary for the extraction process.
   */
  ScrapeResult Result();

  /**
   * Hook to initialize the storage schema.
   * This should be called when the scraper is initialized. The schema may
   * already exist.
   */
  virtual void InitSchema() = 0;

protected:
  /* Subclasses must implement this to properly invoke VisitDispatch */
  virtual bool DoVisit(llvm::DWARFDie &die) = 0;

  /**
   * Hook that is called when beginning to scan a new compilation unit.
   */
  virtual void BeginUnit(llvm::DWARFDie &unit_die) = 0;
  virtual void EndUnit(llvm::DWARFDie &unit_die) = 0;

  StorageManager &sm_;
  std::shared_ptr<const DwarfSource> dwsrc_;

  /* Statistics */
  unsigned long processed_entries_;
  std::chrono::milliseconds elapsed_time_;
};

} /* namespace cheri */
