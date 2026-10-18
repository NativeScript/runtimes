#import "NativeScriptUIViewComponentView.h"

#import <React/RCTFabricComponentsPlugins.h>
#import <React/RCTConversions.h>
#import <react/renderer/components/NativeScriptNativeApiSpec/ComponentDescriptors.h>
#import <react/renderer/components/NativeScriptNativeApiSpec/EventEmitters.h>
#import <react/renderer/components/NativeScriptNativeApiSpec/Props.h>

#import "NativeScriptUIView.h"

using namespace facebook::react;

static const std::shared_ptr<const NativeScriptUIViewProps>& NativeScriptUIViewDefaultProps() {
  static const auto defaultProps = std::make_shared<const NativeScriptUIViewProps>();
  return defaultProps;
}

static BOOL NativeScriptFabricViewIsDescendantOfView(UIView* view, UIView* ancestor) {
  UIView* current = view;
  while (current != nil) {
    if (current == ancestor) {
      return YES;
    }
    current = current.superview;
  }
  return NO;
}

static CGRect NativeScriptFabricEffectiveTabBarHitBounds(UITabBar* tabBar) {
  CGRect bounds = tabBar.bounds;
  CGSize fittingSize = [tabBar sizeThatFits:CGSizeMake(bounds.size.width, bounds.size.height)];
  CGFloat maximumHeight = MAX(fittingSize.height + 32, 96);

  if (bounds.size.height > maximumHeight) {
    bounds.origin.y = CGRectGetMaxY(bounds) - maximumHeight;
    bounds.size.height = maximumHeight;
  }

  return CGRectInset(bounds, -24, -16);
}

static BOOL NativeScriptFabricPointInsideTabBarHitArea(UITabBar* tabBar, UIWindow* window,
                                                       CGPoint windowPoint) {
  if (tabBar == nil || tabBar.hidden || tabBar.alpha <= 0.01 ||
      !tabBar.userInteractionEnabled) {
    return NO;
  }

  CGPoint localPoint = [tabBar convertPoint:windowPoint fromView:window];
  return CGRectContainsPoint(NativeScriptFabricEffectiveTabBarHitBounds(tabBar), localPoint);
}

static UITabBar* NativeScriptFabricVisibleTabBarAtPoint(UIView* root, UIWindow* window,
                                                        CGPoint windowPoint) {
  if (root.hidden || root.alpha <= 0.01 || !root.userInteractionEnabled) {
    return nil;
  }

  if ([root isKindOfClass:UITabBar.class]) {
    UITabBar* tabBar = static_cast<UITabBar*>(root);
    if (NativeScriptFabricPointInsideTabBarHitArea(tabBar, window, windowPoint)) {
      return static_cast<UITabBar*>(root);
    }
  }

  for (UIView* subview in [root.subviews reverseObjectEnumerator]) {
    UITabBar* tabBar = NativeScriptFabricVisibleTabBarAtPoint(subview, window, windowPoint);
    if (tabBar != nil) {
      return tabBar;
    }
  }

  return nil;
}

@interface NativeScriptUIViewComponentView () <NativeScriptUIViewHostReadyDelegate>
@end

@implementation NativeScriptUIViewComponentView {
  NativeScriptUIView* _containerView;
  NSString* _debugName;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    _props = NativeScriptUIViewDefaultProps();

    _containerView = [[NativeScriptUIView alloc] initWithFrame:self.bounds];
    _containerView.hostReadyDelegate = self;
    _containerView.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.contentView = _containerView;
  }

  return self;
}

- (void)dealloc {
  _containerView.hostReadyDelegate = nil;
  [_debugName release];
  [_containerView release];
  [super dealloc];
}

- (void)nativeScriptUIView:(NativeScriptUIView*)view
          didHostReady:(NSDictionary<NSString*, id>*)event {
  (void)view;
  if (_eventEmitter == nullptr) {
    return;
  }

  static_cast<const NativeScriptUIViewEventEmitter&>(*_eventEmitter)
      .onHostReady(NativeScriptUIViewEventEmitter::OnHostReady{
          .hostReadyId = RCTStringFromNSString(event[@"hostReadyId"] ?: @""),
          .hostId = RCTStringFromNSString(event[@"hostId"] ?: @""),
          .nativeViewHandle = RCTStringFromNSString(event[@"nativeViewHandle"] ?: @""),
          .childrenViewHandle = RCTStringFromNSString(event[@"childrenViewHandle"] ?: @""),
          .controllerHandle = RCTStringFromNSString(event[@"controllerHandle"] ?: @""),
          .hasChildren = [event[@"hasChildren"] boolValue],
      });
}

- (NSString*)description {
  if (_debugName.length == 0) {
    return [super description];
  }

  NSString* description = [super description];
  if ([description hasSuffix:@">"]) {
    return [[description substringToIndex:description.length - 1]
        stringByAppendingFormat:@"; debugName = %@>", _debugName];
  }
  return [description stringByAppendingFormat:@" debugName = %@", _debugName];
}

- (void)mountChildComponentView:(UIView<RCTComponentViewProtocol>*)childComponentView
                          index:(NSInteger)index {
  [_containerView insertSubview:childComponentView atIndex:index];
  [_containerView refreshDetachedChildrenHost];
}

- (void)unmountChildComponentView:(UIView<RCTComponentViewProtocol>*)childComponentView
                            index:(NSInteger)index {
  [childComponentView removeFromSuperview];
  [_containerView refreshDetachedChildrenHost];
}

- (void)didMoveToWindow {
  [super didMoveToWindow];
  [_containerView refreshDetachedChildrenHost];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  [_containerView refreshDetachedChildrenHost];
}

- (void)updateLayoutMetrics:(const LayoutMetrics&)layoutMetrics
           oldLayoutMetrics:(const LayoutMetrics&)oldLayoutMetrics {
  [super updateLayoutMetrics:layoutMetrics oldLayoutMetrics:oldLayoutMetrics];
  [_containerView refreshDetachedChildrenHost];
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event {
  [_containerView refreshDetachedChildrenHost];

  UIView* hitView = [super hitTest:point withEvent:event];
  if (hitView == nil && _containerView != nil && _containerView.window != nil) {
    CGPoint containerPoint = [_containerView convertPoint:point fromView:self];
    hitView = [_containerView hitTest:containerPoint withEvent:event];
  }

  if (hitView == nil || self.window == nil) {
    return hitView;
  }

  CGPoint windowPoint = [self convertPoint:point toView:self.window];
  UITabBar* tabBar = NativeScriptFabricVisibleTabBarAtPoint(self.window, self.window, windowPoint);
  if (tabBar != nil) {
    if (NativeScriptFabricViewIsDescendantOfView(tabBar, self)) {
      CGPoint tabBarPoint = [tabBar convertPoint:windowPoint fromView:self.window];
      UIView* tabBarHitView = [tabBar hitTest:tabBarPoint withEvent:event];
      if (tabBarHitView != nil) {
        return tabBarHitView;
      }
      return tabBar;
    }
    if (!NativeScriptFabricViewIsDescendantOfView(self, tabBar)) {
      return nil;
    }
  }

  return hitView;
}

- (void)updateProps:(Props::Shared const&)props oldProps:(Props::Shared const&)oldProps {
  // Diff against oldProps, not _props: React Native leaves _props untouched on
  // recycle, while prepareForRecycle clears the container, so a recycled view
  // mounted with its previous handle would otherwise never attach it.
  const auto oldViewProps =
      oldProps ? std::static_pointer_cast<const NativeScriptUIViewProps>(oldProps)
               : NativeScriptUIViewDefaultProps();
  const auto newViewProps = std::static_pointer_cast<const NativeScriptUIViewProps>(props);
  const std::string oldNativeViewHandle = oldViewProps->nativeViewHandle;
  const std::string newNativeViewHandle = newViewProps->nativeViewHandle;
  const std::string oldChildrenViewHandle = oldViewProps->childrenViewHandle;
  const std::string newChildrenViewHandle = newViewProps->childrenViewHandle;
  const std::string oldControllerHandle = oldViewProps->controllerHandle;
  const std::string newControllerHandle = newViewProps->controllerHandle;
  const auto oldDetachControllerView = oldViewProps->detachControllerView;
  const auto newDetachControllerView = newViewProps->detachControllerView;
  const std::string oldDebugName = oldViewProps->debugName;
  const std::string newDebugName = newViewProps->debugName;
  const std::string oldHostId = oldViewProps->hostId;
  const std::string newHostId = newViewProps->hostId;
  const std::string oldHostReadyId = oldViewProps->hostReadyId;
  const std::string newHostReadyId = newViewProps->hostReadyId;
  const auto oldUpdateRevision = oldViewProps->updateRevision;
  const auto newUpdateRevision = newViewProps->updateRevision;
  const auto oldMountedRevision = oldViewProps->mountedRevision;
  const auto newMountedRevision = newViewProps->mountedRevision;

  [super updateProps:props oldProps:oldProps];

  if (oldDebugName != newDebugName) {
    NSString* debugName =
        newDebugName.empty() ? nil : [NSString stringWithUTF8String:newDebugName.c_str()];
    [_debugName release];
    _debugName = [debugName copy];
    _containerView.debugName = debugName;
  }

  if (oldDetachControllerView != newDetachControllerView) {
    _containerView.detachControllerView = newDetachControllerView;
  }

  if (oldNativeViewHandle != newNativeViewHandle) {
    NSString* nativeViewHandle = newNativeViewHandle.empty()
                                     ? nil
                                     : [NSString stringWithUTF8String:newNativeViewHandle.c_str()];
    _containerView.nativeViewHandle = nativeViewHandle;
  }

  if (oldChildrenViewHandle != newChildrenViewHandle) {
    NSString* childrenViewHandle =
        newChildrenViewHandle.empty()
            ? nil
            : [NSString stringWithUTF8String:newChildrenViewHandle.c_str()];
    _containerView.childrenViewHandle = childrenViewHandle;
  }

  if (oldControllerHandle != newControllerHandle) {
    NSString* controllerHandle = newControllerHandle.empty()
                                     ? nil
                                     : [NSString stringWithUTF8String:newControllerHandle.c_str()];
    _containerView.controllerHandle = controllerHandle;
  }

  if (oldHostId != newHostId) {
    NSString* hostId = newHostId.empty() ? nil : [NSString stringWithUTF8String:newHostId.c_str()];
    _containerView.hostId = hostId;
  }

  if (oldHostReadyId != newHostReadyId) {
    NSString* hostReadyId = newHostReadyId.empty()
                                ? nil
                                : [NSString stringWithUTF8String:newHostReadyId.c_str()];
    _containerView.hostReadyId = hostReadyId;
  }

  if (oldUpdateRevision != newUpdateRevision) {
    _containerView.updateRevision = newUpdateRevision;
  }

  if (oldMountedRevision != newMountedRevision) {
    _containerView.mountedRevision = newMountedRevision;
  }

  [_containerView refreshDetachedChildrenHost];
}

- (void)prepareForRecycle {
  [super prepareForRecycle];
  [_debugName release];
  _debugName = nil;
  _containerView.hostId = nil;
  _containerView.hostReadyId = nil;
  _containerView.debugName = nil;
  _containerView.nativeViewHandle = nil;
  _containerView.childrenViewHandle = nil;
  _containerView.controllerHandle = nil;
  _containerView.detachControllerView = NO;
  _containerView.updateRevision = 0;
  _containerView.mountedRevision = 0;
}

+ (ComponentDescriptorProvider)componentDescriptorProvider {
  return concreteComponentDescriptorProvider<NativeScriptUIViewComponentDescriptor>();
}

@end

Class<RCTComponentViewProtocol> NativeScriptUIViewCls(void) {
  return NativeScriptUIViewComponentView.class;
}
