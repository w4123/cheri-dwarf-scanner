#include <format>
#include <iomanip>

#include "log.hh"
#include "struct_layout_scraper.hh"

namespace fs = std::filesystem;
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

std::ostream &operator<<(std::ostream &os, const MemberBoundsRow &row) {
  os << "MemberBounds{"
     << "id=" << row.id << ", "
     << "owner=" << row.owner << ", "
     << "member=" << row.member << ", " << row.name << " "
     << "@" << row.offset << " "
     << "[" << row.base << ", " << row.top << "] "
     << "imprecise=" << row.is_imprecise;

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
               "file TEXT NOT NULL,"
               // Line where the struct is defined
               "line INTEGER NOT NULL,"
               // Name of the type.
               // If this is anonymous, a synthetic name is created.
               "name TEXT,"
               // Size of the strucutre including any padding
               "size INTEGER NOT NULL,"
               // Flags that determine whether this is a struct/union/class
               "flags INTEGER DEFAULT 0 NOT NULL,"
               // Flag that is set if the structure type layout contains
               // at least one field that is not precisely representably by
               // a sub-object capability
               "has_imprecise BOOLEAN DEFAULT 0,"
               "UNIQUE(name, file, line))");

  /*
   * Pre-compiled queries for struct_type.
   */
  insert_struct_query_ = sm_.Sql(
      "INSERT INTO struct_type (id, file, line, name, size, flags) "
      "VALUES(@id, @file, @line, @name, @size, @flags) "
      "ON CONFLICT DO NOTHING RETURNING id");

  select_struct_query_ = sm_.Sql(
      "SELECT * FROM struct_type WHERE file = @file AND line = @line "
      "AND name = @name");

  set_has_imprecise_query_ = sm_.Sql(
      "UPDATE struct_type SET has_imprecise = 1 WHERE id = @id");

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
              "owner INTEGER NOT NULL,"
              // Optional index of the nested structure
              "nested int,"
              // Member name, anonymous members have synthetic names
              "name TEXT NOT NULL,"
              // Type name of the member, for nested structures, this is the
              // same as struct_type.name
              "type_name TEXT NOT NULL,"
              // Line in the file where the member is defined
              "line INTEGER NOT NULL,"
              // Size (bytes) of the member, this may or may not include internal
              // padding
              "size INTEGER NOT NULL,"
              // Bit remainder of the size, only valid for bitfields
              "bit_size int,"
              // Offset (bytes) of the member with respect to the owner
              "offset INTEGER NOT NULL,"
              // Bit remainder of the offset, only valid for bitfields
              "bit_offset int,"
              // Type flags
              "flags INTEGER DEFAULT 0 NOT NULL,"
              "array_items int,"
              "FOREIGN KEY (owner) REFERENCES struct_type (id),"
              "FOREIGN KEY (nested) REFERENCES struct_type (id),"
              "UNIQUE(owner, name, offset),"
              "CHECK(owner != nested))");

  /*
   * Pre-compiled queries for struct_member
   */
  insert_member_query_ = sm_.Sql(
      "INSERT INTO struct_member ("
      "  id, owner, nested, name, type_name, line, size, "
      "  bit_size, offset, bit_offset, flags, array_items"
      ") VALUES("
      "  @id, @owner, @nested, @name, @type_name, @line, @size,"
      "  @bit_size, @offset, @bit_offset, @flags, @array_items) "
      "ON CONFLICT DO NOTHING RETURNING id");

  /*
   * Create a table holding the representable bounds for each (nested) member
   * of a structure.
   */
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS member_bounds ("
              // ID of the flattened layout entry
              "id INTEGER NOT NULL PRIMARY KEY,"
              // ID of the struct_type containing this member
              "owner INTEGER NOT NULL,"
              // Flattened member index
              "mindex INTEGER NOT NULL,"
              // Flattened name for the layout entry
              "name TEXT NOT NULL,"
              // ID of the corresponding member entry in struct_members
              "member INTEGER NOT NULL,"
              // Cumulative offset of this member from the start of owner
              "offset INTEGER NOT NULL,"
              // Representable sub-object base
              "base INTEGER NOT NULL,"
              // Representable top of the sub-object
              "top INTEGER NOT NULL,"
              // Mark whether the member is not precisely representable
              "is_imprecise BOOL DEFAULT 0,"
              // Require number of precision bits required to exactly represent
              // the capability
              "precision INTEGER,"
              "FOREIGN KEY (owner) REFERENCES struct_type (id),"
              "FOREIGN KEY (member) REFERENCES struct_member (id))");

  /*
   * Pre-compiled queries for member_bounds
   */
  insert_member_bounds_query_ = sm_.Sql(
      "INSERT INTO member_bounds ("
      "  owner, member, mindex, offset, name, base, top, is_imprecise, precision) "
      "VALUES(@owner, @member, @mindex, @offset, @name, @base, @top, @is_imprecise,"
      "  @precision)");

  /*
   * Create table holding imprecise sub-objects for each structure
   */
  sm_.SqlExec("CREATE TABLE IF NOT EXISTS subobject_alias ("
               // Member bounds for which the sub-object capability aliases
               // a set of other members
               "subobj INTEGER NOT NULL,"
               // Member bounds entry that is accessible from the subobj
               // capability
               "alias INTEGER NOT NULL,"
               "PRIMARY KEY (subobj, alias),"
               "FOREIGN KEY (subobj) REFERENCES member_bounds (id),"
               "FOREIGN KEY (alias) REFERENCES member_bounds (id))");

  /*
   * Create view to produce combinations of member_bounds to check for
   * sub-object bounds aliasing.
   */
  sm_.SqlExec("CREATE VIEW IF NOT EXISTS alias_bounds AS "
               "WITH impl ("
               "  owner, id, alias_id, name, alias_name, base, check_base,"
               "  top, check_top) "
               "AS ("
               "SELECT "
               "  mb.owner,"
               "  mb.id,"
               "  alb.id AS alias_id,"
               "  mb.name,"
               "  alb.name AS alias_name,"
               "  mb.base,"
               "  alb.offset AS check_base,"
               "  mb.top,"
               "  (alb.offset + alm.size + IIF(alm.bit_size, 1, 0)) AS check_top "
               "FROM member_bounds alb"
               "  JOIN struct_member alm ON alb.member = alm.id"
               "  JOIN member_bounds mb ON "
               "    mb.owner = alb.owner AND mb.id != alb.id) "
               "SELECT owner, id AS subobj_id, alias_id "
               "FROM impl "
               "WHERE "
               "  MAX(check_base, base) < MIN(check_top, top) AND"
               "  NOT (name LIKE alias_name || '%') AND"
               "  NOT (alias_name LIKE name || '%')");

  /*
   * Pre-compiled queries for subobject alias discovery
   */
  find_imprecise_alias_query_ = sm_.Sql(
      "INSERT INTO subobject_alias (subobj, alias)"
      "  SELECT ab.subobj_id AS subobj, ab.alias_id AS alias"
      "  FROM alias_bounds ab"
      "  WHERE ab.owner = @owner");
  // clang-format on
}

/*
 * Note that we discard top-level record types that don't have a name
 * this is because they must be nested things, otherwise they are invalid C
 * so we already cover them with the nested mechanism.
 * Otherwise, they are covered by visit_typedef().
 */
bool StructLayoutScraper::visit_structure_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    VisitCommon(die, StructTypeFlags::kTypeIsStruct);
  }
  return false;
}

bool StructLayoutScraper::visit_class_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    VisitCommon(die, StructTypeFlags::kTypeIsClass);
  }
  return false;
}

bool StructLayoutScraper::visit_union_type(llvm::DWARFDie &die) {
  if (die.find(dwarf::DW_AT_name)) {
    VisitCommon(die, StructTypeFlags::kTypeIsUnion);
  }
  return false;
}

bool StructLayoutScraper::visit_typedef(llvm::DWARFDie &die) {
  auto type_die = die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                      .resolveTypeUnitReference();
  if (!type_die) {
    // Forward declaration
    return false;
  }
  std::optional<uint64_t> record_id;
  switch (type_die.getTag()) {
  case dwarf::DW_TAG_structure_type:
    record_id = VisitCommon(type_die, StructTypeFlags::kTypeIsStruct);
    break;
  case dwarf::DW_TAG_union_type:
    record_id = VisitCommon(type_die, StructTypeFlags::kTypeIsUnion);
    break;
  case dwarf::DW_TAG_class_type:
    record_id = VisitCommon(type_die, StructTypeFlags::kTypeIsClass);
    break;
  default:
    // Not interested
    return false;
  }
  if (!record_id) {
    // Forward declaration
    return false;
  }

  auto maybe_name = GetStrAttr(die, dwarf::DW_AT_name);
  if (!maybe_name) {
    LOG(kError) << "Invalid typedef, missing type name";
    throw std::runtime_error("Typedef without name");
  }
  auto it = entry_by_id_.find(*record_id);
  assert(it != entry_by_id_.end() && "Invalid record type ID");
  it->second->data.alias_name = maybe_name;

  return false;
}

void StructLayoutScraper::BeginUnit(llvm::DWARFDie &unit_die) {
  auto at_name = unit_die.find(dwarf::DW_AT_name);
  if (at_name) {
    llvm::Expected name_or_err = at_name->getAsCString();
    if (name_or_err) {
      unit_name_ = *name_or_err;
    } else {
      LOG(kError) << "Invalid compilation unit, can't extract AT_name";
      throw std::runtime_error("Invalid compliation unit");
    }
  } else {
    LOG(kError) << "Invalid compliation unit, missing AT_name";
    throw std::runtime_error("Invalid compliation unit");
  }
  LOG(kDebug) << "Enter compilation unit " << unit_name_;
}

void StructLayoutScraper::EndUnit(llvm::DWARFDie &unit_die) {
  // Process the descriptors for this compilation unit

  sm_.Transaction([this](StorageManager *_) {
    // Insert structure layouts first, this allows us to fixup
    // the row ID with the real database ID.
    // The remap_id is used to fixup structure type IDs for duplicate
    // structures that already exist in the database.
    std::unordered_map<uint64_t, uint64_t> remap_id;

    for (auto &entry : record_descriptors_) {
      uint64_t local_id = entry->data.id;
      assert(local_id != 0 && "Unassigned local ID");
      bool new_entry = InsertStructLayout(entry->data);
      assert(entry->data.id != 0 && "Unassigned global ID");
      if (!new_entry) {
        // Need to remap this ID
        remap_id.insert({local_id, entry->data.id});
        entry->skip_postprocess = true;
        // Because IDs are globally unique, we can add the alias mapping
        // directly to the entry_by_id_ map.
        entry_by_id_.insert({entry->data.id, entry});
      }
    }

    // Now that we have stable struct IDs, deal with the members
    // Note that we have filtered out duplicate structs
    for (auto &entry : record_descriptors_) {
      // New entry, need to add members as well
      uint64_t owner = entry->data.id;
      assert(owner != 0 && "Unassigned owner global ID");
      for (auto &m : entry->members) {
        assert(m.id != 0 && "Unassigned member local ID");
        m.owner = owner;
        if (m.nested) {
          auto mapped = remap_id.find(m.nested.value());
          if (mapped != remap_id.end()) {
            assert(m.owner != mapped->second && "Recursive member!");
            m.nested = mapped->second;
          }
          // Ensure nested is valid
          auto tmp = entry_by_id_.find(*m.nested);
          assert(tmp != entry_by_id_.end() && "Invalid nested ID");
        }
        // XXX sync
        InsertStructMember(m);
        assert(m.id != 0 && "Unassigned member global ID");
      }
    }
  });

  // Now that we are done and we know exactly which structures we are
  // responsible for, generate the flattened layout
  // Compute the flattened layout data for the structures in this CU.
  for (auto &entry : record_descriptors_) {
    if (entry->skip_postprocess)
      continue;
    FindSubobjectCapabilities(*entry);
    LOG(kDebug) << "Flattened layout for " << entry->data.name << ": "
                << entry->flattened_layout.size() << " entries";
  }

  sm_.Transaction([this](StorageManager *_) {
    for (auto &entry : record_descriptors_) {
      if (entry->skip_postprocess)
        continue;
      for (auto mb_row : entry->flattened_layout) {
        InsertMemberBounds(mb_row);
      }
      // Determine the alias groups for the member capabilities
      auto find_imprecise = find_imprecise_alias_query_->TakeCursor();
      find_imprecise.Bind(entry->data.id);
      find_imprecise.Run();
    }
  });

  record_descriptors_.clear();
  entry_by_id_.clear();
  source_map_.clear();
  LOG(kDebug) << "Done compilation unit " << unit_name_;
}

uint64_t StructLayoutScraper::GetStructTypeId() {
  static std::atomic<uint64_t> struct_id(1);

  return ++struct_id;
}

uint64_t StructLayoutScraper::GetStructMemberId() {
  static std::atomic<uint64_t> struct_id(1);

  return ++struct_id;
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

  TypeInfo record_type = GetTypeInfo(die);

  auto new_entry = std::make_shared<StructTypeEntry>();
  new_entry->die_offset = die.getOffset();

  StructTypeRow &row = new_entry->data;
  row.flags |= kind;
  if (!!(record_type.flags & TypeInfoFlags::kTypeIsAnon)) {
    row.flags |= StructTypeFlags::kTypeIsAnonymous;
  }
  row.file = *record_type.decl_file;
  row.line = *record_type.decl_line;
  row.size = record_type.byte_size;
  row.name = record_type.type_name;

  // Check whether the entry already exists
  SourceEntrySet *entry_set = nullptr;
  auto key = std::make_tuple(row.file, row.line);
  auto it = source_map_.find(key);
  if (it != source_map_.end()) {
    // Check whether the record type has already been seen.
    // Note that we use the offset as a unique discriminator
    // within the compilation unit.
    for (auto entry : it->second) {
      if (entry->die_offset == die.getOffset()) {
        assert(entry->data.name == row.name && "StructTypeRow mismatch");
        assert(entry->data.file == row.file && "StructTypeRow mismatch");
        assert(entry->data.line == row.line && "StructTypeRow mismatch");
        // Found a match, do not need to add the entry.
        return entry->data.id;
      }
    }
    entry_set = &it->second;
  } else {
    // Nothing defined at this location yet, the structure is definitely new
    // and we need to create the source_map vector.
    auto [new_it, _] =
        source_map_.emplace(std::piecewise_construct,
                            std::forward_as_tuple(key), std::make_tuple());
    entry_set = &new_it->second;
  }
  // Finalize the new entry and insert
  row.id = GetStructTypeId();
  record_descriptors_.push_back(new_entry);
  entry_set->push_back(new_entry);
  entry_by_id_.insert({row.id, new_entry});

  // Collect nested members
  int member_index = 0;
  for (auto &child : die.children()) {
    if (child.getTag() == dwarf::DW_TAG_member) {
      new_entry->members.emplace_back(VisitMember(child, row, member_index++));
    }
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
  member.id = GetStructMemberId();

  auto member_type_die = die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                             .resolveTypeUnitReference();
  TypeInfo member_type = GetTypeInfo(member_type_die);
  // Use the typedef name if possible
  if (member_type.alias_name) {
    member.type_name = *member_type.alias_name;
  } else {
    member.type_name = member_type.type_name;
  }
  member.byte_size = member_type.byte_size;
  member.flags = member_type.flags;
  member.array_items = member_type.array_items;

  /* Extract offsets, taking into account bitfields */
  member.byte_size =
      dwarf::toUnsigned(die.find(dwarf::DW_AT_byte_size), member.byte_size);
  member.bit_size = GetULongAttr(die, dwarf::DW_AT_bit_size);

  auto data_offset =
      GetULongAttr(die, dwarf::DW_AT_data_member_location).value_or(0);
  auto bit_offset = GetULongAttr(die, dwarf::DW_AT_data_bit_offset);

  auto old_style_bit_offset = GetULongAttr(die, dwarf::DW_AT_bit_offset);
  if (old_style_bit_offset) {
    if (dwsrc_->GetContext().isLittleEndian()) {
      auto shift = *old_style_bit_offset + member.bit_size.value_or(0);
      bit_offset = bit_offset.value_or(0) + member.byte_size * 8 - shift;
    } else {
      bit_offset = bit_offset.value_or(0) + *old_style_bit_offset;
    }
  }
  member.byte_offset = data_offset + bit_offset.value_or(0) / 8;
  member.bit_offset =
      (bit_offset) ? std::make_optional(*bit_offset % 8) : std::nullopt;

  std::string name;
  if (!!(row.flags & StructTypeFlags::kTypeIsUnion)) {
    name = std::format("<variant@{:d}>", member_index);
  } else {
    std::string bit_index;
    if (member.bit_offset) {
      name += std::format(":{:d}", *member.bit_offset);
    }
    name = std::format("<field@{:d}{}>", member.byte_offset, bit_index);
  }
  member.name = GetStrAttr(die, dwarf::DW_AT_name).value_or(name);

  /*
   * Now we have all the information to recursively visit nested type.
   * Note that the type name requires disambiguation if the type is
   * anonymous.
   * It is not possible otherwise to distinguish between two anonymous
   * structures with the same file/line combination. This happens with
   * structures defined as part of macros.
   * If the type is anonymous, we create a synthetic typedef if there isn't
   * already a type alias.
   */
  if (member.HasRecordType()) {
    StructTypeFlags flags = StructTypeFlags::kTypeNone;
    if (!!(member.flags & TypeInfoFlags::kTypeIsStruct))
      flags |= StructTypeFlags::kTypeIsStruct;
    else if (!!(member.flags & TypeInfoFlags::kTypeIsUnion))
      flags |= StructTypeFlags::kTypeIsUnion;
    else if (!!(member.flags & TypeInfoFlags::kTypeIsClass))
      flags |= StructTypeFlags::kTypeIsClass;

    member.nested = VisitCommon(member_type.type_die, flags);
    assert(member.nested && *member.nested != 0 &&
           "Structure type ID must be set");
    auto nested_entry = entry_by_id_[*member.nested];

    if (!!(member.flags & TypeInfoFlags::kTypeIsAnon) &&
        !nested_entry->data.alias_name) {
      nested_entry->data.alias_name =
          AnonymousName(member_type.type_die, strip_prefix_, member.name + ":");
    }
  }

  return member;
}

void StructLayoutScraper::InsertMemberBounds(const MemberBoundsRow &row) {
  auto cursor = insert_member_bounds_query_->TakeCursor();
  cursor.Bind(row.owner, row.member, row.mindex, row.offset, row.name, row.base,
              row.top, row.is_imprecise, row.required_precision);
  cursor.Run();

  LOG(kDebug) << "Record member bounds for " << row.name << std::hex
              << " base=0x" << row.base << " off=0x" << row.offset << " top=0x"
              << row.top << std::dec << " p=" << row.required_precision;
}

bool StructLayoutScraper::InsertStructLayout(StructTypeRow &row) {
  bool new_entry = false;
  auto timing = stats_.Timing("insert_type");
  auto cursor = insert_struct_query_->TakeCursor();
  cursor.BindAt("@id", row.id);
  cursor.BindAt("@file", row.file);
  cursor.BindAt("@line", row.line);
  if (row.alias_name) {
    cursor.BindAt("@name", *row.alias_name);
  } else {
    cursor.BindAt("@name", row.name);
  }
  cursor.BindAt("@size", row.size);
  cursor.BindAt("@flags", row.flags);
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
  return new_entry;
}

void StructLayoutScraper::InsertStructMember(StructMemberRow &row) {
  bool new_entry = false;
  auto timing = stats_.Timing("insert_member");
  auto cursor = insert_member_query_->TakeCursor();
  cursor.BindAt("@id", row.id);
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

  cursor.Run([&new_entry, &row](SqlRowView result) {
    new_entry = true;
    result.Fetch("id", row.id);
    LOG(kDebug) << "Insert record member for " << row.name << " at "
                << row.byte_offset << ":"
                << ((row.bit_offset) ? *row.bit_offset : 0)
                << " with ID=" << row.id;
    return true;
  });

  if (!new_entry) {
    std::string q =
        std::format("SELECT id FROM struct_member WHERE owner={} AND "
                    "name='{}' AND offset={}",
                    row.owner, row.name, row.byte_offset);
    sm_.SqlExec(q, [&row](SqlRowView result) {
      result.Fetch("id", row.id);
      return true;
    });
  }
}

void StructLayoutScraper::FindSubobjectCapabilities(StructTypeEntry &entry) {
  if (!entry.flattened_layout.empty()) {
    // Already scanned, skip
    return;
  }

  std::function<void(StructTypeEntry &, uint64_t, std::string)> FlattenedLayout;
  uint64_t member_index = 0;

  FlattenedLayout = [this, &member_index, &entry,
                     &FlattenedLayout](StructTypeEntry &curr, uint64_t offset,
                                       std::string prefix) {
    for (auto m : curr.members) {
      MemberBoundsRow mb_row;
      mb_row.owner = entry.data.id;
      mb_row.member = m.id;
      mb_row.mindex = member_index++;
      mb_row.name = prefix + "::" + m.name;
      mb_row.offset = offset + m.byte_offset;
      uint64_t req_length = m.byte_size + (m.bit_size ? 1 : 0);
      auto [base, length] =
          dwsrc_->FindRepresentableRange(mb_row.offset, req_length);
      mb_row.required_precision =
          dwsrc_->FindRequiredPrecision(mb_row.offset, req_length);
      mb_row.base = base;
      mb_row.top = base + length;
      mb_row.is_imprecise = mb_row.offset != base || length != req_length;
      entry.data.has_imprecise |= mb_row.is_imprecise;
      if (m.nested) {
        assert(*m.nested != 0 && "Missing member nested ID");
        auto it = entry_by_id_.find(m.nested.value());
        if (it == entry_by_id_.end()) {
          LOG(kDebug) << "Invalid nested ID " << *m.nested;
          for (auto tmp : entry_by_id_) {
            LOG(kDebug) << "[" << tmp.first << "] " << tmp.second->data.name;
          }
        }
        assert(it != entry_by_id_.end() &&
               "Entry is not in the compilation unit?");
        StructTypeEntry &nested = *it->second;
        FlattenedLayout(*it->second, mb_row.offset, mb_row.name);
      }
      entry.flattened_layout.emplace_back(std::move(mb_row));
    }
  };
  FlattenedLayout(entry, 0, entry.data.name);

  // If we found imprecise members, record it
  if (entry.data.has_imprecise) {
    auto cursor = set_has_imprecise_query_->TakeCursor();
    cursor.BindAt("@id", entry.data.id);
    cursor.Run();
  }
}

} /* namespace cheri */
