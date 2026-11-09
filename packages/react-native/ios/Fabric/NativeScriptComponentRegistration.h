#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Registers `name` as a Fabric component that resolves to a fresh, per-name
// dynamic subclass of NativeScriptComponentView, via the PUBLIC RN API
// (`+componentDescriptorProvider` + `registerComponentViewClass:` on
// RCTComponentViewFactory); no private ivars, no `_providerRegistry`
// reach-around (ARCHITECTURE.md §4.1). Idempotent: calling twice with the
// same name is a no-op except for updating the stored hook mask.
//
// Called from `defineNativeComponent(name, spec)`'s native registration step
// (ARCHITECTURE.md §5.2 step 2) via NativeScriptNativeApiModule::registerComponent.
// `hookMask` is a bitwise-OR of NativeScriptComponentHook values
// (NativeScriptFabricGateway.h); which optional Fabric callbacks this
// definition actually declared, stored on the per-flavor dynamic Class so
// every instance can read it back without a lookup (the same trick
// RCTComponentViewFactory itself uses to decide
// `observesMountingTransactionWillMount` per class).
//
// M1 review §2/(c), fix-list item 3: `hasShouldBeRecycled`/`shouldBeRecycled`
// wire the spec's `shouldBeRecycled` flag onto a per-flavor `+(BOOL)
// shouldBeRecycled` class method via `class_addMethod`/`class_replaceMethod`
// on the dynamic subclass's metaclass; the SAME per-flavor-dynamic-class
// trick `+componentDescriptorProvider` below already uses, applied to the
// selector `RCTComponentViewFactory` itself probes (optionally, via
// `class_respondsToSelector`) to decide whether a view goes through
// `-invalidate` (never recycled) or the default recycle pool. Omitted
// (`hasShouldBeRecycled == NO`) when the spec never set the flag, leaving
// RN's own default (`shouldBeRecycled: true`, RCTComponentViewClassDescriptor.h)
// in effect.
FOUNDATION_EXPORT void NativeScriptRegisterFlavoredComponent(NSString* name, uint32_t hookMask,
                                                              BOOL hasShouldBeRecycled,
                                                              BOOL shouldBeRecycled);

// Stable associated-object keys (function-local static addresses, so they
// are guaranteed identical across translation units) under which the
// registered Fabric component name / hook mask are stored on each
// per-flavor dynamic Class, so instances can read both back without parsing
// them out of the (otherwise-arbitrary) dynamic class name.
FOUNDATION_EXPORT const void* NativeScriptFlavorNameAssociationKey(void);
FOUNDATION_EXPORT const void* NativeScriptFlavorHookMaskAssociationKey(void);

NS_ASSUME_NONNULL_END
