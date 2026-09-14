#import "TNSRuntimeOnlyStructs.h"

typedef struct {
  double x;
  double y;
} TNSRuntimeOnlyPair;

@interface TNSRuntimeOnlyStructProvider : NSObject
@end

id TNSRuntimeOnlyStructProviderMake(void) {
  return [[TNSRuntimeOnlyStructProvider alloc] init];
}

@implementation TNSRuntimeOnlyStructProvider {
  TNSRuntimeOnlyPair _pair;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _pair = (TNSRuntimeOnlyPair){12.5, 25.5};
  }
  return self;
}

- (TNSRuntimeOnlyPair)runtimeOnlyPair {
  return _pair;
}

- (void)setRuntimeOnlyPair:(TNSRuntimeOnlyPair)pair {
  _pair = pair;
}

@end
