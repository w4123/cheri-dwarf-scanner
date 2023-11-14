
#pragma once

#include <concepts>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "scraper.hh"

namespace cheri {

/**
 * Flags used in the StructTypes.flags table field
 * These signal the type of the aggregate and other
 * boolean metadata.
 */
enum class StructTypeFlags {
  kTypeNone = 0,
  kTypeIsAnonymous = 1,
  kTypeIsStruct = 1 << 1,
  kTypeIsUnion = 1 << 2,
  kTypeIsClass = 1 << 3
};

template <> struct EnumTraits<StructTypeFlags> {
  static constexpr bool is_bitflag = true;
};

/**
 * Helper to hold the data for a row in the struct_types table.
 */
struct StructTypeRow {
  /**
   * Construct the type row from an SQL result view.
   */
  static StructTypeRow FromSql(SqlRowView view);

  StructTypeRow()
      : id(0), line(0), size(0), flags(StructTypeFlags::kTypeNone) {}
  uint64_t id;
  std::string file;
  unsigned long line;
  std::string name;
  uint64_t size;
  StructTypeFlags flags;
};

/**
 * Helper to hold the data for a row in the struct_members table.
 */
struct StructMemberRow {
  /**
   * Construct the member row from an SQL result view.
   */
  static StructMemberRow FromSql(SqlRowView view);

  StructMemberRow()
      : id(0), owner(0), line(0), byte_size(0), byte_offset(0),
        flags(TypeInfoFlags::kTypeNone) {}
  uint64_t id;
  uint64_t owner;
  std::optional<uint64_t> nested;
  std::string name;
  std::string type_name;
  unsigned long line;
  unsigned long byte_size;
  std::optional<unsigned char> bit_size;
  unsigned long byte_offset;
  std::optional<unsigned char> bit_offset;
  TypeInfoFlags flags;
  std::optional<unsigned long> array_items;
};

std::ostream &operator<<(std::ostream &os, const StructMemberRow &row);

/**
 * Helper to hold the data for a row in the member_bounds table.
 */
struct MemberBoundsRow {
  uint64_t id;
  uint64_t owner;
  uint64_t member;
  std::string name;
  uint64_t offset;
  uint64_t base;
  uint64_t top;
  short required_precision;
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
   * Returns the ID of the struct_type entry corresponding to the DIE.
   * If processing failed for some reason, the ID may not be valid.
   */
  std::optional<int64_t> VisitCommon(const llvm::DWARFDie &die,
                                     StructTypeFlags kind);

  /**
   * Visit a structure/union/class member DIE
   */
  StructMemberRow VisitMember(const llvm::DWARFDie &die,
                              const StructTypeRow &row, int member_index);

  /**
   * Visit the DIE chain for a structure member type.
   */
  std::optional<uint64_t> VisitMemberType(const llvm::DWARFDie &die,
                                          StructMemberRow &member);

  /**
   * Compute sub-object member capabilities for all nested members of
   * a given type. The type is assumed to have been fully evaluated
   * and we have DB entries for all nested members.
   */
  void FindSubobjectCapabilities(int64_t struct_type_id);

  /**
   * Insert a new struct layout into the layouts table.
   * Returns true if a new row was inserted.
   */
  bool InsertStructLayout(const llvm::DWARFDie &die, StructTypeRow &row);

  /**
   * Insert a new struct member into the members table.
   * Returns true if a new row was inserted.
   */
  void InsertStructMembers(std::vector<StructMemberRow> &rows);

  /**
   * Insert a new record in the member_bounds table.
   */
  void InsertMemberBounds(const MemberBoundsRow &row);

  /**
   * Cache DIE offsets to struct_type indices.
   */
  std::unordered_map<uint64_t, int64_t> struct_type_cache_;

  /*
   * Pre-compiled queries for InsertStructLayout.
   */
  std::unique_ptr<SqlQuery> insert_struct_query_;
  std::unique_ptr<SqlQuery> select_struct_query_;

  /*
   * Pre-compiled queries for InsertStructMember.
   */
  std::unique_ptr<SqlQuery> insert_member_query_;

  /*
   * Pre-compiled queries for FindSubobjectCapabilities.
   */
  std::unique_ptr<SqlQuery> insert_member_bounds_query_;
  std::unique_ptr<SqlQuery> find_imprecise_alias_query_;
};

} /* namespace cheri */
