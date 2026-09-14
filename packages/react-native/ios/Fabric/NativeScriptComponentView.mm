#import "NativeScriptComponentView.h"

#import <objc/runtime.h>
#import <pthread.h>

#import <React/RCTConversions.h>
#import <React/RCTSurfaceTouchHandler.h>
#import <React/UIView+React.h>
#import <ReactCommon/RCTTurboModule.h>

#include <jsi/JSIDynamic.h>

#include <react/renderer/core/ConcreteState.h>
#include <react/renderer/core/ReactPrimitives.h>
#include <react/renderer/mounting/ShadowViewMutation.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "NativeApiJsi.h"
#include "NativeScriptComponentDescriptor.h"
#import "NativeScriptComponentRegistration.h"
#include "NativeScriptFabricGateway.h"

using namespace facebook::react;
using namespace nativescript;
namespace jsi = facebook::jsi;

namespace {

const void* NativeScriptWindowOverlayAssociationKey() {
  static char key;
  return &key;
}

const void* NativeScriptWindowOverlayTouchHandlerAssociationKey() {
  static char key;
  return &key;
}

const void* NativeScriptObjectMetadataAssociationKey() {
  static char key;
  return &key;
}

id NativeScriptObjectMetadataGetter(id object, SEL selector) {
  NSDictionary* metadata = objc_getAssociatedObject(object, NativeScriptObjectMetadataAssociationKey());
  return metadata[NSStringFromSelector(selector)];
}

bool NativeScriptIsWindowOverlay(UIView* view) {
  return [objc_getAssociatedObject(view, NativeScriptWindowOverlayAssociationKey()) boolValue];
}

UIView* NativeScriptFindDescendantView(UIView* root, Class targetClass) {
  if (root == nil || targetClass == Nil) {
    return nil;
  }
  if ([root isKindOfClass:targetClass]) {
    return root;
  }
  // Keep UIKit's traversal inside one synchronous native call. Private
  // navigation-bar subviews can be replaced during layout; exposing each
  // intermediate child to the UI JavaScript runtime leaves stale wrappers.
  for (UIView* child in root.subviews) {
    UIView* match = NativeScriptFindDescendantView(child, targetClass);
    if (match != nil) {
      return match;
    }
  }
  return nil;
}

void NativeScriptInstallWindowOverlayOrdering();
void NativeScriptInstallChildControllerTraitForwarding();

// Reads the hook mask + registered Fabric name stashed on this instance's
// class by NativeScriptRegisterFlavoredComponent; one associated-object
// read, not a lookup keyed by anything string-parsed.
uint32_t NativeScriptHookMaskForClass(Class cls) {
  NSNumber* stored = objc_getAssociatedObject(cls, NativeScriptFlavorHookMaskAssociationKey());
  return stored != nil ? (uint32_t)stored.unsignedIntegerValue : 0;
}

// Forward Insert, Remove, and Delete mutations as plain
// {type, tag, parentTag, index} values. Delete mutations do not have a
// meaningful parentTag, so TypeScript performs the final relevance check.
// Native code still skips UI-runtime work when no registered hook could
// observe a transaction:
// mountingTransactionDidMount needs Insert/Remove for either this component
// or one of its children. Including the component's own insertion is what
// lets leaf hosts perform their first post-mount attachment; container hosts
// retain the child-mutation fast path. willMount cannot be parentTag-gated,
// so it receives transactions containing any relevant mutation and lets the
// component definition decide whether the payload matters.
jsi::Array NativeScriptBuildMutationsArray(jsi::Runtime& rt,
                                           const facebook::react::MountingTransaction& transaction) {
  const auto& mutations = transaction.getMutations();
  std::vector<size_t> relevant;
  for (size_t i = 0; i < mutations.size(); i++) {
    auto type = mutations[i].type;
    if (type == facebook::react::ShadowViewMutation::Insert ||
        type == facebook::react::ShadowViewMutation::Remove ||
        type == facebook::react::ShadowViewMutation::Delete) {
      relevant.push_back(i);
    }
  }
  jsi::Array array(rt, relevant.size());
  for (size_t i = 0; i < relevant.size(); i++) {
    const auto& mutation = mutations[relevant[i]];
    const char* typeName = mutation.type == facebook::react::ShadowViewMutation::Insert ? "insert"
                           : mutation.type == facebook::react::ShadowViewMutation::Remove ? "remove"
                                                                                          : "delete";
    facebook::react::Tag tag = mutation.type == facebook::react::ShadowViewMutation::Insert
                                   ? mutation.newChildShadowView.tag
                                   : mutation.oldChildShadowView.tag;
    jsi::Object object(rt);
    object.setProperty(rt, "type", jsi::String::createFromUtf8(rt, typeName));
    object.setProperty(rt, "tag", (double)tag);
    object.setProperty(rt, "parentTag", (double)mutation.parentTag);
    object.setProperty(rt, "index", (double)mutation.index);
    array.setValueAtIndex(rt, i, object);
  }
  return array;
}

bool NativeScriptTransactionHasChildMutation(const facebook::react::MountingTransaction& transaction,
                                             facebook::react::Tag myTag) {
  for (const auto& mutation : transaction.getMutations()) {
    if (mutation.parentTag == myTag &&
        (mutation.type == facebook::react::ShadowViewMutation::Insert ||
         mutation.type == facebook::react::ShadowViewMutation::Remove)) {
      return true;
    }
  }
  return false;
}

bool NativeScriptTransactionTouchesTag(const facebook::react::MountingTransaction& transaction,
                                       facebook::react::Tag myTag) {
  for (const auto& mutation : transaction.getMutations()) {
    if (mutation.type != facebook::react::ShadowViewMutation::Insert &&
        mutation.type != facebook::react::ShadowViewMutation::Remove) {
      continue;
    }
    if (mutation.parentTag == myTag) {
      return true;
    }
    const auto childTag = mutation.type == facebook::react::ShadowViewMutation::Insert
                              ? mutation.newChildShadowView.tag
                              : mutation.oldChildShadowView.tag;
    if (childTag == myTag) {
      return true;
    }
  }
  return false;
}

// M1 review §5/#5: intentional, tiny, documented leak; the alternative is
// destructing a `jsi::Function` (whose destructor talks back to the Runtime
// that created it) against a Runtime that a reload may have already torn
// down, which is a use-after-free, not a hypothetical one (this is exactly
// the crash class NativeScriptFabricGateway.mm's own comment on the
// deleted `static std::shared_ptr<Function>` cache describes). Held forever
// in a process-lifetime vector rather than freed at an unsafe moment; this
// only happens on the (rare, dev-reload-only) generation-mismatch path, not
// on every scheduleOnMainQueue call.
void NativeScriptLeakScheduledCallback(std::shared_ptr<jsi::Function> callback) {
  static std::mutex mutex;
  static std::vector<std::shared_ptr<jsi::Function>>* leaked = new std::vector<std::shared_ptr<jsi::Function>>();
  std::lock_guard<std::mutex> lock(mutex);
  leaked->push_back(std::move(callback));
}

bool NativeScriptTransactionHasAnyRelevantMutation(const facebook::react::MountingTransaction& transaction,
                                                    facebook::react::Tag myTag) {
  if (NativeScriptTransactionHasChildMutation(transaction, myTag)) {
    return true;
  }
  for (const auto& mutation : transaction.getMutations()) {
    if (mutation.type == facebook::react::ShadowViewMutation::Delete) {
      return true;
    }
  }
  return false;
}

}  // namespace

@interface UIWindow (NativeScriptWindowOverlay)
- (void)nativeScript_windowOverlayDidAddSubview:(UIView*)subview;
@end

@implementation UIWindow (NativeScriptWindowOverlay)

- (void)nativeScript_windowOverlayDidAddSubview:(UIView*)subview {
  // After swizzling, this invokes UIWindow's previous implementation.
  [self nativeScript_windowOverlayDidAddSubview:subview];
  if (NativeScriptIsWindowOverlay(subview)) {
    return;
  }
  for (UIView* candidate in self.subviews.copy) {
    if (NativeScriptIsWindowOverlay(candidate)) {
      [self bringSubviewToFront:candidate];
    }
  }
}

@end

@protocol NativeScriptOrientationProviding <NSObject>
- (NSUInteger)nativeScriptEvaluateOrientationMask;
@end

@protocol NativeScriptWindowTraitProviding <NSObject>
- (BOOL)nativeScriptProvidesWindowTraits;
@end

@interface UIViewController (NativeScriptControllerTraitForwarding)
- (UIViewController*)nativeScript_childViewControllerForStatusBarStyle;
- (UIViewController*)nativeScript_childViewControllerForStatusBarHidden;
- (UIStatusBarAnimation)nativeScript_preferredStatusBarUpdateAnimation;
- (UIInterfaceOrientationMask)nativeScript_supportedInterfaceOrientations;
- (UIViewController*)nativeScript_childViewControllerForHomeIndicatorAutoHidden;
@end

@implementation UIViewController (NativeScriptControllerTraitForwarding)

- (UIViewController*)nativeScriptWindowTraitProvidingChild {
  UIViewController* child = self.childViewControllers.lastObject;
  if ([child respondsToSelector:@selector(nativeScriptProvidesWindowTraits)] &&
      [(id<NativeScriptWindowTraitProviding>)child nativeScriptProvidesWindowTraits]) {
    return child;
  }
  return nil;
}

- (UIViewController*)nativeScript_childViewControllerForStatusBarStyle {
  UIViewController* child = [self nativeScriptWindowTraitProvidingChild];
  return child != nil ? child : [self nativeScript_childViewControllerForStatusBarStyle];
}

- (UIViewController*)nativeScript_childViewControllerForStatusBarHidden {
  UIViewController* child = [self nativeScriptWindowTraitProvidingChild];
  return child != nil ? child : [self nativeScript_childViewControllerForStatusBarHidden];
}

- (UIStatusBarAnimation)nativeScript_preferredStatusBarUpdateAnimation {
  UIViewController* child = [self nativeScriptWindowTraitProvidingChild];
  return child != nil ? child.preferredStatusBarUpdateAnimation
                      : [self nativeScript_preferredStatusBarUpdateAnimation];
}

- (UIInterfaceOrientationMask)nativeScript_supportedInterfaceOrientations {
  UIViewController* child = self.childViewControllers.lastObject;
  if ([child respondsToSelector:@selector(nativeScriptEvaluateOrientationMask)]) {
    NSUInteger mask = [(id<NativeScriptOrientationProviding>)child nativeScriptEvaluateOrientationMask];
    if (mask != 0) {
      return (UIInterfaceOrientationMask)mask;
    }
    return [UIApplication.sharedApplication supportedInterfaceOrientationsForWindow:self.view.window];
  }
  // After swizzling, this invokes UIViewController's previous implementation.
  return [self nativeScript_supportedInterfaceOrientations];
}

- (UIViewController*)nativeScript_childViewControllerForHomeIndicatorAutoHidden {
  UIViewController* child = [self nativeScriptWindowTraitProvidingChild];
  return child != nil ? child : [self nativeScript_childViewControllerForHomeIndicatorAutoHidden];
}

@end

namespace {

void NativeScriptInstallWindowOverlayOrdering() {
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    Class windowClass = UIWindow.class;
    SEL originalSelector = @selector(didAddSubview:);
    Method inherited = class_getInstanceMethod(windowClass, originalSelector);
    // didAddSubview: is inherited from UIView. Exchanging that inherited
    // Method directly would replace UIView's implementation process-wide and
    // send the UIWindow-only alias to every view subclass. Materialize the
    // inherited implementation on UIWindow before swapping.
    class_addMethod(windowClass, originalSelector, method_getImplementation(inherited),
                    method_getTypeEncoding(inherited));
    Method original = class_getInstanceMethod(windowClass, originalSelector);
    Method replacement = class_getInstanceMethod(
        windowClass, @selector(nativeScript_windowOverlayDidAddSubview:));
    method_exchangeImplementations(original, replacement);
  });
}

void NativeScriptInstallChildControllerTraitForwarding() {
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    Class controllerClass = UIViewController.class;
    method_exchangeImplementations(
        class_getInstanceMethod(controllerClass, @selector(childViewControllerForStatusBarStyle)),
        class_getInstanceMethod(controllerClass, @selector(nativeScript_childViewControllerForStatusBarStyle)));
    method_exchangeImplementations(
        class_getInstanceMethod(controllerClass, @selector(childViewControllerForStatusBarHidden)),
        class_getInstanceMethod(controllerClass, @selector(nativeScript_childViewControllerForStatusBarHidden)));
    method_exchangeImplementations(
        class_getInstanceMethod(controllerClass, @selector(preferredStatusBarUpdateAnimation)),
        class_getInstanceMethod(controllerClass, @selector(nativeScript_preferredStatusBarUpdateAnimation)));
    method_exchangeImplementations(
        class_getInstanceMethod(controllerClass, @selector(supportedInterfaceOrientations)),
        class_getInstanceMethod(controllerClass, @selector(nativeScript_supportedInterfaceOrientations)));
    method_exchangeImplementations(
        class_getInstanceMethod(controllerClass, @selector(childViewControllerForHomeIndicatorAutoHidden)),
        class_getInstanceMethod(
            controllerClass, @selector(nativeScript_childViewControllerForHomeIndicatorAutoHidden)));
  });
}

}  // namespace

typedef jsi::Value (^NativeScriptArgBuilder)(jsi::Runtime& rt);

@implementation NativeScriptComponentView {
  BOOL _nsCreated;
  uint64_t _nsLifecycleToken;
  facebook::react::Tag _nsComponentTag;
  facebook::react::State::Shared _nsState;
  // M1 review §2/(d)/§5/#2 verification finding: `ctx.setContentSize`
  // called from `create()`; same bottom-up-mounting ordering hazard as
  // `_nsPendingEvents` above (`-updateProps:` and its `create()` call can
  // run before `-updateState:oldState:` ever has); was being silently
  // dropped: `nativeScriptSetContentSizeWidth:...` bailed on a null
  // `_nsState` with nothing buffered, so the FIRST (often only) call an
  // author makes from `create()` never reached Yoga even after `adopt()`
  // was implemented. Buffered here, applied the moment -updateState:
  // provides a real state pointer; proven on-sim: without this, `adopt()`
  // alone was not sufficient (0 layouts observed for the requested size).
  bool _nsHasPendingState;
  NativeScriptState _nsPendingState;
  bool _nsPendingStateUpdateSynchronously;
  // ctx.emit calls made before `_eventEmitter` exists (see the comment on
  // -nsEnsureCreated below for why that can happen) are buffered here and
  // flushed the moment -updateEventEmitter: makes one available; never
  // silently dropped.
  std::vector<std::pair<std::string, folly::dynamic>> _nsPendingEvents;
}

- (void)nativeScriptSetObjectMetadataName:(NSString*)name value:(id)value {
  NSCharacterSet* invalidCharacters =
      [[NSCharacterSet characterSetWithCharactersInString:
                           @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"] invertedSet];
  if (name.length == 0 ||
      [[NSCharacterSet decimalDigitCharacterSet] characterIsMember:[name characterAtIndex:0]] ||
      [name rangeOfCharacterFromSet:invalidCharacters].location != NSNotFound) {
    return;
  }

  SEL selector = NSSelectorFromString(name);
  Class componentClass = self.class;
  Method existingMethod = class_getInstanceMethod(componentClass, selector);
  if (existingMethod == nullptr) {
    class_addMethod(componentClass, selector, (IMP)NativeScriptObjectMetadataGetter, "@@:");
  } else if (method_getImplementation(existingMethod) != (IMP)NativeScriptObjectMetadataGetter) {
    return;
  }

  NSMutableDictionary* metadata = objc_getAssociatedObject(self, NativeScriptObjectMetadataAssociationKey());
  if (metadata == nil) {
    metadata = [NSMutableDictionary new];
    objc_setAssociatedObject(
        self, NativeScriptObjectMetadataAssociationKey(), metadata, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  if (value != nil) {
    metadata[name] = value;
  } else {
    [metadata removeObjectForKey:name];
  }
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    static const auto defaultProps = std::make_shared<const NativeScriptProps>();
    _props = defaultProps;
    _nsPendingStateUpdateSynchronously = false;
  }
  return self;
}

#pragma mark - Identity helpers

- (NSString*)nsComponentName {
  NSString* registered = objc_getAssociatedObject(self.class, NativeScriptFlavorNameAssociationKey());
  return registered != nil ? registered : NSStringFromClass(self.class);
}

- (BOOL)nsHasHook:(NativeScriptComponentHook)hook {
  return (NativeScriptHookMaskForClass(self.class) & (uint32_t)hook) != 0;
}

#pragma mark - Gateway dispatch

// Shared low-level entry point: wraps `self` as `ctx.view`, builds up to
// three additional args (a/b/c) via the supplied blocks (called INSIDE the
// runSync, so they may safely construct jsi::Values), and forwards to
// NativeScriptFabricGatewayDispatchComponentHook. The jsi::Value result is
// deliberately never returned to the ObjC caller; a JSI Value must not be
// touched once its runSync lock is released; callers that need the result
// use nsDispatchCreateHook/nsDispatchLayoutHook below, which interpret the
// result INSIDE the lambda and return a plain (POD) ObjC/C++ value instead.
- (void)nsDispatchHook:(NSString*)hookName
                      a:(nullable NativeScriptArgBuilder)aBuilder
                      b:(nullable NativeScriptArgBuilder)bBuilder
                      c:(nullable NativeScriptArgBuilder)cBuilder {
  std::string flavorName = self.nsComponentName.UTF8String ?: "";
  std::string hookNameStd = hookName.UTF8String ?: "";
  if (self.tag != 0) {
    _nsComponentTag = (facebook::react::Tag)self.tag;
  }
  double tag = (double)_nsComponentTag;
  NativeScriptComponentView* __unsafe_unretained weakSelf = self;
  nativescript::NativeScriptFabricGatewayRunSyncOnMain(
      [flavorName, hookNameStd, tag, weakSelf, aBuilder, bBuilder, cBuilder](jsi::Runtime& rt) -> bool {
        jsi::Value viewValue = weakSelf != nil
                                   ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakSelf)
                                   : jsi::Value::null();
        jsi::Value a = aBuilder != nil ? aBuilder(rt) : jsi::Value::undefined();
        jsi::Value b = bBuilder != nil ? bBuilder(rt) : jsi::Value::undefined();
        jsi::Value c = cBuilder != nil ? cBuilder(rt) : jsi::Value::undefined();
        try {
          nativescript::NativeScriptFabricGatewayDispatchComponentHook(
              rt, flavorName, tag, hookNameStd, viewValue, a, b, c);
        } catch (const jsi::JSError& error) {
          NSLog(@"NativeScript component %@ hook %@ failed: %s\n%s",
                [NSString stringWithUTF8String:flavorName.c_str()],
                [NSString stringWithUTF8String:hookNameStd.c_str()],
                error.getMessage().c_str(),
                error.getStack().c_str());
          throw;
        }
        return true;
      });
}

- (void)nsDispatchUnmountHookForChild:
                                   (UIView<RCTComponentViewProtocol>*)childComponentView
                                                          tag:(double)childTag
                                                        index:(double)indexValue {
  std::string flavorName = self.nsComponentName.UTF8String ?: "";
  double tag = (double)self.tag;
  NativeScriptComponentView* __unsafe_unretained weakSelf = self;
  UIView<RCTComponentViewProtocol>* __unsafe_unretained weakChild = childComponentView;
  nativescript::NativeScriptFabricGatewayRunSyncOnMain(
      [flavorName, tag, childTag, indexValue, weakSelf, weakChild](jsi::Runtime& rt) {
        jsi::Value viewValue = weakSelf != nil
                                   ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakSelf)
                                   : jsi::Value::null();
        jsi::Value childValue = weakChild != nil
                                    ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakChild)
                                    : jsi::Value::null();
        nativescript::NativeScriptFabricGatewayDispatchComponentHook(
            rt, flavorName, tag, "unmountChildComponentView", viewValue, childValue,
            jsi::Value(childTag), jsi::Value(indexValue));
        return true;
      });
}

- (void)nsDispatchWillMountHook:
    (const facebook::react::MountingTransaction&)transaction {
  std::string flavorName = self.nsComponentName.UTF8String ?: "";
  double tag = (double)self.tag;
  NativeScriptComponentView* __unsafe_unretained weakSelf = self;
  const facebook::react::MountingTransaction* transactionPtr = &transaction;
  nativescript::NativeScriptFabricGatewayRunSyncOnMain(
      [flavorName, tag, weakSelf, transactionPtr](jsi::Runtime& rt) {
        jsi::Value viewValue = weakSelf != nil
                                   ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakSelf)
                                   : jsi::Value::null();
        jsi::Value undef = jsi::Value::undefined();
        nativescript::NativeScriptFabricGatewayDispatchComponentHook(
            rt, flavorName, tag, "mountingTransactionWillMount", viewValue,
            jsi::Value(rt, NativeScriptBuildMutationsArray(rt, *transactionPtr)), undef, undef);
        return true;
      });
}

// `create` is unconditional (every `defineNativeComponent` spec provides
// it, per the worked example; ARCHITECTURE.md §6) and lazy: it runs on
// the FIRST Fabric lifecycle call this instance receives; called
// defensively at the top of every hook below, not just -updateProps: --
// rather than eagerly in `-initWithFrame:` (ARCHITECTURE.md §8.10's "eager
// attach" cost). "First call" is deliberately not assumed to be
// -updateProps: specifically: RCTMountingManager.mm inserts children
// bottom-up (a child's full Insert lifecycle, ending in
// `[parent mountChildComponentView:child]`, runs before the PARENT's own
// Insert lifecycle starts), so a container can see
// -mountChildComponentView: fire before its own -updateProps:/
// -updateEventEmitter: ever have. `_eventEmitter` may therefore still be
// null when `create`'s `ctx.emit` calls run; nativeScriptDispatchEventName:
// payload: buffers them; -updateEventEmitter: flushes the buffer the
// moment a real emitter exists. If the hook returns a wrapped UIView, it is
// installed as `contentView`.
- (void)nsEnsureCreated {
  if (_nsCreated) {
    return;
  }

  // Scope deferred callbacks to this exact Fabric view incarnation. This is
  // advanced before create() because create hooks may schedule work themselves.
  _nsLifecycleToken += 1;
  std::string flavorName = self.nsComponentName.UTF8String ?: "";
  double tag = (double)self.tag;
  _nsComponentTag = (facebook::react::Tag)self.tag;
  NativeScriptComponentView* __unsafe_unretained weakSelf = self;
  bool ran = false;
  void* contentViewPtr = nativescript::NativeScriptFabricGatewayRunSyncOnMain(
      [flavorName, tag, weakSelf](jsi::Runtime& rt) -> void* {
        jsi::Value viewValue = weakSelf != nil
                                   ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakSelf)
                                   : jsi::Value::null();
        jsi::Value undef = jsi::Value::undefined();
        jsi::Value result = nativescript::NativeScriptFabricGatewayDispatchComponentHook(
            rt, flavorName, tag, "create", viewValue, undef, undef, undef);
        if (!result.isObject()) {
          return nullptr;
        }
        return nativescript::NativeScriptUnwrapNativeObject(rt, result);
      },
      &ran);

  if (!ran) {
    // M1 review §4/(i): the gateway found no live UI runtime (e.g. `create`
    // requested before installUIRuntime() has run, or during the dead
    // window of a reload); do NOT latch `_nsCreated`, or this view is
    // permanently, silently dead: every later hook's own `nsEnsureCreated`
    // defensive call would see YES and skip forever. Leaving it NO means the
    // very next hook dispatch (updateProps/mountChild/etc., all of which
    // call this defensively) retries.
    _nsLifecycleToken += 1;
    return;
  }
  _nsCreated = YES;

  if (contentViewPtr != nullptr) {
    id maybeView = (__bridge id)contentViewPtr;
    if ([maybeView isKindOfClass:UIView.class]) {
      self.contentView = (UIView*)maybeView;
    }
  }
}

- (BOOL)nsDispatchLayoutHook:(const facebook::react::LayoutMetrics&)next
                          old:(const facebook::react::LayoutMetrics&)prev {
  std::string flavorName = self.nsComponentName.UTF8String ?: "";
  double tag = (double)self.tag;
  facebook::react::Rect nextFrame = next.frame;
  facebook::react::Rect prevFrame = prev.frame;
  NativeScriptComponentView* __unsafe_unretained weakSelf = self;
  bool ran = false;
  bool accept = nativescript::NativeScriptFabricGatewayRunSyncOnMain(
      [flavorName, tag, weakSelf, nextFrame, prevFrame](jsi::Runtime& rt) -> bool {
        jsi::Value viewValue = weakSelf != nil
                                   ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakSelf)
                                   : jsi::Value::null();
        auto frameObject = [&rt](const facebook::react::Rect& frame) -> jsi::Value {
          jsi::Object object(rt);
          object.setProperty(rt, "x", (double)frame.origin.x);
          object.setProperty(rt, "y", (double)frame.origin.y);
          object.setProperty(rt, "width", (double)frame.size.width);
          object.setProperty(rt, "height", (double)frame.size.height);
          return jsi::Value(rt, object);
        };
        jsi::Value undef = jsi::Value::undefined();
        jsi::Value result = nativescript::NativeScriptFabricGatewayDispatchComponentHook(
            rt, flavorName, tag, "updateLayoutMetrics", viewValue, frameObject(nextFrame),
            frameObject(prevFrame), undef);
        return !result.isBool() || result.getBool();
      },
      &ran);
  return ran ? (accept ? YES : NO) : YES;
}

#pragma mark - RCTComponentViewProtocol

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  // Generic/unflavored provider for the base class itself (never rendered
  // directly by JS; only the per-name dynamic subclasses created by
  // NativeScriptRegisterFlavoredComponent are). Registering the base class
  // is still useful: it is exactly the `constructor` every flavored
  // subclass's provider reuses.
  return concreteComponentDescriptorProvider<NativeScriptComponentDescriptor>();
}

- (void)nsForwardUpdateProps:(const std::shared_ptr<const NativeScriptProps>&)nextProps
                          old:(const std::shared_ptr<const NativeScriptProps>&)prevProps {
  if (![self nsHasHook:NativeScriptComponentHookUpdateProps] || nextProps == nullptr) {
    return;
  }
  folly::dynamic nextRaw = nextProps->rawProps;
  folly::dynamic prevRaw = prevProps != nullptr ? prevProps->rawProps : folly::dynamic::object();
  [self nsDispatchHook:@"updateProps"
                      a:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::valueFromDynamic(rt, nextRaw);
                      }
                      b:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::valueFromDynamic(rt, prevRaw);
                      }
                      c:nil];
}

- (void)updateProps:(const Props::Shared&)props oldProps:(const Props::Shared&)oldProps {
  [super updateProps:props oldProps:oldProps];
  [self nsEnsureCreated];
  auto nextProps = std::static_pointer_cast<const NativeScriptProps>(props);
  if (nextProps == nullptr) {
    return;
  }
  [self nsForwardUpdateProps:nextProps old:std::static_pointer_cast<const NativeScriptProps>(oldProps)];
}

- (void)updateEventEmitter:(const facebook::react::EventEmitter::Shared&)eventEmitter {
  // NS_REQUIRES_SUPER on RCTViewComponentView; the base implementation
  // stores `_eventEmitter`.
  [super updateEventEmitter:eventEmitter];
  [self nsEnsureCreated];  // idempotent; a no-op on the (common) path where -updateProps: already ran first.
  [self nsFlushPendingEvents];
}

- (void)updateState:(const facebook::react::State::Shared&)state
           oldState:(const facebook::react::State::Shared&)oldState {
  // Not NS_REQUIRES_SUPER on RCTViewComponentView (the base UIView category
  // implementation is a no-op); we own storing `_nsState` entirely so
  // `ctx.setContentSize` has something to write back into (§4.2).
  _nsState = state;
  if (_nsHasPendingState) {
    _nsHasPendingState = false;
    bool updateSynchronously = _nsPendingStateUpdateSynchronously;
    _nsPendingStateUpdateSynchronously = false;
    auto concreteState =
        std::static_pointer_cast<const facebook::react::ConcreteState<NativeScriptState>>(_nsState);
    if (concreteState != nullptr) {
      concreteState->updateState(
          NativeScriptState{_nsPendingState},
          updateSynchronously
              ? facebook::react::EventQueue::UpdateMode::unstable_Immediate
              : facebook::react::EventQueue::UpdateMode::Asynchronous);
    }
  }
}

// A component that declares this hook owns child mounting. Fabric's default
// implementation is used only when the hook is absent, which lets UIKit
// containers retain or reparent child views without violating Fabric's
// superview assertion.
- (void)mountChildComponentView:(UIView<RCTComponentViewProtocol>*)childComponentView index:(NSInteger)index {
  [self nsEnsureCreated];
  if (![self nsHasHook:NativeScriptComponentHookMountChild]) {
    [super mountChildComponentView:childComponentView index:index];
    return;
  }
  double childTag = (double)childComponentView.tag;
  double indexValue = (double)index;
  // Wrap the actual child view regardless of its concrete class; a
  // NativeScript-defined component's `mountChildComponentView` hook may
  // receive a plain RN-native child too (§5.1: "child in mount/unmount is
  // `{ tag, view, instance? }`; view by reference... instance present when
  // the child is NS-defined"). dispatcher.ts resolves `instance` itself via
  // its own tag-keyed table; it is not native's job to pre-filter by class.
  UIView<RCTComponentViewProtocol>* __unsafe_unretained weakChild = childComponentView;
  [self nsDispatchHook:@"mountChildComponentView"
                      a:^jsi::Value(jsi::Runtime& rt) {
                        return nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)weakChild);
                      }
                      b:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::Value(childTag);
                      }
                      c:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::Value(indexValue);
                      }];
}

- (void)unmountChildComponentView:(UIView<RCTComponentViewProtocol>*)childComponentView index:(NSInteger)index {
  [self nsEnsureCreated];
  if (![self nsHasHook:NativeScriptComponentHookUnmountChild]) {
    [super unmountChildComponentView:childComponentView index:index];
    return;
  }
  double childTag = (double)childComponentView.tag;
  double indexValue = (double)index;
  [self nsDispatchUnmountHookForChild:childComponentView tag:childTag index:indexValue];
  // The hook owns controller bookkeeping. Native code owns the final UIView
  // detach because the Objective-C argument remains valid after a cached JSI
  // wrapper has been released or consumed. Calling super here would assert
  // when UIKit reparented the child, so remove only the live child itself.
  if (childComponentView.superview != nil) {
    [childComponentView removeFromSuperview];
  }
}

- (void)mountingTransactionWillMount:(const facebook::react::MountingTransaction&)transaction
                withSurfaceTelemetry:(const facebook::react::SurfaceTelemetry&)surfaceTelemetry {
  [self nsEnsureCreated];
  if (![self nsHasHook:NativeScriptComponentHookWillMount]) {
    return;
  }
  facebook::react::Tag myTag = (facebook::react::Tag)self.tag;
  if (!NativeScriptTransactionHasAnyRelevantMutation(transaction, myTag)) {
    return;
  }
  [self nsDispatchWillMountHook:transaction];
}

- (void)mountingTransactionDidMount:(const facebook::react::MountingTransaction&)transaction
               withSurfaceTelemetry:(const facebook::react::SurfaceTelemetry&)surfaceTelemetry {
  [self nsEnsureCreated];
  if (![self nsHasHook:NativeScriptComponentHookDidMount]) {
    return;
  }
  facebook::react::Tag myTag = (facebook::react::Tag)self.tag;
  if (!NativeScriptTransactionTouchesTag(transaction, myTag)) {
    return;
  }
  const facebook::react::MountingTransaction* transactionPtr = &transaction;
  [self nsDispatchHook:@"mountingTransactionDidMount"
                      a:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::Value(rt, NativeScriptBuildMutationsArray(rt, *transactionPtr));
                      }
                      b:nil
                      c:nil];
}

- (void)updateLayoutMetrics:(const facebook::react::LayoutMetrics&)layoutMetrics
           oldLayoutMetrics:(const facebook::react::LayoutMetrics&)oldLayoutMetrics {
  [self nsEnsureCreated];
  BOOL accept = YES;
  if ([self nsHasHook:NativeScriptComponentHookUpdateLayoutMetrics]) {
    accept = [self nsDispatchLayoutHook:layoutMetrics old:oldLayoutMetrics];
  }
  if (accept) {
    [super updateLayoutMetrics:layoutMetrics oldLayoutMetrics:oldLayoutMetrics];
  }
  // When the hook declines, UIKit owns the frame and `_layoutMetrics`
  // intentionally does not track Fabric's proposal.
}

- (void)nsDispatchSafeAreaInsets {
  if (![self nsHasHook:NativeScriptComponentHookSafeAreaInsetsDidChange]) {
    return;
  }
  [self nsEnsureCreated];
  UIEdgeInsets insets = self.safeAreaInsets;
  [self nsDispatchHook:@"safeAreaInsetsDidChange"
                      a:^jsi::Value(jsi::Runtime& rt) {
                        jsi::Object object(rt);
                        object.setProperty(rt, "top", (double)insets.top);
                        object.setProperty(rt, "right", (double)insets.right);
                        object.setProperty(rt, "bottom", (double)insets.bottom);
                        object.setProperty(rt, "left", (double)insets.left);
                        return object;
                      }
                      b:nil
                      c:nil];
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  [self nsDispatchSafeAreaInsets];
}

- (void)safeAreaInsetsDidChange {
  [super safeAreaInsetsDidChange];
  [self nsDispatchSafeAreaInsets];
}

- (void)finalizeUpdates:(RNComponentViewUpdateMask)updateMask {
  [super finalizeUpdates:updateMask];
  [self nsEnsureCreated];
  if (![self nsHasHook:NativeScriptComponentHookFinalizeUpdates]) {
    return;
  }
  double maskValue = (double)updateMask;
  [self nsDispatchHook:@"finalizeUpdates"
                      a:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::Value(maskValue);
                      }
                      b:nil
                      c:nil];
}

- (void)handleCommand:(NSString*)commandName args:(NSArray*)args {
  [self nsEnsureCreated];
  if (![self nsHasHook:NativeScriptComponentHookCommands]) {
    return;
  }
  [self nsDispatchHook:@"handleCommand"
                      a:^jsi::Value(jsi::Runtime& rt) {
                        return jsi::Value(rt, jsi::String::createFromUtf8(rt, commandName.UTF8String ?: ""));
                      }
                      b:^jsi::Value(jsi::Runtime& rt) {
                        return facebook::react::TurboModuleConvertUtils::convertObjCObjectToJSIValue(rt, args);
                      }
                      c:nil];
}

// Definitions registered with `shouldBeRecycled: false` are torn down through
// `-invalidate`, while pooled definitions use `-prepareForRecycle`. Dispose
// through both paths so the UI-runtime instance table and retained view
// wrapper cannot leak.
// `viaInvalidate`: forwarded to the `prepareForRecycle` hook as its second
// argument so a component can tell which teardown path ran.
- (void)nsDisposeViaInvalidate:(BOOL)viaInvalidate {
  // Always fires (not hookMask-gated): dispatcher.ts's instance table
  // (tag -> {ctx, instance}) must drop this tag regardless of whether the
  // spec declared a `prepareForRecycle` hook, or the UI-runtime-side entry
  // leaks forever.
  if (_nsCreated) {
    [self nsDispatchHook:@"prepareForRecycle"
                        a:^jsi::Value(jsi::Runtime& rt) {
                          return jsi::Value(viaInvalidate == YES);
                        }
                        b:nil
                        c:nil];
    // Verification-round finding: a `ctx.emit` call made FROM INSIDE the
    // dispose hook itself lands in `_nsPendingEvents` (via
    // -nativeScriptDispatchEventName:payload:'s existing null-emitter
    // buffering) exactly as often as a `create()`-time emit does; flush
    // it here, BEFORE the unconditional clear below, while `_eventEmitter`
    // is still whatever it was at dispose time. Without this, the clear
    // immediately below silently discarded every event a `prepareForRecycle`
    // hook ever emitted (on-sim: `InvalidateProbe`'s own `onDisposed`).
    [self nsFlushPendingEvents];
  }
  // Invalidate callbacks scheduled by either ordinary hooks or the teardown
  // hook before a pooled view can acquire a new tag and native object graph.
  _nsLifecycleToken += 1;
  _nsCreated = NO;
  _nsComponentTag = 0;
  _nsState = nullptr;
  // M1 review §4: a buffered ctx.emit call (see nativeScriptDispatchEventName:
  // payload:'s comment) left in `_nsPendingEvents` at teardown must not
  // survive into the NEXT tag that reuses this pooled instance; clear it
  // here rather than only ever draining it from -updateEventEmitter:.
  _nsPendingEvents.clear();
  // Same reasoning for a buffered ctx.setContentSize; see the ivar's
  // comment above.
  _nsHasPendingState = false;
  _nsPendingStateUpdateSynchronously = false;
}

- (void)prepareForRecycle {
#ifndef NDEBUG
  // Fabric has detached the shadow node by this lifecycle point, so emitted
  // events cannot identify which teardown path ran. The test harness checks
  // this log entry instead.
  NSLog(@"NativeScriptComponentView[%@] -prepareForRecycle nsCreated=%d", self.nsComponentName, _nsCreated);
#endif
  [self nsDisposeViaInvalidate:NO];
  [super prepareForRecycle];
}

- (void)invalidate {
#ifndef NDEBUG
  NSLog(@"NativeScriptComponentView[%@] -invalidate nsCreated=%d", self.nsComponentName, _nsCreated);
#endif
  [self nsDisposeViaInvalidate:YES];
  [super invalidate];
}

#pragma mark - ctx.emit / ctx.setContentSize targets

- (uint64_t)nativeScriptLifecycleToken {
  return _nsLifecycleToken;
}

- (BOOL)nativeScriptIsLifecycleTokenCurrent:(uint64_t)token {
  return _nsLifecycleToken == token;
}

- (void)nativeScriptDispatchEventName:(const std::string&)name payload:(folly::dynamic&&)payload {
  if (name.empty()) {
    return;
  }
  if (_eventEmitter == nullptr) {
    // No emitter yet; buffer instead of dropping (see -nsEnsureCreated's
    // comment for why `create` can run before -updateEventEmitter: has
    // fired). -updateEventEmitter: flushes this the moment one exists.
    _nsPendingEvents.emplace_back(name, std::move(payload));
    return;
  }
  _eventEmitter->dispatchEvent(name, std::move(payload));
}

- (void)nsFlushPendingEvents {
  if (_nsPendingEvents.empty() || _eventEmitter == nullptr) {
    return;
  }
  auto pending = std::move(_nsPendingEvents);
  _nsPendingEvents.clear();
  for (auto& entry : pending) {
    _eventEmitter->dispatchEvent(entry.first, std::move(entry.second));
  }
}

- (void)nativeScriptSetContentSizeWidth:(double)width
                                  height:(double)height
                                offsetX:(double)offsetX
                                offsetY:(double)offsetY
                              authority:(BOOL)authority
                       contentOffsetMode:(uint8_t)contentOffsetMode
                     updateSynchronously:(BOOL)updateSynchronously {
  auto concreteState =
      std::static_pointer_cast<const facebook::react::ConcreteState<NativeScriptState>>(_nsState);
  NativeScriptState newState = concreteState != nullptr ? concreteState->getData() : _nsPendingState;
  newState.contentSize =
      facebook::react::Size{(facebook::react::Float)width, (facebook::react::Float)height};
  newState.contentOffsetX = (facebook::react::Float)offsetX;
  newState.contentOffsetY = (facebook::react::Float)offsetY;
  newState.nativeSizeAuthority = authority == YES;
  newState.contentOffsetMode = contentOffsetMode;
  if (concreteState == nullptr) {
    // No state yet (e.g. called from `create()`, before -updateState:
    // oldState: has ever fired; see the ivar's own comment); buffer
    // rather than silently drop; -updateState:oldState: flushes this the
    // moment a real state pointer exists.
    _nsHasPendingState = true;
    _nsPendingState = newState;
    _nsPendingStateUpdateSynchronously = updateSynchronously == YES;
    return;
  }
  concreteState->updateState(
      std::move(newState),
      updateSynchronously == YES
          ? facebook::react::EventQueue::UpdateMode::unstable_Immediate
          : facebook::react::EventQueue::UpdateMode::Asynchronous);
}

- (void)nativeScriptSetLayoutInsetsTop:(double)top
                                  right:(double)right
                                 bottom:(double)bottom
                                   left:(double)left
                              enableTop:(BOOL)enableTop
                            enableRight:(BOOL)enableRight
                           enableBottom:(BOOL)enableBottom
                             enableLeft:(BOOL)enableLeft {
  auto concreteState =
      std::static_pointer_cast<const facebook::react::ConcreteState<NativeScriptState>>(_nsState);
  NativeScriptState newState = concreteState != nullptr ? concreteState->getData() : _nsPendingState;
  newState.insetTop = (facebook::react::Float)top;
  newState.insetRight = (facebook::react::Float)right;
  newState.insetBottom = (facebook::react::Float)bottom;
  newState.insetLeft = (facebook::react::Float)left;
  newState.insetTopEnabled = enableTop == YES;
  newState.insetRightEnabled = enableRight == YES;
  newState.insetBottomEnabled = enableBottom == YES;
  newState.insetLeftEnabled = enableLeft == YES;
  newState.hasLayoutInsets = true;
  if (concreteState == nullptr) {
    _nsHasPendingState = true;
    _nsPendingState = newState;
    _nsPendingStateUpdateSynchronously = false;
    return;
  }
  concreteState->updateState(std::move(newState));
}

- (void)nativeScriptSetContentInsetsTop:(double)top
                                   right:(double)right
                                  bottom:(double)bottom
                                    left:(double)left
                     updateSynchronously:(BOOL)updateSynchronously {
  auto concreteState =
      std::static_pointer_cast<const facebook::react::ConcreteState<NativeScriptState>>(_nsState);
  NativeScriptState newState = concreteState != nullptr ? concreteState->getData() : _nsPendingState;
  newState.contentInsetTop = (facebook::react::Float)top;
  newState.contentInsetRight = (facebook::react::Float)right;
  newState.contentInsetBottom = (facebook::react::Float)bottom;
  newState.contentInsetLeft = (facebook::react::Float)left;
  newState.hasContentInsets = true;
  if (concreteState == nullptr) {
    _nsHasPendingState = true;
    _nsPendingState = newState;
    _nsPendingStateUpdateSynchronously = updateSynchronously == YES;
    return;
  }
  concreteState->updateState(
      std::move(newState),
      updateSynchronously == YES
          ? facebook::react::EventQueue::UpdateMode::unstable_Immediate
          : facebook::react::EventQueue::UpdateMode::Asynchronous);
}

- (void)nativeScriptSetWindowOverlay:(UIView*)overlay enabled:(BOOL)enabled {
  if (enabled) {
    NativeScriptInstallWindowOverlayOrdering();
    objc_setAssociatedObject(overlay, NativeScriptWindowOverlayAssociationKey(), @YES,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    RCTSurfaceTouchHandler* touchHandler =
        objc_getAssociatedObject(overlay, NativeScriptWindowOverlayTouchHandlerAssociationKey());
    if (touchHandler == nil) {
      touchHandler = [RCTSurfaceTouchHandler new];
      objc_setAssociatedObject(overlay, NativeScriptWindowOverlayTouchHandlerAssociationKey(),
                               touchHandler, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
      [touchHandler attachToView:overlay];
    }
    if ([overlay.superview isKindOfClass:UIWindow.class]) {
      [(UIWindow*)overlay.superview bringSubviewToFront:overlay];
      UIAccessibilityPostNotification(UIAccessibilityLayoutChangedNotification, overlay);
    }
    return;
  }

  RCTSurfaceTouchHandler* touchHandler =
      objc_getAssociatedObject(overlay, NativeScriptWindowOverlayTouchHandlerAssociationKey());
  [touchHandler detachFromView:overlay];
  objc_setAssociatedObject(overlay, NativeScriptWindowOverlayTouchHandlerAssociationKey(), nil,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  objc_setAssociatedObject(overlay, NativeScriptWindowOverlayAssociationKey(), nil,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

- (void)nativeScriptInvalidateControllerTraitsStatusBar:(BOOL)statusBar
                                           homeIndicator:(BOOL)homeIndicator
                                            orientations:(BOOL)orientations {
  UIWindow* window = self.window;
  if (window == nil) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:UIWindowScene.class]) {
        continue;
      }
      for (UIWindow* candidate in ((UIWindowScene*)scene).windows) {
        if (candidate.isKeyWindow) {
          window = candidate;
          break;
        }
      }
      if (window != nil) {
        break;
      }
    }
  }

  UIViewController* rootController = window.rootViewController;
  if (rootController == nil) {
    return;
  }
  if (statusBar) {
    [UIView animateWithDuration:0.4
                     animations:^{
                       [rootController setNeedsStatusBarAppearanceUpdate];
                     }];
  }
  if (homeIndicator) {
    [rootController setNeedsUpdateOfHomeIndicatorAutoHidden];
  }
  if (!orientations) {
    return;
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    UIInterfaceOrientationMask mask = rootController.supportedInterfaceOrientations;
    if (@available(iOS 16.0, *)) {
      UIWindowScene* windowScene = window.windowScene;
      if (windowScene == nil) {
        return;
      }
      UIWindowSceneGeometryPreferencesIOS* preferences =
          [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
      [windowScene requestGeometryUpdateWithPreferences:preferences
                                           errorHandler:^(__unused NSError* error) {
                                           }];
      UIViewController* topController = rootController;
      while (topController.presentedViewController != nil) {
        topController = topController.presentedViewController;
      }
      [topController setNeedsUpdateOfSupportedInterfaceOrientations];
    } else {
      [UIViewController attemptRotationToDeviceOrientation];
    }
  });
}

- (BOOL)nativeScriptAttachChildViewController:(UIViewController*)controller
                              toContainerView:(UIView*)containerView {
  if (controller == nil || containerView == nil) {
    return NO;
  }
  if (controller.parentViewController != nil) {
    return YES;
  }

  UIViewController* parentController = self.reactViewController;
  if (parentController == nil) {
    parentController = self.window.rootViewController;
  }
  if (parentController == nil || parentController == controller) {
    return NO;
  }

  [parentController addChildViewController:controller];
  [controller.view removeFromSuperview];
  controller.view.frame = containerView.bounds;
  controller.view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [containerView addSubview:controller.view];
  [controller didMoveToParentViewController:parentController];
  return YES;
}

@end

void NativeScriptInstallComponentHostFunctions(jsi::Runtime& runtime) {
  // Always replace these globals during installation. Worklets can create
  // an own-property placeholder with value `undefined` for a referenced
  // global before this function runs, so `hasProperty` is not a valid
  // idempotency test. Installation is once per UI-runtime generation and
  // replacing the ten functions is both cheap and reload-safe.
  auto emitFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentEmit"), 3,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 2 || !args[1].isString()) {
          return jsi::Value::undefined();
        }
        void* viewPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (viewPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* view = (__bridge NativeScriptComponentView*)viewPtr;
        std::string name = args[1].asString(rt).utf8(rt);
        folly::dynamic payload =
            count > 2 && !args[2].isUndefined() ? jsi::dynamicFromValue(rt, args[2]) : folly::dynamic::object();
        [view nativeScriptDispatchEventName:name payload:std::move(payload)];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentEmit", std::move(emitFn));

  auto setContentSizeFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentSetContentSize"), 8,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 3) {
          return jsi::Value::undefined();
        }
        void* viewPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (viewPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* view = (__bridge NativeScriptComponentView*)viewPtr;
        double width = args[1].isNumber() ? args[1].getNumber() : 0;
        double height = args[2].isNumber() ? args[2].getNumber() : 0;
        double offsetX = count > 3 && args[3].isNumber() ? args[3].getNumber() : 0;
        double offsetY = count > 4 && args[4].isNumber() ? args[4].getNumber() : 0;
        bool authority = count <= 5 || !args[5].isBool() || args[5].getBool();
        uint8_t contentOffsetMode =
            count > 6 && args[6].isNumber() ? (uint8_t)args[6].getNumber() : 0;
        [view nativeScriptSetContentSizeWidth:width
                                      height:height
                                    offsetX:offsetX
                                    offsetY:offsetY
                                  authority:authority ? YES : NO
                           contentOffsetMode:contentOffsetMode
                     updateSynchronously:count > 7 && args[7].isBool() && args[7].getBool()];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentSetContentSize", std::move(setContentSizeFn));

  auto setLayoutInsetsFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentSetLayoutInsets"), 9,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 9) {
          return jsi::Value::undefined();
        }
        void* viewPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (viewPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* view = (__bridge NativeScriptComponentView*)viewPtr;
        [view nativeScriptSetLayoutInsetsTop:args[1].isNumber() ? args[1].getNumber() : 0
                                       right:args[2].isNumber() ? args[2].getNumber() : 0
                                      bottom:args[3].isNumber() ? args[3].getNumber() : 0
                                        left:args[4].isNumber() ? args[4].getNumber() : 0
                                   enableTop:args[5].isBool() && args[5].getBool()
                                 enableRight:args[6].isBool() && args[6].getBool()
                                enableBottom:args[7].isBool() && args[7].getBool()
                                  enableLeft:args[8].isBool() && args[8].getBool()];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentSetLayoutInsets", std::move(setLayoutInsetsFn));

  auto setContentInsetsFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentSetContentInsets"), 6,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 5) {
          return jsi::Value::undefined();
        }
        void* viewPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (viewPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* view = (__bridge NativeScriptComponentView*)viewPtr;
        double top = args[1].isNumber() ? args[1].getNumber() : 0;
        double right = args[2].isNumber() ? args[2].getNumber() : 0;
        double bottom = args[3].isNumber() ? args[3].getNumber() : 0;
        double left = args[4].isNumber() ? args[4].getNumber() : 0;
        [view nativeScriptSetContentInsetsTop:top
                                       right:right
                                      bottom:bottom
                                        left:left
                         updateSynchronously:count > 5 && args[5].isBool() && args[5].getBool()];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentSetContentInsets", std::move(setContentInsetsFn));

  auto setWindowOverlayFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentSetWindowOverlay"), 3,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 3 || !args[2].isBool()) {
          return jsi::Value::undefined();
        }
        void* ownerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        void* overlayPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[1]);
        if (ownerPtr == nullptr || overlayPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* owner = (__bridge NativeScriptComponentView*)ownerPtr;
        UIView* overlay = (__bridge UIView*)overlayPtr;
        [owner nativeScriptSetWindowOverlay:overlay enabled:args[2].getBool() ? YES : NO];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentSetWindowOverlay",
                               std::move(setWindowOverlayFn));

  auto setObjectMetadataFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentSetObjectMetadata"), 3,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 3 || !args[1].isString()) {
          return jsi::Value::undefined();
        }
        void* ownerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (ownerPtr == nullptr) {
          return jsi::Value::undefined();
        }

        id value = nil;
        if (args[2].isString()) {
          std::string stringValue = args[2].getString(rt).utf8(rt);
          value = [NSString stringWithUTF8String:stringValue.c_str()];
        } else if (args[2].isObject() && args[2].asObject(rt).isArray(rt)) {
          jsi::Array array = args[2].asObject(rt).asArray(rt);
          NSMutableArray<NSString*>* strings = [NSMutableArray arrayWithCapacity:array.size(rt)];
          for (size_t index = 0; index < array.size(rt); index++) {
            jsi::Value item = array.getValueAtIndex(rt, index);
            if (!item.isString()) {
              return jsi::Value::undefined();
            }
            std::string stringValue = item.getString(rt).utf8(rt);
            [strings addObject:[NSString stringWithUTF8String:stringValue.c_str()]];
          }
          value = strings;
        } else if (!args[2].isUndefined() && !args[2].isNull()) {
          return jsi::Value::undefined();
        }

        std::string nameValue = args[1].getString(rt).utf8(rt);
        NativeScriptComponentView* owner = (__bridge NativeScriptComponentView*)ownerPtr;
        [owner nativeScriptSetObjectMetadataName:[NSString stringWithUTF8String:nameValue.c_str()]
                                           value:value];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentSetObjectMetadata",
                               std::move(setObjectMetadataFn));

  auto enableChildControllerTraitForwardingFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentEnableChildControllerTraitForwarding"), 0,
      [](jsi::Runtime&, const jsi::Value&, const jsi::Value*, size_t) -> jsi::Value {
        NativeScriptInstallChildControllerTraitForwarding();
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentEnableChildControllerTraitForwarding",
                               std::move(enableChildControllerTraitForwardingFn));

  auto invalidateControllerTraitsFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentInvalidateControllerTraits"), 4,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 4) {
          return jsi::Value::undefined();
        }
        void* ownerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (ownerPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* owner = (__bridge NativeScriptComponentView*)ownerPtr;
        [owner nativeScriptInvalidateControllerTraitsStatusBar:args[1].isBool() && args[1].getBool()
                                                  homeIndicator:args[2].isBool() && args[2].getBool()
                                                   orientations:args[3].isBool() && args[3].getBool()];
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentInvalidateControllerTraits",
                               std::move(invalidateControllerTraitsFn));

  auto findDescendantViewFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentFindDescendantView"), 2,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 2 || !args[1].isString()) {
          return jsi::Value::null();
        }
        void* rootPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (rootPtr == nullptr) {
          return jsi::Value::null();
        }
        std::string className = args[1].asString(rt).utf8(rt);
        Class targetClass = NSClassFromString([NSString stringWithUTF8String:className.c_str()]);
        UIView* match = NativeScriptFindDescendantView((__bridge UIView*)rootPtr, targetClass);
        return match != nil
                   ? nativescript::NativeScriptWrapNativeObject(rt, (__bridge void*)match, false)
                   : jsi::Value::null();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentFindDescendantView",
                               std::move(findDescendantViewFn));

  auto attachChildViewControllerFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentAttachChildViewController"), 3,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 3) {
          return jsi::Value(false);
        }
        void* ownerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        void* controllerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[1]);
        void* containerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[2]);
        if (ownerPtr == nullptr || controllerPtr == nullptr || containerPtr == nullptr) {
          return jsi::Value(false);
        }
        NativeScriptComponentView* owner = (__bridge NativeScriptComponentView*)ownerPtr;
        UIViewController* controller = (__bridge UIViewController*)controllerPtr;
        UIView* containerView = (__bridge UIView*)containerPtr;
        return jsi::Value(
            [owner nativeScriptAttachChildViewController:controller toContainerView:containerView] == YES);
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentAttachChildViewController",
                               std::move(attachChildViewControllerFn));

  auto scheduleFn = jsi::Function::createFromHostFunction(
      runtime, jsi::PropNameID::forAscii(runtime, "__nativeScriptComponentScheduleOnMainQueue"), 2,
      [](jsi::Runtime& rt, const jsi::Value&, const jsi::Value* args, size_t count) -> jsi::Value {
        if (count < 2 || !args[1].isObject() || !args[1].asObject(rt).isFunction(rt)) {
          return jsi::Value::undefined();
        }
        void* ownerPtr = nativescript::NativeScriptUnwrapNativeObject(rt, args[0]);
        if (ownerPtr == nullptr) {
          return jsi::Value::undefined();
        }
        NativeScriptComponentView* owner = (__bridge NativeScriptComponentView*)ownerPtr;
        uint64_t scheduledLifecycleToken = owner.nativeScriptLifecycleToken;
        auto callback = std::make_shared<jsi::Function>(args[1].asObject(rt).asFunction(rt));
        // M1 review §5/#5: two latent lifetime bugs here; (a) if a
        // Worklets reload installs a NEW UI runtime between this call and
        // the dispatch_async firing, `callback` (a jsi::Value bound to the
        // OLD runtime) must never be `.call()`ed against the new one
        // (jsi::Value used with the wrong Runtime is UB); (b) `callback`'s
        // shared_ptr must not be destructed (its destructor talks back to
        // the Runtime that created it) once that runtime is itself already
        // torn down. Fix: generation-tag at schedule time; on mismatch,
        // skip the call AND deliberately leak the jsi::Function (see
        // NativeScriptLeakScheduledCallback below) instead of letting the
        // block's normal teardown destruct it against a dead runtime.
        uint64_t scheduledGeneration = nativescript::NativeScriptFabricGatewayGeneration();
        // Genuine deferral to the next main runloop turn after didMount.
        // Deliberately NOT `worklets::scheduleOnUI` (which may run inline
        // when already on main; see NativeScriptFabricGateway.h's note on
        // why that helper is reserved for the general async-entry path).
        dispatch_async(dispatch_get_main_queue(), ^{
          if (nativescript::NativeScriptFabricGatewayGeneration() != scheduledGeneration) {
            NativeScriptLeakScheduledCallback(callback);
            return;
          }
          // This target is compiled under manual reference counting. Copying
          // the dispatch block retains owner until the next runloop turn,
          // then the lifecycle token decides whether the callback is current.
          if (![owner nativeScriptIsLifecycleTokenCurrent:scheduledLifecycleToken]) {
            return;
          }
          nativescript::NativeScriptFabricGatewayRunSyncOnMain([callback](jsi::Runtime& rt2) -> bool {
            try {
              callback->call(rt2);
            } catch (const jsi::JSError& error) {
              NSLog(@"NativeScript scheduled component callback failed: %s\n%s",
                    error.getMessage().c_str(), error.getStack().c_str());
            }
            return true;
          });
        });
        return jsi::Value::undefined();
      });
  runtime.global().setProperty(runtime, "__nativeScriptComponentScheduleOnMainQueue", std::move(scheduleFn));
}
