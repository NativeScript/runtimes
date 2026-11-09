#import "TNSDeclarationConflicts.h"
#import <Foundation/Foundation.h>

@implementation TNSInterfaceProtocolConflict
@end

void TNSStructFunctionConflict(struct TNSStructFunctionConflict str) {
    TNSLog(@(str.x).stringValue);
}

const int TNSStructVarConflict = 42;
