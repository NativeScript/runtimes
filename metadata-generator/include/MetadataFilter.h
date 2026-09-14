#pragma once

#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace metagen {

class MetadataFilter {
 public:
  void configure(const std::string& whitelistFile,
                 const std::string& blacklistFile) {
    whitelistDefined = !whitelistFile.empty();
    whitelist = readPatterns(whitelistFile);
    blacklist = readPatterns(blacklistFile);
  }

  bool active() const {
    return whitelistDefined || !blacklist.globalSymbols.empty() ||
           !blacklist.patterns.empty();
  }

  bool isAllowed(std::string_view module, std::string_view symbol) const {
    // Preserve the original NativeScript filter invariant: NSObject is
    // required by both the runtime and the generator and is never removed.
    if (symbol == "NSObject") {
      return true;
    }
    bool enabled = !whitelistDefined || matchesAny(whitelist, module, symbol);
    return enabled && !matchesAny(blacklist, module, symbol);
  }

  static bool wildcardMatch(std::string_view pattern,
                            std::string_view value) {
    size_t patternIndex = 0;
    size_t valueIndex = 0;
    size_t starIndex = std::string_view::npos;
    size_t starValueIndex = 0;

    while (valueIndex < value.size()) {
      if (patternIndex < pattern.size() &&
          (pattern[patternIndex] == '?' ||
           pattern[patternIndex] == value[valueIndex])) {
        patternIndex++;
        valueIndex++;
      } else if (patternIndex < pattern.size() &&
                 pattern[patternIndex] == '*') {
        starIndex = patternIndex++;
        starValueIndex = valueIndex;
      } else if (starIndex != std::string_view::npos) {
        patternIndex = starIndex + 1;
        valueIndex = ++starValueIndex;
      } else {
        return false;
      }
    }

    while (patternIndex < pattern.size() && pattern[patternIndex] == '*') {
      patternIndex++;
    }
    return patternIndex == pattern.size();
  }

 private:
  struct Pattern {
    std::string module;
    std::string symbol;
  };

  struct TransparentStringHash {
    using is_transparent = void;

    size_t operator()(std::string_view value) const {
      return std::hash<std::string_view>{}(value);
    }
  };

  struct PatternSet {
    // The bundle analyzer emits one `*:symbol` rule per unresolved native
    // symbol. Index that common case so filtering stays O(1) per declaration.
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>>
        globalSymbols;
    std::vector<Pattern> patterns;
  };

  static PatternSet readPatterns(const std::string& path) {
    PatternSet result;
    if (path.empty()) {
      return result;
    }

    std::ifstream input(path);
    if (!input) {
      throw std::invalid_argument("Metadata filter file not found: " + path);
    }

    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line.starts_with('#') || line.starts_with("//")) {
        continue;
      }

      size_t colon = line.find(':');
      Pattern pattern = {line.substr(0, colon),
                         colon == std::string::npos ? std::string()
                                                    : line.substr(colon + 1)};
      if (pattern.module == "*" &&
          pattern.symbol.find_first_of("*?") == std::string::npos) {
        result.globalSymbols.insert(std::move(pattern.symbol));
      } else {
        result.patterns.push_back(std::move(pattern));
      }
    }
    return result;
  }

  static bool matchesAny(const PatternSet& patternSet,
                         std::string_view module, std::string_view symbol) {
    if (patternSet.globalSymbols.contains(symbol)) {
      return true;
    }
    for (const Pattern& pattern : patternSet.patterns) {
      if ((pattern.module.empty() || wildcardMatch(pattern.module, module)) &&
          (pattern.symbol.empty() || wildcardMatch(pattern.symbol, symbol))) {
        return true;
      }
    }
    return false;
  }

  bool whitelistDefined = false;
  PatternSet whitelist;
  PatternSet blacklist;
};

}  // namespace metagen
