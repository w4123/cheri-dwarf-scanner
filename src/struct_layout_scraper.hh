
#pragma once

#include <concepts>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include <llvm/ADT/Hashing.h>

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
      : id(0), line(0), size(0), flags(StructTypeFlags::kTypeNone),
        has_imprecise(false) {}
  uint64_t id;
  std::string file;
  unsigned long line;
  std::string name;
  std::optional<std::string> alias_name;
  uint64_t size;
  StructTypeFlags flags;
  bool has_imprecise;
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

  /**
   * Helper to check whether the member type is an anonymous
   * struct, union or class.
   */
  bool HasAnonRecordType() const {
    if (!(flags & TypeInfoFlags::kTypeIsAnon))
      return false;
    return HasRecordType();
  }

  /**
   * Helper to check whether the member type is a record type.
   */
  bool HasRecordType() const {
    if (!(flags & TypeInfoFlags::kTypeIsStruct) &&
        !(flags & TypeInfoFlags::kTypeIsClass) &&
        !(flags & TypeInfoFlags::kTypeIsUnion))
      return false;
    return true;
  }

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
 * This represents the flattened layout of a strucutre.
 */
struct MemberBoundsRow {
  uint64_t id;
  uint64_t owner;
  uint64_t member;
  uint64_t mindex;
  std::string name;
  uint64_t offset;
  uint64_t base;
  uint64_t top;
  bool is_imprecise;
  short required_precision;
};

std::ostream &operator<<(std::ostream &os, const MemberBoundsRow &row);

/**
 * Entry in the layout scraper temporary storage.
 */
struct StructTypeEntry {
  StructTypeEntry() : skip_postprocess(false), die_offset(-1) {}

  // Flag this entry as already exising in the DB
  bool skip_postprocess;
  // Offset of the related type DIE
  uint64_t die_offset;
  // Structure entry data
  StructTypeRow data;
  // Direct member information
  std::vector<StructMemberRow> members;
  // Flattened layout entries
  std::vector<MemberBoundsRow> flattened_layout;
};
using SourceLoc = std::tuple<std::string, size_t>;
using SourceEntrySet = std::vector<std::shared_ptr<StructTypeEntry>>;

struct SourceLocHash {
  std::size_t operator()(const SourceLoc &k) const noexcept {
    std::size_t h0 = std::hash<std::string>{}(std::get<0>(k));
    std::size_t h1 = std::hash<std::size_t>{}(std::get<1>(k));

    return llvm::hash_combine(h0, h1);
  }
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
   * Get globally unique ID for the struct_type table
   */
  uint64_t GetStructTypeId();

  /**
   * Get globally unique ID for the struct_member table
   */
  uint64_t GetStructMemberId();

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
   * Compute sub-object member capabilities for all nested members of
   * a given type. The type is assumed to have been fully evaluated
   * and we have DB entries for all nested members.
   */
  void FindSubobjectCapabilities(StructTypeEntry &entry);

  /**
   * Insert a new struct layout into the layouts table.
   * Returns true if a new row was inserted.
   */
  bool InsertStructLayout(StructTypeRow &row);

  /**
   * Insert a new struct member into the members table.
   * Returns true if a new row was inserted.
   */
  void InsertStructMember(StructMemberRow &row);

  /**
   * Insert a new record in the member_bounds table.
   */
  void InsertMemberBounds(const MemberBoundsRow &row);

  /**
   * Compilation unit currently being scanned
   */
  std::string unit_name_;

  /**
   * Structure descriptor entries.
   * Vector containing the StructTypeEntry descriptors for the current
   * compilation unit.
   */
  std::vector<std::shared_ptr<StructTypeEntry>> record_descriptors_;

  /**
   * Index for the structure descriptor entries.
   * Associate a (file, line) tuple to a set of entries defined at
   * that source location.
   */
  std::unordered_map<SourceLoc, SourceEntrySet, SourceLocHash> source_map_;

  /**
   * Index for the structure descriptor entries.
   * Map local structure type IDs to StructTypeEntry objects.
   */
  std::unordered_map<uint64_t, std::shared_ptr<StructTypeEntry>> entry_by_id_;

  /*
   * Pre-compiled queries for InsertStructLayout.
   */
  std::unique_ptr<SqlQuery> insert_struct_query_;
  std::unique_ptr<SqlQuery> select_struct_query_;
  std::unique_ptr<SqlQuery> set_has_imprecise_query_;

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
