#include <format>

#include "log.hh"
#include "struct_layout_scraper.hh"

namespace dwarf = llvm::dwarf;

/* Shorthand for the kind of file-line spec we want */
using FileLineInfoKind = llvm::DILineInfoSpecifier::FileLineInfoKind;

namespace {

/**
 * Helper to extract an unsigned long DIE attribute
 */
std::optional<unsigned long> GetULongAttr(const llvm::DWARFDie &die,
                                          dwarf::Attribute attr) {
  if (auto opt = dwarf::toUnsigned(die.find(attr))) {
    return *opt;
  }
  return std::nullopt;
}

std::optional<std::string> GetStrAttr(const llvm::DWARFDie &die,
                                      dwarf::Attribute attr) {
  if (auto opt = dwarf::toString(die.find(attr))) {
    return *opt;
  }
  return std::nullopt;
}

std::string AnonymousName(const llvm::DWARFDie &die) {
  std::string file = die.getDeclFile(FileLineInfoKind::AbsoluteFilePath);
  unsigned long line = die.getDeclLine();
  return std::format("<anon>@{}+{:d}", file, line);
}

llvm::DWARFDie FindFirstChild(const llvm::DWARFDie &die, dwarf::Tag tag) {
  for (auto &child : die) {
    if (child.getTag() == tag)
      return child;
  }
  return llvm::DWARFDie();
}

} // namespace

namespace cheri {

/**
 * Initialize the storage schema.
 */
void StructLayoutScraper::InitSchema() {
  LOG(kDebug) << "Initialize StructLayout scraper database";
  // sm_.EnsureTable(&struct_types);
  /*
   * Structure, unions and classes are collected here.
   * Note that we consider two struct to be the same if:
   * 1. Have the same name
   * 2. Have the same size
   * 3. Are defined in the same file, at the same line.
   */
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS StructTypes ("
              "id INTEGER NOT NULL PRIMARY KEY,"
              // File where the struct is defined
              "file text NOT NULL,"
              // Line where the struct is defined
              "line int NOT NULL,"
              // Name of the type (without any struct/union/class prefix)
              // If this is anonymous, a synthetic name is created.
              "name text,"
              // Full name of the type, including struct/union/class prefix
              "c_name text,"
              // Size of the strucutre including any padding
              "size int NOT NULL,"
              // Flags that determine whether this is a struct/union/class
              "flags int DEFAULT 0 NOT NULL,"
              "UNIQUE(name, file, line, size))");

  // sm_.EnsureTable(&struct_members);
  /*
   * Expresses the composition between struct types and
   * their memebrs.
   * There is a one-to-many relationship between StructTypes
   * and StructMembers.
   * If the member is an aggregate type (e.g. another struct),
   * it is associated to the corresponding sturcture in the StructTypes.
   * This forms another many-to-one relationship between the tables,
   * as for each member there is a single associated structure but a
   * structure may be associated to many members.
   */
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS StructMembers ("
              "id INTEGER NOT NULL PRIMARY KEY,"
              // Index of the owning structure
              "owner int NOT NULL,"
              // Optional index of the nested structure
              "nested int,"
              // Member name, anonymous members have synthetic names
              "name text NOT NULL,"
              // Type name of the member, for nested structures, this is the same as
              // c_name
              "type_name text NOT NULL,"
              // Line in the file where the member is defined
              "line int NOT NULL,"
              // Size (bytes) of the member, this may or may not include internal
              // padding
              "size int NOT NULL,"
              // Bit remainder of the size, only valid for bitfields
              "bit_size int,"
              // Offset (bytes) of the member with respect to the owner
              "offset int NOT NULL,"
              // Bit remainder of the offset, only valid for bitfields
              "bit_offset int,"
              // Type flags
              "flags int DEFAULT 0 NOT NULL,"
              "array_items int,"
              "FOREIGN KEY (owner) REFERENCES StructTypes (id),"
              "FOREIGN KEY (nested) REFERENCES StructTypes (id),"
              "UNIQUE(owner, name, offset))");
}

bool StructLayoutScraper::visit_structure_type(llvm::DWARFDie &die) {
  return VisitCommon(die, kTypeIsStruct);
}

bool StructLayoutScraper::visit_class_type(llvm::DWARFDie &die) {
  return VisitCommon(die, kTypeIsClass);
}

bool StructLayoutScraper::visit_union_type(llvm::DWARFDie &die) {
  return VisitCommon(die, kTypeIsUnion);
}

bool StructLayoutScraper::visit_typedef(llvm::DWARFDie &die) { return false; }

void StructLayoutScraper::BeginUnit(llvm::DWARFDie &unit_die) {
  auto at_name = unit_die.find(dwarf::DW_AT_name);
  std::string unit_name;
  if (at_name) {
    llvm::Expected name_or_err = at_name->getAsCString();
    if (name_or_err) {
      unit_name = *name_or_err;
    } else {
      LOG(kError) << "Invalid compilation unit, can't extract AT_name";
      throw std::runtime_error("Invalid compliation unit");
    }
  } else {
    LOG(kError) << "Invalid compliation unit, missing AT_name";
    throw std::runtime_error("Invalid compliation unit");
  }
  LOG(kDebug) << "Enter compilation unit " << unit_name;
}

void StructLayoutScraper::EndUnit(llvm::DWARFDie &unit_die) {}

bool StructLayoutScraper::VisitCommon(const llvm::DWARFDie &die,
                                      StructTypeFlags kind) {
  /* Skip declarations, we don't care. */
  if (die.find(dwarf::DW_AT_declaration)) {
    return false;
  }
  // Fail if we find a specification, we need to handle this case with
  // findRecursively()
  if (die.find(dwarf::DW_AT_specification)) {
    LOG(kError) << "DW_AT_specification unsupported";
    throw std::runtime_error("Unsupported");
  }

  StructTypeRow row;
  row.flags |= kind;

  /*
   * Need to extract the following in order to determine whether this is a
   * duplicate: (Name, File, Line, Size)
   */
  std::string prefix;
  if (kind == kTypeIsStruct)
    prefix = "struct ";
  else if (kind == kTypeIsUnion)
    prefix = "union ";

  auto opt_size = dwarf::toUnsigned(die.find(dwarf::DW_AT_byte_size));
  if (!opt_size) {
    LOG(kWarn) << "Missing struct size for DIE @ 0x" << std::hex
               << die.getOffset();
    return false;
  }

  row.size = *opt_size;
  row.file = die.getDeclFile(FileLineInfoKind::AbsoluteFilePath);
  row.line = die.getDeclLine();

  auto name = GetStrAttr(die, dwarf::DW_AT_name);
  if (name) {
    row.name = *name;
  } else {
    row.name = AnonymousName(die);
    row.flags |= kTypeIsAnonymous;
  }
  row.c_name = prefix + row.name;

  if (InsertStructLayout(row)) {
    /* Not a duplicate, we should go through the members */
    for (auto &child : die.children()) {
      if (child.getTag() == dwarf::DW_TAG_member) {
        VisitMember(child, row);
      }
    }
  }

  return false;
}

void StructLayoutScraper::VisitMember(const llvm::DWARFDie &die,
                                      const StructTypeRow &row) {
  StructMemberRow member;
  member.line = die.getDeclLine();
  member.owner = row.id;

  auto member_type_die = die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                             .resolveTypeUnitReference();
  /*
   * This is expected to set the following fields:
   * - type_name
   * - array_items
   * - flags
   * - byte_size
   * It will return the ID of the nested structure type, if this is
   * a nested union/struct/class.
   */
  VisitMemberType(member_type_die, member);

  /* Extract offsets, taking into account bitfields */
  member.byte_size =
      dwarf::toUnsigned(die.find(dwarf::DW_AT_byte_size), member.byte_size);
  member.bit_size = GetULongAttr(die, dwarf::DW_AT_bit_size);

  auto data_offset =
      GetULongAttr(die, dwarf::DW_AT_data_member_location).value_or(0);
  auto bit_data_offset = GetULongAttr(die, dwarf::DW_AT_data_bit_offset);
  std::optional<unsigned long> bit_offset =
      (bit_data_offset) ? std::make_optional(data_offset * 8 + *bit_data_offset)
                        : std::nullopt;

  auto old_style_bit_offset = GetULongAttr(die, dwarf::DW_AT_bit_offset);
  if (old_style_bit_offset) {
    if (dwsrc_->GetContext().isLittleEndian()) {
      auto shift = *old_style_bit_offset + member.bit_size.value_or(0);
      bit_offset = bit_offset.value_or(0) + member.byte_size * 8 - shift;
    } else {
      bit_offset = bit_offset.value_or(0) + *old_style_bit_offset;
    }
  }
  member.byte_offset = (bit_offset) ? *bit_offset / 8 : data_offset;
  member.bit_offset =
      (bit_offset) ? std::make_optional(*bit_offset % 8) : std::nullopt;

  std::string name = std::format("<anon>@{:d}", member.byte_offset);
  if (member.bit_offset) {
    name += std::format(":{:d}", *member.bit_offset);
  }
  member.name = GetStrAttr(die, dwarf::DW_AT_name).value_or(name);

  InsertStructMember(member);
}

std::optional<uint64_t> StructLayoutScraper::VisitMemberType(
    const llvm::DWARFDie &die, StructMemberRow &member) {
  /* Returned ID for the nested type, if any */
  std::optional<uint64_t> nested_type_id = std::nullopt;
  /* Initialize member type to void */
  member.type_name = "void";
  member.byte_size = 0;

  /* Collect the DIEs specifying the type in a chain */
  std::vector<llvm::DWARFDie> chain;
  llvm::DWARFDie next = die;
  while (next) {
    chain.push_back(next);
    if (next.getTag() == dwarf::DW_TAG_structure_type ||
        next.getTag() == dwarf::DW_TAG_class_type ||
        next.getTag() == dwarf::DW_TAG_union_type ||
        next.getTag() == dwarf::DW_TAG_subroutine_type ||
        next.getTag() == dwarf::DW_TAG_enumeration_type ||
        next.getTag() == dwarf::DW_TAG_base_type) {
      break;
    }
    next = next.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
           .resolveTypeUnitReference();
  }

  if (chain.size() == 0) {
    LOG(kError) << "Failed to resolve type for member " <<
        GetStrAttr(die, dwarf::DW_AT_name).value_or("<anonymous>");
    throw std::runtime_error("Could not resolve type");
  }
  /* Walk the chain to evaluate the type definition */
  for (auto iter_die = chain.rbegin(); iter_die != chain.rend(); ++iter_die) {
    switch (iter_die->getTag()) {
      case dwarf::DW_TAG_base_type: {
        auto name = GetStrAttr(*iter_die, dwarf::DW_AT_name);
        if (!name) {
          LOG(kError) << "Found DW_TAG_base_type without a name";
          throw std::runtime_error("Base type without a name");
        }
        auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
        if (!size) {
          LOG(kError) << "Found DW_TAG_base_type without a size";
          throw std::runtime_error("Base type without a size");
        }
        member.type_name = *name;
        member.byte_size = *size;
        break;
      }
      case dwarf::DW_TAG_reference_type:
      case dwarf::DW_TAG_rvalue_reference_type:
      case dwarf::DW_TAG_pointer_type: {
        if (iter_die->getTag() == dwarf::DW_TAG_reference_type) {
          member.type_name += " &";
        } else if (iter_die->getTag() == dwarf::DW_TAG_rvalue_reference_type) {
          member.type_name += " &&";
        } else {
          member.type_name += " *";
        }
        member.byte_size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size).value_or(dwsrc_->GetABIPointerSize());
        // Note, reset the flags because this is now a pointer
        member.flags = kMemberIsPtr;
        // Reset the nested value as this is a pointer now.
        member.nested = std::nullopt;
        break;
      }
      case dwarf::DW_TAG_const_type: {
        member.type_name += " const";
        member.flags |= kMemberIsConst;
        break;
      }
      case dwarf::DW_TAG_volatile_type: {
        member.type_name += " volatile";
        break;
      }
      case dwarf::DW_TAG_array_type: {
        llvm::DWARFDie subrange_die = FindFirstChild(*iter_die, dwarf::DW_TAG_subrange_type);
        if (!subrange_die) {
          LOG(kError) << "Found DW_TAG_array_type without a subrange DIE";
          throw std::runtime_error("Array without subrange");
        }
        auto count = GetULongAttr(subrange_die, dwarf::DW_AT_count);
        auto upper_bound = GetULongAttr(subrange_die, dwarf::DW_AT_upper_bound);
        unsigned long array_items = 0;
        if (count) {
          array_items = *count;
        } else if (upper_bound) {
          array_items = *upper_bound + 1;
        }
        member.type_name += std::format(" [{:d}]", array_items);
        member.array_items = array_items;
        member.byte_size = member.byte_size * array_items;
        member.flags |= kMemberIsArray;
        // Reset the nested value as this is an array of structs now.
        member.nested = std::nullopt;
        break;
      }
      case dwarf::DW_TAG_subroutine_type: {
        LOG(kWarn) << "TODO implement subroutine types";
        break;
      }
      case dwarf::DW_TAG_typedef: {
        auto name = GetStrAttr(*iter_die, dwarf::DW_AT_name);
        if (!name) {
          LOG(kError) << "Found DW_TAG_typedef without a name";
          throw std::runtime_error("Typedef without a name");
        }
        // XXX should add an alias table
        member.type_name = *name;
        break;
      }
      case dwarf::DW_TAG_structure_type:
      case dwarf::DW_TAG_class_type:
      case dwarf::DW_TAG_union_type: {
        /*
         * In this case, we want to reference the nested aggregate type,
         * if this does not exist yet, we must create a placeholder for it
         * in the database (this reserves the id essentially).
         */
        auto name = GetStrAttr(*iter_die, dwarf::DW_AT_name)
                    .value_or(AnonymousName(*iter_die));
        if (iter_die->getTag() == dwarf::DW_TAG_structure_type)
          member.type_name = "struct " + name;
        else if (iter_die->getTag() == dwarf::DW_TAG_class_type)
          member.type_name = "class " + name;
        else
          member.type_name = "union " + name;
        // member.flags |= kMemberIsComposite;
        if (iter_die->find(dwarf::DW_AT_declaration)) {
          // XXX handle declarations, we need to reference a row with
          // not enough information to uniquely identify it.
          // Maybe we need to track compilation units?
          LOG(kWarn) << "TODO support member type declarations";
          break;
        }
        StructTypeRow target;
        auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
        if (!size) {
          LOG(kError) << "Found aggregate type without size";
          throw std::runtime_error("Aggregate type without size");
        }
        target.name = name;
        target.size = *size;
        target.file = iter_die->getDeclFile(FileLineInfoKind::AbsoluteFilePath);
        target.line = iter_die->getDeclLine();
        nested_type_id = GetStructOrPlaceholder(target);
        member.nested = nested_type_id;
        break;
      }
      case dwarf::DW_TAG_enumeration_type: {
        auto name = GetStrAttr(*iter_die, dwarf::DW_AT_name)
                    .value_or(AnonymousName(*iter_die));
        member.type_name = "enum " + name;

        if (iter_die->find(dwarf::DW_AT_declaration)) {
          LOG(kWarn) << "TODO support enum member type declarations";
          break;
        }
        auto size = GetULongAttr(*iter_die, dwarf::DW_AT_byte_size);
        if (!size) {
          LOG(kError) << "Found enum type without a size";
          throw std::runtime_error("Enum type without a size");
        }
        member.byte_size = *size;
        break;
      }
      default:
        auto tag_name = dwarf::TagString(iter_die->getTag());
        LOG(kError) << "Unhandled DIE " << tag_name.str();
        throw std::runtime_error("Unhandled DIE");
    }
  }

  return nested_type_id;
}

bool StructLayoutScraper::LinkMemberToStruct(StructMemberRow &member,
                                             StructTypeRow &row) {
  LOG(kDebug) << "Link member " << member.name << " to " << row.name;
  std::string q = std::format(
      "");

  sm_.Sql(q);
  return true;
}

uint64_t StructLayoutScraper::GetStructOrPlaceholder(StructTypeRow &row) {
  std::optional<int64_t> id;
  std::string q = std::format(
      "INSERT OR IGNORE INTO StructTypes (file, line, name, size) "
      "VALUES('{}', {}, '{}', {})",
      row.file, row.line, row.name, row.size);
  std::string s = std::format(
      "SELECT id FROM StructTypes WHERE file='{}' AND line={} "
      "AND name='{}' AND size={}",
      row.file, row.line, row.name, row.size);

  sm_.SqlExec(q, nullptr);
  sm_.SqlExec(s, [&id](SqlRowView row) {
    id = row.At<int64_t>("id");
    LOG(kDebug) << "StructTypes placeholder at ID=" << *id;
    return false;
  });

  if (!id) {
    LOG(kError) << "No ID returned for placeholder";
    throw std::runtime_error("Unexpected condition");
  }
  return *id;
}

bool StructLayoutScraper::InsertStructLayout(StructTypeRow &row) {
  /* XXX should escape more safely but do I care? */
  LOG(kDebug) << "Insert layout row for " << row.c_name;
  bool new_entry = false;
  std::string q = std::format(
      "INSERT INTO StructTypes (file, line, name, c_name, size, flags) "
      "VALUES('{}', {}, '{}', '{}', {}, {}) "
      "ON CONFLICT(file, line, name, size) DO NOTHING RETURNING id",
      row.file, row.line, row.name, row.c_name, row.size,
      static_cast<int>(row.flags));

  sm_.SqlExec(q, [&new_entry](SqlRowView row) {
    LOG(kDebug) << "Insert layout with ID=" << *row.ExtractAt<size_t>("id");
    new_entry = true;
    return false;
  });
  return new_entry;
}

bool StructLayoutScraper::InsertStructMember(StructMemberRow &row) {
  LOG(kDebug) << "Insert layout member for " << row.name;
  bool new_entry = false;
  std::string q = std::format(
      "INSERT INTO StructMembers (owner, name, type_name, line, size, "
      "bit_size, offset, bit_offset, flags, array_items) "
      "VALUES({}, '{}', '{}', {}, {}, {}, {}, {}, {}, {}) "
      "ON CONFLICT(owner, name, offset) DO NOTHING RETURNING id",
      row.owner, row.name, row.type_name, row.line, row.byte_size,
      row.bit_size.value_or(0), row.byte_offset,
      row.bit_offset.value_or(0), static_cast<int>(row.flags),
      row.array_items.value_or(0));

  sm_.SqlExec(q, [&new_entry](SqlRowView row) {
    LOG(kDebug) << "Insert member with ID=" << *row.ExtractAt<size_t>("id");
    new_entry = true;
    return false;
  });
  return new_entry;
}

} /* namespace cheri */
