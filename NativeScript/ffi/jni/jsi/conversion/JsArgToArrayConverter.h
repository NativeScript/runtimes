#ifndef JSARGTOARRAYCONVERTER_H_
#define JSARGTOARRAYCONVERTER_H_

#include "JEnv.h"
#include "JniLocalRef.h"
#include "Engine.h"
#include <vector>
#include <string>

namespace tns {
class ObjectManager;

class JsArgToArrayConverter {
    public:
        JsArgToArrayConverter(JsRuntime &rt, size_t argc, const JsValue* argv, bool hasImplementationObject);

        // `objectManager` may be supplied pre-resolved (avoids a locked runtime
        // lookup); falls back to resolving internally when omitted.
        JsArgToArrayConverter(JsRuntime &rt, const JsValue &arg, bool isImplementationObject, int classReturnType,
                              ObjectManager* objectManager = nullptr);

        ~JsArgToArrayConverter();

        jobjectArray ToJavaArray();

        jobject GetConvertedArg();

        int Length() const;

        bool IsValid() const;

        struct Error;

        Error GetError() const;

        struct Error {
            Error() :
                index(-1), msg(std::string()) {
            }

            int index;
            std::string msg;
        };

    private:
        bool ConvertArg(JsRuntime &rt, const JsValue &arg, int index);

        void SetConvertedObject(JEnv& env, int index, jobject obj, bool isGlobal = false);

        int m_argsLen;

        int m_return_type;

        bool m_isValid;

        Error m_error;

        std::vector<int> m_storedIndexes;

        jobject* m_argsAsObject;

        // Inline storage for the common small-arity case (esp. the single-arg
        // path used per object-array element); heap only when larger.
        static const int INLINE_CAPACITY = 8;
        jobject m_inlineArgs[INLINE_CAPACITY];

        // Cached ObjectManager threaded from the caller.
        ObjectManager* m_objectManager = nullptr;

        jobjectArray m_arr;

        short MAX_JAVA_PARAMS_COUNT = 256;

        static jclass JAVA_LANG_OBJECT_CLASS;
};
}

#endif /* JSARGTOARRAYCONVERTER_H_ */
