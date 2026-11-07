/*
 * ArgsWrapper.h
 *
 *  Created on: Dec 20, 2013
 *      Author: slavchev
 */

#ifndef ARGSWRAPPER_H_
#define ARGSWRAPPER_H_
#include "Engine.h"

namespace tns {
enum class ArgType {
    Class,
    Interface
};

struct ArgsWrapper {
    public:
        ArgsWrapper(const JsValue* argv_, size_t argc_, ArgType t)
            :
            argv(argv_), argc(argc_), type(t) {
        }
        const JsValue* argv;
        size_t argc;
        ArgType type;
};
}

#endif /* ARGSWRAPPER_H_ */
