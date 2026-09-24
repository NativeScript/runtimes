#ifndef ARRAYBUFFERHELPER_H_
#define ARRAYBUFFERHELPER_H_

#include "ObjectManager.h"

namespace tns {
    class ArrayBufferHelper {
        public:
            ArrayBufferHelper();

            void CreateConvertFunctions(JsRuntime &rt, JsObject &global, ObjectManager* objectManager);

        private:

            JsValue CreateFromCallbackImpl(JsRuntime &rt, const JsValue* args, size_t argc);

            ObjectManager* m_objectManager;

            jclass m_ByteBufferClass;
            jmethodID m_isDirectMethodID;
            jmethodID m_remainingMethodID;
            jmethodID m_getMethodID;
    };
}


#endif /* ARRAYBUFFERHELPER_H_ */
