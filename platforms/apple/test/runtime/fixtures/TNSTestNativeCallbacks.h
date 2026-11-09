#include <TargetConditionals.h>
#include "Api/TNSApi.h"
#include "Interfaces/TNSInheritance.h"
#include "Marshalling/TNSRecords.h"

#if TARGET_OS_OSX
@class NSView;
typedef NSView TNSPlatformView;
#else
@class UIView;
typedef UIView TNSPlatformView;
#endif

@interface TNSTestNativeCallbacks : NSObject

+ (void)inheritanceMethodCalls:(TNSDerivedInterface*)derivedInterface;

+ (void)inheritanceConstructorCalls:(Class)JSDerivedInterface;

+ (void)inheritancePropertyCalls:(TNSDerivedInterface*)object;

+ (void)inheritanceVoidSelector:(id)object;

+ (id)inheritanceVariadicSelector:(id)object;

+ (void)inheritanceOptionalProtocolMethodsAndCategories:(TNSIDerivedInterface*)object;

+ (void)apiCustomGetterAndSetter:(TNSApi*)object;

+ (void)apiOverrideWithCustomGetterAndSetter:(TNSApi*)object;

+ (void)apiReadonlyPropertyInProtocolAndOverrideWithSetterInInterface:(TNSPlatformView*)object;

+ (void)apiDescriptionOverride:(id)object;

+ (void)apiNSErrorOverride:(TNSApi*)object;

+ (void)apiNSErrorExpose:(TNSApi*)object;

+ (void)protocolImplementationMethods:(id<TNSBaseProtocol1, NSObject>)object;

+ (void)categoryProtocolImplementationMethods:(id<TNSBaseCategoryProtocol1, NSObject>)object;

+ (void)protocolImplementationProtocolInheritance:(id<TNSBaseProtocol2, NSObject>)object;

+ (void)protocolImplementationOptionalMethods:(id<TNSBaseProtocol2, NSObject>)object;

+ (void)protocolImplementationProperties:(id<TNSBaseProtocol1, NSObject>)object;

+ (BOOL)protocolWithNameConflict:(id<TNSPropertyMethodConflictProtocol, NSObject>)object;

+ (TNSSimpleStruct)recordsSimpleStruct:(TNSSimpleStruct)object;

+ (TNSStruct16)recordsStruct16:(TNSStruct16)object;

+ (TNSStruct24)recordsStruct24:(TNSStruct24)object;

+ (TNSStruct32)recordsStruct32:(TNSStruct32)object;

+ (TNSNestedStruct)recordsNestedStruct:(TNSNestedStruct)object;

+ (TNSStructWithArray)recordsStructWithArray:(TNSStructWithArray)object;

+ (TNSNestedAnonymousStruct)recordsNestedAnonymousStruct:(TNSNestedAnonymousStruct)object;

+ (TNSComplexStruct)recordsComplexStruct:(TNSComplexStruct)object;

+ (void)recordsPointer:(TNSSimpleStruct*)object;

+ (void)apiNSMutableArrayMethods:(NSMutableArray*)object;

+ (void)apiSwizzle:(TNSSwizzleKlass*)object;

+ (NSString*)callRecursively:(NSString* (^)())block;

+ (NSString*)callOnThread:(NSString* (^)())block;

- (void (^)())getBlock;
- (void (^)())getBlockFromNative;

@end

// Fixtures for the "protocol-only method metadata" resolution path: a
// selector declared solely on an Objective-C protocol, with the conforming
// class's *public* header (the only thing the metadata generator parses)
// never declaring that conformance. This mirrors
// UIViewControllerTransitionCoordinator, where every method lives on the
// protocol and the concrete class is private -- interop.Block(fn, encoding)
// is the documented workaround for exactly this shape of metadata gap.
@protocol TNSProtocolOnlyBlockProtocol <NSObject>
- (void)invokeBlockCallback:(void (^)(NSInteger value))callback;
- (NSInteger)invokeBlockCallbackReturningSum:(NSInteger (^)(NSInteger a, NSInteger b))callback;
@end

// Deliberately declared WITHOUT <TNSProtocolOnlyBlockProtocol> here: the
// metadata generator only ever sees this public interface. Conformance is
// added in the .m via a class extension, which is invisible to the
// metadata generator but real at the Objective-C runtime level -- the same
// gap as a private Apple class implementing a public protocol.
@interface TNSProtocolOnlyMembersImplementor : NSObject
@end

// Control case: the same protocol, but conformance IS declared on the
// public interface, so metadata already resolves it today. Used to prove
// the fix doesn't regress (or change the behavior of) the already-working
// path.
@interface TNSProtocolDeclaredMembersImplementor : NSObject <TNSProtocolOnlyBlockProtocol>
@end

@interface TNSProtocolOnlyMembersFactory : NSObject
+ (id<TNSProtocolOnlyBlockProtocol>)createImplementorWithHiddenConformance;
+ (id<TNSProtocolOnlyBlockProtocol>)createImplementorWithDeclaredConformance;
@end
