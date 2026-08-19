#ifndef METADATA_NODE_H
#define METADATA_NODE_H

#include <string>
#include "MetadataTreeNode.h"
#include "MetadataEntry.h"
#include "robin_hood.h"
#include "MetadataReader.h"
#include "Runtime.h"
#include "ObjectManager.h"

#include "FieldCallbackData.h"
using namespace tns;

class MetadataNode {
public:
    // Set once from Runtime::Init, out of the app's config: the static binding
    // generator picked the naming scheme at build time and the runtime has to
    // look names up the same way. Only ever widens -- a runtime compiled to
    // require content keys cannot be turned back off by an app.
    static void SetContentKeyedBindings(bool enabled);

    static void Init(JsRuntime &rt);

    static void BuildMetadata(const std::string &filesPath);

    static void CreateTopLevelNamespaces(JsRuntime &rt);

    JsValue CreateWrapper(JsRuntime &rt);

    JsValue CreateJSWrapper(JsRuntime &rt, tns::ObjectManager *objectManager);

    JsValue CreateArrayWrapper(JsRuntime &rt);

    static MetadataNode *GetOrCreate(const std::string &className);

    static MetadataReader *getMetadataReader();

    static JsValue GetImplementationObject(JsRuntime &rt, const JsValue &object);

    inline static MetadataNode* GetInstanceMetadata(JsRuntime &rt, const JsValue &object) {
        // Metadata lives on the per-instance JSInstanceInfo (set in
        // ObjectManager::Link / GetOrCreateProxy). The napi tree had a second,
        // non-host branch reading a "#instance_metadata" external; a host object
        // is the only path here, so there is one lookup.
        return tns::Runtime::GetRuntime(rt)->GetObjectManager()->GetInstanceNode(object);
    }

    inline static MetadataNode* GetNodeFromHandle(JsRuntime &rt, const JsValue &value) {
        auto node = GetInstanceMetadata(rt, value);
        return node;
    }

    // The napi tree marked a "null object" by hanging a napi_external carrying
    // this MetadataNode* off a `nullNode` property. engine:: has no external, and
    // the native-state slot is both faster (a field load, not a prototype-chain
    // walk) and free on these objects -- they are constructor-level singletons,
    // never Java-backed wrappers, so they never carry a JSInstanceInfo.
    // Returns nullptr when `value` is not a null object.
    static MetadataNode *GetNullNode(JsRuntime &rt, const JsValue &value);

    static string GetTypeMetadataName(JsRuntime &rt, const JsValue &value);

    static JsValue CreateExtendedJSWrapper(JsRuntime &rt, ObjectManager *objectManager,
                                           const std::string &proxyClassName, int javaObjectID,
                                           MetadataNode **outNode = nullptr);

    std::string GetName();

    static void onDisposeRuntime(JsRuntime &rt);

    bool isArray();

private:
    struct CtorCacheData;
    struct MethodCallbackData;
    struct PropertyCallbackData;
    struct ExtendedClassCallbackData;
    struct ExtendedClassCacheData;
    struct MetadataNodeCache;

    static string CreateFullClassName(const std::string& className, const std::string& extendNameAndLocation);

    static string CreateContentKey(const std::string& baseClassName,
                                   std::vector<std::string> interfaceNames,
                                   std::vector<std::string> methodNames);

    // Whether an unnamed extend() names its class by content rather than by call
    // site. Always available as a fallback when no usable location exists; the
    // default is on wherever the bundler is the embedder's, since the generator
    // and the runtime cannot agree on coordinates there.
    static bool ContentKeyedBindingsEnabled();

    static JsValue CreateArrayObjectConstructor(JsRuntime &rt);

    static void SetInstanceMetadata(JsRuntime &rt, const JsValue &object, MetadataNode* node);


    static bool
    CheckClassHierarchy(JEnv &env, jclass currentClass, MetadataTreeNode *currentTreeNode,
                        MetadataTreeNode *baseTreeNode,
                        std::vector<MetadataTreeNode *> &skippedBaseTypes);

    static MetadataNode *GetOrCreateInternal(MetadataTreeNode *treeNode);

    static MetadataNodeCache *GetMetadataNodeCache(JsRuntime &rt);

    explicit MetadataNode(MetadataTreeNode *treeNode);

    void SetMissingBaseMethods(
            JsRuntime &rt, const std::vector<MetadataTreeNode *> &skippedBaseTypes,
            const std::vector<MethodCallbackData *> &instanceMethodData,
            JsObject constructor);




    JsValue GetConstructorFunction(JsRuntime &rt);

    JsValue GetConstructorFunctionInternal(JsRuntime &rt, MetadataTreeNode *treeNode,
                                           std::vector<MethodCallbackData *> instanceMethodsCallbackData);

    JsValue CreatePackageObject(JsRuntime &rt);


    static bool IsValidExtendName(JsRuntime &rt, const JsValue &name);
    static bool GetExtendLocation(JsRuntime &rt, std::string& extendLocation, bool isTypeScriptExtend);
    static ExtendedClassCacheData GetCachedExtendedClassData(JsRuntime &rt, const std::string& proxyClassName);
    static std::string GetJniClassName(const MetadataTreeNode* node);

#if defined(NS_METADATA_USAGE_TRACE)
    static void TraceUsage(MetadataTreeNode *treeNode);
#endif


    static void SetClassAccessor(JsRuntime &rt, JsObject constructor);

    static MetadataEntry GetChildMetadataForPackage(MetadataNode *node, const char *propName);

    static MetadataTreeNode *GetOrCreateTreeNodeByName(const std::string &className);

    bool IsNodeTypeInterface();

    std::vector<MetadataNode::MethodCallbackData *> SetClassMembersFromStaticMetadata(
            JsRuntime &rt, JsObject constructor,
            std::vector<MethodCallbackData *> &instanceMethodsCallbackData,
            const std::vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
            MetadataTreeNode *treeNode);

    std::vector<MetadataNode::MethodCallbackData *> SetInstanceMembersFromRuntimeMetadata(
            JsRuntime &rt, JsObject constructor,
            std::vector<MethodCallbackData *> &instanceMethodsCallbackData,
            const std::vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
            MetadataTreeNode *treeNode);


    inline static MethodCallbackData *tryGetExtensionMethodCallbackData(
            const robin_hood::unordered_map<std::string, MethodCallbackData *> &collectedMethodCallbackData,
            const std::string &lookupName);

    std::vector<MetadataNode::MethodCallbackData *> SetClassMembers(
            JsRuntime &rt, JsObject constructor,
            std::vector<MethodCallbackData *> &instanceMethodsCallbackData,
            const std::vector<MethodCallbackData *> &baseInstanceMethodsCallbackData,
            MetadataTreeNode *treeNode);


    static JsValue NullObjectAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                    MetadataNode *node);

    // Returns true if `jsThis` is a real backed instance rather than the class
    // prototype (i.e. someone did Class.prototype.<member>). Every real instance
    // is a host-object proxy and the prototype is not; the napi tree's
    // non-host fallback (an identity compare against a cached prototype ref) has
    // no counterpart here because host objects are the only path.
    static bool IsInstanceReceiver(JsRuntime &rt, const JsValue &jsThis);

    static JsValue FieldAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                               FieldCallbackData *fieldData);

    static JsValue FieldAccessorSetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                               const JsValue *args, size_t argc,
                                               FieldCallbackData *fieldData);

    static JsValue ArraySetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                       const JsValue *args, size_t argc);

    static JsValue ArrayGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                       const JsValue *args, size_t argc);

    static JsValue ArrayGetAllValuesCallback(JsRuntime &rt, const JsValue &thisVal,
                                             const JsValue *args, size_t argc);

    static JsValue ArrayLengthCallback(JsRuntime &rt, const JsValue &thisVal,
                                       const JsValue *args, size_t argc);

    // Native equivalents of the helpers that used to live in getNativeArrayProp.
    static JsValue ArrayMapCallback(JsRuntime &rt, const JsValue &thisVal,
                                    const JsValue *args, size_t argc);

    static JsValue ArrayForEachCallback(JsRuntime &rt, const JsValue &thisVal,
                                        const JsValue *args, size_t argc);

    static JsValue ArrayToStringCallback(JsRuntime &rt, const JsValue &thisVal,
                                         const JsValue *args, size_t argc);

    static JsValue ArraySymbolIteratorCallback(JsRuntime &rt, const JsValue &thisVal,
                                               const JsValue *args, size_t argc);

    static JsValue PropertyAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  PropertyCallbackData *data);

    static JsValue PropertyAccessorSetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                                  const JsValue *args, size_t argc,
                                                  PropertyCallbackData *data);

    static JsValue ExtendMethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                        const JsValue *args, size_t argc, MetadataNode *node);

    static JsValue MethodCallback(JsRuntime &rt, const JsValue &thisVal,
                                  const JsValue *args, size_t argc, MethodCallbackData *data);

    static JsValue ClassAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal);

    static JsValue PackageGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                         MetadataTreeNode *childTreeNode);

    static JsValue ExtendedClassConstructorCallback(JsRuntime &rt, const JsValue &thisVal,
                                                    const JsValue *args, size_t argc,
                                                    ExtendedClassCallbackData *extData);

    static JsValue InterfaceConstructorCallback(JsRuntime &rt, const JsValue &thisVal,
                                                const JsValue *args, size_t argc,
                                                MetadataNode *node);

    static JsValue ClassConstructorCallback(JsRuntime &rt, const JsValue &thisVal,
                                            const JsValue *args, size_t argc, MetadataNode *node);

    static void SetInnerTypes(JsRuntime &rt, JsObject constructor, MetadataTreeNode *treeNode);

    static JsValue InnerTypeGetterCallback(JsRuntime &rt, const JsValue &thisVal,
                                           MetadataTreeNode *curChild);

    static JsValue NullValueOfCallback(JsRuntime &rt, const JsValue &thisVal,
                                       const JsValue *args, size_t argc);

    // The napi tree needed a SymbolHasInstanceData heap holder because PrimJS
    // boxed the napi `data` pointer into 48 bits and corrupted a raw JNI global
    // ref. engine:: host functions carry their state in the callback's own
    // capture, so there is no `data` pointer to box and no holder to allocate.
    static void RegisterSymbolHasInstanceCallback(JsRuntime &rt, const MetadataTreeNode *treeNode,
                                                  const JsValue &interface);

    static JsValue SuperAccessorGetterCallback(JsRuntime &rt, const JsValue &thisVal);

    static bool ValidateExtendArguments(JsRuntime &rt, size_t argc, const JsValue *argv,
                                        bool extendLocationFound, string &extendLocation,
                                        JsValue *extendName, JsValue *implementationObject,
                                        bool isTypeScriptExtend);

    MetadataTreeNode *m_treeNode;

    std::string m_name;
    std::string m_implType;
    bool m_isArray;

    static bool IsJavascriptKeyword(const std::string &word);

    static std::string TNS_PREFIX;
    static MetadataReader s_metadataReader;

    static robin_hood::unordered_map<std::string, MetadataNode *> s_name2NodeCache;
    static robin_hood::unordered_map<std::string, MetadataTreeNode *> s_name2TreeNodeCache;
    static robin_hood::unordered_map<MetadataTreeNode *, MetadataNode *> s_treeNode2NodeCache;
    // Both keyed by JsRuntime::identity(); &rt is not stable across callbacks.
    static tns::ConcurrentMap<const void *, MetadataNodeCache *> s_metadata_node_cache;
    static robin_hood::unordered_map<const void *, JsValue> s_arrayObjects;

    // An owned engine handle replaces every napi_ref below: it survives handle
    // scopes and is released by destroying/erasing the owner, so there are no
    // napi_delete_reference calls left in the teardown paths.
    struct CtorCacheData {
        CtorCacheData(JsValue _constructorFunction,
                      std::vector<MethodCallbackData *> _instanceMethodCallbacks)
                :
                constructorFunction(std::move(_constructorFunction)),
                instanceMethodCallbacks(std::move(_instanceMethodCallbacks)) {
        }

        JsValue constructorFunction;
        std::vector<MethodCallbackData *> instanceMethodCallbacks;
    };

    struct MethodCallbackData {
        MethodCallbackData()
                :
                node(nullptr), parent(nullptr), isSuper(false) {
        }

        MethodCallbackData(MetadataNode *_node)
                :
                node(_node), parent(nullptr), isSuper(false) {
        }

        std::vector<MetadataEntry> candidates;
        MetadataNode *node;
        MethodCallbackData *parent;
        bool isSuper;
        // Lazily-cached, per-class invariants resolved on first dispatch
        // (-1 = not yet computed, 0 = false, 1 = true).
        int8_t cachedIsFromInterface = -1;
        int8_t cachedIsValueOf = -1;
        // Cached per-runtime ObjectManager (this data is created per runtime, so
        // the pointer's lifetime matches it — no staleness across runtimes).
        tns::ObjectManager *objectManager = nullptr;
    };

    struct ExtendedClassCacheData {
        ExtendedClassCacheData()
                :
                node(nullptr) {
        }

        ExtendedClassCacheData(JsValue extCtorFunc, const std::string &_extendedName,
                               MetadataNode *_node)
                :
                extendedCtorFunction(std::move(extCtorFunc)), extendedName(_extendedName),
                node(_node) {
        }

        JsValue extendedCtorFunction;
        std::string extendedName;
        MetadataNode *node;
    };

    struct PropertyCallbackData {
        PropertyCallbackData(std::string _propertyName, std::string _getterMethodName,
                             std::string _setterMethodName)
                :
                propertyName(std::move(_propertyName)),
                getterMethodName(std::move(_getterMethodName)),
                setterMethodName(std::move(_setterMethodName)) {

        }

        std::string propertyName;
        std::string getterMethodName;
        std::string setterMethodName;
        // Direct-dispatch support: the resolved getter/setter method entries plus
        // cached invariants let the accessor call CallJavaMethod directly, with no
        // JS method lookup or nested MethodCallback. nullptr => no getter/setter.
        MetadataEntry *getterEntry = nullptr;
        MetadataEntry *setterEntry = nullptr;
        MetadataNode *node = nullptr;
        int8_t cachedIsFromInterface = -1;
        tns::ObjectManager *objectManager = nullptr;
    };

    struct ExtendedClassCallbackData {
        ExtendedClassCallbackData(MetadataNode *_node, const std::string &_extendedName,
                                  JsValue _implementationObject, std::string _fullClassName)
                :
                node(_node), extendedName(_extendedName),
                implementationObject(std::move(_implementationObject)),
                fullClassName(std::move(_fullClassName)) {
        }

        MetadataNode *node;
        std::string extendedName;
        JsValue implementationObject;

        std::string fullClassName;
    };

    struct MetadataNodeCache {
        robin_hood::unordered_map<MetadataTreeNode *, CtorCacheData> CtorFuncCache;
        robin_hood::unordered_map<std::string, MetadataNode::ExtendedClassCacheData> ExtendedCtorFuncCache;
        std::vector<FieldCallbackData *> fieldCallbackData;
        std::vector<PropertyCallbackData *> propertyCallbackData;
        std::vector<ExtendedClassCallbackData *> extendedClassCallbackData;
    };

    static bool s_profilerEnabled;

};

#endif //METADATA_NODE_H
