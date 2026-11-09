//
// Any changes in this file will be removed after you update your platform!
//
#import <Foundation/Foundation.h>
#import "NativeScriptStart.h"

int main(int argc, char *argv[]) {
   @autoreleasepool {
       [NativeScriptStart setup];
       [NativeScriptStart boot];
       return 0;
   }
}

