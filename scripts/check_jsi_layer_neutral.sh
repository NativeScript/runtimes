#!/bin/bash
# The shared JSI layer (NativeScript/jsi) is compiled by the Apple runtime today
# and by the Android runtime in future. Android has no Objective-C compiler and
# no Foundation framework, so a single stray #import there does not merely warn:
# it makes the file unbuildable on Android.
#
# This is easy to regress and hard to notice, because `#import
# <Foundation/Foundation.h>` compiles perfectly well under -x c++ on macOS. A
# green Apple build proves nothing. Hence this check.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LAYER="$ROOT/NativeScript/jsi"

if [ ! -d "$LAYER" ]; then
  echo "check_jsi_layer_neutral: $LAYER does not exist yet; nothing to check."
  exit 0
fi

# Objective-C constructs, and platform headers that only exist on Apple.
PATTERN='#import|@interface|@implementation|@protocol|@selector|objc_msgSend|objc_get|<Foundation/|<UIKit/|<CoreFoundation/'

if MATCHES=$(grep -rnE "$PATTERN" "$LAYER" 2>/dev/null); then
  echo "ERROR: Objective-C found in the shared JSI layer (NativeScript/jsi)."
  echo "This code must compile on Android, which has no Objective-C compiler."
  echo "Move the Apple-specific part back under NativeScript/ffi/objc/."
  echo
  echo "$MATCHES"
  exit 1
fi

# .mm is Objective-C++ by definition and cannot build on Android.
if MM=$(find "$LAYER" -name '*.mm' -print 2>/dev/null | grep .); then
  echo "ERROR: .mm sources in the shared JSI layer; these cannot build on Android:"
  echo "$MM"
  exit 1
fi

echo "check_jsi_layer_neutral: OK ($(find "$LAYER" -type f | wc -l | tr -d ' ') files, no Objective-C)"
