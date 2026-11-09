#include <cassert>
#include <filesystem>
#include <fstream>

#include "MetadataFilter.h"

using metagen::MetadataFilter;

int main() {
  assert(MetadataFilter::wildcardMatch("*", ""));
  assert(MetadataFilter::wildcardMatch("UI*", "UIView"));
  assert(MetadataFilter::wildcardMatch("NS?rray", "NSArray"));
  assert(!MetadataFilter::wildcardMatch("UI*", "NSObject"));

  std::filesystem::path root =
      std::filesystem::temp_directory_path() / "metadata-filter-tests";
  std::filesystem::create_directories(root);
  std::filesystem::path whitelist = root / "whitelist.mdg";
  std::filesystem::path blacklist = root / "blacklist.mdg";

  {
    std::ofstream output(whitelist);
    output << "Foundation:NSObject\n";
    output << "UIKit:UI*\n";
  }
  {
    std::ofstream output(blacklist);
    output << "UIKit:UIWebView\n";
    output << "Foundation:NSObject\n";
  }

  MetadataFilter filter;
  filter.configure(whitelist, blacklist);
  assert(filter.isAllowed("Foundation", "NSObject"));
  assert(filter.isAllowed("UIKit", "UIView"));
  assert(!filter.isAllowed("UIKit", "UIWebView"));
  assert(!filter.isAllowed("AppKit", "NSView"));
  assert(filter.isAllowed("Foundation", "NSObject"));

  std::filesystem::remove_all(root);
  return 0;
}
