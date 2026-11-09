#import "TNSObjCTypes.h"

#import <objc/message.h>

void TNSFunctionWithCFTypeRefArgument(CFTypeRef x) {
  NSString* str = (__bridge NSString*)x;
  TNSLog([NSString stringWithFormat:@"%@", str]);
}

CFTypeRef TNSFunctionWithSimpleCFTypeRefReturn() { return CFSTR("test"); }

CFTypeRef TNSFunctionWithCreateCFTypeRefReturn() {
  return CFStringCreateWithCString(kCFAllocatorDefault, "test", kCFStringEncodingUTF8);
}

#if TARGET_OS_IPHONE
double TNSRNMeasureNativeUITabBarControllerNew(int iterations, int touchView) {
  if (iterations <= 0) {
    return 0;
  }

  Class controllerClass = NSClassFromString(@"UITabBarController");
  if (controllerClass == nil) {
    return 0;
  }

  SEL newSelector = sel_registerName("new");
  SEL viewSelector = sel_registerName("view");
  uintptr_t sink = 0;
  CFAbsoluteTime startedAt = CFAbsoluteTimeGetCurrent();
  for (int i = 0; i < iterations; i++) {
    @autoreleasepool {
      id controller = ((id (*)(Class, SEL))objc_msgSend)(controllerClass, newSelector);
      if (touchView) {
        sink ^= (uintptr_t)((id (*)(id, SEL))objc_msgSend)(controller, viewSelector);
      } else {
        sink ^= (uintptr_t)controller;
      }
#if !__has_feature(objc_arc)
      ((void (*)(id, SEL))objc_msgSend)(controller, sel_registerName("release"));
#endif
    }
  }
  if (sink == 0xfeedface) {
    TNSLog(@"unreachable");
  }
  return (CFAbsoluteTimeGetCurrent() - startedAt) * 1000.0;
}

double TNSRNMeasureNativeUIColorFactory(int iterations) {
  if (iterations <= 0) {
    return 0;
  }

  Class colorClass = NSClassFromString(@"UIColor");
  if (colorClass == nil) {
    return 0;
  }

  SEL colorSelector = sel_registerName("colorWithRed:green:blue:alpha:");
  uintptr_t sink = 0;
  CFAbsoluteTime startedAt = CFAbsoluteTimeGetCurrent();
  for (int i = 0; i < iterations; i++) {
    @autoreleasepool {
      id color = ((id (*)(Class, SEL, double, double, double, double))objc_msgSend)(
          colorClass, colorSelector, 0.1, 0.2, 0.3, 1.0);
      sink ^= (uintptr_t)color;
    }
  }
  if (sink == 0xfeedface) {
    TNSLog(@"unreachable");
  }
  return (CFAbsoluteTimeGetCurrent() - startedAt) * 1000.0;
}
#else
double TNSRNMeasureNativeUITabBarControllerNew(int iterations, int touchView) {
  (void)iterations;
  (void)touchView;
  return 0;
}

double TNSRNMeasureNativeUIColorFactory(int iterations) {
  (void)iterations;
  return 0;
}
#endif

@implementation TNSRNDelegateProbe

- (void)fire {
  NSString* value = self.value ?: @"delegate";
  [self.delegate probeDidFire:self value:value];
}

- (NSString*)fireOnBackground {
  __block NSString* nativeThreadHash = nil;
  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    nativeThreadHash =
        [NSString stringWithFormat:@"%lu", (unsigned long)NSThread.currentThread.hash];
    [self fire];
    dispatch_semaphore_signal(semaphore);
  });
  dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
  return nativeThreadHash;
}

@end

@implementation TNSRNObservableProbe
@end

@implementation TNSObjCTypes
+ (void)methodWithComplexBlock:(id (^)(int, id, SEL, NSObject*, TNSOStruct))block {
  TNSOStruct str = {5, 6, 7};
  id result = block(1, @2, @selector(init), @[ @3, @4 ], str);
  TNSLog([NSString stringWithFormat:@"\n%@", NSStringFromClass([result class])]);
}

+ (id)methodWithObject:(id)x {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundeclared-selector"
  NSDictionary* result = [x performSelector:@selector(getData)];
#pragma clang diagnostic pop
  TNSLog([NSString stringWithFormat:@"%s %d", [(NSString*)result[@"a"] UTF8String],
                                    [(NSNumber*)result[@"b"] intValue]]);
  return x;
}

- (void)methodWithIdOutParameter:(NSString**)value {
  TNSLog([NSString stringWithFormat:@"%@", *value]);
  *value = @"test";
}

- (void)methodWithLongLongOutParameter:(long long*)value {
  TNSLog([NSString stringWithFormat:@"%lld", *value]);
  *value = 1;
}

- (void)methodWithStructOutParameter:(TNSOStruct*)value {
  TNSLog([NSString stringWithFormat:@"%d %d %d", value->x, value->y, value->z]);
  *value = (TNSOStruct){4, 5, 6};
}

- (void)methodWithSimpleBlock:(void (^)(void))block {
  block();
}

- (NSString*)methodWithSimpleBlockOnBackground:(void (^)(NSString* callerThreadHash))block {
  __block NSString* nativeThreadHash = nil;
  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    nativeThreadHash =
        [NSString stringWithFormat:@"%lu", (unsigned long)NSThread.currentThread.hash];
    block(nativeThreadHash);
    dispatch_semaphore_signal(semaphore);
  });
  dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
  return nativeThreadHash;
}

- (void)methodWithSimpleBlockOnBackgroundAsync:(void (^)(NSString* callerThreadHash))block {
  void (^blockCopy)(NSString*) = [block copy];
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    NSString* nativeThreadHash =
        [NSString stringWithFormat:@"%lu", (unsigned long)NSThread.currentThread.hash];
    blockCopy(nativeThreadHash);
#if !__has_feature(objc_arc)
    [blockCopy release];
#endif
  });
}

- (void)methodRetainingBlock:(void (^)(void))block {
  _retainedBlock = block;
}
- (void)methodCallRetainingBlock {
  _retainedBlock();
}
- (void)methodReleaseRetainingBlock {
  _retainedBlock = NULL;
}

- (void)methodWithComplexBlock:(id (^)(int, id, SEL, NSObject*, TNSOStruct))block {
  TNSOStruct str = {5, 6, 7};
  id result = block(1, @2, @selector(init), @[ @3, @4 ], str);
  TNSLog([NSString stringWithFormat:@"\n%@", NSStringFromClass([result class])]);
}

- (NumberReturner)methodWithBlockScope:(int)number {
  return ^(int a, int b, int c) {
    return (number + a + b + c);
  };
}

- (id)methodReturningBlockAsId:(int)number {
  return ^(int a, int b, int c) {
    return (number + a + b + c);
  };
}

- (NSDate*)methodWithNSDate:(NSDate*)date {
  TNSLog(date.description);
  return date;
}

- (void (^)(void))methodWithBlock:(void (^)(void))block {
  if (block) {
    block();
  }
  return block;
}

- (NSArray*)methodWithNSArray:(NSArray*)array {
  for (id x in array) {
    TNSLog([NSString stringWithFormat:@"%@", x]);
  }
  return array;
}

- (id)methodWithNSArrayWrappingDictionary:(id)array {
  NSArray* arr = (NSArray*)array;
  id result = [arr objectAtIndex:0];
  return result;
}

- (NSDictionary*)methodWithNSDictionary:(NSDictionary*)dictionary {
  for (id x in dictionary) {
    TNSLog([NSString stringWithFormat:@"%@ %@", x, dictionary[x]]);
  }

  return dictionary;
}

- (NSData*)methodWithNSData:(NSData*)data {
  NSString* string = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
  TNSLog(string);
  return data;
}

- (NSDecimalNumber*)methodWithNSDecimalNumber:(NSDecimalNumber*)number {
  TNSLog([number stringValue]);
  return number;
}

- (NSNumber*)methodWithNSCFBool {
  return @YES;
}

- (NSNull*)methodWithNSNull {
  return [NSNull null];
}

- (NSArray*)getNSArrayOfNSURLs {
  NSURL* url1 = [NSURL URLWithString:(@"dummy://url1")];
  NSURL* url2 = [NSURL URLWithString:(@"dummy://url2")];
  NSArray* urlArray = @[ url1, url2 ];

  return urlArray;
}

@end
