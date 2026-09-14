#include <TargetConditionals.h>

typedef struct TNSOStruct {
  int x;
  int y;
  int z;
} TNSOStruct;

void TNSFunctionWithCFTypeRefArgument(CFTypeRef x);

CFTypeRef TNSFunctionWithSimpleCFTypeRefReturn() CF_RETURNS_NOT_RETAINED;
CFTypeRef TNSFunctionWithCreateCFTypeRefReturn() CF_RETURNS_RETAINED;

typedef int (^NumberReturner)(int, int, int);

#if TARGET_OS_IPHONE
double TNSRNMeasureNativeUITabBarControllerNew(int iterations, int touchView);
double TNSRNMeasureNativeUIColorFactory(int iterations);
#endif

@protocol TNSRNDelegateProbeDelegate <NSObject>
- (void)probeDidFire:(id)probe value:(NSString*)value;
@end

@interface TNSRNDelegateProbe : NSObject
@property(nonatomic, weak) id<TNSRNDelegateProbeDelegate> delegate;
@property(nonatomic, copy) NSString* value;
- (void)fire;
- (NSString*)fireOnBackground;
@end

@interface TNSRNObservableProbe : NSObject
@property(nonatomic, copy) NSString* value;
@end

@interface TNSObjCTypes : NSObject
@property(nonatomic, copy) void (^retainedBlock)(void);
+ (void)methodWithComplexBlock:(id (^)(int, id, SEL, NSObject*, TNSOStruct))block;
+ (id)methodWithObject:(id)x;

- (void)methodWithIdOutParameter:(NSString**)value;
- (void)methodWithLongLongOutParameter:(long long*)value;
- (void)methodWithStructOutParameter:(TNSOStruct*)value;

- (void)methodWithSimpleBlock:(void (^)(void))block;
- (NSString*)methodWithSimpleBlockOnBackground:(void (^)(NSString* callerThreadHash))block;
- (void)methodWithSimpleBlockOnBackgroundAsync:(void (^)(NSString* callerThreadHash))block;
- (void)methodWithComplexBlock:(id (^)(int, id, SEL, NSObject*, TNSOStruct))block;

- (void)methodRetainingBlock:(void (^)(void))block;
- (void)methodCallRetainingBlock;
- (void)methodReleaseRetainingBlock;

- (NumberReturner)methodWithBlockScope:(int)number;
- (id)methodReturningBlockAsId:(int)number;

- (NSDate*)methodWithNSDate:(NSDate*)date;
- (void (^)(void))methodWithBlock:(void (^)(void))block;
- (NSArray*)methodWithNSArray:(NSArray*)array;
- (id)methodWithNSArrayWrappingDictionary:(id)array;
- (NSDictionary*)methodWithNSDictionary:(NSDictionary*)dictionary;
- (NSData*)methodWithNSData:(NSData*)data;
- (NSDecimalNumber*)methodWithNSDecimalNumber:(NSDecimalNumber*)number;
- (NSNumber*)methodWithNSCFBool;
- (NSNull*)methodWithNSNull;
- (NSArray*)getNSArrayOfNSURLs;
@end
