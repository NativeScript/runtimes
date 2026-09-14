#ifndef RuntimeConfig_h
#define RuntimeConfig_h

#include <sys/types.h>

#include <string>
#include <vector>

struct RuntimeConfig {
  std::string BaseDir;
  std::string ApplicationPath;
  std::vector<std::string> Arguments;
  void* MetadataPtr;
  bool IsDebug;
  bool LogToSystemConsole;
  void (*CustomLogCallback)(const char* message);
};

extern struct RuntimeConfig RuntimeConfig;

#endif /* RuntimeConfig_h */
