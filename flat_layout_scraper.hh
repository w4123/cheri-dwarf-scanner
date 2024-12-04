
#pragma once

#include <cstdint>
#include <variant>

#include "scraper.hh"

namespace cheri {

struct TypeDesc;

enum class LayoutKind {
  Struct = 1,
  Class = 2,
  Union = 3,
};

/**
 * In-memory representation of a member in a flattened layout
 */
struct LayoutMember {
  // Fully qualified flattened member name
  std::string name;
  // Name of the member type
  std::string type_name;
  // Size, in bytes, of the member
  unsigned long byte_size;
  // Fractional bit remainder of the size
  uint8_t bit_size;
  // Offset from the start of the layout
  unsigned long byte_offset;
  // Fractional bit remainder of the size
  uint8_t bit_offset;
  // Number of array items, if the field is an array
  std::optional<unsigned long> array_items;
  // Flags used to mark member properties
  bool is_pointer : 1;
  bool is_function : 1;
  bool is_array : 1;
  bool is_anon : 1;
  bool is_union : 1;
  bool is_imprecise : 1;
  // Capability base for this sub-object
  uint64_t base;
  // Capability top for this sub-object
  uint64_t top;
  // Number of precision bits required to represent this field exactly
  short required_precision;
};

/**
 * In-memory representation of a flattened structure layout
 */
struct FlattenedLayout {
  FlattenedLayout() : line(0), size(0), die_offset(0) {}
  FlattenedLayout(const TypeDesc &desc);

  // Source file where the structure is defined
  std::string file;
  // Line where the structure is defined
  unsigned long line;
  // Name of the structure, without considering any typedef
  std::string name;
  // Alias (typedef) names for the structure
  std::optional<std::string> alias_name;
  // Total size of the structure
  uint64_t size;
  // Differentiate struct, union and class types.
  LayoutKind kind;

  // Offset of the related type DIE
  uint64_t die_offset;
  // Members as part of the flattened layout
  // Direct member information
  std::vector<LayoutMember> members;
};

/**
 * Indicates whether a TypeDesc is a pointer.
 */
enum class PointerKind {
  Pointer = 1,
  Reference = 2,
  Function = 3,
};

/**
 * Indicates the type represented by a TypeDecl.
 */
enum class DeclKind {
  Struct = 1,
  Class = 2,
  Union = 3,
  Enum = 4,
};

/**
 * Internal description of a struct, union, class or enum declaration.
 */
struct TypeDecl {
  TypeDecl(const llvm::DWARFDie &die);

  DeclKind kind;
  std::optional<std::string> name;
  std::string file;
  unsigned long line;
};

/**
 * Internal type description helper
 */
struct TypeDesc {
  TypeDesc(const llvm::DWARFDie &die)
      : die(die), is_const(false), is_volatile(false), byte_size(0) {}

  llvm::DWARFDie die;
  // Base type information
  std::string name;
  bool is_const;
  bool is_volatile;
  std::optional<PointerKind> pointer;
  std::optional<uint64_t> array_count;
  uint64_t byte_size;
  // Compound type definition information
  std::optional<TypeDecl> decl;
};

/**
 * Scraper to extract flattened structure layout information from DWARF.
 *
 * This creates a simplified schema with only two tables.
 * - The 'struct_types' table contains a record for each structure found,
 *   uniquely identified by the tuple (name, file, line, size).
 * - The 'members' table contains the flattened layout for all structures.
 *   Each member is associated with one and only one entry in the 'types' table.
 */
class FlatLayoutScraper : public DwarfScraper {
public:
  FlatLayoutScraper(StorageManager &sm,
                    std::unique_ptr<const DwarfSource> dwsrc)
      : DwarfScraper(sm, std::move(dwsrc)) {}

  std::string name() override { return "flat-layout"; }

  bool visit_structure_type(llvm::DWARFDie &die);
  bool visit_class_type(llvm::DWARFDie &die);
  bool visit_union_type(llvm::DWARFDie &die);
  bool visit_typedef(llvm::DWARFDie &die);

protected:
  void initSchema() override;
  void beginUnit(llvm::DWARFDie &unit_die) override;
  void endUnit(llvm::DWARFDie &unit_die) override;
  bool doVisit(llvm::DWARFDie &die) override {
    return impl::visitDispatch(*this, die);
  }

  /**
   * Common visitor logic for all aggregate types.
   * Returns a reference to the in-memory flattend layout.
   */
  std::optional<FlattenedLayout> visitCommon(const llvm::DWARFDie &die);



  /**
   * Visit a structure/union/class member DIE
   */
  // StructMemberRow visitMember(const llvm::DWARFDie &die,
  //                             const StructTypeRow &row, int member_index);

  /**
   * Compute sub-object member capabilities for all nested members of
   * a given type. The type is assumed to have been fully evaluated
   * and we have DB entries for all nested members.
   */
  // void findSubobjectCapabilities(StructTypeEntry &entry);

  /**
   * Insert a new struct layout into the layouts table.
   * Returns true if a new row was inserted.
   */
  // bool insertStructLayout(StructTypeRow &row);

  /**
   * Insert a new struct member into the members table.
   * Returns true if a new row was inserted.
   */
  // void insertStructMember(StructMemberRow &row);

  /**
   * Insert a new record in the member_bounds table.
   */
  // void InsertMemberBounds(const MemberBoundsRow &row);

  /**
   * Compilation unit currently being scanned
   */
  std::string current_unit_;

  /**
   * Structure descriptor entries.
   * Vector containing the StructTypeEntry descriptors for the current
   * compilation unit.
   */
  // std::vector<std::shared_ptr<StructTypeEntry>> record_descriptors_;

  /**
   * Index for the structure descriptor entries.
   * Associate a (file, line) tuple to a set of entries defined at
   * that source location.
   */
  // std::unordered_map<SourceLoc, SourceEntrySet, SourceLocHash> source_map_;

  /**
   * Index for the structure descriptor entries.
   * Map local structure type IDs to StructTypeEntry objects.
   */
  // std::unordered_map<uint64_t, std::shared_ptr<StructTypeEntry>>
  // entry_by_id_;
};

} /* namespace cheri */
