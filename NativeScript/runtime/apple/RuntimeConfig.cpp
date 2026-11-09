#include "RuntimeConfig.h"

struct RuntimeConfig RuntimeConfig = {.BaseDir = "",
                                      .ApplicationPath = "",
                                      .Arguments = {},
                                      .MetadataPtr = nullptr,
                                      .IsDebug = false,
                                      .LogToSystemConsole = false,
                                      .CustomLogCallback = nullptr};
