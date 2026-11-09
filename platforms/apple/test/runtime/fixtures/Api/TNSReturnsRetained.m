#import "TNSReturnsRetained.h"

id functionReturnsNSRetained() {
    return [[NSObject alloc] init];
}
id functionReturnsCFRetained() {
    return [[NSObject alloc] init];
}
CFTypeRef functionImplicitCreate() {
#if __has_feature(objc_arc)
    return CFBridgingRetain([[NSObject alloc] init]);
#else
    return (CFTypeRef)[[NSObject alloc] init];
#endif
}
id functionExplicitCreateNSObject() {
    return [[NSObject alloc] init];
}

@implementation TNSReturnsRetained
+ (id)methodReturnsNSRetained {
    return [[NSObject alloc] init];
}
+ (id)methodReturnsCFRetained {
    return [[NSObject alloc] init];
}
+ (id)newNSObjectMethod {
    return [[TNSReturnsRetained alloc] init];
}
@end
