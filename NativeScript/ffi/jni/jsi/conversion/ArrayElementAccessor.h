#ifndef ARRAYELEMENTACCESSOR_H_
#define ARRAYELEMENTACCESSOR_H_

#include "JEnv.h"
#include "JniLocalRef.h"
#include "Engine.h"
#include <string>
#include <string_view>
#include "ObjectManager.h"


namespace tns {
    class ArrayElementAccessor {
    public:
        // `objectManager` and `arrayObject` may be supplied pre-resolved by the
        // caller (e.g. the host object's indexed get/set trap or an array-loop
        // helper) to avoid a locked runtime lookup and re-resolving the Java array
        // on every element. Both fall back to resolving internally when omitted.
        JsValue GetArrayElement(JsRuntime &rt, const JsValue &array, uint32_t index,
                                const std::string& arraySignature,
                                ObjectManager* objectManager = nullptr,
                                jobject arrayObject = nullptr);

        void SetArrayElement(JsRuntime &rt, const JsValue &array, uint32_t index,
                             const std::string& arraySignature, const JsValue &value,
                             ObjectManager* objectManager = nullptr,
                             jobject arrayObject = nullptr);

    private:
        JsValue ConvertToJsValue(JsRuntime &rt, ObjectManager* objectManager, JEnv& jEnv,
                                 std::string_view elementSignature, const void* value);
        void assertNonNullNativeArray(tns::JniLocalRef& arrayReference);
    };
}

#endif /* ARRAYELEMENTACCESSOR_H_ */
