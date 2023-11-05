#pragma once

#include <chrono>
#include <concepts>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

#include <llvm/DebugInfo/DWARF/DWARFContext.h>
#include <llvm/Object/Binary.h>

#include "bit_flag_enum.hh"
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
 * Helper to extract an unsigned long DIE attribute
 */
std::optional<unsigned long> GetULongAttr(const llvm::DWARFDie &die,
                                          llvm::dwarf::Attribute attr);
/**
 * Helper to extract a string DIE attribute
 */
std::optional<std::string> GetStrAttr(const llvm::DWARFDie &die,
                                      llvm::dwarf::Attribute attr);

/**
 * Helper to find the first child DIE with a given tag.
 */
llvm::DWARFDie FindFirstChild(const llvm::DWARFDie &die, llvm::dwarf::Tag tag);

/**
 * Helper to build an unique anonymous name for a DIE.
 * This is used to construct anonymous names of record types.
 */
std::string AnonymousName(const llvm::DWARFDie &die);

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
 * Flags used in the StructMembers.flags table field
 * These flags signal type modifiers and kind.
 */
enum class TypeInfoFlags {
  kTypeNone = 0,
  kTypeIsPtr = 1,
  kTypeIsFn = 1 << 1,
  kTypeIsArray = 1 << 2,
  kTypeIsDecl = 1 << 3,
  kTypeIsStruct = 1 << 4,
  kTypeIsUnion = 1 << 5,
  kTypeIsClass = 1 << 6
};

template<>
struct EnumTraits<TypeInfoFlags> {
  static constexpr bool is_bitflag = true;
};

/**
 * Common type information data.
 * This can be extracted from a DIE with the DW_AT_type attribute.
 */
struct TypeInfo {
  TypeInfo() : byte_size(0), flags(TypeInfoFlags::kTypeNone) {}

  std::string type_name;
  unsigned long byte_size;
  TypeInfoFlags flags;
  std::optional<unsigned long> array_items;
  std::optional<std::string> decl_name;
  std::optional<std::string> decl_file;
  std::optional<unsigned long> decl_line;
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
  /* Helper to resolve a type definition given a Die with a DW_AT_type attribute */
  void GetTypeInfo(const llvm::DWARFDie &die, TypeInfo &info);

  TypeInfo GetTypeInfo(const llvm::DWARFDie &die) {
    TypeInfo info;

    GetTypeInfo(die, info);
    return info;
  }


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
