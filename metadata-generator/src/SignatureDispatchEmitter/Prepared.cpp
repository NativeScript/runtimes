#include "SignatureDispatchEmitter/Shared.h"

#include <sstream>
#include <vector>

namespace metagen::signature_dispatch {

std::string makePreparedWrapperName(DispatchKind kind, size_t index) {
  std::ostringstream stream;
  stream << "dp";
  switch (kind) {
    case DispatchKind::ObjCMethod:
      stream << "o";
      break;
    case DispatchKind::CFunction:
      stream << "c";
      break;
    case DispatchKind::BlockInvoke:
      stream << "b";
      break;
  }
  stream << toBase36(index);
  return stream.str();
}

void writePreparedWrapper(std::ostringstream& out, DispatchKind kind,
                          const std::string& wrapperName,
                          const MDSignature* signature) {
  std::string returnType;
  if (!mapTypeToCpp(signature->returnType, &returnType, true)) {
    return;
  }

  std::vector<std::string> argTypes;
  argTypes.reserve(signature->arguments.size());
  for (const auto* arg : signature->arguments) {
    std::string argType;
    if (!mapTypeToCpp(arg, &argType, false)) {
      return;
    }
    argTypes.push_back(argType);
  }

  out << "static inline void " << wrapperName
      << "(void* fnptr, void** avalues, void* rvalue) {\n";
  out << "  using Fn = " << returnType << " (*)(";
  bool first = true;
  if (kind == DispatchKind::ObjCMethod) {
    out << "id, SEL";
    first = false;
  } else if (kind == DispatchKind::BlockInvoke) {
    out << "void*";
    first = false;
  }
  for (const auto& argType : argTypes) {
    if (!first) {
      out << ", ";
    }
    out << argType;
    first = false;
  }
  out << ");\n";
  out << "  auto fn = reinterpret_cast<Fn>(fnptr);\n";
  size_t implicitArgumentCount = 0;
  if (kind == DispatchKind::ObjCMethod) {
    out << "  id self = *reinterpret_cast<id*>(avalues[0]);\n";
    out << "  SEL selector = *reinterpret_cast<SEL*>(avalues[1]);\n";
    implicitArgumentCount = 2;
  } else if (kind == DispatchKind::BlockInvoke) {
    out << "  void* block = *reinterpret_cast<void**>(avalues[0]);\n";
    implicitArgumentCount = 1;
  }
  for (size_t i = 0; i < argTypes.size(); i++) {
    out << "  " << argTypes[i] << " arg" << i << " = *reinterpret_cast<"
        << argTypes[i] << "*>(avalues[" << (i + implicitArgumentCount)
        << "]);\n";
  }

  std::ostringstream callExpr;
  callExpr << "fn(";
  bool hasAnyCallArg = false;
  if (kind == DispatchKind::ObjCMethod) {
    callExpr << "self, selector";
    hasAnyCallArg = true;
  } else if (kind == DispatchKind::BlockInvoke) {
    callExpr << "block";
    hasAnyCallArg = true;
  }
  for (size_t i = 0; i < argTypes.size(); i++) {
    if (hasAnyCallArg) {
      callExpr << ", ";
    }
    callExpr << "arg" << i;
    hasAnyCallArg = true;
  }
  callExpr << ")";

  if (returnType == "void") {
    out << "  " << callExpr.str() << ";\n";
  } else {
    out << "  *reinterpret_cast<" << returnType
        << "*>(rvalue) = " << callExpr.str() << ";\n";
  }
  out << "}\n\n";
}

}  // namespace metagen::signature_dispatch
