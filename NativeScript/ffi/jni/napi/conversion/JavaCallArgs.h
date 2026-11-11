#ifndef JAVACALLARGS_H_
#define JAVACALLARGS_H_

#include "jni.h"
#include "JType.h"

namespace tns {
    struct JavaCallArgs {
        jint count;
        jlongArray values;
        jobjectArray refs;

        static constexpr int PackedLength(int count) {
            return count + (count + 7) / 8;
        }

        static Type TypeAt(const jlong *packed, int count, int i) {
            return static_cast<Type>((packed[count + i / 8] >> ((i % 8) * 8)) & 0xff);
        }
    };
}

#endif /* JAVACALLARGS_H_ */
