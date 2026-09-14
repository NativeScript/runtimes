#include "NativeScriptComponentDescriptor.h"

#include <yoga/Yoga.h>

namespace facebook::react {

extern const char NativeScriptComponentName[] = "NativeScriptComponent";

NativeScriptProps::NativeScriptProps(const PropsParserContext& context,
                                     const NativeScriptProps& sourceProps,
                                     const RawProps& rawProps)
    : ViewProps(context, sourceProps, rawProps), rawProps(rawProps.toDynamic()) {}

namespace {

yoga::Style::Length NativeScriptEdgeValue(yoga::Style::Length edge,
                                          yoga::Style::Length axis,
                                          yoga::Style::Length fallback) {
  if (edge.isDefined()) {
    return edge;
  }
  if (axis.isDefined()) {
    return axis;
  }
  return fallback;
}

yoga::Style::Length NativeScriptInsetMargin(bool enabled,
                                            Float inset,
                                            yoga::Style::Length margin) {
  return yoga::Style::Length::points(
      margin.value().unwrapOrDefault(0) + (enabled ? inset : 0));
}

}  // namespace

void NativeScriptShadowNode::adjustLayoutWithState() {
  ensureUnsealed();

  auto state = std::static_pointer_cast<const ConcreteState>(getState());
  if (state == nullptr) {
    return;
  }

  const auto& data = state->getData();
  const auto& props = getConcreteProps();
  auto adjusted = props.yogaStyle;

  if (data.hasLayoutInsets) {
    const auto all = props.yogaStyle.margin(yoga::Edge::All);
    const auto top = NativeScriptEdgeValue(props.yogaStyle.margin(yoga::Edge::Top),
                                           props.yogaStyle.margin(yoga::Edge::Vertical), all);
    const auto left = NativeScriptEdgeValue(props.yogaStyle.margin(yoga::Edge::Left),
                                            props.yogaStyle.margin(yoga::Edge::Horizontal), all);
    const auto right = NativeScriptEdgeValue(props.yogaStyle.margin(yoga::Edge::Right),
                                             props.yogaStyle.margin(yoga::Edge::Horizontal), all);
    const auto bottom = NativeScriptEdgeValue(props.yogaStyle.margin(yoga::Edge::Bottom),
                                              props.yogaStyle.margin(yoga::Edge::Vertical), all);
    adjusted.setMargin(yoga::Edge::Top,
                       NativeScriptInsetMargin(data.insetTopEnabled, data.insetTop, top));
    adjusted.setMargin(yoga::Edge::Left,
                       NativeScriptInsetMargin(data.insetLeftEnabled, data.insetLeft, left));
    adjusted.setMargin(yoga::Edge::Right,
                       NativeScriptInsetMargin(data.insetRightEnabled, data.insetRight, right));
    adjusted.setMargin(yoga::Edge::Bottom,
                       NativeScriptInsetMargin(data.insetBottomEnabled, data.insetBottom, bottom));
  }

  if (data.hasContentInsets) {
    const auto all = props.yogaStyle.padding(yoga::Edge::All);
    const auto top = NativeScriptEdgeValue(props.yogaStyle.padding(yoga::Edge::Top),
                                           props.yogaStyle.padding(yoga::Edge::Vertical), all);
    const auto left = NativeScriptEdgeValue(props.yogaStyle.padding(yoga::Edge::Left),
                                            props.yogaStyle.padding(yoga::Edge::Horizontal), all);
    const auto right = NativeScriptEdgeValue(props.yogaStyle.padding(yoga::Edge::Right),
                                             props.yogaStyle.padding(yoga::Edge::Horizontal), all);
    const auto bottom = NativeScriptEdgeValue(props.yogaStyle.padding(yoga::Edge::Bottom),
                                              props.yogaStyle.padding(yoga::Edge::Vertical), all);
    adjusted.setPadding(yoga::Edge::Top,
                        NativeScriptInsetMargin(true, data.contentInsetTop, top));
    adjusted.setPadding(yoga::Edge::Left,
                        NativeScriptInsetMargin(true, data.contentInsetLeft, left));
    adjusted.setPadding(yoga::Edge::Right,
                        NativeScriptInsetMargin(true, data.contentInsetRight, right));
    adjusted.setPadding(yoga::Edge::Bottom,
                        NativeScriptInsetMargin(true, data.contentInsetBottom, bottom));
  }

  const auto& current = yogaNode_.style();
  const bool changed =
      adjusted.margin(yoga::Edge::Top) != current.margin(yoga::Edge::Top) ||
      adjusted.margin(yoga::Edge::Left) != current.margin(yoga::Edge::Left) ||
      adjusted.margin(yoga::Edge::Right) != current.margin(yoga::Edge::Right) ||
      adjusted.margin(yoga::Edge::Bottom) != current.margin(yoga::Edge::Bottom) ||
      adjusted.padding(yoga::Edge::Top) != current.padding(yoga::Edge::Top) ||
      adjusted.padding(yoga::Edge::Left) != current.padding(yoga::Edge::Left) ||
      adjusted.padding(yoga::Edge::Right) != current.padding(yoga::Edge::Right) ||
      adjusted.padding(yoga::Edge::Bottom) != current.padding(yoga::Edge::Bottom);
  if (changed) {
    yogaNode_.setStyle(adjusted);
    yogaNode_.setDirty(true);
  }
}

void NativeScriptShadowNode::layout(LayoutContext layoutContext) {
  YogaLayoutableShadowNode::layout(layoutContext);

  auto state = std::static_pointer_cast<const ConcreteState>(getState());
  if (state == nullptr || state->getData().contentOffsetMode != 1) {
    return;
  }

  ensureUnsealed();
  const auto& data = state->getData();
  layoutMetrics_.frame.origin.x = data.contentOffsetX;
  layoutMetrics_.frame.origin.y = data.contentOffsetY;
}

Point NativeScriptShadowNode::getContentOriginOffset(
    bool includeTransform) const {
  auto state = std::static_pointer_cast<const ConcreteState>(getState());
  if (state != nullptr && state->getData().contentOffsetMode == 2) {
    const auto& data = state->getData();
    return Point{data.contentOffsetX, data.contentOffsetY};
  }
  return YogaLayoutableShadowNode::getContentOriginOffset(includeTransform);
}

ComponentHandle NativeScriptComponentDescriptor::getComponentHandle() const {
  return reinterpret_cast<ComponentHandle>(getComponentName());
}

ComponentName NativeScriptComponentDescriptor::getComponentName() const {
  if (flavor_ == nullptr) {
    // No flavor: fall back to the generic compile-time name. In practice
    // every NativeScript-registered name goes through
    // NativeScriptRegisterFlavoredComponent, which always sets a flavor.
    return NativeScriptShadowNode::Name();
  }
  return static_cast<const std::string*>(flavor_.get())->c_str();
}

void NativeScriptComponentDescriptor::adopt(ShadowNode& shadowNode) const {
  auto& concreteShadowNode = static_cast<NativeScriptShadowNode&>(shadowNode);
  concreteShadowNode.adjustLayoutWithState();

  auto& layoutableShadowNode = static_cast<YogaLayoutableShadowNode&>(shadowNode);

  auto state = std::static_pointer_cast<const NativeScriptShadowNode::ConcreteState>(shadowNode.getState());
  if (state != nullptr) {
    const NativeScriptState& stateData = state->getData();
    if (stateData.nativeSizeAuthority && stateData.contentSize.width != 0 &&
        stateData.contentSize.height != 0) {
      layoutableShadowNode.setSize(Size{stateData.contentSize.width, stateData.contentSize.height});
    }
  }

  ConcreteComponentDescriptor::adopt(shadowNode);
}

}  // namespace facebook::react
