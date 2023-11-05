
#pragma once

#include <concepts>
#include <optional>
#include <string>

#include "scraper.hh"

namespace cheri {

/**
 * Flags used in the StructTypes.flags table field
 * These signal the type of the aggregate and other
 * boolean metadata.
 */
enum StructTypeFlags {
  kTypeNone = 0,
  kTypeIsAnonymous = 1,
  kTypeIsStruct = 1 << 1,
  kTypeIsUnion = 1 << 2,
  kTypeIsClass = 1 << 3
};

template<>
struct EnumTraits<StructTypeFlags> {
  static constexpr bool is_bitflag = true;
};

/**
 * Helper to hold the data for a row in the StructTypes table.
 */
struct StructTypeRow {
  StructTypeRow()
      : id(0), line(0), size(0), flags(kTypeNone) {}
  uint64_t id;
  std::string file;
  unsigned long line;
  std::string name;
  std::string c_name;
  uint64_t size;
  StructTypeFlags flags;
  // std::set<std::string> aliases;
};

/**
 * Helper to hold the data for a row in the StructMembers table
 */
struct StructMemberRow {
  StructMemberRow()
      : id(0), owner(0), line(0), byte_size(0),
        byte_offset(0), flags(TypeInfoFlags::kTypeNone) {}
  uint64_t id;
  uint64_t owner;
  std::optional<uint64_t> nested;
  std::string name;
  // TypeInfo type_info;
  std::string type_name;
  unsigned long line;
  unsigned long byte_size;
  std::optional<unsigned char> bit_size;
  unsigned long byte_offset;
  std::optional<unsigned char> bit_offset;
  TypeInfoFlags flags;
  std::optional<unsigned long> array_items;
};

/**
 * Specialized scraper that extract DWARF structure layout information.
 *
 * The structure layouts are nested tree-like objects, where the top-level
 * structure definition is the root of the tree.
 *
 * The scraper initialized the storage with the following schema:
 * - The Types table records all types we have seen, if a Type is aggregate
 * (i.e. struct, union, class), there will be a one-to-many relationship
 * with the Members table.
 * - The Members table records structure members. These are associated to
 * one and only one entry in Types table, which is the containing object.
 */
class StructLayoutScraper : public DwarfScraper {
public:
  StructLayoutScraper(StorageManager &sm,
                      std::shared_ptr<const DwarfSource> dwsrc)
      : DwarfScraper(sm, dwsrc) {}

  bool visit_structure_type(llvm::DWARFDie &die);
  bool visit_class_type(llvm::DWARFDie &die);
  bool visit_union_type(llvm::DWARFDie &die);
  bool visit_typedef(llvm::DWARFDie &die);

protected:
  void InitSchema() override;
  void BeginUnit(llvm::DWARFDie &unit_die) override;
  void EndUnit(llvm::DWARFDie &unit_die) override;
  bool DoVisit(llvm::DWARFDie &die) override {
    return impl::VisitDispatch(*this, die);
  }
  /**
   * Common visitor logic for all aggregate types.
   */
  bool VisitCommon(const llvm::DWARFDie &die, StructTypeFlags kind);

  /**
   * Visit a structure/union/class member DIE
   */
  void VisitMember(const llvm::DWARFDie &die, const StructTypeRow &row);

  /**
   * Visit the DIE chain for a structure member type.
   */
  std::optional<uint64_t> VisitMemberType(const llvm::DWARFDie &die,
                                          StructMemberRow &member);

  /**
   * insert a new struct layout into the layouts table.
   * Returns true if a new row was inserted.
   */
  bool InsertStructLayout(StructTypeRow &row);

  /**
   * Find a matching struct layout given a possibly incomplete row.
   * This creates a placeholder entry if the struct is not present yet.
   * The input row specifier must have at least the (name, file, line, size)
   * fields.
   */
  uint64_t GetStructOrPlaceholder(StructTypeRow &row);

  /**
   * insert a new struct member into the members table.
   * Returns true if a new row was inserted.
   */
  bool InsertStructMember(StructMemberRow &row);

  /**
   * Cache type information by DIE offset in the dwarf section.
   * This is safe as long as the scraper is used to extract information
   * from a single file.
   */
  // std::map<uint64_t, > info_cache_;
};

} /* namespace cheri */
