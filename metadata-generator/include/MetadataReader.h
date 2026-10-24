#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Metadata.h"

namespace metagen {

typedef char MDHeaderMagic[MD_HEADER_MAGIC_SIZE];

class MDMetadataReader {
 public:
  void* data;
  bool ownsData = false;
  uint16_t version = 0;

  MDSectionOffset stringsOffset;
  MDSectionOffset constantsOffset;
  MDSectionOffset enumsOffset;
  MDSectionOffset signaturesOffset;
  MDSectionOffset functionsOffset;
  MDSectionOffset protocolsOffset;
  MDSectionOffset classesOffset;
  MDSectionOffset structsOffset;
  MDSectionOffset unionsOffset;

  MDMetadataReader(void* data, bool ownsData = false)
      : data(data), ownsData(ownsData) {
    MDHeaderMagic magic;
    memcpy(magic, data, MD_HEADER_MAGIC_SIZE);
    if (memcmp(magic, MD_HEADER_MAGIC, MD_HEADER_MAGIC_SIZE) != 0) {
      fprintf(stderr, "Invalid metadata header magic\n");
      exit(1);
    }
    ptr_add(&data, MD_HEADER_MAGIC_SIZE);

    memcpy(&version, data, MD_HEADER_VERSION_SIZE);
    if (version < MD_HEADER_MIN_VERSION || version > MD_HEADER_VERSION) {
      fprintf(stderr, "Invalid metadata header version\n");
      exit(1);
    }
    ptr_add(&data, MD_HEADER_VERSION_SIZE);

    stringsOffset =
        MD_HEADER_SIZE + sizeof(MDSectionOffset) * (MD_NUM_SECTIONS - 1);
    constantsOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    enumsOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    signaturesOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    functionsOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    protocolsOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    classesOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    structsOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
    unionsOffset = *(MDSectionOffset*)data;
    ptr_add(&data, sizeof(MDSectionOffset));
  }

  ~MDMetadataReader() {
    if (ownsData && data != nullptr) {
      free(data);
      data = nullptr;
    }
  }

  inline char* resolveString(MDSectionOffset offset) {
    return (char*)data + stringsOffset + offset;
  }

  inline MDSectionOffset getOffset(MDSectionOffset offset) {
    return *(MDSectionOffset*)((char*)data + offset);
  }

  inline char* getString(MDSectionOffset offset) {
    MDSectionOffset stringOffset = getOffset(offset);
    return resolveString(stringOffset);
  }

  inline MDTypeKind getTypeKind(MDSectionOffset offset) {
    return *(MDTypeKind*)((char*)data + offset);
  }

  inline uint16_t getArraySize(MDSectionOffset offset) {
    return *(uint16_t*)((char*)data + offset);
  }

  inline int64_t getEnumValue(MDSectionOffset offset) {
    return *(int64_t*)((char*)data + offset);
  }

  inline bool hasEnumMemberCNames() const {
    return version >= MD_HEADER_VERSION_ENUM_MEMBER_C_NAMES;
  }

  // Visits fn(name, cName, value) for each member of the enum entry at
  // enumOffset and returns the offset just past the entry. cName is empty when
  // the metadata predates stored C names; it cannot be derived from name.
  template <typename Fn>
  inline MDSectionOffset forEachEnumMember(MDSectionOffset enumOffset,
                                           Fn&& fn) {
    MDSectionOffset offset = enumOffset + sizeof(MDSectionOffset);
    const bool hasCNames = hasEnumMemberCNames();
    const char* prefix = "";
    const char* suffix = "";
    if (hasCNames) {
      prefix = getString(offset);
      offset += sizeof(MDSectionOffset);
      suffix = getString(offset);
      offset += sizeof(MDSectionOffset);
    }

    bool next = true;
    while (next) {
      MDSectionOffset nameOffset = getOffset(offset);
      next = (nameOffset & mdSectionOffsetNext) != 0;
      const bool nameIsCName =
          hasCNames && (nameOffset & mdSectionOffsetEnumMemberCName) != 0;
      if (hasCNames) {
        nameOffset &= ~mdSectionOffsetEnumMemberCName;
      }
      nameOffset &= ~mdSectionOffsetNext;
      offset += sizeof(MDSectionOffset);
      int64_t value = getEnumValue(offset);
      offset += sizeof(int64_t);

      const char* name = resolveString(nameOffset);
      std::string cName;
      if (nameIsCName) {
        cName = name;
      } else if (hasCNames) {
        cName.append(prefix).append(name).append(suffix);
      }
      fn(name, cName, value);
    }
    return offset;
  }

  inline MDSectionOffset skipEnum(MDSectionOffset enumOffset) {
    MDSectionOffset offset = enumOffset + sizeof(MDSectionOffset);
    if (hasEnumMemberCNames()) {
      offset += 2 * sizeof(MDSectionOffset);
    }
    bool next = true;
    while (next) {
      next = (getOffset(offset) & mdSectionOffsetNext) != 0;
      offset += sizeof(MDSectionOffset) + sizeof(int64_t);
    }
    return offset;
  }

  inline MDMemberFlag getMemberFlag(MDSectionOffset offset) {
    return *(MDMemberFlag*)((char*)data + offset);
  }

  inline MDFunctionFlag getFunctionFlag(MDSectionOffset offset) {
    return *(MDFunctionFlag*)((char*)data + offset);
  }

  inline MDVariableEvalKind getVariableEvalKind(MDSectionOffset offset) {
    return *(MDVariableEvalKind*)((char*)data + offset);
  }

  inline double getDouble(MDSectionOffset offset) {
    return *(double*)((char*)data + offset);
  }

  inline int64_t getInt64(MDSectionOffset offset) {
    return *(int64_t*)((char*)data + offset);
  }
};

}  // namespace metagen
