#pragma once

#include <cstdint>

// M1 (ARCHITECTURE.md §4.2): the real Fabric types shared by every
// flavor-registered NativeScript component name. Replaces M0's placeholder
// reuse of RN's built-in ViewProps/ViewEventEmitter with:
//
//  - NativeScriptProps: extends ViewProps (so standard layout/style props
//    keep behaving through Yoga/RN's own diffing) and retains the
//    non-view raw props verbatim as `folly::dynamic`; no codegen, no typed
//    C++ struct; typing lives entirely in the TS `defineNativeComponent`
//    spec. Precedented verbatim by RN's own
//    `LegacyViewManagerInteropViewProps` (react-native/ReactCommon/react/
//    renderer/components/legacyviewmanagerinterop/LegacyViewManagerInteropViewProps.h).
//  - NativeScriptState: the generic UIKit -> shadow-tree write-back slot
//    used by `ctx.setContentSize`.
//  - NativeScriptEventEmitter: `ctx.emit` lands here via
//    `EventEmitter::dispatchEvent`. Correction to ARCHITECTURE.md §4.2: on
//    RN 0.85 `EventEmitter::dispatchEvent` is already `public` (older RN had
//    it `protected`, which is what the doc's "exposes dispatchEvent... over
//    the protected EventEmitter::dispatchEvent" phrasing assumed); no
//    exposing wrapper is needed. Kept as a real (if thin) subclass anyway,
//    both to match the design's naming and as a NativeScript-specific
//    extension point.

#include <folly/dynamic.h>

#include <react/renderer/components/view/ConcreteViewShadowNode.h>
#include <react/renderer/components/view/ViewEventEmitter.h>
#include <react/renderer/components/view/ViewProps.h>
#include <react/renderer/components/view/YogaLayoutableShadowNode.h>
#include <react/renderer/core/ConcreteComponentDescriptor.h>
#include <react/renderer/core/LayoutContext.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/core/RawProps.h>
#include <react/renderer/core/ShadowNode.h>
#include <react/renderer/graphics/Float.h>
#include <react/renderer/graphics/Size.h>

namespace facebook::react {

// Placeholder compile-time name baked into the ShadowNode template; the
// Fabric-visible name actually used for registration/lookup comes from
// `flavor_` (see NativeScriptComponentDescriptor::getComponentName below),
// exactly like the LegacyViewManagerInterop precedent this mirrors.
extern const char NativeScriptComponentName[];

class NativeScriptProps final : public ViewProps {
 public:
  NativeScriptProps() = default;
  NativeScriptProps(const PropsParserContext& context,
                    const NativeScriptProps& sourceProps,
                    const RawProps& rawProps);

  // Every prop the TS spec declared, verbatim, as a folly::dynamic object.
  // Delivered to a worklet `updateProps(ctx, next, prev)` hook via
  // `jsi::valueFromDynamic` (a real JSI/folly::dynamic bridge, NOT
  // JSON.stringify/parse; ARCHITECTURE.md's "no JSON marshalling" rule).
  const folly::dynamic rawProps{folly::dynamic::object()};
};

// UIKit-to-shadow-tree state shared by the generic hosts. Content size is
// used by intrinsically sized controls. Layout insets let native containers
// feed dynamic system geometry (safe areas) back into Yoga without involving
// the React JS thread.
struct NativeScriptState {
  Size contentSize{};
  Float contentOffsetX{0};
  Float contentOffsetY{0};
  bool nativeSizeAuthority{false};
  // 0: none, 1: replace this node's frame origin, 2: offset its content.
  uint8_t contentOffsetMode{0};
  Float insetTop{0};
  Float insetRight{0};
  Float insetBottom{0};
  Float insetLeft{0};
  bool insetTopEnabled{false};
  bool insetRightEnabled{false};
  bool insetBottomEnabled{false};
  bool insetLeftEnabled{false};
  bool hasLayoutInsets{false};
  Float contentInsetTop{0};
  Float contentInsetRight{0};
  Float contentInsetBottom{0};
  Float contentInsetLeft{0};
  bool hasContentInsets{false};
};

class NativeScriptEventEmitter final : public ViewEventEmitter {
 public:
  using ViewEventEmitter::ViewEventEmitter;
};

class NativeScriptShadowNode final
    : public ConcreteViewShadowNode<NativeScriptComponentName,
                                    NativeScriptProps,
                                    NativeScriptEventEmitter,
                                    NativeScriptState> {
  using ConcreteViewShadowNode::ConcreteViewShadowNode;

 public:
  void adjustLayoutWithState();
  void layout(LayoutContext layoutContext) override;
  Point getContentOriginOffset(bool includeTransform) const override;
};

class NativeScriptComponentDescriptor final
    : public ConcreteComponentDescriptor<NativeScriptShadowNode> {
 public:
  using ConcreteComponentDescriptor::ConcreteComponentDescriptor;

  // `name`/`handle` are derived from `flavor_` (set by the per-name
  // registration in NativeScriptComponentRegistration.mm), not from
  // `NativeScriptShadowNode::Name()`. See ComponentDescriptor.h's `Flavor`
  // doc comment: "designed to allow registering instances of the exact same
  // ComponentDescriptor class with different ComponentName and
  // ComponentHandle."
  ComponentHandle getComponentHandle() const override;
  ComponentName getComponentName() const override;

  // Feed authoritative, non-zero native content sizes into Yoga so
  // `onLayout` observes `ctx.setContentSize(..., {authority: true})`.
  void adopt(ShadowNode& shadowNode) const override;
};

}  // namespace facebook::react
