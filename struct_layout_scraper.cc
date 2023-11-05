#include <format>
#include <iomanip>

#include "log.hh"
#include "struct_layout_scraper.hh"

namespace dwarf = llvm::dwarf;

/* Shorthand for the kind of file-line spec we want */
using FileLineInfoKind = llvm::DILineInfoSpecifier::FileLineInfoKind;

namespace {

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
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS struct_type ("
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
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS struct_member ("
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
              "FOREIGN KEY (owner) REFERENCES struct_type (id),"
              "FOREIGN KEY (nested) REFERENCES struct_type (id),"
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
  row.c_name = row.name;

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
  if (member.owner == 0) {
    LOG(kError) << "Can not visit member of " << std::quoted(row.c_name) <<
        " with invalid owner ID";
    throw std::runtime_error("Invalid member owner ID");
  }


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
  const auto record_type_mask = TypeInfoFlags::kTypeIsStruct |
                                TypeInfoFlags::kTypeIsUnion |
                                TypeInfoFlags::kTypeIsClass;
  /* Returned ID for the nested type, if any */
  std::optional<uint64_t> nested_type_id = std::nullopt;

  TypeInfo member_type = GetTypeInfo(die);

  member.type_name = member_type.type_name;
  member.byte_size = member_type.byte_size;
  member.flags = member_type.flags;
  member.array_items = member_type.array_items;

  /*
   * In this case, we want to reference the nested aggregate type,
   * if this does not exist yet, we must create a placeholder for it
   * in the database (this reserves the id essentially).
   */
  if ((member.flags & record_type_mask) != TypeInfoFlags::kTypeNone) {
    StructTypeRow member_type_row;
    member_type_row.name = member_type.decl_name.value();
    member_type_row.size = member_type.byte_size;
    member_type_row.file = member_type.decl_file.value();
    member_type_row.line = member_type.decl_line.value();
    member.nested = GetStructOrPlaceholder(member_type_row);
    nested_type_id = member.nested;
  }
  return nested_type_id;
}

uint64_t StructLayoutScraper::GetStructOrPlaceholder(StructTypeRow &row) {
  std::optional<int64_t> id;
  std::string q = std::format(
      "INSERT OR IGNORE INTO struct_type (file, line, name, size) "
      "VALUES('{}', {}, '{}', {})",
      row.file, row.line, row.name, row.size);
  std::string s = std::format(
      "SELECT id FROM struct_type WHERE file='{}' AND line={} "
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
      "INSERT INTO struct_type (file, line, name, c_name, size, flags) "
      "VALUES('{0}', {1}, '{2}', '{3}', {4}, {5}) "
      "ON CONFLICT(file, line, name, size) DO UPDATE SET "
      "c_name = '{3}', flags = {5} RETURNING id",
      row.file, row.line, row.name, row.c_name, row.size,
      static_cast<int>(row.flags));

  sm_.SqlExec(q, [&new_entry, &row](SqlRowView result) {
    row.id = *result.ExtractAt<decltype(row.id)>("id");
    LOG(kDebug) << "Inserted layout with ID=" << row.id;
    new_entry = true;
    return false;
  });
  return new_entry;
}

bool StructLayoutScraper::InsertStructMember(StructMemberRow &row) {
  LOG(kDebug) << "Insert layout member " << row.name <<
      " of type " << row.type_name;
  bool new_entry = false;
  std::string q = std::format(
      "INSERT INTO struct_member (owner, nested, name, type_name, line, size, "
      "bit_size, offset, bit_offset, flags, array_items) "
      "VALUES({0}, {1}, '{2}', '{3}', {4}, {5}, {6}, {7}, {8}, {9}, {10}) "
      "ON CONFLICT(owner, name, offset) DO NOTHING RETURNING id",
      row.owner, row.nested ? std::to_string(*row.nested) : "NULL",
      row.name, row.type_name, row.line, row.byte_size,
      row.bit_size.value_or(0), row.byte_offset,
      row.bit_offset.value_or(0), static_cast<int>(row.flags),
      row.array_items.value_or(0));

  sm_.SqlExec(q, [&new_entry, &row](SqlRowView result) {
    row.id = *result.ExtractAt<decltype(row.id)>("id");
    LOG(kDebug) << "Inserted member with ID=" << row.id;
    new_entry = true;
    return false;
  });
  return new_entry;
}

} /* namespace cheri */
