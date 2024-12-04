#include <algorithm>
#include <cassert>

#include "flat_layout_scraper.hh"
#include "log.hh"

namespace fs = std::filesystem;
namespace dwarf = llvm::dwarf;

namespace {

using namespace cheri;

/**
 * Scan basic information about a given DW_TAG_*_type DIE.
 */
TypeDesc resolveTypeDie(const DwarfSource &dwsrc, const llvm::DWARFDie &die) {
  TypeDesc desc(die);

  // Resolve the type name in a readable form.
  llvm::raw_string_ostream type_name_stream(desc.name);
  llvm::dumpTypeUnqualifiedName(die, type_name_stream);

  llvm::DWARFDie iter_die = die;
  std::optional<uint64_t> byte_size;
  uint64_t size_multiplier = 1;
  while (iter_die) {
    switch (iter_die.getTag()) {
    case dwarf::DW_TAG_const_type:
      desc.is_const = true;
      break;
    case dwarf::DW_TAG_volatile_type:
      desc.is_volatile = true;
      break;
    case dwarf::DW_TAG_base_type: {
      auto size = getULongAttr(iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        qCritical() << "Found DW_TAG_base_type without a size";
        throw ScraperError("Base type without a size");
      }
      if (!byte_size)
        byte_size = *size;
      break;
    }
    case dwarf::DW_TAG_reference_type:
    case dwarf::DW_TAG_rvalue_reference_type:
      desc.pointer = PointerKind::Reference;
      /* fallthrough */
    case dwarf::DW_TAG_pointer_type: {
      auto size = getULongAttr(iter_die, dwarf::DW_AT_byte_size)
                      .value_or(dwsrc.getABIPointerSize());
      if (!byte_size)
        byte_size = size;
      if (!desc.pointer)
        desc.pointer = PointerKind::Pointer;
      break;
    }
    case dwarf::DW_TAG_subroutine_type: {
      desc.pointer = PointerKind::Function;
      break;
    }
    case dwarf::DW_TAG_array_type: {
      // Find the first child DIE with TAG subrange_type;
      // this contains the number of items in the array.
      if (iter_die.find(dwarf::DW_AT_byte_stride) ||
          iter_die.find(dwarf::DW_AT_bit_stride)) {
        qCritical()
            << "Unsupported attributes DW_AT_{byte, bit}_stride on arrays";
        throw ScraperError("Not implemented");
      }
      if (iter_die.find(dwarf::DW_AT_bit_size)) {
        qCritical() << "Unsupported attribute DW_AT_bit_size on arrays";
        throw ScraperError("Not implemented");
      }

      // If this is a pointer type to something, we don't care about
      // the array information because it is erased by the pointerness.
      assert(!desc.pointer &&
             "Unexpectedly reached DW_TAG_array_type with a pointer");

      // Every subrange die describes an array dimension.
      // Note that only the last one may be without a size in C.
      std::optional<uint64_t> array_count;
      for (auto &child : iter_die) {
        if (child.getTag() != dwarf::DW_TAG_subrange_type) {
          break;
        }
        auto count = getULongAttr(child, dwarf::DW_AT_count);
        auto upper_bound = getULongAttr(child, dwarf::DW_AT_upper_bound);
        if (count) {
          array_count = array_count.value_or(1) * (*count);
        } else if (upper_bound) {
          array_count = array_count.value_or(1) * (*upper_bound + 1);
        } else {
          // No size given, assume 0
          array_count = 0;
        }
      }

      if (!array_count) {
        qCritical() << "Found DW_TAG_array_type without a subrange DIE";
        throw ScraperError("Array without subrange");
      }
      desc.array_count = array_count;

      auto arr_bytes = getULongAttr(iter_die, dwarf::DW_AT_byte_size);
      if (arr_bytes && !byte_size) {
        // Override type size calculation as we already know the size
        byte_size = *arr_bytes;
      } else {
        size_multiplier = array_count.value();
      }
      break;
    }
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
    case dwarf::DW_TAG_enumeration_type:
    case dwarf::DW_TAG_union_type: {
      using FLIKind = llvm::DILineInfoSpecifier::FileLineInfoKind;
      TypeDecl decl(iter_die);
      decl.file = iter_die.getDeclFile(FLIKind::AbsoluteFilePath);
      decl.line = iter_die.getDeclLine();
      decl.name = getStrAttr(iter_die, dwarf::DW_AT_name);
      desc.decl = decl;

      /*
       * This only happens when we have a pointer to something,
       * it is safe to ignore the size here, as the pointer DIE
       * will set it.
       * Note that in this case this should not be reached.
       */
      assert(!iter_die.find(dwarf::DW_AT_declaration));

      auto size = getULongAttr(iter_die, dwarf::DW_AT_byte_size);
      if (!size) {
        auto tag_name = dwarf::TagString(iter_die.getTag());
        qCritical() << "Found aggregate type without size at DIE"
                    << tag_name.str();
        throw ScraperError("Aggregate type without size");
      }
      if (!byte_size)
        byte_size = *size;
      break;
    }
    case dwarf::DW_TAG_typedef:
      // Pass-through the typedef, we don't care.
      break;
    default:
      auto tag_name = dwarf::TagString(iter_die.getTag());
      qCritical() << "Unhandled DIE " << tag_name.str();
      throw ScraperError("Unhandled DIE");
    }

    // If we reached a pointer type, stop scanning.
    // We don't care about the rest, as the pointer forces the size to be
    // a pointer size.
    if (desc.pointer) {
      break;
    }

    iter_die = iter_die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                   .resolveTypeUnitReference();
  }

  if (!byte_size) {
    qCritical() << "Failed to resolve type description for DIE @"
                << die.getOffset();
    throw ScraperError("Failed to resolve type description");
  }

  desc.byte_size = byte_size.value() * size_multiplier;
  return desc;
}

}; // namespace

namespace cheri {

/*
 * Initialize a layout entry from a type description.
 */
FlattenedLayout::FlattenedLayout(const TypeDesc &desc) : FlattenedLayout() {
  // Expect this to be a compound type descriptor.
  if (!desc.decl) {
    qCritical() << "Invalid TypeDesc for " << desc.name
                << " is not a compound type";
    throw ScraperError("Invalid TypeDesc");
  }

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
}


TypeDecl::TypeDecl(const llvm::DWARFDie &die) : line(0) {
  if (die.getTag() == dwarf::DW_TAG_structure_type)
    kind = DeclKind::Struct;
  else if (die.getTag() == dwarf::DW_TAG_union_type)
    kind = DeclKind::Union;
  else if (die.getTag() == dwarf::DW_TAG_class_type)
    kind = DeclKind::Class;
  else if (die.getTag() == dwarf::DW_TAG_enumeration_type)
    kind = DeclKind::Enum;
  else
    throw ScraperError("Invalid DIE for TypeDecl");
}

void FlatLayoutScraper::initSchema() {
  // clang-format off
  /* Attempt to initialize tables */
  sm_.query("CREATE TABLE IF NOT EXISTS type_layout ("
            "id INTEGER NOT NULL PRIMARY KEY,"
            // File where the struct is defined
            "file TEXT NOT NULL,"
            // Line where the struct is defined
            "line INTEGER NOT NULL,"
            // Name of the type.
            // If this is anonymous, a synthetic name is created.
            "name TEXT NOT NULL,"
            // Size of the strucutre including any padding
            "size INTEGER NOT NULL,"
            "UNIQUE(name, file, line, size))");

  sm_.query("CREATE TABLE IF NOT EXISTS layout_member ("
            "id INTEGER NOT NULL PRIMARY KEY,"
            // FK for the corresponding type_layout
            "owner INTEGER NOT NULL,"
            // Fields match the LayoutMember structure
            "name TEXT NOT NULL,"
            "type_name TEXT NOT NULL,"
            "byte_size INTEGER NOT NULL,"
            "bit_size INTEGER DEFAULT 0 NOT NULL,"
            "byte_offset INTEGER NOT NULL,"
            "bit_offset INTEGER DEFAULT 0 NOT NULL,"
            "array_items INTEGER,"
            "base INTEGER,"
            "top INTEGER,"
            "required_precision INTEGER,"
            "is_pointer INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_pointer >= 0 AND is_pointer <= 1),"
            "is_function INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_function >= 0 AND is_function <= 1),"
            "is_array INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_array >= 0 AND is_array <= 1),"
            "is_anon INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_anon >= 0 AND is_anon <= 1),"
            "is_union INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_union >= 0 AND is_union <= 1),"
            "is_imprecise INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_imprecise >= 0 AND is_imprecise <= 1),"
            "FOREIGN KEY (owner) REFERENCES type_layout (id),"
            "UNIQUE(owner, name, byte_offset, bit_offset))");
  // clang-format on
}

void FlatLayoutScraper::beginUnit(llvm::DWARFDie &unit_die) {
  auto at_name = unit_die.find(dwarf::DW_AT_name);
  if (at_name) {
    llvm::Expected name = at_name->getAsCString();
    if (name) {
      current_unit_ = *name;
    } else {
      qCritical() << "Invalid compilation unit, can't extract AT_name";
      throw ScraperError("Invalid compilation unit:", name);
    }
  } else {
    qCritical() << "Invalid compliation unit, missing AT_name";
    throw ScraperError("Invalid compliation unit");
  }
  qDebug() << "Enter compilation unit" << current_unit_;
}

void FlatLayoutScraper::endUnit(llvm::DWARFDie &unit_die) {
  qDebug() << "Done compilation unit" << current_unit_;
}

/*
 * Note that we discard top-level record types that don't have a name
 * this is because they must be nested things, otherwise they are invalid C
 * so we already cover them with the nested mechanism.
 * Otherwise, they are covered by visit_typedef().
 */
bool FlatLayoutScraper::visit_structure_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    visitCommon(die);
    // layout.kind = LayoutKind::Struct;
  }
  return false;
}

/* See FlatLayoutScraper::visit_structure_type */
bool FlatLayoutScraper::visit_class_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    visitCommon(die);
    // layout.kind = LayoutKind::Class;
  }
  return false;
}

/* See FlatLayoutScraper::visit_structure_type */
bool FlatLayoutScraper::visit_union_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    visitCommon(die);
    // layout.kind = LayoutKind::Union;
  }
  return false;
}

bool FlatLayoutScraper::visit_typedef(llvm::DWARFDie &die) { return false; }

std::optional<FlattenedLayout>
FlatLayoutScraper::visitCommon(const llvm::DWARFDie &die) {
  /* Skip declarations, we don't care. */
  if (die.find(dwarf::DW_AT_declaration)) {
    return std::nullopt;
  }

  // Fail if we find a specification, this is not supported.
  // XXX do I need to findRecursively()?
  if (die.find(dwarf::DW_AT_specification)) {
    qCritical() << "DW_AT_specification unsupported";
    throw ScraperError("Not implemented");
  }

  // Parse the top-level layout.
  TypeDesc td = resolveTypeDie(source(), die);
  qInfo() << "Scanning top-level type" << td.name;

  auto layout = std::make_unique<FlattenedLayout>(td);
  // Flatten the description of the type we found.
  for (auto &child : die.children()) {
    
  }

  // auto flp = flatten(die);
  return std::nullopt;
}

} /* namespace cheri */
