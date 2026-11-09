#include <TargetConditionals.h>

#if TARGET_OS_OSX
#include <AppKit/AppKit.h>
#define UIView NSView
#define UIColor NSColor
#else
#include <UIKit/UIKit.h>
#endif

#include "TNSTestCommon.h"
