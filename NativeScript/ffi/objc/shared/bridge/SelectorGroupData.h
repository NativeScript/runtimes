#pragma once

#include "SelectorGroupState.h"

struct NativeApiSelectorGroupData : NativeApiSelectorGroupState {
  template <typename RuntimeState>
  NativeApiSelectorGroupData(
      std::shared_ptr<RuntimeState> state,
      std::shared_ptr<NativeApiBridge> bridge, Class lookupClass,
      bool receiverIsClass,
      std::shared_ptr<std::vector<NativeApiSelectorGroupEntry>> selectors,
      std::shared_ptr<
          std::vector<std::shared_ptr<NativeApiPreparedObjCInvocation>>>
          preparedInvocations,
      std::weak_ptr<NativeApiObjectHostObject> boundReceiver = {},
      std::shared_ptr<NativeApiObjectLifetimeState> boundReceiverState = nullptr)
      : NativeApiSelectorGroupState(
            std::move(bridge), lookupClass, receiverIsClass,
            std::move(selectors), std::move(preparedInvocations),
            std::move(boundReceiver), std::move(boundReceiverState)),
        runtime(std::move(state)) {}

  Runtime runtime;
};
