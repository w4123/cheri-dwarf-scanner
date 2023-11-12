#include <format>
#include <iomanip>

#include "log.hh"
#include "struct_layout_scraper.hh"

namespace dwarf = llvm::dwarf;

/* Shorthand for the kind of file-line spec we want */
using FileLineInfoKind = llvm::DILineInfoSpecifier::FileLineInfoKind;

namespace cheri {

constexpr auto record_type_mask = TypeInfoFlags::kTypeIsStruct |
                                  TypeInfoFlags::kTypeIsUnion |
                                  TypeInfoFlags::kTypeIsClass;

StructTypeRow StructTypeRow::FromSql(SqlRowView view) {
  StructTypeRow row;

  view.Fetch("id", row.id);
  view.Fetch("file", row.file);
  view.Fetch("line", row.line);
  view.Fetch("name", row.name);
  view.Fetch("size", row.size);
  view.Fetch("flags", row.flags);
  return row;
}

StructMemberRow StructMemberRow::FromSql(SqlRowView view) {
  StructMemberRow row;

  view.Fetch("id", row.id);
  view.Fetch("owner", row.owner);
  view.Fetch("nested", row.nested);
  view.Fetch("name", row.name);
  view.Fetch("type_name", row.type_name);
  view.Fetch("line", row.line);
  view.Fetch("size", row.byte_size);
  view.Fetch("bit_size", row.bit_size);
  view.Fetch("offset", row.byte_offset);
  view.Fetch("bit_offset", row.bit_offset);
  view.Fetch("flags", row.flags);
  view.Fetch("array_items", row.array_items);
  return row;
}

std::ostream &operator<<(std::ostream &os, const StructMemberRow &row) {
  os << "StructMemberRow{"
     << "id=" << row.id << ", "
     << "owner=" << row.owner << ", "
     << "nested=" << (row.nested ? std::to_string(*row.nested) : "NULL") << ", "
     << "name=" << std::quoted(row.name) << ", "
     << "tname=" << std::quoted(row.type_name) << ", "
     << "line=" << row.line << ", "
     << "off=" << row.byte_offset << "/" << row.bit_offset.value_or(0) << ", "
     << "size=" << row.byte_size << "/" << row.bit_size.value_or(0) << ", "
     << "flags=0x" << std::hex << row.flags << std::dec << ", "
     << "arrcnt="
     << (row.array_items ? std::to_string(*row.array_items) : "NULL");

  return os;
}

/**
 * Initialize the storage schema.
 */
void StructLayoutScraper::InitSchema() {
  LOG(kDebug) << "Initialize StructLayout scraper database";

  // clang-format off
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
              // Name of the type.
              // If this is anonymous, a synthetic name is created.
              "name text,"
              // Size of the strucutre including any padding
              "size int NOT NULL,"
              // Flags that determine whether this is a struct/union/class
              "flags int DEFAULT 0 NOT NULL,"
              "UNIQUE(name, file, line))");

  /*
   * Pre-compiled queries for struct_type.
   */
  insert_struct_query_ = sm_.Sql(
      "INSERT INTO struct_type (file, line, name, size, flags) "
      "VALUES(@file, @line, @name, @size, @flags) "
      "ON CONFLICT DO NOTHING RETURNING id");
  select_struct_query_ = sm_.Sql(
      "SELECT * FROM struct_type WHERE file = @file AND line = @line "
      "AND name = @name");

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
              // Type name of the member, for nested structures, this is the
              // same as struct_type.name
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

  /*
   * Pre-compiled queries for struct_member
   */
  insert_member_query_ = sm_.Sql(
      "INSERT INTO struct_member ("
      "  owner, nested, name, type_name, line, size, "
      "  bit_size, offset, bit_offset, flags, array_items"
      ") VALUES("
      "  @owner, @nested, @name, @type_name, @line, @size,"
      "  @bit_size, @offset, @bit_offset, @flags, @array_items) "
      "RETURNING id");

  /*
   * Create a view to produce a flattened structure layout with all nested
   * members.
   */
  sm_.SqlExec("CREATE VIEW IF NOT EXISTS flattened_layout AS "
              "WITH RECURSIVE "
              "  flattened_layout_impl("
              "    type_id, member_id, flat_name, flat_offset, size"
              "  ) "
              "AS ("
              "  SELECT "
              "    st.id AS type_id,"
              "    sm.id AS member_id,"
              "    (st.name || '::' || sm.name) AS flat_name,"
              "    sm.offset AS flat_offset,"
              "    sm.size AS size "
              "  FROM "
              "    struct_type st JOIN struct_member sm ON st.id = sm.owner "
              "  UNION ALL "
              "  SELECT "
              "    fl.type_id AS type_id,"
              "    sm2.id AS member_id,"
              "    (fl.flat_name || '::' || sm2.name) AS flat_name,"
              "    (fl.flat_offset + sm2.offset) AS flat_offset,"
              "    sm2.size AS size "
              "  FROM "
              "    flattened_layout_impl fl "
              "      JOIN struct_member sm ON sm.id = fl.member_id "
              "      JOIN struct_member sm2 ON sm.nested = sm2.owner"
              ") SELECT * FROM flattened_layout_impl");

  /*
   * Create a table holding the representable bounds for each (nested) member
   * of a structure.
   */
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS member_bounds ("
              // Flattened name for the layout entry
              "name text PRIMARY KEY,"
              // ID of the struct_type containing this member
              "owner int NOT NULL,"
              // ID of the corresponding member entry in struct_members
              "member int NOT NULL,"
              // Cumulative offset of this member from the start of owner
              "offset int NOT NULL,"
              // Representable sub-object base
              "representable_base int NOT NULL,"
              // Representable top of the sub-object
              "representable_top int NOT NULL,"
              "FOREIGN KEY (owner) REFERENCES struct_type (id),"
              "FOREIGN KEY (member) REFERENCES struct_member (id))");

  /*
   * Create a table view that determines whether struct layout is complete
   * (does not have any nested incomplete placeholders) and
   * does not have member_bounds extracted.
   */
  sm_.SqlExec("CREATE VIEW IF NOT EXISTS can_extract_member_bounds AS "
              "WITH RECURSIVE impl(type_id, valid) AS ("
              "  SELECT"
              "    st.id AS type_id,"
              "    st.flags != 0 AS valid"
              "  FROM struct_type st"
              "    JOIN struct_member sm ON st.id = sm.owner"
              "  UNION ALL"
              "  SELECT"
              "    st.id AS type_id,"
              "    (fl.valid & (st.flags != 0)) AS valid"
              "  FROM"
              "    struct_member sm"
              "    JOIN struct_type st ON st.id = sm.owner"
              "    JOIN impl fl ON fl.type_id = sm.nested"
              ") "
              "SELECT type_id, MIN(valid) AS valid FROM impl "
              "WHERE NOT EXISTS("
              "  SELECT * FROM member_bounds"
              "    WHERE owner = impl.type_id"
              ") GROUP BY type_id");
  // clang-format on
}

bool StructLayoutScraper::visit_structure_type(llvm::DWARFDie &die) {
  VisitCommon(die, StructTypeFlags::kTypeIsStruct);
  return false;
}

bool StructLayoutScraper::visit_class_type(llvm::DWARFDie &die) {
  VisitCommon(die, StructTypeFlags::kTypeIsClass);
  return false;
}

bool StructLayoutScraper::visit_union_type(llvm::DWARFDie &die) {
  VisitCommon(die, StructTypeFlags::kTypeIsUnion);
  return false;
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

void StructLayoutScraper::EndUnit(llvm::DWARFDie &unit_die) {
  // std::string q = "SELECT * FROM can_extract_member_bounds";

  // sm_.SqlExec(q, [this](SqlRowView result) {
  //   if (result.FetchAs<bool>("valid")) {
  //     int64_t id = result.FetchAs<int64_t>("type_id");
  //     LOG(kDebug) << "Trigger inspect sub-objects for " << id;
  //     FindSubobjectCapabilities(id);
  //   }
  //   return false;
  // });
}

std::optional<int64_t>
StructLayoutScraper::VisitCommon(const llvm::DWARFDie &die,
                                 StructTypeFlags kind) {
  /* Skip declarations, we don't care. */
  if (die.find(dwarf::DW_AT_declaration)) {
    return std::nullopt;
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
  auto opt_size = GetULongAttr(die, dwarf::DW_AT_byte_size);
  if (!opt_size) {
    LOG(kWarn) << "Missing struct size for DIE @ 0x" << std::hex
               << die.getOffset();
    return std::nullopt;
  }

  row.size = *opt_size;
  row.file = die.getDeclFile(FileLineInfoKind::AbsoluteFilePath);
  row.line = die.getDeclLine();

  auto name = GetStrAttr(die, dwarf::DW_AT_name);
  if (name) {
    row.name = *name;
  } else {
    row.name = AnonymousName(die);
    row.flags |= StructTypeFlags::kTypeIsAnonymous;
  }

  if (InsertStructLayout(die, row)) {
    // Not a duplicate, we must collect the members
    int member_index = 0;
    std::vector<StructMemberRow> m_rows;
    for (auto &child : die.children()) {
      if (child.getTag() == dwarf::DW_TAG_member) {
        m_rows.emplace_back(VisitMember(child, row, member_index++));
      }
    }
    InsertStructMembers(m_rows);
    // Recursion guarantees that the layout is complete here.
    // Proceed to inspect the flattened layout.
  }

  return row.id;
}

StructMemberRow StructLayoutScraper::VisitMember(const llvm::DWARFDie &die,
                                                 const StructTypeRow &row,
                                                 int member_index) {
  StructMemberRow member;
  member.line = die.getDeclLine();
  member.owner = row.id;
  if (member.owner == 0) {
    LOG(kError) << "Can not visit member of " << std::quoted(row.name)
                << " with invalid owner ID";
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

  std::string name;
  if (!!(row.flags & StructTypeFlags::kTypeIsUnion)) {
    name = std::format("<anon>@{:d}", member_index);
  } else {
    name = std::format("<anon>@{:d}", member.byte_offset);
    if (member.bit_offset) {
      name += std::format(":{:d}", *member.bit_offset);
    }
  }
  member.name = GetStrAttr(die, dwarf::DW_AT_name).value_or(name);

  return member;
}

std::optional<uint64_t>
StructLayoutScraper::VisitMemberType(const llvm::DWARFDie &die,
                                     StructMemberRow &member) {
  /* Returned ID for the nested type, if any */
  std::optional<uint64_t> nested_type_id = std::nullopt;

  TypeInfo member_type = GetTypeInfo(die);

  member.type_name = member_type.type_name;
  member.byte_size = member_type.byte_size;
  member.flags = member_type.flags;
  member.array_items = member_type.array_items;

  /*
   * In this case, we want to reference the nested aggregate type,
   * if this does not exist yet, we must visit it to create an entry
   * in the database.
   */
  if (!!(member.flags & record_type_mask)) {
    StructTypeFlags flags = StructTypeFlags::kTypeNone;
    if (!!(member.flags & TypeInfoFlags::kTypeIsStruct))
      flags |= StructTypeFlags::kTypeIsStruct;
    else if (!!(member.flags & TypeInfoFlags::kTypeIsUnion))
      flags |= StructTypeFlags::kTypeIsUnion;
    else if (!!(member.flags & TypeInfoFlags::kTypeIsClass))
      flags |= StructTypeFlags::kTypeIsClass;

    member.nested = VisitCommon(member_type.type_die, flags);
    nested_type_id = member.nested;
  }
  return nested_type_id;
}

void StructLayoutScraper::FindSubobjectCapabilities(int64_t struct_type_id) {

  std::string q = std::format(
      "SELECT * FROM flattened_layout WHERE type_id = {}", struct_type_id);

  sm_.SqlExec(q, [this, struct_type_id](SqlRowView result) {
    MemberBoundsRow mb_row;
    mb_row.owner = struct_type_id;
    result.Fetch("member_id", mb_row.member);
    result.Fetch("flat_name", mb_row.name);
    result.Fetch("flat_offset", mb_row.offset);
    // XXX bit rounding
    auto [base, length] = dwsrc_->FindRepresentableRange(
        mb_row.offset, result.FetchAs<uint64_t>("size"));
    mb_row.base = base;
    mb_row.top = base + length;
    InsertMemberBounds(mb_row);
    return false;
  });
}

bool StructLayoutScraper::InsertStructLayout(const llvm::DWARFDie &die,
                                             StructTypeRow &row) {
  // Try to see if we already observed the layout,
  // if so there is no need to query the DB.
  auto cached = struct_type_cache_.find(die.getOffset());
  if (cached != struct_type_cache_.end()) {
    row.id = cached->second;
    return false;
  }

  bool new_entry = false;
  auto timing = stats_.Timing("insert_type");
  auto cursor = insert_struct_query_->TakeCursor();
  cursor.Bind(row.file, row.line, row.name, row.size, row.flags);
  cursor.Run([&new_entry, &row](SqlRowView result) {
    result.Fetch("id", row.id);
    LOG(kDebug) << "Insert record type for " << row.name << " at " << row.file
                << ":" << row.line << " with ID=" << row.id;
    new_entry = true;
    return true;
  });

  if (!new_entry) {
    // Need to extract the ID from the database
    // XXX this may be done lazily maybe? as we do not always use it.
    auto cursor = select_struct_query_->TakeCursor();
    cursor.Bind(row.file, row.line, row.name);
    cursor.Run([&row](SqlRowView result) {
      result.Fetch("id", row.id);
      return true;
    });
    stats_.dup_structs++;
  }
  struct_type_cache_.insert({die.getOffset(), row.id});
  return new_entry;
}

/*
 * Note: here it is an error to find duplicate members.
 * This is because we only scan members if the owner was not in the DB
 * yet. Concurrent member generation should not occur, therefore we
 * expect the INSERT to succeed.
 */
void StructLayoutScraper::InsertStructMembers(
    std::vector<StructMemberRow> &member_rows) {

  auto timing = stats_.Timing("insert_member");
  auto cursor = insert_member_query_->TakeCursor();
  auto tx = sm_.BeginTransaction();
  for (auto &row : member_rows) {
    cursor.BindAt("@owner", row.owner);
    cursor.BindAt("@nested", row.nested);
    cursor.BindAt("@name", row.name);
    cursor.BindAt("@type_name", row.type_name);
    cursor.BindAt("@line", row.line);
    cursor.BindAt("@size", row.byte_size);
    cursor.BindAt("@bit_size", row.bit_size);
    cursor.BindAt("@offset", row.byte_offset);
    cursor.BindAt("@bit_offset", row.bit_offset);
    cursor.BindAt("@flags", row.flags);
    cursor.BindAt("@array_items", row.array_items);
    try {
      cursor.Run([&row](SqlRowView result) {
        result.Fetch("id", row.id);
        return true;
      });
    } catch (const std::exception &ex) {
      StructMemberRow existing;
      sm_.SqlExec(std::format("SELECT * FROM struct_member WHERE owner={} AND "
                              "name='{}' AND offset={}",
                              row.owner, row.name, row.byte_offset),
                  [&existing](SqlRowView result) {
                    existing = StructMemberRow::FromSql(result);
                    return true;
                  });
      LOG(kError) << "Failed to insert struct member " << row << " found "
                  << existing;
      throw;
    }
  }
  tx.Commit();
}

void StructLayoutScraper::InsertMemberBounds(const MemberBoundsRow &row) {
  std::string q = std::format(
      "INSERT INTO member_bounds (owner, member, offset, name, "
      "representable_base, representable_top) "
      "VALUES({0}, {1}, {2}, '{3}', {4}, {5})",
      row.owner, row.member, row.offset, row.name, row.base, row.top);

  LOG(kDebug) << "Record member bounds for " << row.name << std::hex
              << " base=0x" << row.base << " off=0x" << row.offset << " top=0x"
              << row.top;
  sm_.SqlExec(q);
}

} /* namespace cheri */
