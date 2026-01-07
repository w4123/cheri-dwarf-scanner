/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023-2025 Alfredo Mazzinghi
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <algorithm>
#include <cassert>

#include <QVariant>

#include "flat_layout_scraper.hh"

namespace fs = std::filesystem;
namespace dwarf = llvm::dwarf;

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
            "total_padding INTEGER NOT NULL,"
            "has_extra_padding INTEGER DEFAULT 0 NOT NULL"
            " CHECK (has_extra_padding >= 0 AND has_extra_padding <= 1),"
            // Whether the type is a struct, union or not
            "is_union INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_union >= 0 AND is_union <= 1),"
            // Does the structure contain a VLA
            "has_vla INTEGER DEFAULT 0 NOT NULL"
            " CHECK(has_vla >= 0 AND has_vla <= 1),"
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
            "alignment INTEGER DEFAULT 0 NOT NULL,"
            "array_items INTEGER,"
            "base TEXT,"
            "top TEXT,"
            "required_precision INTEGER,"
            "max_vla_size INTEGER,"
            "is_pointer INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_pointer >= 0 AND is_pointer <= 1),"
            "is_function INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_function >= 0 AND is_function <= 1),"
            "is_anon INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_anon >= 0 AND is_anon <= 1),"
            "is_union INTEGER DEFAULT 0 NOT NULL"
            " CHECK(is_union >= 0 AND is_union <= 1),"
            "is_imprecise INTEGER NOT NULL"
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

std::optional<FlattenedLayout *>
FlatLayoutScraper::visitCommon(const llvm::DWARFDie &die,
                               std::optional<std::string> typedef_name) {
  // Skip declarations, we don't care.
  if (die.find(dwarf::DW_AT_declaration)) {
    return std::nullopt;
  }

  bool is_union = die.getTag() == dwarf::DW_TAG_union_type;

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
    qDebug() << "Structure " << layout->name << " already scanned "
             << layout->file << layout->line;
    return std::nullopt;
  }

  // Flatten the description of the type we found.
  long member_index = 0;
  LayoutMember *last_member = nullptr;
  for (auto &child : die.children()) {
    if (child.getTag() == dwarf::DW_TAG_member || child.getTag() == dwarf::DW_TAG_inheritance) {
      last_member = visitNested(child, layout.get(), td.name, member_index++,
                                /*offset=*/0, /*depth=*/0);
      if (is_union)
        checkVLAMember(layout.get(), last_member);
    }
  }
  if (!is_union)
    checkVLAMember(layout.get(), last_member);

  auto [pos, inserted] =
      layouts_.emplace(std::make_pair(layout->id(), std::move(layout)));
  assert(inserted && "Could not insert duplicate layout");

  return (*pos).second.get();
}

/*
 * Recursively scan through a structure member and attach member
 * data to the layout.
 */
LayoutMember *FlatLayoutScraper::visitNested(const llvm::DWARFDie &die,
                                             FlattenedLayout *layout,
                                             std::string prefix, long mindex,
                                             unsigned long parent_offset, uint64_t depth) {
  /* Skip declarations, we don't care. */
  if (die.find(dwarf::DW_AT_declaration)) {
    return nullptr;
  }

  if (die.find(dwarf::DW_AT_specification)) {
    qCritical() << "Unsupported DW_AT_specification";
    throw ScraperError("Not implemented");
  }

  auto m = std::make_unique<LayoutMember>();
  auto member_type_die = die.getAttributeValueAsReferencedDie(dwarf::DW_AT_type)
                             .resolveTypeUnitReference();

  std::string member_name;
  if (auto tag_name = getStrAttr(die, dwarf::DW_AT_name)) {
    member_name = tag_name.value();
  } else {
    // Anonymous member, use member index to generate unique name
    member_name = std::format("_anon{}", mindex);
  }
  m->name = std::format("{}::{}", prefix, member_name);

  // Relay type information into the member
  TypeDesc member_desc = resolveTypeDie(member_type_die);
  m->type_name = member_desc.name;
  m->byte_size = member_desc.byte_size;
  auto tag_bit_size = getULongAttr(die, dwarf::DW_AT_bit_size).value_or(0);
  if (tag_bit_size > std::numeric_limits<decltype(m->bit_size)>::max()) {
    qCritical() << "Found DW_AT_bit_size overflowing uint8_t";
    throw ScraperError("Not implemented");
  }
  m->bit_size = static_cast<decltype(m->bit_size)>(tag_bit_size);

  m->byte_offset = parent_offset;
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
      auto shift = *tag_old_bit_offset + m->bit_size;
      bit_offset = m->byte_size * 8 - shift;
    } else {
      bit_offset = *tag_old_bit_offset;
    }
  } else if (tag_bit_offset) {
    bit_offset = *tag_bit_offset;
  }

  m->bit_offset = bit_offset;
  m->byte_offset += tag_offset.value_or(0);

  m->array_items = member_desc.array_count;

  auto alignment = getULongAttr(die, dwarf::DW_AT_alignment).value_or(0);

  if (member_desc.pointer) {
    m->is_pointer = true;
    if (*member_desc.pointer == PointerKind::Function) {
      m->is_function = true;
    }
  }

  // Determine bounds
  // Is this really correct?
  uint64_t rlen = m->byte_size + (m->bit_size ? 1 : 0);
  auto [base, length] = source().findRepresentableRange(m->byte_offset, rlen);
  m->base = base;
  m->top = base + length;
  m->required_precision = source().findRequiredPrecision(m->byte_offset, rlen);
  m->is_imprecise = (m->byte_offset != m->base) || (length != rlen);
  m->depth = depth;

  qDebug() << "Traversed member"
           << std::format("+{:#x}:{} {} {} ({:#x}) -> [{:#x}, {:#x}] {}",
                          m->byte_offset, m->bit_offset, m->type_name, m->name,
                          rlen, m->base, m->top, m->is_imprecise ? "I" : "P");

  // Max alignment of all members
  uint64_t struct_alignment = 0;
  // Keep the member pointer for later updates, maybe use shared_ptr?
  LayoutMember *mp = m.get();
  layout->members.emplace_back(std::move(m));
  if (member_desc.decl) {
    auto decl = *member_desc.decl;
    if (decl.kind == DeclKind::Union) {
      mp->is_union = true;
    }

    if (decl.kind != DeclKind::Enum) {
      // Descend into the member
      long member_index = 0;
      LayoutMember *last_member = nullptr;
      prefix += "::" + member_name;
      for (auto &child : decl.type_die.children()) {
        if (child.getTag() == dwarf::DW_TAG_member || child.getTag() == dwarf::DW_TAG_inheritance) {
          last_member = visitNested(child, layout, prefix, member_index++,
                                    mp->byte_offset, depth + 1);
          if (last_member)
            struct_alignment = std::max(struct_alignment, last_member->alignment);
          if (mp->is_union)
            checkVLAMember(layout, last_member);
        }
      }
      if (!mp->is_union)
        checkVLAMember(layout, last_member);
    }
  }

  if (alignment == 0) {
    if (struct_alignment) {
      alignment = struct_alignment;
    } else if (member_desc.array_count && member_desc.array_count.value()) {
      assert(member_desc.byte_size % member_desc.array_count.value() == 0);
      alignment = member_desc.byte_size / member_desc.array_count.value();
    } else {
      alignment = member_desc.byte_size;
    }
  }

  mp->alignment = alignment;

  return mp;
}

void FlatLayoutScraper::checkVLAMember(FlattenedLayout *layout,
                                       LayoutMember *member) {
  if (member == nullptr)
    return;

  if (member->array_items && member->array_items.value() <= 1) {
    member->max_vla_size =
        source().findMaxRepresentableLength(member->byte_offset);
    layout->has_vla = true;
    qDebug() << "Mark VLA member"
             << std::format("{:#x} {} max size {:#x}", member->byte_offset,
                            member->name, *member->max_vla_size);
  }
}

void FlatLayoutScraper::recordLayout(std::unique_ptr<FlattenedLayout> layout) {
  sm_.transaction([&](StorageManager &sm) {
    qDebug() << "Transaction for" << layout->name;
    
    // Calculate padding here
    std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> occupied_bytes;
    uint64_t total_padding = 0;
    bool has_extra_padding = false;
    if (layout->kind != LayoutKind::Union) {
      for (auto &m : layout->members) {
        if (m->depth != 0) continue;
        auto actual_start = m->byte_offset + m->bit_offset / 8;
        auto actual_end = std::max(m->byte_offset + m->byte_size, m->byte_offset + (m->bit_offset + m->bit_size + 7) / 8);
        occupied_bytes.emplace_back(actual_start, actual_end, m->alignment);
      }

      std::sort(occupied_bytes.begin(), occupied_bytes.end());

      uint64_t struct_align = 0;
      uint64_t last_end = 0;
      for (auto&& [start, end, align] : occupied_bytes) {
        if (start > last_end) {
          auto padding = start - last_end;
          total_padding += padding;
          if (align && padding >= align) {
            has_extra_padding = true;
          }
        }
        last_end = end;
        struct_align = std::max(struct_align, align);
      } 

      auto end_padding = layout->size - last_end;
      total_padding += end_padding;
      if (struct_align && end_padding >= struct_align) {
        has_extra_padding = true;
      }
    }
   
    // clang-format off
    auto insert_layout = sm.prepare(
        "INSERT INTO type_layout (name, file, line, size, is_union, has_vla, total_padding, has_extra_padding) "
        "VALUES (:name, :file, :line, :size, :is_union, :has_vla, :total_padding, :has_extra_padding) "
        "ON CONFLICT DO NOTHING RETURNING id");

    auto fetch_layout = sm.prepare(
        "SELECT id FROM type_layout WHERE "
        "name = :name AND file = :file AND line = :line AND size = :size");

    auto insert_member = sm.prepare(
        "INSERT INTO layout_member ("
        "owner, name, type_name, byte_offset, bit_offset, "
        "byte_size, bit_size, array_items, alignment, "
        "base, top, required_precision, max_vla_size, "
        "is_pointer, is_function, is_anon, is_union, is_imprecise"
        ") VALUES ("
        ":owner, :name, :type_name, :byte_offset, :bit_offset, "
        ":byte_size, :bit_size, :array_items, :alignment, "
        ":base, :top, :required_precision, :max_vla_size, "
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
    insert_layout.bindValue(":has_vla", layout->has_vla);
    insert_layout.bindValue(":total_padding", (unsigned long long)total_padding);
    insert_layout.bindValue(":has_extra_padding", (unsigned long long)has_extra_padding);
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
      insert_member.bindValue(":name", QString::fromStdString(m->name));
      insert_member.bindValue(":type_name",
                              QString::fromStdString(m->type_name));
      insert_member.bindValue(":byte_offset", m->byte_offset);
      insert_member.bindValue(":bit_offset", m->bit_offset);
      insert_member.bindValue(":byte_size", m->byte_size);
      insert_member.bindValue(":bit_size", m->bit_size);
      if (m->array_items) {
        insert_member.bindValue(":array_items", *m->array_items);
      } else {
        insert_member.bindValue(":array_items", QVariant::fromValue(nullptr));
      }
      insert_member.bindValue(":alignment", (unsigned long long)m->alignment);
      insert_member.bindValue(":base", QString::fromStdString(std::to_string(m->base)));
      insert_member.bindValue(":top",  QString::fromStdString(std::to_string(m->top)));
      insert_member.bindValue(":required_precision", m->required_precision);
      if (m->max_vla_size) {
        insert_member.bindValue(":max_vla_size", *m->max_vla_size);
      } else {
        insert_member.bindValue(":max_vla_size", QVariant::fromValue(nullptr));
      }
      insert_member.bindValue(":is_pointer", m->is_pointer);
      insert_member.bindValue(":is_function", m->is_function);
      insert_member.bindValue(":is_anon", m->is_anon);
      insert_member.bindValue(":is_union", m->is_union);
      insert_member.bindValue(":is_imprecise", m->is_imprecise);
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
