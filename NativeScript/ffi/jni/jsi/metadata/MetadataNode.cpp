#include <string>
#include <sstream>
#include <cctype>
#include <regex>
#include <dirent.h>
#include <set>
#include <cerrno>
#include <unistd.h>
#include "NativeScriptException.h"
#include "MetadataNode.h"
#include "CallbackHandlers.h"
#include "NativeScriptAssert.h"
#include "File.h"
#include "Runtime.h"
#include "ArgConverter.h"
#include "FieldCallbackData.h"
#include "MetadataBuilder.h"
#include "ArgsWrapper.h"
// Only referenced from the module-prologue column adjustment below, which is
// itself standalone-only -- a guest has no module wrapper to undo.
#if !defined(NS_JSI_HOST_RUNTIME)
#include "ModuleInternal.h"
#endif
#include "Util.h"
#include "GlobalHelpers.h"
#include "JSONObjectHelper.h"

using namespace std;

namespace {
// Carries a MetadataNode* in an object's native-state slot; see
// MetadataNode::GetNullNode. It overrides none of the HostObject traps and is
// never handed to JS as an object of its own.
struct NullNodeState : public engine::HostObject {
    explicit NullNodeState(MetadataNode *node) : node(node) {}

    MetadataNode *node;
};

// Wraps a callback in the NativeScriptException -> JS translation every
// MetadataNode callback repeats. A JSError is already the engine's own throw and
// is left to propagate.
template <typename Fn>
JsValue Guarded(JsRuntime &rt, Fn &&fn) {
    try {
        return fn();
    } catch (NativeScriptException &e) {
        e.ReThrowToJs(rt);
    } catch (JsError &) {
        throw;
    } catch (std::exception &e) {
        stringstream ss;
        ss << "Error: c++ exception: " << e.what() << endl;
        NativeScriptException nsEx(ss.str());
        nsEx.ReThrowToJs(rt);
    } catch (...) {
        NativeScriptException nsEx(std::string("Error: c++ exception!"));
        nsEx.ReThrowToJs(rt);
    }
}

// Defines `name` on `object` as an accessor backed by the given host functions.
void DefineAccessor(JsRuntime &rt, const JsObject &object, const char *name,
                    engine::HostFunctionType getter, engine::HostFunctionType setter) {
    JsFunction getterFn;
    JsFunction setterFn;
    if (getter) {
        getterFn = JsFunction::createFromHostFunction(
                rt, engine::PropNameID::forAscii(rt, name), 0, std::move(getter));
    }
    if (setter) {
        setterFn = JsFunction::createFromHostFunction(
                rt, engine::PropNameID::forAscii(rt, name), 1, std::move(setter));
    }
    js_util::define_property_get_set(rt, object, name, getter ? &getterFn : nullptr,
                                     setter ? &setterFn : nullptr);
}
}

void MetadataNode::Init(JsRuntime &rt) {
    auto cache = GetMetadataNodeCache(rt);
}

JsValue MetadataNode::CreateArrayObjectConstructor(JsRuntime &rt) {
    auto it = s_arrayObjects.find(rt.identity());
    if (it != s_arrayObjects.end()) {
        if (!js_util::is_null_or_undefined(it->second)) return JsValue(rt, it->second);
    }

    auto node = GetOrCreate("java/lang/Object");
    auto objectConstructor = node->GetConstructorFunction(rt);

    // The engine layer synthesises the receiver for a host constructor (with the
    // right prototype) on every backend, so the napi tree's EnsureConstructorThis
    // -- which existed because some engines handed a null `this` -- has no
    // counterpart here and this body is empty.
    auto arrayConstructor = JsFunction::createFromHostConstructor(
            rt, engine::PropNameID::forAscii(rt, "ArrayObjectWrapper"), 0,
            [](JsRuntime &rt, const JsValue &thisVal, const JsValue *, size_t) -> JsValue {
                return JsValue(rt, thisVal);
            });

    auto proto = arrayConstructor.getPropertyAsObject(rt, "prototype");
    ObjectManager::MarkObject(rt, JsValue(rt, proto));

    js_util::set_function(rt, proto, "setValueAtIndex", ArraySetterCallback);
    js_util::set_function(rt, proto, "getValueAtIndex", ArrayGetterCallback);
    js_util::set_function(rt, proto, "getAllValues", ArrayGetAllValuesCallback);
    DefineAccessor(rt, proto, "length", ArrayLengthCallback, nullptr);

    // Native helpers (previously synthesized by the JS getNativeArrayProp).
    js_util::set_function(rt, proto, "map", ArrayMapCallback);
    js_util::set_function(rt, proto, "forEach", ArrayForEachCallback);
    js_util::set_function(rt, proto, "toString", ArrayToStringCallback);
    {
        auto symbolCtor = rt.global().getPropertyAsObject(rt, "Symbol");
        auto symbolIterator = symbolCtor.getProperty(rt, "iterator");
        auto iteratorFn = JsFunction::createFromHostFunction(
                rt, engine::PropNameID::forAscii(rt, "[Symbol.iterator]"), 0,
                ArraySymbolIteratorCallback);
        proto.setProperty(rt, symbolIterator, JsValue(rt, iteratorFn));
    }

    js_util::inherits(rt, arrayConstructor, objectConstructor.asObject(rt));

    JsValue result(rt, arrayConstructor);
    s_arrayObjects.emplace(rt.identity(), JsValue(rt, result));

    return result;
}

JsValue MetadataNode::CreateExtendedJSWrapper(JsRuntime &rt, ObjectManager *objectManager,
                                              const std::string &proxyClassName,
                                              int javaObjectID, MetadataNode **outNode) {
    JsValue extInstance;

    auto cacheData = GetCachedExtendedClassData(rt, proxyClassName);

    if (cacheData.node != nullptr) {
        extInstance = objectManager->GetEmptyObject();
        if (js_util::is_null_or_undefined(extInstance)) {
            return js_util::undefined();
        }
        ObjectManager::MarkSuperCall(rt, extInstance);
        auto extendedCtorFunc = cacheData.extendedCtorFunction.asObjectBorrowed(rt);
        auto extendedPrototype = extendedCtorFunc.getProperty(rt, "prototype");
        js_util::setPrototypeOf(rt, extInstance, extendedPrototype);

        extInstance.asObjectBorrowed(rt).setProperty(rt, "constructor",
                                                     cacheData.extendedCtorFunction);

        SetInstanceMetadata(rt, extInstance, cacheData.node);
        *outNode = cacheData.node;
    }

    return extInstance;
}

string MetadataNode::GetTypeMetadataName(JsRuntime &rt, const JsValue &value) {
    if (!value.isObject()) return "";
    auto typeMetadataName = value.asObjectBorrowed(rt).getProperty(rt, PRIVATE_TYPE_NAME);
    if (!typeMetadataName.isString()) return "";
    return typeMetadataName.asString(rt).utf8(rt);
}


bool MetadataNode::isArray() {
    return m_isArray;
}

JsValue MetadataNode::CreateJSWrapper(JsRuntime &rt, ObjectManager *objectManager) {
    if (m_isArray) {
        return CreateArrayWrapper(rt);
    }

    JsValue obj = objectManager->GetEmptyObject();
    JsValue ctorFunc = GetConstructorFunction(rt);
    auto object = obj.asObjectBorrowed(rt);
    object.setProperty(rt, "constructor", ctorFunc);
    js_util::setPrototypeOf(rt, obj, js_util::get_prototype(rt, ctorFunc));
    SetInstanceMetadata(rt, obj, this);

    return obj;
}

JsValue MetadataNode::ArrayGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        int32_t indexValue = argc > 0 ? js_util::get_int32(args[0]) : 0;
        auto node = GetInstanceMetadata(rt, thisVal);

        return CallbackHandlers::GetArrayElement(rt, thisVal, indexValue, node->m_name);
    });
}

JsValue MetadataNode::ArrayGetAllValuesCallback(JsRuntime &rt, const JsValue &thisVal,
                                                const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        auto node = GetInstanceMetadata(rt, thisVal);
        auto length = CallbackHandlers::GetArrayLength(rt, thisVal);
        engine::Array arr(rt, (size_t) length);

        // Resolve the manager + backing array once for the whole loop.
        auto objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
        JniLocalRef javaArr = objectManager->GetJavaObjectByJsObjectFast(thisVal);
        jobject javaArrObj = javaArr;

        for (int i = 0; i < length; i++) {
            JsValue element = CallbackHandlers::GetArrayElement(rt, thisVal, i, node->m_name,
                                                                objectManager, javaArrObj);
            arr.setValueAtIndex(rt, (size_t) i, element);
        }

        return JsValue(rt, arr);
    });
}

JsValue MetadataNode::ArraySetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        if (argc < 2) return js_util::undefined();

        int32_t indexValue = js_util::get_int32(args[0]);
        auto node = GetInstanceMetadata(rt, thisVal);

        CallbackHandlers::SetArrayElement(rt, thisVal, indexValue, node->m_name, args[1]);
        return JsValue(rt, args[1]);
    });
}

JsValue MetadataNode::ArrayLengthCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        return JsValue(CallbackHandlers::GetArrayLength(rt, thisVal));
    });
}

JsValue MetadataNode::ArrayMapCallback(JsRuntime &rt, const JsValue &thisVal,
                                       const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        if (argc < 1 || !args[0].isObject()) return js_util::undefined();
        auto callback = args[0].asObjectBorrowed(rt).asFunction(rt);
        auto node = GetInstanceMetadata(rt, thisVal);
        int length = CallbackHandlers::GetArrayLength(rt, thisVal);

        engine::Array result(rt, (size_t) length);

        // Resolve the manager + backing array once for the whole loop.
        auto objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
        JniLocalRef javaArr = objectManager->GetJavaObjectByJsObjectFast(thisVal);
        jobject javaArrObj = javaArr;

        for (int i = 0; i < length; i++) {
            JsValue element =
                    CallbackHandlers::GetArrayElement(rt, thisVal, i, node->m_name,
                                                      objectManager, javaArrObj);
            const JsValue cbArgs[] = {element, JsValue(i), JsValue(rt, thisVal)};
            JsValue mapped = callback.call(rt, cbArgs, (size_t) 3);
            result.setValueAtIndex(rt, (size_t) i, mapped);
        }

        return JsValue(rt, result);
    });
}

JsValue MetadataNode::ArrayForEachCallback(JsRuntime &rt, const JsValue &thisVal,
                                           const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        if (argc < 1 || !args[0].isObject()) return js_util::undefined();
        auto callback = args[0].asObjectBorrowed(rt).asFunction(rt);
        auto node = GetInstanceMetadata(rt, thisVal);
        int length = CallbackHandlers::GetArrayLength(rt, thisVal);

        // Resolve the manager + backing array once for the whole loop.
        auto objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
        JniLocalRef javaArr = objectManager->GetJavaObjectByJsObjectFast(thisVal);
        jobject javaArrObj = javaArr;

        for (int i = 0; i < length; i++) {
            JsValue element =
                    CallbackHandlers::GetArrayElement(rt, thisVal, i, node->m_name,
                                                      objectManager, javaArrObj);
            const JsValue cbArgs[] = {element, JsValue(i), JsValue(rt, thisVal)};
            callback.call(rt, cbArgs, (size_t) 3);
        }

        return js_util::undefined();
    });
}

namespace {
// Builds a real JS array snapshot of all elements (native get loop).
engine::Array BuildArraySnapshot(JsRuntime &rt, const JsValue &thisVal,
                                 const std::string &signature) {
    int length = CallbackHandlers::GetArrayLength(rt, thisVal);
    engine::Array values(rt, (size_t) length);

    // Resolve the manager + backing array once for the whole loop.
    auto objectManager = tns::Runtime::GetRuntime(rt)->GetObjectManager();
    JniLocalRef javaArr = objectManager->GetJavaObjectByJsObjectFast(thisVal);
    jobject javaArrObj = javaArr;

    for (int i = 0; i < length; i++) {
        JsValue element =
                CallbackHandlers::GetArrayElement(rt, thisVal, i, signature,
                                                  objectManager, javaArrObj);
        values.setValueAtIndex(rt, (size_t) i, element);
    }
    return values;
}
}

JsValue MetadataNode::ArrayToStringCallback(JsRuntime &rt, const JsValue &thisVal,
                                            const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        auto node = GetInstanceMetadata(rt, thisVal);
        auto values = BuildArraySnapshot(rt, thisVal, node->m_name);

        // values.join(",")
        auto joinFn = values.getPropertyAsFunction(rt, "join");
        const JsValue joinArgs[] = {js_util::to_js_string(rt, ",")};
        return joinFn.callWithThis(rt, values, joinArgs, (size_t) 1);
    });
}

JsValue
MetadataNode::ArraySymbolIteratorCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t argc) {
    return Guarded(rt, [&]() -> JsValue {
        auto node = GetInstanceMetadata(rt, thisVal);
        auto values = BuildArraySnapshot(rt, thisVal, node->m_name);

        // return values[Symbol.iterator]()  -> delegate to the real array iterator
        auto symbolCtor = rt.global().getPropertyAsObject(rt, "Symbol");
        auto symbolIterator = symbolCtor.getProperty(rt, "iterator");
        auto iterMethod = values.getProperty(rt, symbolIterator);
        return iterMethod.asObject(rt).asFunction(rt).callWithThis(rt, values);
    });
}

JsValue MetadataNode::CreateArrayWrapper(JsRuntime &rt) {
    JsValue constructor = CreateArrayObjectConstructor(rt);
    auto instance = constructor.asObjectBorrowed(rt).asFunction(rt)
            .callAsConstructor(rt, static_cast<const JsValue *>(nullptr), (size_t) 0);
    SetInstanceMetadata(rt, instance, this);
    return instance;
}

JsValue MetadataNode::GetImplementationObject(JsRuntime &rt, const JsValue &object) {
    if (!object.isObject()) return js_util::undefined();

    JsValue currentPrototype = JsValue(rt, object);

    JsValue implementationObject =
            object.asObjectBorrowed(rt).getProperty(rt, CLASS_IMPLEMENTATION_OBJECT);

    if (!implementationObject.isUndefined()) {
        return implementationObject;
    }

    auto objectAsObject = object.asObjectBorrowed(rt);

    if (js_util::has_own_property(rt, objectAsObject,
                                  PROP_KEY_IS_PROTOTYPE_IMPLEMENTATION_OBJECT)) {
        if (!js_util::has_own_property(rt, objectAsObject, "prototype")) {
            return js_util::undefined();
        }

        return js_util::get_prototype(rt, object);
    }

    auto activityImplementationObject =
            objectAsObject.getProperty(rt, "t::ActivityImplementationObject");

    if (!activityImplementationObject.isUndefined()) {
        return activityImplementationObject;
    }

    JsValue lastPrototype;

    bool prototypeCycleDetected = false;

    bool foundImplementationObject = false;

    while (!foundImplementationObject) {
        currentPrototype = js_util::getPrototypeOf(rt, currentPrototype);

        if (currentPrototype.isNull() || currentPrototype.isUndefined()) {
            break;
        }

        if (js_util::strict_equal(rt, lastPrototype, currentPrototype)) {
            auto abovePrototype = js_util::getPrototypeOf(rt, currentPrototype);
            prototypeCycleDetected = js_util::strict_equal(rt, abovePrototype, currentPrototype);
            break;
        }

        if (currentPrototype.isNull() || prototypeCycleDetected) {
            return js_util::undefined();
        }

        auto implObject =
                currentPrototype.asObjectBorrowed(rt).getProperty(rt, CLASS_IMPLEMENTATION_OBJECT);

        if (!implObject.isUndefined()) {
            foundImplementationObject = true;
            return currentPrototype;
        }

        lastPrototype = JsValue(rt, currentPrototype);
    }

    return implementationObject;
}

void MetadataNode::SetInstanceMetadata(JsRuntime &rt, const JsValue &object, MetadataNode *node) {
    // node lives on the per-instance JSInstanceInfo (set in ObjectManager::Link /
    // GetOrCreateProxy); the napi tree's non-host "#instance_metadata" external
    // has no counterpart here.
    (void) rt;
    (void) object;
    (void) node;
}

MetadataNode *MetadataNode::GetNullNode(JsRuntime &rt, const JsValue &value) {
    if (!value.isObject()) return nullptr;
    auto state = value.asObjectBorrowed(rt).getNativeState<NullNodeState>(rt);
    return state != nullptr ? state->node : nullptr;
}


JsValue MetadataNode::ExtendedClassConstructorCallback(JsRuntime &rt, const JsValue &thisVal,
                                                       const JsValue *args, size_t argc,
                                                       ExtendedClassCallbackData *extData) {
    return Guarded(rt, [&]() -> JsValue {
        // The engine layer builds the receiver for a host constructor on every
        // backend, so the napi tree's new.target probe and EnsureConstructorThis
        // fallback are both unnecessary here.
        JsValue receiver(rt, thisVal);

        SetInstanceMetadata(rt, receiver, extData->node);

        ObjectManager::MarkSuperCall(rt, receiver);

        string fullClassName = extData->fullClassName;

        ArgsWrapper argWrapper(args, argc, ArgType::Class);
        JsValue jsThisProxy;
        bool success = CallbackHandlers::RegisterInstance(rt, receiver, fullClassName, argWrapper,
                                                          extData->implementationObject, false,
                                                          &jsThisProxy, extData->node->m_name,
                                                          extData->node);

        return jsThisProxy;
    });
}

JsValue MetadataNode::InterfaceConstructorCallback(JsRuntime &rt, const JsValue &thisVal,
                                                   const JsValue *args, size_t argc,
                                                   MetadataNode *node) {
    return Guarded(rt, [&]() -> JsValue {
        JsValue implementationObject;
        JsValue interfaceName;

        if (argc == 1) {
            if (!args[0].isObject()) {
                throw NativeScriptException(
                        string("Invalid arguments provided, first argument must be an object if only one argument is provided"));
            }
            implementationObject = JsValue(rt, args[0]);
        } else if (argc == 2) {
            if (!args[0].isString()) {
                throw NativeScriptException(
                        string("Invalid arguments provided, first argument must be a string if only two argument is provided"));
            }

            if (!args[1].isObject()) {
                throw NativeScriptException(
                        string("Invalid arguments provided, second argument must be an object if only one argument is provided"));
            }

            interfaceName = JsValue(rt, args[0]);
            implementationObject = JsValue(rt, args[1]);
        } else {
            throw NativeScriptException(
                    string("Invalid arguments provided, first argument must be a string and second argument must be an object"));
        }

        auto className = node->m_implType;
        JsValue receiver(rt, thisVal);

        SetInstanceMetadata(rt, receiver, node);

        ObjectManager::MarkSuperCall(rt, receiver);


        js_util::setPrototypeOf(rt, implementationObject,
                                js_util::getPrototypeOf(rt, receiver));

        js_util::setPrototypeOf(rt, receiver, implementationObject);

        receiver.asObjectBorrowed(rt).setProperty(rt, CLASS_IMPLEMENTATION_OBJECT,
                                                  implementationObject);

        ArgsWrapper argsWrapper(args, argc, ArgType::Interface);

        JsValue jsThisProxy;
        auto success = CallbackHandlers::RegisterInstance(rt, receiver, className, argsWrapper,
                                                          implementationObject, true, &jsThisProxy,
                                                          std::string(), node);
        return jsThisProxy;
    });
}

JsValue MetadataNode::ClassConstructorCallback(JsRuntime &rt, const JsValue &thisVal,
                                               const JsValue *args, size_t argc,
                                               MetadataNode *node) {
    return Guarded(rt, [&]() -> JsValue {
        JsValue receiver(rt, thisVal);

        SetInstanceMetadata(rt, receiver, node);

        // Plain construction has no extend name, so the full class name equals the
        // base class name; skip CreateFullClassName (a string copy) and use the
        // node's name directly for both.
        const string &className = node->m_name;

        ArgsWrapper argsWrapper(args, argc, ArgType::Class);
        JsValue jsThisProxy;
        bool success = CallbackHandlers::RegisterInstance(rt, receiver, className, argsWrapper,
                                                          js_util::undefined(), false, &jsThisProxy,
                                                          className, node);

        return jsThisProxy;
    });
}

string MetadataNode::CreateFullClassName(const std::string &className,
                                         const std::string &extendNameAndLocation = "") {
    string fullClassName = className;

    // create a class name consisting only of the base class name + last file name part + line + column + variable identifier
    if (!extendNameAndLocation.empty()) {
        string tempClassName = className;
        fullClassName = Util::ReplaceAll(tempClassName, "$", "_");
        fullClassName += "_" + extendNameAndLocation;
    }

    return fullClassName;
}

bool MetadataNode::ContentKeyedBindingsEnabled() {
#if defined(NS_CONTENT_KEYED_BINDINGS)
    return true;
#elif defined(NS_JSI_HOST_RUNTIME)
    // A guest runs inside the embedder's bundle, so no location key computed here
    // can match one the generator computed from a source tree it never saw.
    return true;
#else
    return false;
#endif
}

// A class-name token derived from what the generated binding is actually made
// of, rather than from where the extend() call happens to sit.
//
// The static binding generator writes a class out of exactly three inputs: the
// base class, the implemented interfaces, and the overridden method names. Two
// extend() sites that agree on all three produce byte-identical Java, so hashing
// them is enough to name the class -- and two sites that collide are meant to
// share, which is why the generator already treats a duplicate file with equal
// content as a warning rather than an error.
//
// Location keys cannot do this once an embedder owns the bundler: the generator
// reads source coordinates and the runtime reads bundle coordinates. A content
// key needs no shared coordinate system at all.
//
// Sorted, because the runtime's order is JS own-property order and the
// generator's is source order; the binding treats both sets as unordered.
// FNV-1a rather than std::hash, which is not stable across implementations and
// so could not be reproduced by the generator.
string MetadataNode::CreateContentKey(const string &baseClassName,
                                      vector<string> interfaceNames,
                                      vector<string> methodNames) {
    sort(interfaceNames.begin(), interfaceNames.end());
    sort(methodNames.begin(), methodNames.end());

    string canonicalBase = baseClassName;
    canonicalBase = Util::ReplaceAll(canonicalBase, std::string("/"), std::string("."));

    string payload = canonicalBase;
    payload += "|";
    for (size_t i = 0; i < interfaceNames.size(); i++) {
        if (i > 0) payload += ",";
        payload += interfaceNames[i];
    }
    payload += "|";
    for (size_t i = 0; i < methodNames.size(); i++) {
        if (i > 0) payload += ",";
        payload += methodNames[i];
    }

    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c: payload) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }

    char buffer[19];
    snprintf(buffer, sizeof(buffer), "h%016llx", static_cast<unsigned long long>(hash));

    DEBUG_WRITE("CreateContentKey: %s -> %s", payload.c_str(), buffer);

    return string(buffer);
}

bool MetadataNode::IsValidExtendName(JsRuntime &rt, const JsValue &name) {
    string extendName = ArgConverter::ConvertToString(rt, name);

    for (char currentSymbol: extendName) {
        bool isValidExtendNameSymbol = isalpha(currentSymbol) ||
                                       isdigit(currentSymbol) ||
                                       currentSymbol == '_';
        if (!isValidExtendNameSymbol) {
            return false;
        }
    }

    return true;
}


bool
MetadataNode::GetExtendLocation(JsRuntime &rt, string &extendLocation, bool isTypeScriptExtend) {
    stringstream extendLocationStream;

    auto frames = tns::BuildStacktraceFrames(rt, nullptr, 4);
    if (frames.empty()) {
        DEBUG_WRITE("%s", "FRAME IS NULL!");
        // Reported as a failure, not a success with an empty location. An empty
        // location makes every unnamed extend() of the same base class produce
        // the same generated class name, and the second one's overrides then
        // dispatch to themselves -- a StackOverflowError far from the cause.
        // The caller turns this into "No name specified for extend at location".
        return false;
    }

    tns::JsStacktraceFrame *frame;
    if (isTypeScriptExtend) {
        if (frames.size() > 3 && Util::Contains(frames[2].text, "call_super")) {
            frame = &frames[3];
        } else if (frames.size() > 2) {
            frame = &frames[2]; // the _super.apply call to ts_helpers will always be the third call frame
        } else {
            frame = &frames[0];
        }
    } else {
        frame = &frames[0];
    }

    string srcFileName = Util::ReplaceAll(frame->filename, "file://", "");

    string fullPathToFile;
    if (srcFileName == "<embedded>" || srcFileName == "<input>" || srcFileName == "JavaScript") {
        fullPathToFile = "script";
    } else {
        // The app-root-relative substring this used to compute was assigned and
        // then immediately overwritten by the full name below, so it was already
        // dead -- and it throws for any path that does not start with the app
        // root, which is every path a guest sees.
        fullPathToFile = srcFileName;
        replace(fullPathToFile.begin(), fullPathToFile.end(), '/', '_');
        replace(fullPathToFile.begin(), fullPathToFile.end(), '.', '_');
        replace(fullPathToFile.begin(), fullPathToFile.end(), '-', '_');
        replace(fullPathToFile.begin(), fullPathToFile.end(), ' ', '_');

        vector<string> pathParts;
        Util::SplitString(fullPathToFile, "_", pathParts);
        fullPathToFile =
                pathParts.back() == "js" ? pathParts[pathParts.size() - 2] : pathParts.back();
    }

    if (frame->line < 0) {
        extendLocationStream << fullPathToFile << " unknown line number";
        extendLocation = extendLocationStream.str();
        return false;
    }

    if (frame->col < 0) {
        extendLocationStream << fullPathToFile << " line:" << frame->line
                             << " unknown column number";
        extendLocation = extendLocationStream.str();
        return false;
    }
    int column = frame->col;

    // Both adjustments below exist to undo the runtime's own module wrapper, so
    // that a call site's key matches what the static binding generator computed
    // from the unwrapped source. A guest has no such wrapper -- the embedder's
    // bundler (Metro) owns module wrapping and the static binding generator
    // never saw the source -- so applying them here only corrupts the key, and
    // two different extend() sites can collide onto one generated class.
#if !defined(NS_JSI_HOST_RUNTIME)
    if (frame->line == 1) {
        column -= ModuleInternal::MODULE_PROLOGUE_LENGTH;
    }

#ifdef TARGET_ENGINE_HERMES
    column = column - 6;
#endif
#endif  // !NS_JSI_HOST_RUNTIME

    extendLocationStream << fullPathToFile << "_" << frame->line << "_" << column << "_";
    extendLocation = extendLocationStream.str();
    return true;
}


bool MetadataNode::ValidateExtendArguments(JsRuntime &rt, size_t argc, const JsValue *argv,
                                           bool extendLocationFound, string &extendLocation,
                                           JsValue *extendName, JsValue *implementationObject,
                                           bool isTypeScriptExtend) {

    if (argc == 1) {
        if (!extendLocationFound) {
            stringstream ss;
            ss << "Invalid extend() call. No name specified for extend at location: "
               << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        if (!argv[0].isObject()) {
            stringstream ss;
            ss << "Invalid extend() call. No implementation object specified at location: "
               << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        *implementationObject = JsValue(rt, argv[0]);
    } else if (argc == 2 || isTypeScriptExtend) {
        if (!argv[0].isString()) {
            stringstream ss;
            ss << "Invalid extend() call. No name for extend specified at location: "
               << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        if (!argv[1].isObject()) {
            stringstream ss;
            ss
                    << "Invalid extend() call. Named extend should be called with second object parameter containing overridden methods at location: "
                    << extendLocation.c_str();
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        DEBUG_WRITE("ExtendsCallMethodHandler: getting extend name");

        *extendName = JsValue(rt, argv[0]);
        bool isValidExtendName = IsValidExtendName(rt, *extendName);
        if (!isValidExtendName) {
            stringstream ss;
            ss << "The extend name \"" << ArgConverter::ConvertToString(rt, *extendName)
               << "\" you provided contains invalid symbols. Try using the symbols [a-z, A-Z, 0-9, _]."
               << endl;
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }
        *implementationObject = JsValue(rt, argv[1]);
    } else {
        stringstream ss;
        ss << "Invalid extend() call at location: " << extendLocation.c_str();
        string exceptionMessage = ss.str();
        throw NativeScriptException(exceptionMessage);
    }

    return true;
}

MetadataNode::ExtendedClassCacheData
MetadataNode::GetCachedExtendedClassData(JsRuntime &rt, const string &proxyClassName) {
    auto cache = GetMetadataNodeCache(rt);
    ExtendedClassCacheData cacheData;
    auto itFound = cache->ExtendedCtorFuncCache.find(proxyClassName);
    if (itFound != cache->ExtendedCtorFuncCache.end()) {
        cacheData.extendedCtorFunction = JsValue(rt, itFound->second.extendedCtorFunction);
        cacheData.extendedName = itFound->second.extendedName;
        cacheData.node = itFound->second.node;
    }

    return cacheData;
}

MetadataNode::MetadataNodeCache *MetadataNode::GetMetadataNodeCache(JsRuntime &rt) {
    const void *key = rt.identity();
    auto cache = s_metadata_node_cache.Get(key);
    if (cache) return cache;
    cache = new MetadataNodeCache;
    s_metadata_node_cache.Insert(key, cache);
    return cache;
}

MetadataNode::MetadataNode(MetadataTreeNode *treeNode) : m_treeNode(treeNode) {
    uint8_t nodeType = s_metadataReader.GetNodeType(treeNode);

    m_name = s_metadataReader.ReadTypeName(m_treeNode);

    uint8_t parentNodeType = s_metadataReader.GetNodeType(treeNode->parent);

    m_isArray = s_metadataReader.IsNodeTypeArray(parentNodeType);

    bool isInterface = s_metadataReader.IsNodeTypeInterface(nodeType);

    if (!m_isArray && isInterface) {
        bool isPrefix;
        auto impTypeName = s_metadataReader.ReadInterfaceImplementationTypeName(m_treeNode,
                                                                                isPrefix);
        m_implType = isPrefix
                     ? (impTypeName + m_name)
                     : impTypeName;
    }
}

void MetadataNode::CreateTopLevelNamespaces(JsRuntime &rt) {
    auto global = rt.global();

    auto root = s_metadataReader.GetRoot();

    const auto &children = *root->children;

    for (auto treeNode: children) {
        uint8_t nodeType = s_metadataReader.GetNodeType(treeNode);

        if (nodeType == MetadataTreeNode::PACKAGE) {
            auto node = GetOrCreateInternal(treeNode);

            JsValue packageObj = node->CreateWrapper(rt);

            string nameSpace = node->m_treeNode->name;
            // if the namespaces matches a javascript keyword, prefix it with $ to avoid TypeScript and JavaScript errors
            if (IsJavascriptKeyword(nameSpace)) {
                nameSpace = "$" + nameSpace;
            }
            global.setProperty(rt, nameSpace.c_str(), packageObj);
        }
    }
}

MetadataTreeNode *MetadataNode::GetOrCreateTreeNodeByName(const string &className) {
    MetadataTreeNode *result = nullptr;

    auto itFound = s_name2TreeNodeCache.find(className);

    if (itFound != s_name2TreeNodeCache.end()) {
        result = itFound->second;
    } else {
        result = s_metadataReader.GetOrCreateTreeNodeByName(className);

        s_name2TreeNodeCache.emplace(className, result);
    }

    return result;
}

string MetadataNode::GetName() {
    return m_name;
}

MetadataNode *MetadataNode::GetOrCreate(const string &className) {
    MetadataNode *node = nullptr;

    auto it = s_name2NodeCache.find(className);

    if (it == s_name2NodeCache.end()) {
        MetadataTreeNode *treeNode = GetOrCreateTreeNodeByName(className);

        node = GetOrCreateInternal(treeNode);

        s_name2NodeCache.emplace(className, node);
    } else {
        node = it->second;
    }

    return node;
}

MetadataNode *MetadataNode::GetOrCreateInternal(MetadataTreeNode *treeNode) {
    MetadataNode *result = nullptr;

    auto it = s_treeNode2NodeCache.find(treeNode);

    if (it != s_treeNode2NodeCache.end()) {
        result = it->second;
    } else {
            auto name = GetJniClassName(treeNode);
            if (!name.empty()) {
                auto it2 = s_name2NodeCache.find(name);
                if ( it2 != s_name2NodeCache.end()) {
                    result = it2->second;
                }
            }

            if (!result) {
                result = new MetadataNode(treeNode);
                s_treeNode2NodeCache.emplace(treeNode, result);
                if (!result->m_name.empty()) {
                    s_name2NodeCache.emplace(result->m_name, result);
                }
            }
    }

    auto found = s_treeNode2NodeCache.find(treeNode);
    if (found == s_treeNode2NodeCache.end()) {
        s_treeNode2NodeCache.emplace(treeNode, result);
    }

    return result;
}

MetadataEntry MetadataNode::GetChildMetadataForPackage(MetadataNode *node, const char *propName) {
    assert(node->m_treeNode->children != nullptr);

    MetadataEntry child(nullptr, NodeType::Class);

    const auto &children = *node->m_treeNode->children;

    for (auto treeNodeChild: children) {
        if (strcmp(treeNodeChild->name.c_str(), propName) == 0) {
            child.name = propName;
            child.treeNode = treeNodeChild;
            child.type = static_cast<NodeType>(s_metadataReader.GetNodeType(treeNodeChild));

            if (s_metadataReader.IsNodeTypeInterface((uint8_t) child.type)) {
                bool isPrefix;
                string declaringType = s_metadataReader.ReadInterfaceImplementationTypeName(
                        treeNodeChild, isPrefix);
                child.declaringType = isPrefix
                                      ? (declaringType +
                                         s_metadataReader.ReadTypeName(child.treeNode))
                                      : declaringType;
            }
        }
    }

    return child;
}

bool MetadataNode::IsJavascriptKeyword(const std::string &word) {
    static set<string> keywords;

    if (keywords.empty()) {
        string kw[]{"abstract", "arguments", "boolean", "break", "byte", "case", "catch", "char",
                    "class", "const", "continue", "debugger", "default", "delete", "do",
                    "double", "else", "enum", "eval", "export", "extends", "false", "final",
                    "finally", "float", "for", "function", "goto", "if", "implements",
                    "import", "in", "instanceof", "int", "interface", "let", "long", "native",
                    "new", "null", "package", "private", "protected", "public", "return",
                    "short", "static", "super", "switch", "synchronized", "this", "throw", "throws",
                    "transient", "true", "try", "typeof", "var", "void", "volatile", "while",
                    "with", "yield"};

        keywords = set<string>(kw, kw + sizeof(kw) / sizeof(kw[0]));
    }

    return keywords.find(word) != keywords.end();
}

JsValue MetadataNode::CreateWrapper(JsRuntime &rt) {
    uint8_t nodeType = s_metadataReader.GetNodeType(m_treeNode);
    bool isClass = s_metadataReader.IsNodeTypeClass(nodeType),
            isInterface = s_metadataReader.IsNodeTypeInterface(nodeType);

    if (isClass || isInterface) {
        return GetConstructorFunction(rt);
    }

    if (s_metadataReader.IsNodeTypePackage(nodeType)) {
        return CreatePackageObject(rt);
    }

    std::stringstream ss;
    ss << "(InternalError): Can't create proxy for this type=" << static_cast<int>(nodeType);
    throw NativeScriptException(ss.str());
}

JsValue MetadataNode::PackageGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                            MetadataTreeNode *childTreeNode) {
    return Guarded(rt, [&]() -> JsValue {
        DEBUG_WRITE("Get package item: %s", childTreeNode->name.c_str());

        auto childNode = MetadataNode::GetOrCreateInternal(childTreeNode);
        JsValue value = childNode->CreateWrapper(rt);

        uint8_t childNodeType = s_metadataReader.GetNodeType(childTreeNode);
        if (s_metadataReader.IsNodeTypeInterface(childNodeType)) {
            // For all java interfaces we register the special Symbol.hasInstance property
            // which is invoked by the instanceof operator (https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Symbol/hasInstance).
            // For example:
            //
            // Object.defineProperty(android.view.animation.Interpolator, Symbol.hasInstance, {
            //    value: function(obj) {
            //        return true;
            //    }
            // });
            RegisterSymbolHasInstanceCallback(rt, childTreeNode, value);
        }

        // org.json.JSONObject special-case. Cheap name check first so the parent
        // lookup only happens for the one class that needs it.
        if (childTreeNode->name == "JSONObject") {
            auto parentNode = GetOrCreateInternal(childTreeNode->parent);
            if (parentNode->m_name == "org/json") {
                JSONObjectHelper::RegisterFromFunction(rt, value);
            }
        }

        // Replace this accessor on the receiver with the resolved value as a plain
        // (configurable) data property, so every subsequent `pkg.Child` access is a
        // direct, inline-cacheable property load instead of re-invoking this getter.
        if (thisVal.isObject()) {
            js_util::define_property_value(rt, thisVal.asObjectBorrowed(rt),
                                           childTreeNode->name.c_str(), value);
        }

        return value;
    });
}

void MetadataNode::RegisterSymbolHasInstanceCallback(JsRuntime &rt,
                                                     const MetadataTreeNode *treeNode,
                                                     const JsValue &interface) {
    if (!interface.isObject()) {
        return;
    }

    JEnv jEnv;

    auto className = GetJniClassName(treeNode);
    auto clazz = jEnv.FindClass(className);
    if (clazz == nullptr) {
        return;
    }

    auto symbol = rt.global().getPropertyAsObject(rt, "Symbol");
    auto hasInstance = symbol.getProperty(rt, "hasInstance");

    // The class ref is captured by the callback itself. The napi tree had to box
    // it in a heap holder because PrimJS packed the napi `data` pointer into 48
    // bits and corrupted a JNI global ref; there is no `data` pointer here.
    auto method = JsFunction::createFromHostFunction(
            rt, engine::PropNameID::forAscii(rt, "hasInstance"), 1,
            [clazz](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                    size_t argc) -> JsValue {
                if (argc != 1) {
                    throw JsError(rt, "Symbol.hasInstance must take exactly 1 argument");
                }

                const JsValue &object = args[0];

                if (!object.isObject()) {
                    return JsValue(false);
                }

                auto runtime = Runtime::GetRuntime(rt);
                auto objectManager = runtime->GetObjectManager();
                auto obj = objectManager->GetJavaObjectByJsObject(object);

                if (obj.IsNull()) {
                    // Couldn't find a corresponding java instance counterpart. This could happen
                    // if the "instanceof" operator is invoked on a pure javascript instance
                    return JsValue(false);
                }

                JEnv jEnv;
                return JsValue((bool) jEnv.IsInstanceOf(obj, clazz));
            });

    // Defined (not assigned) so the well-known symbol lands as a non-enumerable
    // own property, matching napi_define_properties with napi_default.
    JsObject descriptor(rt);
    descriptor.setProperty(rt, "value", JsValue(rt, method));
    descriptor.setProperty(rt, "enumerable", false);
    descriptor.setProperty(rt, "configurable", false);
    descriptor.setProperty(rt, "writable", false);
    const JsValue defineArgs[] = {JsValue(rt, interface), hasInstance, JsValue(rt, descriptor)};
    js_util::Builtins::of(rt).defineProperty.call(rt, defineArgs, (size_t) 3);
}


std::string MetadataNode::GetJniClassName(const MetadataTreeNode *node) {
    std::stack<string> s;

    while (node != nullptr && !node->name.empty()) {
        s.push(node->name);
        node = node->parent;
    }

    string fullClassName;
    while (!s.empty()) {
        auto top = s.top();
        fullClassName = (fullClassName.empty()) ? top : fullClassName + "/" + top;
        s.pop();
    }

    return fullClassName;
}

JsValue MetadataNode::CreatePackageObject(JsRuntime &rt) {
    JsObject packageObj(rt);

    auto ptrChildren = this->m_treeNode->children;

    if (ptrChildren != nullptr) {
        const auto &children = *ptrChildren;
        auto lastChildName = "";
        for (auto childNode: children) {
            if (strcmp(childNode->name.c_str(), lastChildName) == 0) {
                continue;
            }
            lastChildName = childNode->name.c_str();
            DefineAccessor(rt, packageObj, childNode->name.c_str(),
                           [childNode](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                                       size_t) -> JsValue {
                               return PackageGetterCallback(rt, thisVal, childNode);
                           },
                           nullptr);
        }
    }

    return JsValue(rt, packageObj);
}

std::vector<MetadataNode::MethodCallbackData *> MetadataNode::SetClassMembers(
        JsRuntime &rt, JsObject constructor,
        std::vector<MethodCallbackData *> &instanceMethodsCallbackData,
        const std::vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
        MetadataTreeNode *treeNode) {

    if (treeNode->metadata != nullptr) {
        return SetInstanceMembersFromRuntimeMetadata(
                rt, constructor, instanceMethodsCallbackData,
                baseInstanceMethodsCallbackData, treeNode);
    }

    return SetClassMembersFromStaticMetadata(
            rt, constructor, instanceMethodsCallbackData,
            baseInstanceMethodsCallbackData, treeNode);
}

std::vector<MetadataNode::MethodCallbackData *> MetadataNode::SetClassMembersFromStaticMetadata(
        JsRuntime &rt, JsObject constructor,
        std::vector<MethodCallbackData *> &instanceMethodsCallbackData,
        const std::vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
        MetadataTreeNode *treeNode) {

    std::vector<MethodCallbackData *> instanceMethodData;

    uint8_t *curPtr = s_metadataReader.GetValueData() + treeNode->offsetValue + 1;

    auto nodeType = s_metadataReader.GetNodeType(treeNode);
    auto curType = s_metadataReader.ReadTypeName(treeNode);
    curPtr += sizeof(uint16_t /* baseClassId */);

    if (s_metadataReader.IsNodeTypeInterface(nodeType)) {
        curPtr += sizeof(uint8_t) + sizeof(uint32_t);
    }

    std::string lastMethodName;
    MethodCallbackData *callbackData = nullptr;

    robin_hood::unordered_map<std::string, MethodCallbackData *> collectedExtensionMethods;

    auto prototype = constructor.getPropertyAsObject(rt, "prototype");

    // The napi tree also took a strong reference to the prototype here, for the
    // non-host receiver check in IsInstanceReceiver. Host objects are the only
    // path, so nothing needs it.

    auto objectManager = Runtime::GetObjectManager(rt);
    auto cache = GetMetadataNodeCache(rt);
    auto extensionFunctionsCount = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);
    collectedExtensionMethods.reserve(extensionFunctionsCount);

    for (auto i = 0; i < extensionFunctionsCount; i++) {
        auto entry = MetadataReader::ReadExtensionFunctionEntry(&curPtr);

        auto &methodName = entry.getName();
        if (methodName != lastMethodName) {
            callbackData = tryGetExtensionMethodCallbackData(collectedExtensionMethods,
                                                             methodName);

            if (callbackData == nullptr) {
                callbackData = new MethodCallbackData(this);

                auto method = JsFunction::createFromHostFunction(
                        rt, engine::PropNameID::forAscii(rt, methodName), 0,
                        [callbackData](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                       size_t argc) -> JsValue {
                            return MethodCallback(rt, thisVal, args, argc, callbackData);
                        });

                js_util::define_property_value(rt, prototype, methodName.c_str(),
                                               JsValue(rt, method), false, true, true);
                lastMethodName = methodName;
                collectedExtensionMethods.emplace(methodName, callbackData);

            }
        }

        callbackData->candidates.push_back(std::move(entry));
        callbackData->objectManager = objectManager;
    }

    auto instanceMethodCount = *reinterpret_cast<uint16_t *>(curPtr);
    collectedExtensionMethods.reserve(instanceMethodCount);
    curPtr += sizeof(uint16_t);

    for (auto i = 0; i < instanceMethodCount; i++) {
        auto entry = MetadataReader::ReadInstanceMethodEntry(&curPtr);
        auto &methodName = entry.getName();
        if (methodName != lastMethodName) {
            callbackData = tryGetExtensionMethodCallbackData(collectedExtensionMethods,
                                                             methodName);


            if (callbackData == nullptr) {
                callbackData = new MethodCallbackData(this);
                auto method = JsFunction::createFromHostFunction(
                        rt, engine::PropNameID::forAscii(rt, methodName), 0,
                        [callbackData](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                       size_t argc) -> JsValue {
                            return MethodCallback(rt, thisVal, args, argc, callbackData);
                        });
                js_util::define_property_value(rt, prototype, methodName.c_str(),
                                               JsValue(rt, method), false, true, true);
                collectedExtensionMethods.emplace(methodName, callbackData);
            }

            instanceMethodData.push_back(callbackData);
            instanceMethodsCallbackData.push_back(callbackData);

            auto itFound = std::find_if(baseInstanceMethodsCallbackData.begin(),
                                        baseInstanceMethodsCallbackData.end(),
                                        [&methodName](MethodCallbackData *x) {
                                            return x->candidates.front().name == methodName;
                                        });
            if (itFound != baseInstanceMethodsCallbackData.end()) {
                callbackData->parent = *itFound;
            }

            lastMethodName = methodName;
        }

        callbackData->candidates.push_back(std::move(entry));
        callbackData->objectManager = objectManager;
    }
    auto instanceFieldCount = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);
    for (auto i = 0; i < instanceFieldCount; i++) {
        auto entry = MetadataReader::ReadInstanceFieldEntry(&curPtr);
        auto &fieldName = entry.getName();
        auto fieldInfo = new FieldCallbackData(entry);
        fieldInfo->metadata.declaringType = curType;
        fieldInfo->objectManager = objectManager;
        DefineAccessor(rt, prototype, fieldName.c_str(),
                       [fieldInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                                   size_t) -> JsValue {
                           return FieldAccessorGetterCallback(rt, thisVal, fieldInfo);
                       },
                       [fieldInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                   size_t argc) -> JsValue {
                           return FieldAccessorSetterCallback(rt, thisVal, args, argc, fieldInfo);
                       });

        cache->fieldCallbackData.push_back(fieldInfo);

    }

    auto kotlinPropertiesCount = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);
    for (int i = 0; i < kotlinPropertiesCount; ++i) {
        uint32_t nameOffset = *reinterpret_cast<uint32_t *>(curPtr);
        auto propertyName = s_metadataReader.ReadName(nameOffset);
        curPtr += sizeof(uint32_t);

        auto hasGetter = *reinterpret_cast<uint16_t *>(curPtr);
        curPtr += sizeof(uint16_t);

        // Keep the full method entry (not just its name) so the accessor can call
        // CallJavaMethod directly instead of looking up + invoking the JS method.
        MetadataEntry *getterEntry = nullptr;
        std::string getterMethodName;
        if (hasGetter >= 1) {
            getterEntry = new MetadataEntry(MetadataReader::ReadInstanceMethodEntry(&curPtr));
            getterMethodName = getterEntry->getName();
        }

        auto hasSetter = *reinterpret_cast<uint16_t *>(curPtr);
        curPtr += sizeof(uint16_t);

        MetadataEntry *setterEntry = nullptr;
        std::string setterMethodName;
        if (hasSetter >= 1) {
            setterEntry = new MetadataEntry(MetadataReader::ReadInstanceMethodEntry(&curPtr));
            setterMethodName = setterEntry->getName();
        }

        auto propertyInfo = new PropertyCallbackData(propertyName, getterMethodName,
                                                     setterMethodName);
        propertyInfo->getterEntry = getterEntry;
        propertyInfo->setterEntry = setterEntry;
        propertyInfo->node = this;
        propertyInfo->objectManager = objectManager;
        cache->propertyCallbackData.push_back(propertyInfo);
        DefineAccessor(rt, prototype, propertyName.c_str(),
                       [propertyInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                                      size_t) -> JsValue {
                           return PropertyAccessorGetterCallback(rt, thisVal, propertyInfo);
                       },
                       [propertyInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                      size_t argc) -> JsValue {
                           return PropertyAccessorSetterCallback(rt, thisVal, args, argc,
                                                                 propertyInfo);
                       });
    }

    // Set static class members on constructor
    lastMethodName.clear();
    callbackData = nullptr;

    auto origin = Constants::APP_ROOT_FOLDER_PATH + this->m_name;

    // get candidates from static methods metadata
    auto staticMethodCout = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);
    for (auto i = 0; i < staticMethodCout; i++) {
        auto entry = MetadataReader::ReadStaticMethodEntry(&curPtr);
        // In java there can be multiple methods of same name with different parameters.
        auto &methodName = entry.getName();
        if (methodName != lastMethodName) {
            callbackData = new MethodCallbackData(this);
            auto method = JsFunction::createFromHostFunction(
                    rt, engine::PropNameID::forAscii(rt, methodName), 0,
                    [callbackData](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                   size_t argc) -> JsValue {
                        return MethodCallback(rt, thisVal, args, argc, callbackData);
                    });

            js_util::define_property_value(rt, constructor, methodName.c_str(),
                                           JsValue(rt, method), false, true, true);
            lastMethodName = methodName;
        }
        callbackData->candidates.push_back(std::move(entry));
        callbackData->objectManager = objectManager;
    }

    MetadataNode *self = this;
    auto extendMethod = JsFunction::createFromHostFunction(
            rt, engine::PropNameID::forAscii(rt, PROP_KEY_EXTEND), 0,
            [self](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                   size_t argc) -> JsValue {
                return ExtendMethodCallback(rt, thisVal, args, argc, self);
            });
    constructor.setProperty(rt, PROP_KEY_EXTEND, JsValue(rt, extendMethod));

    // Brand the runtime's native extend() so ts_helpers can reliably tell a native class's
    // extend from a user/JS extend. It must NOT rely on Function.prototype.toString() sniffing
    // "[native code]": in release builds JS is compiled to bytecode and every function
    // (native or JS) stringifies to "[native code]", so a plain JS class with a static method
    // named "extend" would be misdetected as native. This brand is a real, non-enumerable
    // property set by the runtime, so it works identically for source and bytecode on all engines.
    js_util::define_property_value(rt, extendMethod, "__isNativeExtend__", JsValue(true),
                                   false, false, false);

    // get candidates from static fields metadata
    auto staticFieldCout = *reinterpret_cast<uint16_t *>(curPtr);
    curPtr += sizeof(uint16_t);
    for (auto i = 0; i < staticFieldCout; i++) {
        auto entry = MetadataReader::ReadStaticFieldEntry(&curPtr);
        auto &fieldName = entry.getName();
        auto fieldInfo = new FieldCallbackData(entry);
        fieldInfo->objectManager = objectManager;
        DefineAccessor(rt, constructor, fieldName.c_str(),
                       [fieldInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                                   size_t) -> JsValue {
                           return FieldAccessorGetterCallback(rt, thisVal, fieldInfo);
                       },
                       [fieldInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                   size_t argc) -> JsValue {
                           return FieldAccessorSetterCallback(rt, thisVal, args, argc, fieldInfo);
                       });
        cache->fieldCallbackData.push_back(fieldInfo);
    }


    DefineAccessor(rt, constructor, PROP_KEY_NULLOBJECT,
                   [self](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                          size_t) -> JsValue {
                       return NullObjectAccessorGetterCallback(rt, thisVal, self);
                   },
                   nullptr);


    std::string tname = s_metadataReader.ReadTypeName(treeNode);
    constructor.setProperty(rt, PRIVATE_TYPE_NAME, ArgConverter::convertToJsString(rt, tname));

    SetClassAccessor(rt, constructor);

    return instanceMethodData;
}

MetadataNode::MethodCallbackData *MetadataNode::tryGetExtensionMethodCallbackData(
        const robin_hood::unordered_map<std::string, MethodCallbackData *> &collectedMethodCallbackData,
        const std::string &lookupName) {

    if (collectedMethodCallbackData.empty()) {
        return nullptr;
    }

    auto itFound = collectedMethodCallbackData.find(lookupName);
    if (itFound != collectedMethodCallbackData.end()) {
        return itFound->second;
    }

    return nullptr;
}

bool MetadataNode::IsNodeTypeInterface() {
    uint8_t nodeType = s_metadataReader.GetNodeType(m_treeNode);
    return s_metadataReader.IsNodeTypeInterface(nodeType);
}

std::vector<MetadataNode::MethodCallbackData *> MetadataNode::SetInstanceMembersFromRuntimeMetadata(
        JsRuntime &rt, JsObject constructor,
        std::vector<MethodCallbackData *> &instanceMethodsCallbackData,
        const std::vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
        MetadataTreeNode *treeNode) {
    assert(treeNode->metadata != nullptr);

    std::vector<MethodCallbackData *> instanceMethodData;

    std::string line;
    const std::string &metadata = *treeNode->metadata;
    std::stringstream s(metadata);

    std::string kind;
    std::string name;
    std::string signature;
    int paramCount;

    std::getline(s, line); // type line
    std::getline(s, line); // base class line

    std::string lastMethodName;
    MethodCallbackData *callbackData = nullptr;

    auto cache = GetMetadataNodeCache(rt);
    auto proto = constructor.getPropertyAsObject(rt, "prototype");
    while (std::getline(s, line)) {
        std::stringstream tmp(line);
        tmp >> kind >> name >> signature >> paramCount;

        char chKind = kind[0];

        assert((chKind == 'M') || (chKind == 'F'));

        MetadataEntry entry(nullptr, NodeType::Field);

        entry.name = name;
        entry.sig = signature;
        entry.paramCount = paramCount;
        entry.isStatic = false;
        if (chKind == 'M') {
            if (entry.name != lastMethodName) {
                entry.type = NodeType::Method;
                callbackData = new MethodCallbackData(this);
                instanceMethodData.push_back(callbackData);
                instanceMethodsCallbackData.push_back(callbackData);

                auto itFound = std::find_if(baseInstanceMethodsCallbackData.begin(),
                                            baseInstanceMethodsCallbackData.end(),
                                            [&entry](MethodCallbackData *x) {
                                                return x->candidates.front().name == entry.name;
                                            });
                if (itFound != baseInstanceMethodsCallbackData.end()) {
                    callbackData->parent = *itFound;
                }

                auto method = JsFunction::createFromHostFunction(
                        rt, engine::PropNameID::forAscii(rt, entry.name), 0,
                        [callbackData](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                       size_t argc) -> JsValue {
                            return MethodCallback(rt, thisVal, args, argc, callbackData);
                        });
                proto.setProperty(rt, entry.name.c_str(), JsValue(rt, method));

                lastMethodName = entry.name;
            }
            callbackData->candidates.push_back(std::move(entry));
        } else if (chKind == 'F') {
            entry.type = NodeType::Field;
            auto *fieldInfo = new FieldCallbackData(entry);
            DefineAccessor(rt, proto, entry.name.c_str(),
                           [fieldInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                                       size_t) -> JsValue {
                               return FieldAccessorGetterCallback(rt, thisVal, fieldInfo);
                           },
                           [fieldInfo](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                       size_t argc) -> JsValue {
                               return FieldAccessorSetterCallback(rt, thisVal, args, argc,
                                                                  fieldInfo);
                           });

            cache->fieldCallbackData.push_back(fieldInfo);
        }
    }

    return instanceMethodData;
}

void MetadataNode::SetClassAccessor(JsRuntime &rt, JsObject constructor) {
    DefineAccessor(rt, constructor, PROP_KEY_CLASS,
                   [](JsRuntime &rt, const JsValue &thisVal, const JsValue *, size_t) -> JsValue {
                       return ClassAccessorGetterCallback(rt, thisVal);
                   },
                   nullptr);
}

JsValue MetadataNode::ClassAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal) {
    return Guarded(rt, [&]() -> JsValue {
        if (!thisVal.isObject()) return js_util::undefined();
        auto name = thisVal.asObjectBorrowed(rt).getProperty(rt, PRIVATE_TYPE_NAME);
        if (!name.isString()) return js_util::undefined();
        auto nameValue = name.asString(rt).utf8(rt);
        return CallbackHandlers::FindClass(rt, nameValue.c_str());
    });
}

JsValue MetadataNode::GetConstructorFunction(JsRuntime &rt) {
    std::vector<MethodCallbackData *> instanceMethodsCallbackData;
    return GetConstructorFunctionInternal(rt, m_treeNode, instanceMethodsCallbackData);
}

JsValue MetadataNode::GetConstructorFunctionInternal(JsRuntime &rt, MetadataTreeNode *treeNode,
                                                     std::vector<MethodCallbackData *> instanceMethodsCallbackData) {

    auto cache = GetMetadataNodeCache(rt);
    auto itFound = cache->CtorFuncCache.find(treeNode);
    if (itFound != cache->CtorFuncCache.end()) {
        if (!js_util::is_null_or_undefined(itFound->second.constructorFunction)) {
            instanceMethodsCallbackData = itFound->second.instanceMethodCallbacks;
            return JsValue(rt, itFound->second.constructorFunction);
        }
    }

    if (itFound != cache->CtorFuncCache.end()) {
        // The napi tree guarded this delete with #ifndef __JSC__. The callback
        // data is owned here and never reachable from JS, so the guard has no
        // engine dependency to express and is dropped.
        for (auto data: itFound->second.instanceMethodCallbacks) {
            delete data;
        }
        itFound->second.instanceMethodCallbacks.clear();
        cache->CtorFuncCache.erase(itFound);
    }

    auto node = GetOrCreateInternal(treeNode);

    JEnv jEnv;
    // if we already have an exception (which will be rethrown later)
    // then we don't want to ignore the next exception
    bool ignoreFindClassException = jEnv.ExceptionCheck() == JNI_FALSE;
    auto currentClass = jEnv.FindClass(node->m_name);
    if (ignoreFindClassException && jEnv.ExceptionCheck()) {
        jEnv.ExceptionClear();
        // JNI found an exception looking up this class
        // but we don't care, because this means this class doesn't exist
        // like when you try to get a class that only exists in a higher API level
        CtorCacheData ctorCacheItem(js_util::undefined(), instanceMethodsCallbackData);
        cache->CtorFuncCache.emplace(treeNode, std::move(ctorCacheItem));
        return js_util::undefined();
    };

    auto currentNode = treeNode;
    std::string finalName(currentNode->name);
    while (currentNode->parent) {
        if (!currentNode->parent->name.empty()) {
            finalName = currentNode->parent->name + "." + finalName;
        }
        currentNode = currentNode->parent;
    }

    // 1. Create the class and get the constructor

    auto isInterface = s_metadataReader.IsNodeTypeInterface(treeNode->type);
    auto constructor = JsFunction::createFromHostConstructor(
            rt, engine::PropNameID::forAscii(rt, finalName), 0,
            isInterface
            ? engine::HostFunctionType(
                    [node](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                           size_t argc) -> JsValue {
                        return InterfaceConstructorCallback(rt, thisVal, args, argc, node);
                    })
            : engine::HostFunctionType(
                    [node](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                           size_t argc) -> JsValue {
                        return ClassConstructorCallback(rt, thisVal, args, argc, node);
                    }));

    // Mark this constructor's prototype as a runtime object.
    ObjectManager::MarkObject(rt, constructor.getProperty(rt, "prototype"));

    // 2. Create the base constructor if it doesn't exist and inherit from it.
    JsValue baseConstructor;
    std::vector<MethodCallbackData *> baseInstanceMethodsCallbackData;
    auto tmpTreeNode = treeNode;
    std::vector<MetadataTreeNode *> skippedBaseTypes;

    while (true) {
        auto baseTreeNode = s_metadataReader.GetBaseClassNode(tmpTreeNode);
        if (CheckClassHierarchy(jEnv, currentClass, treeNode, baseTreeNode, skippedBaseTypes)) {
            tmpTreeNode = baseTreeNode;
            continue;
        }

        if ((baseTreeNode != treeNode) && (baseTreeNode != nullptr) &&
            (baseTreeNode->offsetValue > 0)) {
            baseConstructor = GetConstructorFunctionInternal(rt, baseTreeNode,
                                                             baseInstanceMethodsCallbackData);


            if (baseConstructor.isObject()) {
                js_util::inherits(rt, constructor, baseConstructor.asObjectBorrowed(rt));
            }
        } else {
            baseConstructor = js_util::undefined();
        }
        break;
    }

    // 3. Define the class members now.
    auto instanceMethodData = node->SetClassMembers(rt, constructor,
                                                    instanceMethodsCallbackData,
                                                    baseInstanceMethodsCallbackData, treeNode);

    if (!skippedBaseTypes.empty()) {
        // If there is a mismatch between base type of this class in metadata compared to the class
        // at runtime, we will add methods of base class to this class's prototype.
        node->SetMissingBaseMethods(rt, skippedBaseTypes, instanceMethodData, constructor);
    }


    SetInnerTypes(rt, constructor, treeNode);

    JsValue constructorValue(rt, constructor);

    if (baseConstructor.isObject()) {
        js_util::setPrototypeOf(rt, constructorValue, baseConstructor);
    }

    CtorCacheData ctorCacheItem(JsValue(rt, constructorValue), instanceMethodsCallbackData);
    cache->CtorFuncCache.emplace(treeNode, std::move(ctorCacheItem));

    return constructorValue;
}

void MetadataNode::SetInnerTypes(JsRuntime &rt, JsObject constructor,
                                 MetadataTreeNode *treeNode) {
    if (treeNode->children != nullptr) {
        const auto &children = *treeNode->children;

        for (auto curChild: children) {
            if (!js_util::has_own_property(rt, constructor, curChild->name.c_str())) {
                DefineAccessor(rt, constructor, curChild->name.c_str(),
                               [curChild](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                                          size_t) -> JsValue {
                                   return InnerTypeGetterCallback(rt, thisVal, curChild);
                               },
                               nullptr);
            }
        }
    }
}

JsValue MetadataNode::InnerTypeGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                              MetadataTreeNode *curChild) {
    return Guarded(rt, [&]() -> JsValue {
        auto childNode = GetOrCreateInternal(curChild);
        // GetConstructorFunction caches per node (CtorFuncCache); inner types are
        // always class/interface, both resolved here.
        JsValue constructor = childNode->GetConstructorFunction(rt);

        // Java interfaces need Symbol.hasInstance for `instanceof` support, just
        // like package-level interfaces in PackageGetterCallback.
        uint8_t childNodeType = s_metadataReader.GetNodeType(curChild);
        if (s_metadataReader.IsNodeTypeInterface(childNodeType)) {
            RegisterSymbolHasInstanceCallback(rt, curChild, constructor);
        }

        // Replace this accessor on the receiver (the outer type) with the resolved
        // inner class/interface as a plain (configurable) data property, so every
        // subsequent Outer.Inner access is a direct, inline-cacheable property load
        // instead of re-invoking this getter.
        if (thisVal.isObject()) {
            js_util::define_property_value(rt, thisVal.asObjectBorrowed(rt),
                                           curChild->name.c_str(), constructor);
        }

        return constructor;
    });
}

MetadataReader *MetadataNode::getMetadataReader() {
    return &MetadataNode::s_metadataReader;
}

JsValue MetadataNode::NullObjectAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                       MetadataNode *node) {
    return Guarded(rt, [&]() -> JsValue {
        if (!thisVal.isObject()) return js_util::undefined();

        auto object = thisVal.asObjectBorrowed(rt);

        if (GetNullNode(rt, thisVal) == nullptr) {
            object.setNativeState(rt, std::make_shared<NullNodeState>(node));
            js_util::set_function(rt, object, "valueOf", MetadataNode::NullValueOfCallback);
        }

        return JsValue(rt, thisVal);
    });
}

JsValue MetadataNode::NullValueOfCallback(JsRuntime &rt, const JsValue &thisVal,
                                          const JsValue *args, size_t argc) {
    return js_util::null();
}

bool MetadataNode::IsInstanceReceiver(JsRuntime &rt, const JsValue &jsThis) {
    // Real instances are host-object proxies; the class prototype is not. A
    // non-host receiver means someone touched Class.prototype.<member>.
    return Runtime::GetRuntime(rt)->GetObjectManager()->IsHostObject(jsThis);
}

JsValue MetadataNode::FieldAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  FieldCallbackData *fieldData) {
    return Guarded(rt, [&]() -> JsValue {
        auto &fieldMetadata = fieldData->metadata;

        if (fieldMetadata.getDeclaringType().empty()) {
            return js_util::undefined();
        }

        if (fieldData->objectManager == nullptr) {
            fieldData->objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
        }

        if (fieldMetadata.isStatic) {
            return CallbackHandlers::GetJavaField(rt, thisVal, fieldData,
                                                  fieldData->objectManager);
        }

        // A single probe both validates the receiver and resolves the java
        // object; null + non-host means Class.prototype.<field> access.
        JniLocalRef target = fieldData->objectManager->GetJavaObjectByJsObjectFast(thisVal);
        if (target.IsNull() && !IsInstanceReceiver(rt, thisVal)) {
            return js_util::undefined();
        }
        return CallbackHandlers::GetJavaField(rt, thisVal, fieldData,
                                              fieldData->objectManager, std::move(target));
    });
}

JsValue MetadataNode::FieldAccessorSetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  const JsValue *args, size_t argc,
                                                  FieldCallbackData *fieldData) {
    return Guarded(rt, [&]() -> JsValue {
        if (argc < 1) return js_util::undefined();

        auto &fieldMetadata = fieldData->metadata;

        if (fieldData->objectManager == nullptr) {
            fieldData->objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
        }

        // A single probe both validates the receiver and resolves the java
        // object; null + non-host means Class.prototype.<field> access.
        JniLocalRef target;
        if (!fieldMetadata.isStatic) {
            target = fieldData->objectManager->GetJavaObjectByJsObjectFast(thisVal);
            if (target.IsNull() && !IsInstanceReceiver(rt, thisVal)) {
                return js_util::undefined();
            }
        }

        if (fieldMetadata.getIsFinal()) {
            stringstream ss;
            ss << "You are trying to set \"" << fieldMetadata.getName()
               << "\" which is a final field! Final fields can only be read.";
            string exceptionMessage = ss.str();

            throw NativeScriptException(exceptionMessage);
        }

        CallbackHandlers::SetJavaField(rt, thisVal, args[0], fieldData,
                                       fieldData->objectManager, std::move(target));
        return JsValue(rt, args[0]);
    });
}

JsValue MetadataNode::PropertyAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                     PropertyCallbackData *propertyCallbackData) {
    return Guarded(rt, [&]() -> JsValue {
        if (propertyCallbackData->getterEntry == nullptr) {
            return js_util::undefined();
        }

        if (!IsInstanceReceiver(rt, thisVal)) {
            return js_util::undefined();
        }

        // Call the Java getter directly — no JS method lookup, no nested
        // MethodCallback. Invariants are resolved once and cached.
        if (propertyCallbackData->cachedIsFromInterface < 0) {
            propertyCallbackData->cachedIsFromInterface =
                    propertyCallbackData->node->IsNodeTypeInterface() ? 1 : 0;
        }
        if (propertyCallbackData->objectManager == nullptr) {
            propertyCallbackData->objectManager =
                    Runtime::GetRuntime(rt)->GetObjectManager();
        }
        return CallbackHandlers::CallJavaMethod(
                rt, thisVal, propertyCallbackData->node->m_name,
                propertyCallbackData->getterMethodName, propertyCallbackData->getterEntry,
                propertyCallbackData->cachedIsFromInterface == 1,
                propertyCallbackData->getterEntry->isStatic, false, nullptr, 0,
                propertyCallbackData->objectManager);
    });
}

JsValue MetadataNode::PropertyAccessorSetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                     const JsValue *args, size_t argc,
                                                     PropertyCallbackData *propertyCallbackData) {
    return Guarded(rt, [&]() -> JsValue {
        if (propertyCallbackData->setterEntry == nullptr) {
            return js_util::undefined();
        }

        if (!IsInstanceReceiver(rt, thisVal)) {
            return js_util::undefined();
        }

        // Call the Java setter directly — no JS method lookup, no nested
        // MethodCallback. Invariants are resolved once and cached.
        if (propertyCallbackData->cachedIsFromInterface < 0) {
            propertyCallbackData->cachedIsFromInterface =
                    propertyCallbackData->node->IsNodeTypeInterface() ? 1 : 0;
        }
        if (propertyCallbackData->objectManager == nullptr) {
            propertyCallbackData->objectManager =
                    Runtime::GetRuntime(rt)->GetObjectManager();
        }
        return CallbackHandlers::CallJavaMethod(
                rt, thisVal, propertyCallbackData->node->m_name,
                propertyCallbackData->setterMethodName, propertyCallbackData->setterEntry,
                propertyCallbackData->cachedIsFromInterface == 1,
                propertyCallbackData->setterEntry->isStatic, false, args, argc,
                propertyCallbackData->objectManager);
    });
}

JsValue MetadataNode::ExtendMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                           const JsValue *args, size_t argc, MetadataNode *node) {
    return Guarded(rt, [&]() -> JsValue {
        JsValue extendName;
        JsValue implementationObject;
        string extendLocation;

        auto hasDot = false;
        auto isTypeScriptExtend = false;

        if (argc == 2) {
            if (!args[0].isString()) {
                stringstream ss;
                ss << "Invalid extend() call. No name for extend specified at location: "
                   << extendLocation.c_str();
                string exceptionMessage = ss.str();

                throw NativeScriptException(exceptionMessage);
            }

            if (!args[1].isObject()) {
                stringstream ss;
                ss << "Invalid extend() call. No implementation object specified at location: "
                   << extendLocation.c_str();
                string exceptionMessage = ss.str();

                throw NativeScriptException(exceptionMessage);
            }

            string strName = args[0].asString(rt).utf8(rt);
            hasDot = strName.find('.') != string::npos;
        } else if (argc == 3) {
            if (args[2].isBool()) {
                isTypeScriptExtend = args[2].getBool();
            }
        }

        auto useContentKey = false;

        if (hasDot) {
            extendName = JsValue(rt, args[0]);
            implementationObject = JsValue(rt, args[1]);
        } else {
            useContentKey = ContentKeyedBindingsEnabled();

            // Skipped outright when content keying is on: this walks the stack
            // purely to build a name, on every class creation.
            bool validExtend =
                    useContentKey || GetExtendLocation(rt, extendLocation, isTypeScriptExtend);

            if (!validExtend) {
                // No usable call site -- an anonymous frame, or a bundle the
                // generator never indexed. Naming by content still works here,
                // where before this was fatal.
                useContentKey = true;
                validExtend = true;
                extendLocation.clear();
            }

            extendName = js_util::to_js_string(rt, "");
            auto validArgs = ValidateExtendArguments(rt, argc, args, validExtend,
                                                     extendLocation,
                                                     &extendName, &implementationObject,
                                                     isTypeScriptExtend);
            if (!validArgs) {
                return js_util::undefined();
            }
        }

        string baseClassName = node->m_name;

        // An explicitly named extend keeps its name in front of the hash. The
        // generator reads the same name out of the bundle, so the two still
        // agree, and it buys back two things the bare hash costs: a class name
        // that says what it is in a stack trace, and uniqueness for the classes
        // that need it. Anything Java instantiates by name -- an Activity, a
        // Fragment, a Worker, anything reached by reflection -- cannot share a
        // generated class, because the runtime has only the class name to find
        // the JS implementation with. Naming such a class is what makes it
        // unique; unnamed extends stay shared, which is the common case and the
        // point of the scheme.
        string extendNameString = ArgConverter::ConvertToString(rt, extendName);
        string extendNameAndLocation;

        if (useContentKey) {
            string contentKey =
                    CreateContentKey(baseClassName,
                                     CallbackHandlers::CollectImplementedInterfaceNames(
                                             rt, implementationObject),
                                     CallbackHandlers::CollectMethodOverrideNames(
                                             rt, implementationObject, /* functionsOnly */ false));
            extendNameAndLocation =
                    extendNameString.empty() ? contentKey : extendNameString + "_" + contentKey;
        } else {
            extendNameAndLocation = extendLocation + extendNameString;
        }

        string fullClassName;
        if (!hasDot) {
            fullClassName = TNS_PREFIX + CreateFullClassName(baseClassName, extendNameAndLocation);
        } else {
            fullClassName = ArgConverter::ConvertToString(rt, args[0]);
        }

        uint8_t nodeType = s_metadataReader.GetNodeType(node->m_treeNode);
        bool isInterface = s_metadataReader.IsNodeTypeInterface(nodeType);
        auto clazz = CallbackHandlers::ResolveClass(rt, baseClassName, fullClassName,
                                                    implementationObject, isInterface);
        auto fullExtendedName = CallbackHandlers::ResolveClassName(rt, clazz);

        auto implObject = implementationObject.asObjectBorrowed(rt);

        // Which JS constructor to reuse is a different question from which Java
        // class to load, and only the second one has to agree with the static
        // binding generator.
        //
        // Two extend() calls with the same base, interfaces and overridden names
        // share one generated Java class -- the Java is identical, and sharing it
        // is the point. Their JS bodies are not identical, though, so caching the
        // constructor under the class name would hand the second call the first
        // call's implementation. Under content keying that is not a rare
        // collision but the common case, so the constructor is cached per
        // implementation object instead.
        // The stamp identifies the object, not the class it was used to build:
        // combining it with fullExtendedName keeps a second extend() of the same
        // object against a *different* base a cache miss, so it still reaches the
        // "used to extend another class" check below instead of silently
        // returning the first class.
        string ctorCacheKey = fullExtendedName;
        if (useContentKey) {
            auto stamped = implObject.getProperty(rt, EXTEND_CTOR_CACHE_KEY);
            string implObjectId;
            if (stamped.isString()) {
                implObjectId = stamped.asString(rt).utf8(rt);
            } else {
                static std::atomic<uint64_t> s_implObjectCounter{0};
                implObjectId = std::to_string(s_implObjectCounter.fetch_add(1));
                implObject.setProperty(rt, EXTEND_CTOR_CACHE_KEY,
                                       ArgConverter::convertToJsString(rt, implObjectId));
            }
            ctorCacheKey = fullExtendedName + "#" + implObjectId;
        }

        auto cachedData = GetCachedExtendedClassData(rt, ctorCacheKey);
        if (!js_util::is_null_or_undefined(cachedData.extendedCtorFunction)) {
            return JsValue(rt, cachedData.extendedCtorFunction);
        }
        auto implementationObjectName = implObject.getProperty(rt, CLASS_IMPLEMENTATION_OBJECT);

        if (js_util::is_null_or_undefined(implementationObjectName)) {
            implObject.setProperty(rt, CLASS_IMPLEMENTATION_OBJECT,
                                   ArgConverter::convertToJsString(rt, fullExtendedName));
        } else {
            string usedClassName = ArgConverter::ConvertToString(rt, implementationObjectName);
            stringstream s;
            s << "This object is used to extend another class '" << usedClassName << "'";
            throw NativeScriptException(s.str());
        }

        auto baseClassCtorFunction = node->GetConstructorFunction(rt);

        auto *extData = new ExtendedClassCallbackData(node, extendNameAndLocation,
                                                      JsValue(rt, implementationObject),
                                                      fullClassName);
        GetMetadataNodeCache(rt)->extendedClassCallbackData.push_back(extData);

        auto extendFuncCtor = JsFunction::createFromHostConstructor(
                rt, engine::PropNameID::forAscii(rt, fullExtendedName), 0,
                [extData](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                          size_t argc) -> JsValue {
                    return ExtendedClassConstructorCallback(rt, thisVal, args, argc, extData);
                });

        auto extendFuncPrototype = extendFuncCtor.getPropertyAsObject(rt, "prototype");
        ObjectManager::MarkObject(rt, JsValue(rt, extendFuncPrototype));

        js_util::setPrototypeOf(rt, implementationObject,
                                js_util::get_prototype(rt, baseClassCtorFunction));

        DefineAccessor(rt, implObject, PROP_KEY_SUPER,
                       [](JsRuntime &rt, const JsValue &thisVal, const JsValue *,
                          size_t) -> JsValue {
                           return SuperAccessorGetterCallback(rt, thisVal);
                       },
                       nullptr);

        js_util::setPrototypeOf(rt, JsValue(rt, extendFuncPrototype), implementationObject);

        JsValue extendFuncCtorValue(rt, extendFuncCtor);
        js_util::setPrototypeOf(rt, extendFuncCtorValue, baseClassCtorFunction);

        SetClassAccessor(rt, extendFuncCtor);

        extendFuncCtor.setProperty(rt, PRIVATE_TYPE_NAME,
                                   ArgConverter::convertToJsString(rt, fullExtendedName));

        s_name2NodeCache.emplace(fullExtendedName, node);

        auto cache = GetMetadataNodeCache(rt);

        // Two entries, because there are two ways in.
        //
        // From JS, extend() asks "have I already built a constructor for this
        // implementation object?" -- keyed per object, so two identical-looking
        // extends keep their own bodies.
        //
        // From Java, createJSInstanceNative has only the class name of the
        // instance it is wrapping (CreateExtendedJSWrapper), so the class name
        // has to resolve too. emplace leaves an existing entry alone, so for a
        // shared generated class the first implementation registered is the one
        // Java-side instantiation finds. That is unambiguous for a named extend
        // -- naming it makes the class unique -- and inherently ambiguous for an
        // unnamed one, which is why anything Java constructs by name should be
        // named.
        ExtendedClassCacheData byImplObject(JsValue(rt, extendFuncCtorValue), fullExtendedName,
                                            node);
        cache->ExtendedCtorFuncCache.emplace(ctorCacheKey, std::move(byImplObject));

        if (ctorCacheKey != fullExtendedName) {
            ExtendedClassCacheData byClassName(JsValue(rt, extendFuncCtorValue), fullExtendedName,
                                               node);
            cache->ExtendedCtorFuncCache.emplace(fullExtendedName, std::move(byClassName));
        }

        return extendFuncCtorValue;
    });
}


JsValue MetadataNode::SuperAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal) {
    return Guarded(rt, [&]() -> JsValue {
        if (!thisVal.isObject()) return js_util::undefined();

        auto jsThis = thisVal.asObjectBorrowed(rt);

        JsValue superValue = jsThis.getProperty(rt, PROP_KEY_SUPERVALUE);

        if (js_util::is_null_or_undefined(superValue)) {
            auto objectManager = Runtime::GetRuntime(rt)->GetObjectManager();
            superValue = objectManager->GetEmptyObject();

            js_util::delete_property(rt, superValue, js_util::to_js_string(rt, PROP_KEY_TOSTRING));
            js_util::delete_property(rt, superValue, js_util::to_js_string(rt, PROP_KEY_VALUEOF));
            ObjectManager::MarkSuperCall(rt, superValue);

            JsValue superProto = js_util::getPrototypeOf(
                    rt, js_util::getPrototypeOf(rt, js_util::getPrototypeOf(rt, thisVal)));

            js_util::setPrototypeOf(rt, superValue, superProto);
            objectManager->CloneLink(thisVal, superValue);
            auto node = GetInstanceMetadata(rt, thisVal);
            SetInstanceMetadata(rt, superValue, node);

            int javaObjectID = -1;
            objectManager->GetJavaObjectByJsObject(thisVal, &javaObjectID);
            if (javaObjectID != -1) {
                superValue = objectManager->GetOrCreateProxyWeak(javaObjectID, superValue);
            }
            jsThis.setProperty(rt, PROP_KEY_SUPERVALUE, superValue);
        }

        return superValue;
    });
}

JsValue MetadataNode::MethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                     const JsValue *args, size_t argc,
                                     MethodCallbackData *initialCallbackData) {
    return Guarded(rt, [&]() -> JsValue {
        MetadataEntry *entry = nullptr;

        auto callbackData = initialCallbackData;

        string *className;
        auto &first = callbackData->candidates.front();
        auto &methodName = first.getName();

        // Fast path for the overwhelmingly common single-overload, non-extension
        // method with no parent chain: skip the candidate-search loop entirely.
        if (callbackData->parent == nullptr &&
            callbackData->candidates.size() == 1 &&
            !first.isExtensionFunction &&
            first.getParamCount() == argc) {
            className = &callbackData->node->m_name;
            entry = &first;
        }

        while ((callbackData != nullptr) && (entry == nullptr)) {
            auto &candidates = callbackData->candidates;

            className = &callbackData->node->m_name;

            // Iterates through all methods and finds the best match based on the number of arguments
            auto found = false;
            for (auto &c: candidates) {
                found = (!c.isExtensionFunction && c.getParamCount() == argc) ||
                        (c.isExtensionFunction && c.getParamCount() == argc + 1);
                if (found) {
                    if (c.isExtensionFunction) {
                        className = &c.getDeclaringType();
                    }
                    entry = &c;
                    DEBUG_WRITE("MetaDataEntry Method %s's signature is: %s",
                                entry->getName().c_str(),
                                entry->getSig().c_str());
                    break;
                }
            }

            // Iterates through the parent class's methods to find a good match
            if (!found) {
                callbackData = callbackData->parent;
            }
        }


        if (initialCallbackData->cachedIsValueOf < 0) {
            initialCallbackData->cachedIsValueOf =
                    (methodName == PROP_KEY_VALUEOF) ? 1 : 0;
        }
        if (argc == 0 && initialCallbackData->cachedIsValueOf == 1) {
            return JsValue(rt, thisVal);
        }

        if (initialCallbackData->cachedIsFromInterface < 0) {
            initialCallbackData->cachedIsFromInterface =
                    initialCallbackData->node->IsNodeTypeInterface() ? 1 : 0;
        }
        bool isFromInterface = initialCallbackData->cachedIsFromInterface == 1;
        if (initialCallbackData->objectManager == nullptr) {
            initialCallbackData->objectManager =
                    Runtime::GetRuntime(rt)->GetObjectManager();
        }
        return CallbackHandlers::CallJavaMethod(rt, thisVal, *className, methodName, entry,
                                                isFromInterface, first.isStatic, false,
                                                args, argc, initialCallbackData->objectManager);
    });
}

/**
 * Compare class hierarchy in metadata with that at runtime. If a base class is missing
 * at runtime, we must add all it's methods to the current class.
 */
bool
MetadataNode::CheckClassHierarchy(JEnv &env, jclass currentClass, MetadataTreeNode *currentTreeNode,
                                  MetadataTreeNode *baseTreeNode,
                                  std::vector<MetadataTreeNode *> &skippedBaseTypes) {
    auto shouldSkipBaseClass = false;
    if ((currentClass != nullptr) && (baseTreeNode != currentTreeNode) &&
        (baseTreeNode != nullptr) &&
        (baseTreeNode->offsetValue > 0)) {
        auto baseNode = GetOrCreateInternal(baseTreeNode);
        auto baseClass = env.FindClass(baseNode->m_name);
        if (baseClass != nullptr) {
            auto isBaseClass = env.IsAssignableFrom(currentClass, baseClass) == JNI_TRUE;
            if (!isBaseClass) {
                skippedBaseTypes.push_back(baseTreeNode);
                shouldSkipBaseClass = true;
            }
        }
    }
    return shouldSkipBaseClass;
}

void MetadataNode::SetMissingBaseMethods(
        JsRuntime &rt, const std::vector<MetadataTreeNode *> &skippedBaseTypes,
        const std::vector<MethodCallbackData *> &instanceMethodData,
        JsObject constructor) {
    for (auto treeNode: skippedBaseTypes) {
        uint8_t *curPtr = s_metadataReader.GetValueData() + treeNode->offsetValue + 1;

        auto nodeType = s_metadataReader.GetNodeType(treeNode);
        auto curType = s_metadataReader.ReadTypeName(treeNode);
        curPtr += sizeof(uint16_t /* baseClassId */);

        if (s_metadataReader.IsNodeTypeInterface(nodeType)) {
            curPtr += sizeof(uint8_t) + sizeof(uint32_t);
        }

        // Get candidates from instance methods metadata
        auto instanceMethodCount = *reinterpret_cast<uint16_t *>(curPtr);
        curPtr += sizeof(uint16_t);
        MethodCallbackData *callbackData = nullptr;

        for (auto i = 0; i < instanceMethodCount; i++) {
            auto entry = MetadataReader::ReadInstanceMethodEntry(&curPtr);
            auto &methodName = entry.getName();
            auto isConstructor = methodName == "<init>";
            if (isConstructor) {
                continue;
            }

            for (auto data: instanceMethodData) {
                if (data->candidates.front().name == methodName) {
                    callbackData = data;
                    break;
                }
            }

            if (callbackData == nullptr) {
                callbackData = new MethodCallbackData(this);
                auto proto = constructor.getPropertyAsObject(rt, "prototype");
                auto method = JsFunction::createFromHostFunction(
                        rt, engine::PropNameID::forAscii(rt, methodName), 0,
                        [callbackData](JsRuntime &rt, const JsValue &thisVal, const JsValue *args,
                                       size_t argc) -> JsValue {
                            return MethodCallback(rt, thisVal, args, argc, callbackData);
                        });
                proto.setProperty(rt, methodName.c_str(), JsValue(rt, method));
            }

            bool foundSameSig = false;
            for (auto &m: callbackData->candidates) {
                foundSameSig = m.getSig() == entry.getSig();
                if (foundSameSig) {
                    break;
                }
            }

            if (!foundSameSig) {
                callbackData->candidates.push_back(std::move(entry));
            }
        }
    }
}

void MetadataNode::BuildMetadata(const std::string &filesPath) {
    s_metadataReader = MetadataBuilder::BuildMetadata(filesPath);
}

void MetadataNode::onDisposeRuntime(JsRuntime &rt) {
    const void *key = rt.identity();
    {
        auto it = s_metadata_node_cache.Get(key);
        if (it != nullptr) {
            // Erasing the maps releases every owned constructor handle; the napi
            // tree had to napi_delete_reference each one (and its check was
            // inverted, so it only ever deleted null refs).
            for (const auto &entry: it->CtorFuncCache) {
                for (const auto data: entry.second.instanceMethodCallbacks) {
                    delete data;
                }
            }
            it->CtorFuncCache.clear();

            it->ExtendedCtorFuncCache.clear();

            for (const auto &entry: it->fieldCallbackData) {
                delete entry;
            }
            for (const auto &entry: it->propertyCallbackData) {
                delete entry->getterEntry;
                delete entry->setterEntry;
                delete entry;
            }
            for (const auto &entry: it->extendedClassCallbackData) {
                delete entry;
            }
        }
        s_metadata_node_cache.Remove(key);
        delete it;
    }
    {
        auto it = s_arrayObjects.find(key);
        if (it != s_arrayObjects.end()) {
            s_arrayObjects.erase(it);
        }
    }
}


string MetadataNode::TNS_PREFIX = "com/tns/gen/";
MetadataReader MetadataNode::s_metadataReader;
robin_hood::unordered_map<std::string, MetadataNode *> MetadataNode::s_name2NodeCache;
robin_hood::unordered_map<std::string, MetadataTreeNode *> MetadataNode::s_name2TreeNodeCache;
robin_hood::unordered_map<MetadataTreeNode *, MetadataNode *> MetadataNode::s_treeNode2NodeCache;
tns::ConcurrentMap<const void *, MetadataNode::MetadataNodeCache *> MetadataNode::s_metadata_node_cache;
robin_hood::unordered_map<const void *, JsValue> MetadataNode::s_arrayObjects;

bool MetadataNode::s_profilerEnabled = false;
