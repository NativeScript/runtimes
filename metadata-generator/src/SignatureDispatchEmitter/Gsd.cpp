#include "SignatureDispatchEmitter/Shared.h"

#include <sstream>
#include <vector>

// Engine-neutral Generated Signature Dispatch (GSD) emitter.
//
// Emits one set of invoker functions and a dispatch table that are shared by
// every embedded engine backend (V8, JSC, QuickJS, Hermes). Each invoker takes
// a single `GsdObjCContext&` argument. The context is a small concrete struct
// defined per engine that knows how to read JS arguments and write the JS
// return value using that engine's native value API. Native calls are routed
// through ctx.invokeNative so backends that must release/coordinate their JS
// runtime around Objective-C dispatch can do so without moving JS conversion
// outside the engine thread. Backends without that requirement take the direct
// exception-safe path. The generated code only references the engine-neutral
// `GsdObjCContext` interface, so the same `.inc` compiles unchanged in every
// backend translation unit with inline context calls.
//
// GSD handles primitives, SEL, Class, and already-native Objective-C object
// values. Any value that does not match the fast representation makes the
// relevant reader return false, which makes the whole invoker fall back to the
// fully correct generic marshalling path.

namespace metagen::signature_dispatch {

std::string makeGsdWrapperName(size_t index) {
  return "gsd" + toBase36(index);
}

static void writeGsdArgConversion(std::ostringstream& out,
                                  const MDTypeInfo* type, size_t index) {
  switch (type->kind) {
    case mdTypeBool:
      out << "  if (!ctx.readBool(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeChar:
    case mdTypeSShort:
    case mdTypeSInt:
    case mdTypeSLong:
    case mdTypeSInt64:
      out << "  if (!ctx.readSigned(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeUChar:
    case mdTypeUInt8:
    case mdTypeUShort:
    case mdTypeUInt:
    case mdTypeULong:
    case mdTypeUInt64:
      out << "  if (!ctx.readUnsigned(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeFloat:
      out << "  if (!ctx.readFloat(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeDouble:
      out << "  if (!ctx.readDouble(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeSelector:
      out << "  if (!ctx.readSelector(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeClass:
      out << "  if (!ctx.readClass(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      // Object arguments are accepted only when the JS value is already a
      // native host object (cheap pointer unwrap); anything that would require
      // allocation/conversion makes readObject return false and the whole
      // invoker falls back to the generic path.
      out << "  if (!ctx.readObject(" << index << ", &arg" << index
          << ")) return false;\n";
      break;
    default:
      out << "  return false;\n";
      break;
  }
}

static void writeGsdReturnConversion(std::ostringstream& out,
                                     const MDTypeInfo* type) {
  switch (type->kind) {
    case mdTypeVoid:
      out << "  ctx.setVoid();\n";
      break;
    case mdTypeBool:
      out << "  ctx.setBool(nativeResult != 0);\n";
      break;
    case mdTypeChar:
    case mdTypeSShort:
    case mdTypeSInt:
      out << "  ctx.setInt32(static_cast<int32_t>(nativeResult));\n";
      break;
    case mdTypeUChar:
    case mdTypeUInt8:
    case mdTypeUInt:
      out << "  ctx.setUInt32(static_cast<uint32_t>(nativeResult));\n";
      break;
    case mdTypeUShort:
      out << "  ctx.setUInt16(static_cast<uint16_t>(nativeResult));\n";
      break;
    case mdTypeSLong:
    case mdTypeSInt64:
      out << "  ctx.setInt64(static_cast<int64_t>(nativeResult));\n";
      break;
    case mdTypeULong:
    case mdTypeUInt64:
      out << "  ctx.setUInt64(static_cast<uint64_t>(nativeResult));\n";
      break;
    case mdTypeFloat:
    case mdTypeDouble:
      out << "  ctx.setDouble(static_cast<double>(nativeResult));\n";
      break;
    case mdTypeSelector:
      out << "  ctx.setSelector(nativeResult);\n";
      break;
    case mdTypeClass:
      out << "  ctx.setClass(nativeResult);\n";
      break;
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      // Object returns use the exact metadata return type (carried by the
      // context) so JS conversion + ownership match the generic path exactly.
      out << "  ctx.setObject(nativeResult);\n";
      break;
    default:
      out << "  return false;\n";
      break;
  }
}

static bool isGsdFastScalarType(const MDTypeInfo* type) {
  if (type == nullptr) return false;
  switch (type->kind) {
    case mdTypeBool:
    case mdTypeChar:
    case mdTypeUChar:
    case mdTypeUInt8:
    case mdTypeSShort:
    case mdTypeUShort:
    case mdTypeSInt:
    case mdTypeUInt:
    case mdTypeSLong:
    case mdTypeULong:
    case mdTypeSInt64:
    case mdTypeUInt64:
    case mdTypeFloat:
    case mdTypeDouble:
    case mdTypeSelector:
    case mdTypeClass:
      return true;
    default:
      return false;
  }
}

static bool isGsdObjectType(const MDTypeInfo* type) {
  if (type == nullptr) return false;
  switch (type->kind) {
    case mdTypeAnyObject:
    case mdTypeProtocolObject:
    case mdTypeClassObject:
    case mdTypeInstanceObject:
    case mdTypeNSStringObject:
    case mdTypeNSMutableStringObject:
      return true;
    default:
      return false;
  }
}

// Arguments accept scalars + Objective-C object types (the reader unwraps a
// native host object's backing pointer or falls back). Returns additionally
// accept void; object returns route through setObject with the exact metadata
// return type so JS conversion and ownership match the generic path.
static bool isGsdFastArgType(const MDTypeInfo* type) {
  return isGsdFastScalarType(type) || isGsdObjectType(type);
}

static bool isGsdFastReturnType(const MDTypeInfo* type) {
  if (type != nullptr && type->kind == mdTypeVoid) return true;
  return isGsdFastScalarType(type) || isGsdObjectType(type);
}

bool isGsdSignatureSupported(const MDSignature* signature) {
  if (signature == nullptr || signature->isVariadic) return false;
  if (signature->arguments.size() > 8) return false;
  if (!isGsdFastReturnType(signature->returnType)) return false;
  for (const auto* arg : signature->arguments) {
    if (!isGsdFastArgType(arg)) return false;
  }
  return true;
}

void writeGsdWrapper(std::ostringstream& out, const std::string& wrapperName,
                     const MDSignature* signature) {
  std::string returnType;
  if (!mapTypeToCpp(signature->returnType, &returnType, true)) return;

  std::vector<std::string> argTypes;
  for (const auto* arg : signature->arguments) {
    std::string argType;
    if (!mapTypeToCpp(arg, &argType, false)) return;
    argTypes.push_back(argType);
  }

  out << "static inline bool " << wrapperName << "(GsdObjCContext& ctx) {\n";

  out << "  using Fn = " << returnType << " (*)(id, SEL";
  for (const auto& argType : argTypes) {
    out << ", " << argType;
  }
  out << ");\n";
  out << "  auto fn = reinterpret_cast<Fn>(objc_msgSend);\n";

  for (size_t i = 0; i < argTypes.size(); i++) {
    out << "  " << argTypes[i] << " arg" << i << "{};\n";
  }
  for (size_t i = 0; i < argTypes.size(); i++) {
    writeGsdArgConversion(out, signature->arguments[i], i);
  }

  if (returnType == "void") {
    out << "  ctx.invokeNative([&]() {\n";
    out << "    fn(ctx.self, ctx.selector";
    for (size_t i = 0; i < argTypes.size(); i++) out << ", arg" << i;
    out << ");\n";
    out << "  });\n";
  } else {
    out << "  " << returnType << " nativeResult{};\n";
    out << "  ctx.invokeNative([&]() {\n";
    out << "    nativeResult = fn(ctx.self, ctx.selector";
    for (size_t i = 0; i < argTypes.size(); i++) out << ", arg" << i;
    out << ");\n";
    out << "  });\n";
  }

  writeGsdReturnConversion(out, signature->returnType);

  out << "  return true;\n";
  out << "}\n\n";
}

std::string makeGsdWrapperShapeKey(const MDSignature* signature) {
  if (signature == nullptr) return {};
  std::string base = makeWrapperShapeKey(DispatchKind::ObjCMethod, signature);
  if (base.empty()) return {};
  return "gsd|" + base;
}

}  // namespace metagen::signature_dispatch
