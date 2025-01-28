#include <algorithm>
#include <cassert>

#include "flat_layout_scraper.hh"
#include "log.hh"

namespace fs = std::filesystem;
namespace dwarf = llvm::dwarf;

namespace {

using namespace cheri;

/**
 * Generate unique name for anonymous types.
 */
std::string anonymousName(const llvm::DWARFDie &die) {
  return std::format("<anon@{:#x}>", die.getOffset());
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
  auto decl = desc.decl.value();

  name = desc.name;
  size = desc.byte_size;
  file = decl.file;
  line = decl.line;
  if (decl.kind == DeclKind::Struct)
    kind = LayoutKind::Struct;
  else if (decl.kind == DeclKind::Class)
    kind = LayoutKind::Class;
  else if (decl.kind == DeclKind::Union)
    kind = LayoutKind::Union;
}

TypeDecl::TypeDecl(const llvm::DWARFDie &die) : type_die(die), line(0) {
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
            "id INTEGER PRIMARY KEY,"
            // File where the struct is defined
            "file TEXT NOT NULL,"
            // Line where the struct is defined
            "line INTEGER NOT NULL,"
            // Name of the type.
            // If this is anonymous, a synthetic name is created.
            "name TEXT NOT NULL,"
            // Size of the strucutre including any padding
            "size INTEGER NOT NULL,"
            // Whether the type is a struct, union or not
            "is_union INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_union >= 0 AND is_union <= 1),"
            "UNIQUE(name, file, line, size))");

  sm_.query("CREATE TABLE IF NOT EXISTS layout_member ("
            "id INTEGER PRIMARY KEY,"
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

  for (auto i = layouts_.begin(); i != layouts_.end(); i++) {
    std::unique_ptr<FlattenedLayout> layout;
    std::swap(i->second, layout);
    recordLayout(std::move(layout));
  }

  layouts_.clear();
}

/*
 * Note that we discard top-level record types that don't have a name
 * this is because they must be nested things, otherwise they are invalid C
 * so we already cover them with the nested mechanism.
 * Otherwise, they are covered by visit_typedef().
 */
bool FlatLayoutScraper::visit_structure_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    auto layout = visitCommon(die);
    if (layout) {
      layout.value()->kind = LayoutKind::Struct;
    }
  }
  return false;
}

/* See FlatLayoutScraper::visit_structure_type */
bool FlatLayoutScraper::visit_class_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    auto layout = visitCommon(die);
    if (layout) {
      layout.value()->kind = LayoutKind::Class;
    }
  }
  return false;
}

/* See FlatLayoutScraper::visit_structure_type */
bool FlatLayoutScraper::visit_union_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    auto layout = visitCommon(die);
    if (layout) {
      layout.value()->kind = LayoutKind::Union;
    }
  }
  return false;
}

bool FlatLayoutScraper::visit_typedef(llvm::DWARFDie &die) {
  auto type_die = die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                      .resolveTypeUnitReference();
  if (!type_die) {
    // Forward declaration
    return false;
  }

  auto typedef_name = getStrAttr(die, dwarf::DW_AT_name);
  if (!typedef_name) {
    qCritical() << "Invalid typedef, missing type name";
    throw ScraperError("Typedef without name");
  }

  /*
   * We only care about typedefs of an aggregate type definition, not typedefs
   * of type form `typedef const struct foo bar;`.
   * We skip typedefs if the immediate type DIE is not a struct, union or class.
   */
  std::optional<FlattenedLayout *> layout;
  switch (type_die.getTag()) {
  case dwarf::DW_TAG_structure_type:
    layout = visitCommon(type_die, typedef_name);
    if (layout) {
      layout.value()->kind = LayoutKind::Struct;
    }
    break;
  case dwarf::DW_TAG_union_type:
    layout = visitCommon(type_die, typedef_name);
    if (layout) {
      layout.value()->kind = LayoutKind::Union;
    }
    break;
  case dwarf::DW_TAG_class_type:
    layout = visitCommon(type_die, typedef_name);
    if (layout) {
      layout.value()->kind = LayoutKind::Class;
    }
    break;
  default:
    // Not interested
    return false;
  }
  return false;
}

/**
 * Scan basic information about a given DW_TAG_*_type DIE.
 */
TypeDesc FlatLayoutScraper::resolveTypeDie(const llvm::DWARFDie &die) {
  assert(die.isValid() && "Invalid DIE");
  TypeDesc desc(die);

  // Resolve the type name in a readable form.
  llvm::raw_string_ostream type_name_stream(desc.name);
  llvm::dumpTypeUnqualifiedName(die, type_name_stream);

  bool has_typedef = false;
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
                      .value_or(source().getABIPointerSize());
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
      decl.file =
          normalizePath(iter_die.getDeclFile(FLIKind::AbsoluteFilePath));
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
      // Signal that the declaration will not be anonymous.
      has_typedef = true;
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
  /*
   * If this is an anonymous definition, dumpTypeUnqualified name will be of
   * the form "struct ". Therefore, we generate a unique name based on the
   * location of the typedef.
   */
  if (desc.decl && !desc.decl->name && !has_typedef) {
    desc.name += anonymousName(desc.decl->type_die);
    desc.is_anonymous = true;
  }
  return desc;
}

std::optional<FlattenedLayout *>
FlatLayoutScraper::visitCommon(const llvm::DWARFDie &die,
                               std::optional<std::string> typedef_name) {
  // Skip declarations, we don't care.
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
  TypeDesc td = resolveTypeDie(die);
  // If a top-level layout is anonymous, we need to be given a name for it.
  if (td.is_anonymous) {
    if (!typedef_name) {
      qWarning() << "Top level anonymous structure without typedef @ "
                 << die.getOffset();
    } else {
      td.name = *typedef_name;
    }
  }
  qDebug() << "Scanning top-level type" << td.name;

  auto layout = std::make_unique<FlattenedLayout>(td);
  if (auto search = layouts_.find(layout->id()); search != layouts_.end()) {
    // We already have the structure, no need to scan it.
    qDebug() << "Structure" << layout->name << "already present" << layout->file
             << layout->line;
    return std::nullopt;
  }

  // Flatten the description of the type we found.
  long member_index = 0;
  for (auto &child : die.children()) {
    if (child.getTag() == dwarf::DW_TAG_member) {
      visitNested(child, layout.get(), td.name, member_index++, /*offset=*/0);
    }
  }

  auto [pos, inserted] =
      layouts_.emplace(std::make_pair(layout->id(), std::move(layout)));
  assert(inserted && "Could not insert duplicate layout");

  return (*pos).second.get();
}

/*
 * Recursively scan through a structure member and attach member
 * data to the layout.
 */
void FlatLayoutScraper::visitNested(const llvm::DWARFDie &die,
                                    FlattenedLayout *layout, std::string prefix,
                                    long mindex, unsigned long parent_offset) {
  /* Skip declarations, we don't care. */
  if (die.find(dwarf::DW_AT_declaration)) {
    return;
  }

  if (die.find(dwarf::DW_AT_specification)) {
    qCritical() << "Unsupported DW_AT_specification";
    throw ScraperError("Not implemented");
  }

  LayoutMember m;
  auto member_type_die = die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                             .resolveTypeUnitReference();

  std::string member_name;
  if (auto tag_name = getStrAttr(die, dwarf::DW_AT_name)) {
    member_name = tag_name.value();
  } else {
    // Anonymous member, use member index to generate unique name
    member_name = std::format("_anon{}", mindex);
  }
  m.name = std::format("{}::{}", prefix, member_name);

  // Relay type information into the member
  TypeDesc member_desc = resolveTypeDie(member_type_die);
  m.type_name = member_desc.name;
  m.byte_size = member_desc.byte_size;
  auto tag_bit_size = getULongAttr(die, dwarf::DW_AT_bit_size).value_or(0);
  if (tag_bit_size > std::numeric_limits<decltype(m.bit_size)>::max()) {
    qCritical() << "Found DW_AT_bit_size overflowing uint8_t";
    throw ScraperError("Not implemented");
  }
  m.bit_size = static_cast<decltype(m.bit_size)>(tag_bit_size);

  m.byte_offset = parent_offset;
  auto tag_offset = getULongAttr(die, dwarf::DW_AT_data_member_location);
  auto tag_bit_offset = getULongAttr(die, dwarf::DW_AT_data_bit_offset);
  // Old-style bit offset deprecated in DWARF v5.
  auto tag_old_bit_offset = getULongAttr(die, dwarf::DW_AT_bit_offset);

  if (tag_bit_offset && tag_old_bit_offset) {
    qCritical()
        << "Can not have both DW_AT_bit_offset and DW_AT_data_bit_offset";
    throw ScraperError("Invalid member DIE");
  }

  unsigned long bit_offset = 0;
  if (tag_old_bit_offset) {
    if (source().getContext().isLittleEndian()) {
      auto shift = *tag_old_bit_offset + m.bit_size;
      bit_offset = m.byte_size * 8 - shift;
    } else {
      bit_offset = *tag_old_bit_offset;
    }
  } else if (tag_bit_offset) {
    bit_offset = *tag_bit_offset;
  }
  m.bit_offset = bit_offset % 8;
  m.byte_offset += tag_offset.value_or(0) + bit_offset / 8;
  m.array_items = member_desc.array_count;

  if (member_desc.pointer) {
    m.is_pointer = true;
    if (*member_desc.pointer == PointerKind::Function) {
      m.is_function = true;
    }
  }

  // Determine bounds
  uint64_t rlen = m.byte_size + (m.bit_size ? 1 : 0);
  auto [base, length] = source().findRepresentableRange(m.byte_offset, rlen);
  m.base = base;
  m.top = base + length;
  m.required_precision = source().findRequiredPrecision(m.byte_offset, rlen);
  m.is_imprecise = m.byte_offset != m.base || length != rlen;

  qDebug() << "Traversed member"
           << std::format("+{:#x}:{} {} {} [{}, {}]", m.byte_offset,
                          m.bit_offset, m.type_name, m.name, m.base, m.top);

  if (member_desc.decl) {
    auto decl = *member_desc.decl;
    if (decl.kind == DeclKind::Union) {
      m.is_union = true;
    }
    // set is_anon
    layout->members.push_back(m);

    if (decl.kind != DeclKind::Enum) {
      // Descend into the member
      long member_index = 0;
      prefix += "::" + member_name;
      for (auto &child : decl.type_die.children()) {
        if (child.getTag() == dwarf::DW_TAG_member) {
          visitNested(child, layout, prefix, member_index++, m.byte_offset);
        }
      }
    }
  } else {
    layout->members.push_back(m);
  }
}

void FlatLayoutScraper::recordLayout(std::unique_ptr<FlattenedLayout> layout) {
  sm_.transaction([&](StorageManager &sm) {
    qDebug() << "Transaction for" << layout->name;

    // clang-format off
    auto insert_layout = sm.prepare(
        "INSERT INTO type_layout (name, file, line, size, is_union) "
        "VALUES (:name, :file, :line, :size, :is_union) "
        "ON CONFLICT DO NOTHING RETURNING id");

    auto fetch_layout = sm.prepare(
        "SELECT id FROM type_layout WHERE "
        "name = :name AND file = :file AND line = :line AND size = :size");

    auto insert_member = sm.prepare(
        "INSERT INTO layout_member ("
        "owner, name, type_name, byte_offset, bit_offset, "
        "byte_size, bit_size, array_items, "
        "base, top, required_precision, "
        "is_pointer, is_function, is_anon, is_union, is_imprecise "
        ") VALUES ("
        ":owner, :name, :type_name, :byte_offset, :bit_offset, "
        ":byte_size, :bit_size, :array_items, "
        ":base, :top, :required_precision, "
        ":is_pointer, :is_function, :is_anon, :is_union, :is_imprecise"
        ") ON CONFLICT DO NOTHING RETURNING id");
    // clang-format on

    insert_layout.bindValue(":name", QString::fromStdString(layout->name));
    insert_layout.bindValue(":file", QString::fromStdString(layout->file));
    insert_layout.bindValue(":line", layout->line);
    insert_layout.bindValue(":size", layout->size);
    if (layout->kind == LayoutKind::Union) {
      insert_layout.bindValue(":is_union", 1ULL);
    } else {
      insert_layout.bindValue(":is_union", 0ULL);
    }
    if (!insert_layout.exec()) {
      // Failed, abort the transaction
      qCritical() << "Failed to insert layout:" << insert_layout.lastQuery();
      throw DBError(insert_layout.lastError());
    }
    QVariant layout_id;
    if (!insert_layout.first()) {
      fetch_layout.bindValue(":name", QString::fromStdString(layout->name));
      fetch_layout.bindValue(":file", QString::fromStdString(layout->file));
      fetch_layout.bindValue(":line", layout->line);
      fetch_layout.bindValue(":size", layout->size);
      if (!fetch_layout.exec()) {
        qCritical() << "Failed to fetch layout ID:"
                    << insert_layout.lastQuery();
        throw DBError(insert_layout.lastError());
      }
      if (!fetch_layout.first()) {
        qCritical() << "Layout for existing structure could not be found";
        throw ScraperError("Unexpected missing type_layout");
      }
      layout_id = fetch_layout.value(0);
      fetch_layout.finish();
    } else {
      layout_id = insert_layout.value(0);
    }
    insert_layout.finish();

    for (auto &m : layout->members) {
      insert_member.bindValue(":owner", layout_id);
      insert_member.bindValue(":name", QString::fromStdString(m.name));
      insert_member.bindValue(":type_name",
                              QString::fromStdString(m.type_name));
      insert_member.bindValue(":byte_offset", m.byte_offset);
      insert_member.bindValue(":bit_offset", m.bit_offset);
      insert_member.bindValue(":byte_size", m.byte_size);
      insert_member.bindValue(":bit_size", m.bit_size);
      if (m.array_items) {
        insert_member.bindValue(":array_items", *m.array_items);
      } else {
        insert_member.bindValue(":array_items", QVariant::fromValue(nullptr));
      }
      insert_member.bindValue(":base", static_cast<unsigned long long>(m.base));
      insert_member.bindValue(":top", static_cast<unsigned long long>(m.top));
      insert_member.bindValue(":required_precision", m.required_precision);
      insert_member.bindValue(":is_pointer", m.is_pointer);
      insert_member.bindValue(":is_function", m.is_function);
      insert_member.bindValue(":is_anon", m.is_anon);
      insert_member.bindValue(":is_union", m.is_union);
      insert_member.bindValue(":is_imprecise", m.is_imprecise);
      if (!insert_member.exec()) {
        // Failed, abort the transaction
        qCritical() << "Failed to insert layout member:"
                    << insert_member.lastQuery();
        throw DBError(insert_member.lastError());
      }
      insert_member.finish();
    }

    qDebug() << "Transaction for" << layout->name << "Done";
  });
}

} /* namespace cheri */
