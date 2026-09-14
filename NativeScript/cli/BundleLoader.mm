#include "BundleLoader.h"
#include <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <stdlib.h>

// Check if Resources/app/ exists, then load package.json["main"] || app/index.js full file path

static NSString* resourcesPathForExecutable(NSString* executablePath) {
  if (executablePath == nil || [executablePath length] == 0) {
    return nil;
  }

  NSString* standardizedPath = [executablePath stringByStandardizingPath];
  NSString* macOSPath = [standardizedPath stringByDeletingLastPathComponent];
  NSString* contentsPath = [macOSPath stringByDeletingLastPathComponent];
  if ([[macOSPath lastPathComponent] isEqualToString:@"MacOS"] &&
      [[contentsPath lastPathComponent] isEqualToString:@"Contents"]) {
    return [contentsPath stringByAppendingPathComponent:@"Resources"];
  }

  return nil;
}

static bool shouldLogBundleResolution() {
  return getenv("NS_BUNDLE_LOADER_DEBUG") != nullptr;
}

static void addCandidatePath(NSMutableArray<NSString*>* candidates, NSString* path) {
  if (path == nil || [path length] == 0) {
    return;
  }

  NSString* standardizedPath = [path stringByStandardizingPath];
  if (![candidates containsObject:standardizedPath]) {
    [candidates addObject:standardizedPath];
  }
}

static std::string resolveMainPathInResources(NSString* resourcesPath) {
  NSFileManager* fileManager = [NSFileManager defaultManager];
  NSString* appPath = [resourcesPath stringByAppendingPathComponent:@"app"];
  BOOL isDir;

  if ([fileManager fileExistsAtPath:appPath isDirectory:&isDir] && isDir) {
    if (shouldLogBundleResolution()) {
      NSLog(@"NativeScript BundleLoader checking app path: %@", appPath);
    }
    NSString* packageJsonPath = [appPath stringByAppendingPathComponent:@"package.json"];
    if ([fileManager fileExistsAtPath:packageJsonPath]) {
      NSData* jsonData = [NSData dataWithContentsOfFile:packageJsonPath];
      NSError* error = nil;
      NSDictionary* packageDict = [NSJSONSerialization JSONObjectWithData:jsonData
                                                                  options:0
                                                                    error:&error];
      if (error == nil) {
        NSString* mainEntry = packageDict[@"main"];
        if (shouldLogBundleResolution()) {
          NSLog(@"NativeScript BundleLoader package main: %@ from %@", mainEntry, packageJsonPath);
        }
        if (mainEntry != nil) {
          NSString* mainPath = [appPath stringByAppendingPathComponent:mainEntry];
          if ([fileManager fileExistsAtPath:mainPath]) {
            if (shouldLogBundleResolution()) {
              NSLog(@"NativeScript BundleLoader resolved main: %@", mainPath);
            }
            return std::string([mainPath UTF8String]);
          }

          if ([[mainEntry pathExtension] length] == 0) {
            NSString* mainPathMjs = [mainPath stringByAppendingPathExtension:@"mjs"];
            if ([fileManager fileExistsAtPath:mainPathMjs]) {
              if (shouldLogBundleResolution()) {
                NSLog(@"NativeScript BundleLoader resolved main: %@", mainPathMjs);
              }
              return std::string([mainPathMjs UTF8String]);
            }

            NSString* mainPathJs = [mainPath stringByAppendingPathExtension:@"js"];
            if ([fileManager fileExistsAtPath:mainPathJs]) {
              if (shouldLogBundleResolution()) {
                NSLog(@"NativeScript BundleLoader resolved main: %@", mainPathJs);
              }
              return std::string([mainPathJs UTF8String]);
            }
          }
        }
      } else if (shouldLogBundleResolution()) {
        NSLog(@"NativeScript BundleLoader failed to parse %@: %@", packageJsonPath, error);
      }
    }

    // Fallback to app/index.js
    NSString* indexPath = [appPath stringByAppendingPathComponent:@"index.js"];
    if ([fileManager fileExistsAtPath:indexPath]) {
      if (shouldLogBundleResolution()) {
        NSLog(@"NativeScript BundleLoader resolved fallback main: %@", indexPath);
      }
      return std::string([indexPath UTF8String]);
    }
  } else if (shouldLogBundleResolution()) {
    NSLog(@"NativeScript BundleLoader skipped resources path: %@ appPath=%@ exists=%d isDir=%d",
          resourcesPath,
          appPath,
          [fileManager fileExistsAtPath:appPath],
          isDir);
  }

  return "";
}

std::string resolveMainPath() {
  NSMutableArray<NSString*>* candidates = [NSMutableArray array];
  addCandidatePath(candidates, [[NSBundle mainBundle] resourcePath]);
  addCandidatePath(candidates, resourcesPathForExecutable([[NSBundle mainBundle] executablePath]));

  NSArray<NSString*>* arguments = [[NSProcessInfo processInfo] arguments];
  if ([arguments count] > 0) {
    addCandidatePath(candidates, resourcesPathForExecutable([arguments objectAtIndex:0]));
  }

  uint32_t executablePathLength = 0;
  _NSGetExecutablePath(nullptr, &executablePathLength);
  if (executablePathLength > 0) {
    char* executablePathBuffer = static_cast<char*>(malloc(executablePathLength));
    if (executablePathBuffer != nullptr) {
      if (_NSGetExecutablePath(executablePathBuffer, &executablePathLength) == 0) {
        addCandidatePath(candidates, resourcesPathForExecutable([NSString stringWithUTF8String:executablePathBuffer]));
      }
      free(executablePathBuffer);
    }
  }

  NSString* currentDirectory = [[NSFileManager defaultManager] currentDirectoryPath];
  addCandidatePath(candidates, currentDirectory);
  addCandidatePath(candidates, [currentDirectory stringByAppendingPathComponent:@"Resources"]);
  addCandidatePath(candidates, [[currentDirectory stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"Resources"]);

  for (NSString* resourcesPath in candidates) {
    if (shouldLogBundleResolution()) {
      NSLog(@"NativeScript BundleLoader candidate resources: %@", resourcesPath);
    }
    std::string mainPath = resolveMainPathInResources(resourcesPath);
    if (!mainPath.empty()) {
      return mainPath;
    }
  }

  return "";
}
