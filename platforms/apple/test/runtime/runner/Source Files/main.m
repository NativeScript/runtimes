#import <NativeScript/NativeScript.h>

extern char startOfMetadataSection __asm("section$start$__DATA$__TNSMetadata");
NativeScript* nativescript;

static NSString* ResolveBaseDir(void) {
    NSBundle* mainBundle = [NSBundle mainBundle];
    NSFileManager* fileManager = [NSFileManager defaultManager];

    NSMutableArray<NSString*>* seeds = [NSMutableArray array];
    if (mainBundle.resourcePath.length > 0) {
        [seeds addObject:mainBundle.resourcePath];
    }
    if (mainBundle.bundlePath.length > 0) {
        [seeds addObject:mainBundle.bundlePath];
    }
    if (mainBundle.executablePath.length > 0) {
        [seeds addObject:[mainBundle.executablePath stringByDeletingLastPathComponent]];
    }

    for (NSString* seed in seeds) {
        NSString* current = seed;
        for (NSInteger depth = 0; depth < 8; depth++) {
            if (current.length == 0) {
                break;
            }

            BOOL isDirectory = NO;
            NSString* appPath = [current stringByAppendingPathComponent:@"app"];
            if ([fileManager fileExistsAtPath:appPath isDirectory:&isDirectory] && isDirectory) {
                return current;
            }

            NSString* parent = [current stringByDeletingLastPathComponent];
            if ([parent isEqualToString:current]) {
                break;
            }
            current = parent;
        }
    }

    if (mainBundle.resourcePath.length > 0) {
        return mainBundle.resourcePath;
    }
    if (mainBundle.bundlePath.length > 0) {
        return mainBundle.bundlePath;
    }
    return @"";
}

int main(int argc, char *argv[]) {
    @autoreleasepool {
        void* metadataPtr = &startOfMetadataSection;

        NSString* baseDir = ResolveBaseDir();

        bool isDebug =
#ifdef DEBUG
            true;
#else
            false;
#endif

        Config* config = [[Config alloc] init];
        config.IsDebug = isDebug;
        config.LogToSystemConsole = YES;
        config.MetadataPtr = metadataPtr;
        config.BaseDir = baseDir;
        config.ArgumentsCount = argc;
        config.Arguments = argv;

        nativescript = [[NativeScript alloc] initWithConfig:config];
        [nativescript runMainApplication];

        return 0;
    }
}
