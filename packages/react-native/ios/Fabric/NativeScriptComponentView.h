#import <React/RCTMountingTransactionObserving.h>
#import <React/RCTViewComponentView.h>

#include <folly/dynamic.h>
#include <jsi/jsi.h>

#include <cstdint>
#include <string>

NS_ASSUME_NONNULL_BEGIN

// Every NativeScript component name uses this component view. Fabric keeps its
// default behavior unless the registered TypeScript definition declares a
// hook. The per-name dynamic class stores that hook mask as associated data.
@interface NativeScriptComponentView : RCTViewComponentView <RCTMountingTransactionObserving>

// The UI-runtime host functions unwrap `ctx.view` and call these methods on the
// live component view. Events go to Fabric's EventEmitter. Size and inset
// updates write component state for the shadow node.
- (void)nativeScriptDispatchEventName:(const std::string&)name payload:(folly::dynamic&&)payload;
- (void)nativeScriptSetContentSizeWidth:(double)width
                                  height:(double)height
                                offsetX:(double)offsetX
                                offsetY:(double)offsetY
                              authority:(BOOL)authority
                       contentOffsetMode:(uint8_t)contentOffsetMode
                     updateSynchronously:(BOOL)updateSynchronously;
- (void)nativeScriptSetLayoutInsetsTop:(double)top
                                  right:(double)right
                                 bottom:(double)bottom
                                   left:(double)left
                             enableTop:(BOOL)enableTop
                           enableRight:(BOOL)enableRight
                          enableBottom:(BOOL)enableBottom
                            enableLeft:(BOOL)enableLeft;
- (void)nativeScriptSetContentInsetsTop:(double)top
                                   right:(double)right
                                  bottom:(double)bottom
                                    left:(double)left
                     updateSynchronously:(BOOL)updateSynchronously;
- (void)nativeScriptSetWindowOverlay:(UIView*)overlay enabled:(BOOL)enabled;
- (void)nativeScriptSetObjectMetadataName:(NSString*)name value:(nullable id)value;
- (void)nativeScriptInvalidateControllerTraitsStatusBar:(BOOL)statusBar
                                           homeIndicator:(BOOL)homeIndicator
                                            orientations:(BOOL)orientations;
- (BOOL)nativeScriptAttachChildViewController:(UIViewController*)controller
                              toContainerView:(UIView*)containerView;
- (uint64_t)nativeScriptLifecycleToken;
- (BOOL)nativeScriptIsLifecycleTokenCurrent:(uint64_t)token;

@end

// Installs the eleven host functions used by ctx.emit, ctx.setContentSize,
// ctx.setLayoutInsets, ctx.setContentInsets, ctx.setWindowOverlay,
// ctx.setNativeObjectMetadata, ctx.enableChildControllerTraitForwarding,
// ctx.invalidateControllerTraits, native descendant lookup,
// ctx.attachChildViewController, and ctx.scheduleOnMainQueue.
// Call this inside runSync on the UI runtime. Repeated calls are safe.
void NativeScriptInstallComponentHostFunctions(facebook::jsi::Runtime& runtime);

NS_ASSUME_NONNULL_END
