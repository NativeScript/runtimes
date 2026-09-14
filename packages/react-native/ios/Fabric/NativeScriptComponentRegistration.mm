#import "NativeScriptComponentRegistration.h"

#import <objc/runtime.h>

#import <React/RCTComponentViewFactory.h>
#import <React/RCTComponentViewProtocol.h>

#include <memory>
#include <mutex>
#include <string>

#include "NativeScriptComponentDescriptor.h"
#include "NativeScriptComponentView.h"

using namespace facebook::react;

namespace {

std::mutex& NativeScriptRegistrationMutex() {
  static std::mutex mutex;
  return mutex;
}

NSString* NativeScriptDynamicClassName(NSString* name) {
  return [@"NativeScriptComponentView_Flavor_" stringByAppendingString:name];
}

}  // namespace

const void* NativeScriptFlavorNameAssociationKey(void) {
  static int key;
  return &key;
}

const void* NativeScriptFlavorHookMaskAssociationKey(void) {
  static int key;
  return &key;
}

void NativeScriptRegisterFlavoredComponent(NSString* name, uint32_t hookMask, BOOL hasShouldBeRecycled,
                                           BOOL shouldBeRecycled) {
  if (name.length == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(NativeScriptRegistrationMutex());

  NSString* dynClassName = NativeScriptDynamicClassName(name);
  Class dynClass = NSClassFromString(dynClassName);

  if (dynClass == Nil) {
    // A per-name, otherwise-empty subclass of the ONE generic ComponentView
    // (ARCHITECTURE.md §4.1 step 1). It adds no ivars/methods of its own
    // except the class-side `+componentDescriptorProvider` override below --
    // every instance behaves exactly like NativeScriptComponentView.
    dynClass = objc_allocateClassPair(NativeScriptComponentView.class, dynClassName.UTF8String, 0);
    if (dynClass == Nil) {
      NSLog(@"NativeScript: failed to allocate flavored component class for %@", name);
      return;
    }

    // `flavor` is retained by the ComponentDescriptorProvider's shared_ptr,
    // not by the block; capture a std::string copy, not the NSString.
    auto flavorName = std::make_shared<const std::string>(name.UTF8String != nullptr ? name.UTF8String : "");

    // Reuse the base class's constructor (the actual C++ NativeScriptComponentDescriptor
    // template instantiation is shared across every flavor; only name/handle/flavor differ).
    ComponentDescriptorConstructor* sharedConstructor =
        [NativeScriptComponentView componentDescriptorProvider].constructor;

    ComponentDescriptorProvider (^providerBlock)(id) = ^ComponentDescriptorProvider(id self) {
      ComponentName componentName = flavorName->c_str();
      ComponentHandle componentHandle = reinterpret_cast<ComponentHandle>(componentName);
      return ComponentDescriptorProvider{
          .handle = componentHandle,
          .name = componentName,
          .flavor = flavorName,
          .constructor = sharedConstructor,
      };
    };

    // `imp_implementationWithBlock` builds a real, ABI-correct trampoline for
    // the block's signature (it does not need the type-encoding string to be
    // byte-accurate for ordinary objc_msgSend dispatch; that string is only
    // consulted by introspection APIs, not by a compile-time-typed message
    // send like `[componentViewClass componentDescriptorProvider]`, which is
    // exactly how RCTComponentViewFactory calls it). This sidesteps hand
    // writing a raw C IMP with the correct large-non-POD-struct return ABI.
    IMP providerImp = imp_implementationWithBlock(providerBlock);
    Class metaClass = object_getClass(dynClass);
    // The type-encoding string below is NOT byte-accurate (ComponentDescriptorProvider
    // is a non-POD C++ type; @encode has no notion of it) and does not need
    // to be: objc_msgSend at RCTComponentViewFactory's call site dispatches
    // using the return type it knows statically from RCTComponentViewProtocol's
    // declared `+(ComponentDescriptorProvider)componentDescriptorProvider`, not
    // from this string (that string is only consulted by introspection APIs --
    // NSInvocation/KVO/-methodSignatureForSelector:; none of which
    // registerComponentViewClass: uses). imp_implementationWithBlock builds a
    // real ABI-correct trampoline from the block's own (compiler-checked)
    // signature, which is what actually makes the struct return work.
    class_addMethod(metaClass, @selector(componentDescriptorProvider), providerImp, "{ComponentDescriptorProvider=}@:");

    objc_registerClassPair(dynClass);

    // Store the ACTUAL registered Fabric name on the class itself, so
    // instances need not parse it back out of the dynamic class name.
    objc_setAssociatedObject((id)dynClass, NativeScriptFlavorNameAssociationKey(), name,
                              OBJC_ASSOCIATION_RETAIN);
  }

  // Hook mask can legitimately change across `defineNativeComponent` reload
  // re-invocations (fast refresh editing a spec's hook set); always
  // refresh it, even when the class itself already existed.
  objc_setAssociatedObject((id)dynClass, NativeScriptFlavorHookMaskAssociationKey(), @(hookMask),
                            OBJC_ASSOCIATION_RETAIN);

  // M1 review §2/(c): `+shouldBeRecycled`, per flavor, class_replaceMethod'd
  // onto the metaclass (idempotent across re-registration, unlike
  // class_addMethod); the same trick as `+componentDescriptorProvider`
  // above. RCTComponentViewFactory reads this OPTIONAL class method (it is
  // not part of RCTComponentViewProtocol's required set) to decide whether
  // RCTComponentViewRegistry recycles a torn-down view (default, when this
  // method is absent) or calls `-invalidate` instead (see
  // NativeScriptComponentView.mm's `-invalidate` override for the matching
  // dispose-path fix).
  if (hasShouldBeRecycled) {
    Class metaClass = object_getClass(dynClass);
    BOOL recycledValue = shouldBeRecycled;
    BOOL (^shouldBeRecycledBlock)(id) = ^BOOL(id self) {
      return recycledValue;
    };
    class_replaceMethod(metaClass, @selector(shouldBeRecycled), imp_implementationWithBlock(shouldBeRecycledBlock),
                        "c@:");
  }

  [[RCTComponentViewFactory currentComponentViewFactory]
      registerComponentViewClass:(Class<RCTComponentViewProtocol>)dynClass];
}
