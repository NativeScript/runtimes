#include "ESModuleSupport.h"

#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace nativescript::esm {

namespace {

// Deliberately leaked rather than held in a namespace-scope object: the Apple
// runtime clears this cache from ~Runtime, which runs during static destruction
// at exit(). Destruction order between translation units is unspecified, and a
// namespace-scope map was being destroyed before that call, so clear() ran on
// a dead object and aborted the process in libmalloc. A function-local pointer
// that is never deleted has no destruction order to get wrong.
std::unordered_map<std::string, bool>& PackageTypeCache() {
  static auto* cache = new std::unordered_map<std::string, bool>();
  return *cache;
}

std::string TrimFallbackESMToken(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> SplitFallbackESMList(const std::string& value) {
  std::vector<std::string> parts;
  std::stringstream stream(value);
  std::string part;
  while (std::getline(stream, part, ',')) {
    part = TrimFallbackESMToken(part);
    if (!part.empty()) {
      parts.push_back(part);
    }
  }
  return parts;
}

std::string EscapeFallbackESMSpecifier(const std::string& specifier) {
  std::string escaped;
  escaped.reserve(specifier.size());
  for (char c : specifier) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\'':
        escaped += "\\'";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      default:
        escaped += c;
        break;
    }
  }
  return escaped;
}

std::string FallbackESMRequireExpression(const std::string& specifier) {
  return "__esm_require('" + EscapeFallbackESMSpecifier(specifier) + "')";
}

std::string RewriteFallbackESMImportBindings(const std::string& bindings) {
  std::string result;
  for (const auto& part : SplitFallbackESMList(bindings)) {
    static const std::regex kAliasPattern(
        R"(^([A-Za-z_$][A-Za-z0-9_$]*)\s+as\s+([A-Za-z_$][A-Za-z0-9_$]*)$)");
    std::smatch alias;
    if (std::regex_match(part, alias, kAliasPattern)) {
      result += (result.empty() ? "" : ", ");
      result += alias[1].str() + ": " + alias[2].str();
    } else {
      result += (result.empty() ? "" : ", ");
      result += part;
    }
  }
  return result;
}

std::string FallbackESMExportAssignments(const std::string& exports,
                                         const std::string& sourceObject = "") {
  std::string result;
  for (const auto& part : SplitFallbackESMList(exports)) {
    static const std::regex kAliasPattern(
        R"(^([A-Za-z_$][A-Za-z0-9_$]*)\s+as\s+([A-Za-z_$][A-Za-z0-9_$]*)$)");
    std::smatch alias;
    std::string local = part;
    std::string exported = part;
    if (std::regex_match(part, alias, kAliasPattern)) {
      local = alias[1].str();
      exported = alias[2].str();
    }

    if (!result.empty()) {
      result += "\n";
    }
    result += "__esm_exports." + exported + " = ";
    result += sourceObject.empty() ? local : sourceObject + "." + local;
    result += ";";
  }
  return result;
}

template <typename Callback>
std::string RegexReplaceWithFallbackESMCallback(const std::string& source,
                                                const std::regex& pattern,
                                                Callback callback) {
  std::string result;
  std::sregex_iterator it(source.begin(), source.end(), pattern);
  std::sregex_iterator end;
  size_t last = 0;
  for (; it != end; ++it) {
    const std::smatch& match = *it;
    result.append(source, last, match.position() - last);
    result += callback(match);
    last = match.position() + match.length();
  }
  result.append(source, last, std::string::npos);
  return result;
}

}  // namespace

std::string StripShebang(const std::string& source) {
  if (source.size() >= 2 && source[0] == '#' && source[1] == '!') {
    size_t lineEnd = source.find('\n');
    if (lineEnd != std::string::npos) {
      return source.substr(lineEnd + 1);
    }
    return "";
  }
  return source;
}

namespace {

bool IsIdentifierChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

// Replaces every `import` keyword that `matchLength` accepts. Given the index
// just past the keyword, `matchLength` returns how many further characters the
// replacement swallows, or npos to leave the keyword alone. Plain scanning
// instead of std::regex: bundles run to megabytes and a regex pass over the
// whole buffer takes seconds on device.
template <typename MatchLength>
std::string RewriteImportKeyword(const std::string& source,
                                 const char* replacement,
                                 MatchLength matchLength) {
  static const std::string kKeyword = "import";
  std::string result;
  result.reserve(source.size() + 256);
  size_t last = 0;
  size_t pos = source.find(kKeyword);
  while (pos != std::string::npos) {
    size_t end = pos + kKeyword.size();
    bool startsToken = pos == 0 || !IsIdentifierChar(source[pos - 1]);
    size_t extra =
        startsToken ? matchLength(source, pos, end) : std::string::npos;
    if (extra != std::string::npos) {
      result.append(source, last, pos - last);
      result += replacement;
      end += extra;
      last = end;
    }
    pos = source.find(kKeyword, end);
  }
  result.append(source, last, std::string::npos);
  return result;
}

}  // namespace

std::string RewriteCommonJSDynamicImportsForFallbackEngines(
    const std::string& source) {
  return RewriteImportKeyword(
      source, "__dynamicImport",
      [](const std::string& text, size_t start, size_t end) -> size_t {
        // `foo.import(` is a method call, not the operator.
        if (start > 0 && text[start - 1] == '.') {
          return std::string::npos;
        }
        size_t cursor = end;
        while (cursor < text.size() &&
               (text[cursor] == ' ' || text[cursor] == '\t' ||
                text[cursor] == '\r' || text[cursor] == '\n')) {
          cursor++;
        }
        bool isCall = cursor < text.size() && text[cursor] == '(';
        return isCall ? 0 : std::string::npos;
      });
}

std::string RewriteImportMeta(const std::string& source) {
  return RewriteImportKeyword(
      source, "__importMeta",
      [](const std::string& text, size_t, size_t end) -> size_t {
        static const std::string kMeta = ".meta";
        if (text.compare(end, kMeta.size(), kMeta) != 0) {
          return std::string::npos;
        }
        size_t after = end + kMeta.size();
        bool endsToken = after >= text.size() || !IsIdentifierChar(text[after]);
        return endsToken ? kMeta.size() : std::string::npos;
      });
}

namespace {

// True when `line` begins (after indentation) with an import or export
// keyword, i.e. it may open a static module statement.
bool StartsModuleStatement(const std::string& line) {
  size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    i++;
  }
  for (const char* keyword : {"import", "export"}) {
    size_t length = std::strlen(keyword);
    if (line.compare(i, length, keyword) == 0 &&
        (i + length >= line.size() || !IsIdentifierChar(line[i + length]))) {
      return true;
    }
  }
  return false;
}

int BraceBalance(const std::string& text) {
  int balance = 0;
  for (char c : text) {
    if (c == '{') {
      balance++;
    } else if (c == '}') {
      balance--;
    }
  }
  return balance;
}

std::string TransformModuleStatement(const std::string& statement,
                                     int& tempIndex);

}  // namespace

std::string TransformESModuleForFallbackEngines(const std::string& source) {
  // Only the handful of lines that open an import/export statement go through
  // the regex chain; everything else is copied through untouched. A statement
  // whose binding list is split across lines is gathered until its braces
  // balance so the multi-line regex forms still see the whole statement.
  std::string result;
  result.reserve(source.size() + 1024);
  int tempIndex = 0;
  size_t pos = 0;
  while (pos < source.size()) {
    size_t lineEnd = source.find('\n', pos);
    size_t next = lineEnd == std::string::npos ? source.size() : lineEnd + 1;
    std::string line = source.substr(pos, next - pos);
    if (!StartsModuleStatement(line)) {
      result += line;
      pos = next;
      continue;
    }
    std::string statement = line;
    int balance = BraceBalance(statement);
    while (balance > 0 && next < source.size()) {
      size_t continuationEnd = source.find('\n', next);
      size_t continuationNext = continuationEnd == std::string::npos
                                    ? source.size()
                                    : continuationEnd + 1;
      std::string continuation = source.substr(next, continuationNext - next);
      statement += continuation;
      balance += BraceBalance(continuation);
      next = continuationNext;
    }
    result += TransformModuleStatement(statement, tempIndex);
    pos = next;
  }

  result = RewriteImportMeta(result);
  return RewriteCommonJSDynamicImportsForFallbackEngines(result);
}

namespace {

std::string TransformModuleStatement(const std::string& statement,
                                     int& tempIndex) {
  std::string result = statement;

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*,[ \t]*\*[ \t]+as[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [&](const std::smatch& match) {
        std::string module = "__esm_import_" + std::to_string(tempIndex++);
        return "const " + module + " = " +
               FallbackESMRequireExpression(match[3].str()) + ";\nconst " +
               match[1].str() + " = " + module + ".default;\nconst " +
               match[2].str() + " = " + module + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*,[ \t]*\{([^}]*)\}[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [&](const std::smatch& match) {
        std::string module = "__esm_import_" + std::to_string(tempIndex++);
        return "const " + module + " = " +
               FallbackESMRequireExpression(match[3].str()) + ";\nconst " +
               match[1].str() + " = " + module + ".default;\nconst {" +
               RewriteFallbackESMImportBindings(match[2].str()) +
               "} = " + module + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+\{([^}]*)\}[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "const {" + RewriteFallbackESMImportBindings(match[1].str()) +
               "} = " + FallbackESMRequireExpression(match[2].str()) + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+\*[ \t]+as[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "const " + match[1].str() + " = " +
               FallbackESMRequireExpression(match[2].str()) + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*import[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "const " + match[1].str() + " = " +
               FallbackESMRequireExpression(match[2].str()) + ".default;";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*import[ \t]+['"]([^'"]+)['"][ \t]*;?)",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return FallbackESMRequireExpression(match[1].str()) + ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+\*[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "Object.assign(__esm_exports, " +
               FallbackESMRequireExpression(match[1].str()) + ");";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+\{([^}]*)\}[ \t]+from[ \t]+['"]([^'"]+)['"][ \t]*;?)",
          std::regex::ECMAScript | std::regex::multiline),
      [&](const std::smatch& match) {
        std::string module = "__esm_export_" + std::to_string(tempIndex++);
        return "const " + module + " = " +
               FallbackESMRequireExpression(match[2].str()) + ";\n" +
               FallbackESMExportAssignments(match[1].str(), module);
      });

  result = std::regex_replace(
      result,
      std::regex(R"(^[ \t]*export[ \t]+default[ \t]+function[ \t]*)",
                 std::regex::ECMAScript | std::regex::multiline),
      "__esm_exports.default = function ");

  result = std::regex_replace(
      result,
      std::regex(R"(^[ \t]*export[ \t]+default[ \t]+class[ \t]*)",
                 std::regex::ECMAScript | std::regex::multiline),
      "__esm_exports.default = class ");

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+function[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*\()",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "__esm_exports." + match[1].str() + " = function " +
               match[1].str() + "(";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*export[ \t]+class[ \t]+([A-Za-z_$][A-Za-z0-9_$]*))",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "__esm_exports." + match[1].str() + " = class " + match[1].str();
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(
          R"(^[ \t]*export[ \t]+(const|let|var)[ \t]+([A-Za-z_$][A-Za-z0-9_$]*)[ \t]*=[ \t]*([^;\r\n]*);?)",
          std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return match[1].str() + " " + match[2].str() + " = " + match[3].str() +
               ";\n__esm_exports." + match[2].str() + " = " + match[2].str() +
               ";";
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*export[ \t]*\{([^}]*)\}[ \t]*;)",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return FallbackESMExportAssignments(match[1].str());
      });

  result = RegexReplaceWithFallbackESMCallback(
      result,
      std::regex(R"(^[ \t]*export[ \t]+default[ \t]+([^;\r\n]*);?)",
                 std::regex::ECMAScript | std::regex::multiline),
      [](const std::smatch& match) {
        return "__esm_exports.default = " + match[1].str() + ";";
      });

  return result;
}

}  // namespace

bool IsCJSModule(const std::string& path) {
  return path.size() >= 4 && path.compare(path.size() - 4, 4, ".cjs") == 0;
}

std::string FindNearestPackageJson(const std::filesystem::path& startDir) {
  std::filesystem::path current = startDir;

  while (!current.empty() && current != current.root_path()) {
    std::filesystem::path packagePath = current / "package.json";
    std::error_code ec;
    if (std::filesystem::exists(packagePath, ec) && !ec) {
      return packagePath.string();
    }
    current = current.parent_path();
  }

  return "";
}

bool IsPackageTypeModule(const std::string& packageJsonPath) {
  auto& cache = PackageTypeCache();
  auto cacheIt = cache.find(packageJsonPath);
  if (cacheIt != cache.end()) {
    return cacheIt->second;
  }

  bool isModule = false;

  std::ifstream file(packageJsonPath);
  if (file.is_open()) {
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // Only the top-level "type" key matters; a full JSON parse is not needed.
    size_t typePos = content.find("\"type\"");
    if (typePos != std::string::npos) {
      size_t colonPos = content.find(':', typePos + 6);
      if (colonPos != std::string::npos) {
        size_t valueStart = content.find('"', colonPos + 1);
        if (valueStart != std::string::npos) {
          size_t valueEnd = content.find('"', valueStart + 1);
          if (valueEnd != std::string::npos) {
            std::string typeValue =
                content.substr(valueStart + 1, valueEnd - valueStart - 1);
            isModule = (typeValue == "module");
          }
        }
      }
    }
  }

  cache[packageJsonPath] = isModule;
  return isModule;
}

bool ShouldTreatJsAsESModule(const std::string& path) {
  std::filesystem::path filePath(path);
  std::string packageJson = FindNearestPackageJson(filePath.parent_path());

  if (!packageJson.empty()) {
    return IsPackageTypeModule(packageJson);
  }

  return false;
}

bool IsESModulePath(const std::string& path) {
  if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".mjs") == 0) {
    return true;
  }

  if (IsCJSModule(path)) {
    return false;
  }

  if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".js") == 0) {
    return ShouldTreatJsAsESModule(path);
  }

  return false;
}

void ClearPackageTypeCache() { PackageTypeCache().clear(); }

}  // namespace nativescript::esm
