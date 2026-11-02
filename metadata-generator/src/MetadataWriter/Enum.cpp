#include "MetadataWriter.h"

namespace metagen {

void MDMetadataWriter::write(EnumDecl &decl) {
  if (decl.mdOffset != MD_SECTION_OFFSET_NULL) {
    return;
  }

  MDEnum *mdEnum = new MDEnum();

  mdEnum->name = strings.add(decl.name, decl.name);
  mdEnum->memberPrefix = strings.add(decl.memberPrefix, decl.memberPrefix);
  mdEnum->memberSuffix = strings.add(decl.memberSuffix, decl.memberSuffix);
  mdEnum->members.reserve(decl.constants.size());

  for (const EnumConstDecl &c : decl.constants) {
    MDEnumMember member;
    member.name = strings.add(c.name, c.name);
    if ((member.name & mdSectionOffsetEnumMemberCName) != 0) {
      std::cerr << "Strings section too large to flag enum member names"
                << std::endl;
      exit(1);
    }
    member.nameIsCName = c.name == c.cName;
    if (!member.nameIsCName &&
        decl.memberPrefix + c.name + decl.memberSuffix != c.cName) {
      std::cerr << "Enum " << decl.name << ": cannot encode the C name of "
                << c.cName << std::endl;
      exit(1);
    }
    member.value = c.value;
    mdEnum->members.push_back(member);
  }

  decl.mdOffset = enums.add(mdEnum, decl.name);
}

size_t MDEnumMemberSerde::size(MDEnumMember *value) {
  size_t size = 0;
  // Name
  addsize(value->name);
  // Value
  addsize(value->value);
  return size;
}

void MDEnumMemberSerde::serialize(MDEnumMember *value, void *data) {
  // Name
  MDSectionOffset name = value->name;
  if (value->nameIsCName) {
    name |= mdSectionOffsetEnumMemberCName;
  }
  binwrite(name);
  // Value
  binwrite(value->value);
}

size_t MDEnumSerde::size(MDEnum *value) {
  size_t size = 0;
  // Name
  addsize(value->name);
  addsize(value->memberPrefix);
  addsize(value->memberSuffix);
  // Members
  MDEnumMemberSerde memberSerde;
  if (!value->members.empty()) {
    size += value->members.size() * memberSerde.size(&value->members.front());
  }
  return size;
}

void MDEnumSerde::serialize(MDEnum *value, void *data) {
  // Name
  binwrite(value->name);
  binwrite(value->memberPrefix);
  binwrite(value->memberSuffix);
  // Members
  size_t membersSize = value->members.size();
  MDEnumMemberSerde memberSerde;
  for (size_t i = 0; i < membersSize; i++) {
    MDEnumMember &member = value->members[i];
    size_t memberSize = memberSerde.size(&member);
    memberSerde.serialize(&member, data);
    if (i != membersSize - 1) {
      MDSectionOffset *serializedPtr = (MDSectionOffset *)data;
      *serializedPtr |= mdSectionOffsetNext;
    }
    ptr_add(&data, memberSize);
  }
}

} // namespace metagen
