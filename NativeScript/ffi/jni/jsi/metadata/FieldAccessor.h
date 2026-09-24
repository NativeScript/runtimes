#ifndef FIELDACCESSOR_H_
#define FIELDACCESSOR_H_

#include "JEnv.h"
#include <string>
#include "ObjectManager.h"
#include "FieldCallbackData.h"

namespace tns {
class FieldAccessor {
    public:
        // `objectManager` and `targetJavaObject` may be supplied pre-resolved by
        // the caller (the accessor callback) so this avoids a locked runtime
        // lookup and a second host-object probe. Both fall back to resolving
        // internally when omitted.
        JsValue GetJavaField(JsRuntime& rt, const JsValue& target, FieldCallbackData* fieldData,
                             ObjectManager* objectManager = nullptr,
                             JniLocalRef targetJavaObject = JniLocalRef());

        void SetJavaField(JsRuntime& rt, const JsValue& target, const JsValue& value,
                          FieldCallbackData* fieldData,
                          ObjectManager* objectManager = nullptr,
                          JniLocalRef targetJavaObject = JniLocalRef());
};
}

#endif /* FIELDACCESSOR_H_ */
