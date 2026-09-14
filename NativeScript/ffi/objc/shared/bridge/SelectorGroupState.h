#pragma once

struct NativeApiSelectorGroupState {
  NativeApiSelectorGroupState(
      std::shared_ptr<NativeApiBridge> bridge, Class lookupClass,
      bool receiverIsClass,
      std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors,
      std::shared_ptr<
          std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
          preparedInvocations,
      std::weak_ptr<NativeApiObjectHostObject> boundReceiver = {},
      std::shared_ptr<NativeApiObjectLifetimeState> boundReceiverState = nullptr)
      : bridge(std::move(bridge)),
        lookupClass(lookupClass),
        receiverIsClass(receiverIsClass),
        selectors(std::move(selectors)),
        preparedInvocations(std::move(preparedInvocations)),
        boundReceiver(std::move(boundReceiver)),
        boundReceiverState(std::move(boundReceiverState)) {}

  std::shared_ptr<NativeApiBridge> bridge;
  Class lookupClass = Nil;
  bool receiverIsClass = false;
  std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors;
  std::shared_ptr<
      std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
      preparedInvocations;
  std::weak_ptr<NativeApiObjectHostObject> boundReceiver;
  std::shared_ptr<NativeApiObjectLifetimeState> boundReceiverState;
  Class cachedReceiverClass = Nil;
  Class cachedDispatchClass = Nil;
};
