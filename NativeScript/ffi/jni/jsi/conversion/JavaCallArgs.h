#ifndef JAVACALLARGS_H_
#define JAVACALLARGS_H_

#include "jni.h"

namespace tns {
    // The arrays com.tns.Runtime.packageArgs builds for a Java -> JS call.
    struct JavaCallArgs {
        jintArray types;
        jlongArray values;
        jobjectArray refs;
    };
}

#endif /* JAVACALLARGS_H_ */
