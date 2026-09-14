Object CreateNativeApi(Runtime& runtime, const NativeApiConfig& config) {
  auto bridge = std::make_shared<NativeApiBridge>(config);
  return Object::createFromHostObject(runtime,
                                      std::make_shared<NativeApiHostObject>(std::move(bridge)));
}

void NativeApiWriteSmokeStage(const char* stage) {
  const char* enabled = getenv("NATIVESCRIPT_RN_TURBO_SMOKE_MARKER");
  if (enabled == nullptr || enabled[0] == '\0') {
    return;
  }

  NSString* path =
      [NSTemporaryDirectory() stringByAppendingPathComponent:@"NativeScriptNativeApiSmoke.marker"];
  NSString* content = [NSString stringWithFormat:@"stage=%s\n", stage != nullptr ? stage : ""];
  [content writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

void InstallAggregateGlobals(Runtime& runtime, Object& api, const char* namesFunction) {
  Value metadataValue = api.getProperty(runtime, "metadata");
  if (!metadataValue.isObject()) {
    return;
  }
  Object metadata = metadataValue.asObject(runtime);
  Value namesValue = metadata.getProperty(runtime, namesFunction);
  if (!namesValue.isObject()) {
    return;
  }
  Object namesObject = namesValue.asObject(runtime);
  if (!namesObject.isFunction(runtime)) {
    return;
  }
  Value namesResult = namesObject.asFunction(runtime).call(runtime);
  if (!namesResult.isObject() || !namesResult.asObject(runtime).isArray(runtime)) {
    return;
  }
  Array names = namesResult.asObject(runtime).getArray(runtime);
  Object global = runtime.global();
  for (size_t i = 0; i < names.size(runtime); i++) {
    Value nameValue = names.getValueAtIndex(runtime, i);
    if (!nameValue.isString()) {
      continue;
    }
    std::string name = nameValue.asString(runtime).utf8(runtime);
    if (name.empty() || global.hasProperty(runtime, name.c_str())) {
      continue;
    }
    try {
      Value aggregate = api.getProperty(runtime, name.c_str());
      if (!aggregate.isUndefined()) {
        global.setProperty(runtime, name.c_str(), aggregate);
      }
    } catch (const std::exception&) {
      // Some React Native globals are read-only even when hasProperty misses
      // them. Keep NativeScript initialization resilient and skip collisions.
    }
  }
}

std::string jsStringLiteral(const char* value) {
  std::string result = "'";
  if (value != nullptr) {
    for (const char* current = value; *current != '\0'; current++) {
      switch (*current) {
        case '\\':
          result += "\\\\";
          break;
        case '\'':
          result += "\\'";
          break;
        case '\n':
          result += "\\n";
          break;
        case '\r':
          result += "\\r";
          break;
        case '\t':
          result += "\\t";
          break;
        default:
          result += *current;
          break;
      }
    }
  }
  result += "'";
  return result;
}

void InstallNativeApiGlobalSymbols(Runtime& runtime, const char* globalName) {
  NativeApiWriteSmokeStage("engine:globals:before-eval");
  static const char* GlobalInstaller = R"Engine_GLOBALS(
(function(nativeApiGlobalName) {
  'use strict';
  var api = globalThis[nativeApiGlobalName];
  var installedFlagName = '__nativeScriptNativeApiGlobalsInstalled';
  if (!api || globalThis[installedFlagName]) {
    return;
  }

  var cacheName = '__nativeScriptNativeApiGlobalCache';
  var typeCodeKey = '__nativeApiTypeCode';
  var classWrappers = typeof WeakMap === 'function' ? new WeakMap() : null;
  var classWrappersByName = Object.create(null);
  var resolvingGlobal = Object.create(null);

  function globalCache() {
    var existing = globalThis[cacheName];
    if (existing && typeof existing === 'object') {
      return existing;
    }
    var cache = Object.create(null);
    Object.defineProperty(globalThis, cacheName, {
      configurable: false,
      enumerable: false,
      writable: false,
      value: cache
    });
    return cache;
  }

  function cacheGlobal(name, value) {
    if (name && value !== undefined) {
      globalCache()[name] = value;
    }
  }

  function resolveCachedGlobal(name, expectedKind) {
    if (!name) {
      return undefined;
    }
    var cached = globalCache()[name];
    if (cached && (typeof cached === 'object' || typeof cached === 'function') && cached.kind === expectedKind) {
      return cached;
    }
    if (resolvingGlobal[name] || !Object.prototype.hasOwnProperty.call(globalThis, name)) {
      return undefined;
    }
    resolvingGlobal[name] = true;
    try {
      var value = globalThis[name];
      if (value && (typeof value === 'object' || typeof value === 'function') && value.kind === expectedKind) {
        cacheGlobal(name, value);
        return value;
      }
    } finally {
      delete resolvingGlobal[name];
    }
    return undefined;
  }

  function defineLazyGlobal(name, resolve, force, nativeKind) {
    if (!name) {
      return;
    }
    if (!force && Object.prototype.hasOwnProperty.call(globalThis, name)) {
      try {
        var existingDescriptor = Object.getOwnPropertyDescriptor(globalThis, name);
        if (existingDescriptor && Object.prototype.hasOwnProperty.call(existingDescriptor, 'value')) {
          cacheGlobal(name, existingDescriptor.value);
        }
      } catch (_) {
      }
      return;
    }
    var nativeDefineLazyGlobal = api.__defineLazyGlobal;
    if (nativeKind && typeof nativeDefineLazyGlobal === 'function' &&
        typeof globalThis.__nativeScriptResolveNativeApiLazyGlobal === 'function') {
      try {
        if (nativeDefineLazyGlobal(name, nativeKind, !!force)) {
          return;
        }
      } catch (_) {
      }
    }
    try {
      Object.defineProperty(globalThis, name, {
        configurable: true,
        enumerable: false,
        get: function() {
          var value = resolve(name);
          cacheGlobal(name, value);
          Object.defineProperty(globalThis, name, {
            configurable: true,
            enumerable: false,
            writable: true,
            value: value
          });
          return value;
        },
        set: function(value) {
          // Assignment over a lazy global must behave like a plain global
          // assignment (@nativescript/core writes shims such as
          // global.System), not throw "no setter for property".
          cacheGlobal(name, value);
          Object.defineProperty(globalThis, name, {
            configurable: true,
            enumerable: true,
            writable: true,
            value: value
          });
        }
      });
    } catch (_) {
      var value = resolve(name);
      if (value !== undefined) {
        cacheGlobal(name, value);
        Object.defineProperty(globalThis, name, {
          configurable: true,
          enumerable: false,
          writable: true,
          value: value
        });
      }
    }
  }

  Object.defineProperty(globalThis, '__nativeScriptResolveNativeApiGlobal', {
    configurable: false,
    enumerable: false,
    writable: false,
    value: resolveCachedGlobal
  });

  Object.defineProperty(globalThis, '__nativeScriptResolveNativeApiClassWrapper', {
    configurable: false,
    enumerable: false,
    writable: false,
    value: function(name) {
      return name ? classWrappersByName[name] : undefined;
    }
  });

  function findPrototypeDescriptor(className, property) {
    var prototype;
    if (className && (typeof className === 'object' || typeof className === 'function')) {
      prototype = className;
    } else {
      var wrapper = className ? classWrappersByName[className] : undefined;
      prototype = wrapper && wrapper.prototype;
    }
    while (prototype != null) {
      var descriptor = Object.getOwnPropertyDescriptor(prototype, property);
      if (descriptor) {
        return descriptor;
      }
      prototype = Object.getPrototypeOf(prototype);
    }
    return undefined;
  }

  function setObjectAccessorCallbackState(instance, active) {
    try {
      if (typeof api.__setObjectAccessorCallbackState === 'function') {
        api.__setObjectAccessorCallbackState(instance, !!active);
      }
    } catch (_) {
    }
  }

  function nativeExtensionAccessorWithCallbackState(fn) {
    if (typeof fn !== 'function') {
      return fn;
    }
    return function() {
      setObjectAccessorCallbackState(this, true);
      try {
        var args = Array.prototype.slice.call(arguments);
        return fn.apply(this, args);
      } finally {
        setObjectAccessorCallbackState(this, false);
      }
    };
  }

  // Wraps extend()'s methods object with: (a) the re-entry guard above around
  // any accessor (get/set) so a native accessor invocation calling back into
  // itself through the ObjC runtime is suppressed rather than recursing, and
  // (b) NSFastEnumeration-flavored indexed-collection aliases
  // (objectAtIndexedSubscript / setObjectAtIndexedSubscript / Symbol.iterator)
  // when the methods object looks like an Obj-C indexed collection
  // (objectAtIndex + count), matching what a hand-written ObjC subclass gets
  // for free from the runtime.
  function nativeExtensionMethodsWithIndexedCollectionAliases(methods) {
    if (methods == null || typeof methods !== 'object') {
      return methods;
    }

    var descriptors = Object.getOwnPropertyDescriptors(methods);
    var descriptorKeys =
      typeof Reflect === 'object' && typeof Reflect.ownKeys === 'function'
        ? Reflect.ownKeys(descriptors)
        : Object.keys(descriptors);
    var needsAccessorCallbackState = false;
    for (var descriptorIndex = 0; descriptorIndex < descriptorKeys.length; descriptorIndex++) {
      var descriptor = descriptors[descriptorKeys[descriptorIndex]];
      if (descriptor &&
          (typeof descriptor.get === 'function' ||
           typeof descriptor.set === 'function')) {
        needsAccessorCallbackState = true;
        break;
      }
    }

    var hasObjectAtIndex =
      Object.prototype.hasOwnProperty.call(methods, 'objectAtIndex');
    var hasCount =
      Object.prototype.hasOwnProperty.call(methods, 'count');
    var hasSymbolIterator =
      typeof Symbol === 'function' && Symbol.iterator &&
      Object.prototype.hasOwnProperty.call(methods, Symbol.iterator);
    var needsObjectAtIndexedSubscript =
      hasObjectAtIndex &&
      !Object.prototype.hasOwnProperty.call(methods, 'objectAtIndexedSubscript');
    var needsSetObjectAtIndexedSubscript =
      Object.prototype.hasOwnProperty.call(methods, 'replaceObjectAtIndexWithObject') &&
      !Object.prototype.hasOwnProperty.call(methods, 'setObjectAtIndexedSubscript');
    var needsIndexedCollectionIterator =
      typeof Symbol === 'function' && Symbol.iterator &&
      hasObjectAtIndex && hasCount && !hasSymbolIterator;

    if (!needsObjectAtIndexedSubscript &&
        !needsSetObjectAtIndexedSubscript &&
        !needsIndexedCollectionIterator &&
        !needsAccessorCallbackState) {
      return methods;
    }

    if (needsAccessorCallbackState) {
      for (var accessorIndex = 0; accessorIndex < descriptorKeys.length; accessorIndex++) {
        var accessorKey = descriptorKeys[accessorIndex];
        var accessorDescriptor = descriptors[accessorKey];
        if (!accessorDescriptor) {
          continue;
        }
        if (typeof accessorDescriptor.get === 'function') {
          accessorDescriptor.get =
            nativeExtensionAccessorWithCallbackState(accessorDescriptor.get);
        }
        if (typeof accessorDescriptor.set === 'function') {
          accessorDescriptor.set =
            nativeExtensionAccessorWithCallbackState(accessorDescriptor.set);
        }
      }
    }

    var prepared = Object.create(Object.getPrototypeOf(methods));
    Object.defineProperties(prepared, descriptors);

    if (needsObjectAtIndexedSubscript) {
      Object.defineProperty(prepared, 'objectAtIndexedSubscript', {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function(index) {
          return this.objectAtIndex(index);
        }
      });
    }

    if (needsSetObjectAtIndexedSubscript) {
      Object.defineProperty(prepared, 'setObjectAtIndexedSubscript', {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function(anObject, index) {
          return this.replaceObjectAtIndexWithObject(index, anObject);
        }
      });
    }

    if (needsIndexedCollectionIterator) {
      Object.defineProperty(prepared, Symbol.iterator, {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function() {
          var receiver = this;
          var index = 0;
          return {
            next: function() {
              var countValue = receiver.count;
              var count = typeof countValue === 'function'
                ? countValue.call(receiver)
                : countValue;
              if (!(index < count)) {
                return { done: true };
              }
              return {
                value: receiver.objectAtIndex(index++),
                done: false
              };
            }
          };
        }
      });
    }

    return prepared;
  }

  function nativeExtensionMethodsHaveIterator(methods) {
    return typeof Symbol === 'function' && Symbol.iterator &&
      methods != null && typeof methods === 'object' &&
      Object.prototype.hasOwnProperty.call(methods, Symbol.iterator);
  }

  function nativeExtensionOptionsWithIterator(options, methods) {
    var extendOptions = options || {};
    if (!nativeExtensionMethodsHaveIterator(methods)) {
      return extendOptions;
    }
    try {
      return Object.assign({}, extendOptions, {
        __hasIterator: true
      });
    } catch (_) {
      extendOptions.__hasIterator = true;
      return extendOptions;
    }
  }

  Object.defineProperty(globalThis, '__nativeScriptCreateNativeApiIterator', {
    configurable: false,
    enumerable: false,
    writable: false,
    value: function(receiver, prototype) {
      if (!receiver || typeof Symbol !== 'function') {
        return undefined;
      }
      var descriptor = findPrototypeDescriptor(prototype || receiver.className, Symbol.iterator);
      if (descriptor && typeof descriptor.value === 'function') {
        return descriptor.value.call(receiver);
      }
      if (descriptor && typeof descriptor.get === 'function') {
        var getterValue = descriptor.get.call(receiver);
        if (typeof getterValue === 'function') {
          return getterValue.call(receiver);
        }
      }
      var iteratorMethod = receiver[Symbol.iterator];
      return typeof iteratorMethod === 'function'
        ? iteratorMethod.call(receiver)
        : undefined;
    }
  });

  function wrapAggregateConstructor(nativeConstructor) {
    if (typeof nativeConstructor !== 'function') {
      return nativeConstructor;
    }
    var aggregate = function NativeScriptAggregate(initialValue) {
      return nativeConstructor(initialValue);
    };
    try {
      Object.defineProperty(aggregate, Symbol.hasInstance, {
        configurable: true,
        enumerable: false,
        value: function(value) {
          return !!value &&
            typeof value === 'object' &&
            value.kind === nativeConstructor.kind &&
            value.name === nativeConstructor.runtimeName;
        }
      });
    } catch (_) {
    }
    ['kind', 'runtimeName', 'metadataOffset', 'sizeof', 'fields', 'equals'].forEach(function(key) {
      try {
        Object.defineProperty(aggregate, key, {
          configurable: true,
          enumerable: false,
          writable: false,
          value: nativeConstructor[key]
        });
      } catch (_) {
      }
    });
    return aggregate;
  }

  function setDescriptorValue(target, property, receiver, value) {
    for (var current = target; current; current = Object.getPrototypeOf(current)) {
      var descriptor = Object.getOwnPropertyDescriptor(current, property);
      if (!descriptor) {
        continue;
      }
      if (typeof descriptor.set === 'function') {
        descriptor.set.call(receiver, value);
        return true;
      }
      if (descriptor.writable) {
        if (receiver && receiver !== current) {
          Object.defineProperty(receiver, property, {
            configurable: true,
            enumerable: true,
            writable: true,
            value: value
          });
        } else {
          current[property] = value;
        }
        return true;
      }
      return false;
    }
    return false;
  }

  function setInheritedNativeClassValue(target, property, value) {
    for (var current = Object.getPrototypeOf(target);
         current && current !== Function.prototype;
         current = Object.getPrototypeOf(current)) {
      var nativeClassValue;
      try {
        nativeClassValue = current.__nativeApiClass;
      } catch (_) {
        nativeClassValue = null;
      }
      if (!nativeClassValue) {
        continue;
      }
      try {
        nativeClassValue[property] = value;
        return true;
      } catch (_) {
      }
    }
    return false;
  }

  function isConstructorOptions(value) {
    if (!value || typeof value !== 'object' || Array.isArray(value)) {
      return false;
    }
    if (value.kind || value.nativeAddress || value instanceof Date) {
      return false;
    }
    return Object.getPrototypeOf(value) === Object.prototype ||
      Object.getPrototypeOf(value) === null;
  }

  function capitalizeToken(value) {
    value = String(value || '');
    return value ? value.charAt(0).toUpperCase() + value.slice(1) : value;
  }

  function selectorCandidatesFromOptions(options) {
    var keys = Object.keys(options || {});
    if (!keys.length) {
      return [];
    }
    var first = capitalizeToken(keys[0]);
    var tail = '';
    for (var i = 1; i < keys.length; i++) {
      tail += keys[i] + ':';
    }
    return [
      'initWith' + first + ':' + tail,
      'init' + first + ':' + tail
    ];
  }

  function valuesFromOptions(options) {
    return Object.keys(options || {}).map(function(key) {
      return options[key];
    });
  }

  function selectorScoreForArguments(selectorName, args) {
    if (!selectorName || selectorName.indexOf('init') !== 0) {
      return -1;
    }
    if (selectorName === 'init') {
      return args.length === 0 ? 100 : -1;
    }
    if (args.length === 0) {
      return -1;
    }
    var lower = selectorName.toLowerCase();
    var first = args[0];
    var score = 1;
    if (Array.isArray(first)) {
      if (lower.indexOf('array') !== -1) {
        score += 40;
      }
    } else if (typeof first === 'string') {
      if (lower.indexOf('string') !== -1) {
        score += 40;
      }
      if (lower.indexOf('url') !== -1) {
        score += 10;
      }
    } else if (typeof first === 'number') {
      if (lower.indexOf('primitive') !== -1) {
        score += 50;
      }
      if (lower.indexOf('int') !== -1 ||
          lower.indexOf('integer') !== -1 ||
          lower.indexOf('number') !== -1 ||
          lower.indexOf('float') !== -1 ||
          lower.indexOf('double') !== -1 ||
          lower.indexOf('long') !== -1 ||
          lower.indexOf('short') !== -1) {
        score += 30;
      }
    } else if (isConstructorOptions(first)) {
      if (lower.indexOf('struct') !== -1 ||
          lower.indexOf('structure') !== -1) {
        score += 40;
      }
      if (lower.indexOf('dictionary') !== -1) {
        score += 20;
      }
    } else if (first === null || typeof first === 'undefined') {
      score += 5;
    } else if (lower.indexOf('object') !== -1 ||
               lower.indexOf('url') !== -1 ||
               lower.indexOf('data') !== -1) {
      score += 20;
    }

    var allStrings = args.length > 1 && args.every(function(value) {
      return typeof value === 'string';
    });
    var allNumbers = args.length > 1 && args.every(function(value) {
      return typeof value === 'number';
    });
    if (allStrings && lower.indexOf('string') !== -1) {
      score += 25;
    }
    if (allNumbers &&
        (lower.indexOf('int') !== -1 || lower.indexOf('number') !== -1)) {
      score += 25;
    }
    return score;
  }

  function initializerMembers(nativeClass, argumentCount) {
    var metadataMembers = nativeClass.__instanceMembers || [];
    var runtimeMembers = nativeClass.__runtimeInstanceMembers || [];
    var members = metadataMembers.concat(runtimeMembers);
    var result = [];
    for (var i = 0; i < members.length; i++) {
      var member = members[i];
      if (!member || member.property || !member.selectorName) {
        continue;
      }
      if (member.selectorName.indexOf('init') !== 0) {
        continue;
      }
      if (typeof argumentCount === 'number' &&
          member.argumentCount !== argumentCount) {
        continue;
      }
      result.push(member);
    }
    return result;
  }

  function chooseInitializer(nativeClass, args, optionSelectors) {
    var members = initializerMembers(nativeClass, args.length);
    if (!members.length) {
      return null;
    }
    if (optionSelectors && optionSelectors.length) {
      for (var i = 0; i < optionSelectors.length; i++) {
        for (var j = 0; j < members.length; j++) {
          if (members[j].selectorName === optionSelectors[i]) {
            return members[j];
          }
        }
      }
    }

    var best = null;
    var bestScore = -1;
    for (var k = 0; k < members.length; k++) {
      var score = selectorScoreForArguments(members[k].selectorName, args);
      if (score > bestScore) {
        bestScore = score;
        best = members[k];
      }
    }
    return bestScore >= 0 ? best : null;
  }

  function chooseInitializerBySelectors(nativeClass, args, selectors) {
    if (!selectors || !selectors.length) {
      return null;
    }
    var members = initializerMembers(nativeClass, args.length);
    for (var i = 0; i < selectors.length; i++) {
      for (var j = 0; j < members.length; j++) {
        if (members[j].selectorName === selectors[i]) {
          return members[j];
        }
      }
    }
    return null;
  }

  function unavailableInitializerError(error) {
    return error &&
      /Objective-C selector is not available/.test(String(error.message || error));
  }

  // markConstructing is true for JS-subclass (ClassBuilder) instances: their
  // alloc/init sequence marks the receiver as "under construction" so a
  // non-init method callback landing on a partially-constructed self (e.g.
  // from within an ObjC framework's own init machinery) is suppressed
  // instead of re-entering JS with an object that isn't fully set up yet
  // (see shouldSkipConstructingMethodCallback).
  function shouldUseAllocInitConstructor(constructable, wrapper) {
    var target = wrapper || constructable;
    try {
      return !!(target && target.__nativeApiUseAllocInitConstructor);
    } catch (_) {
      return false;
    }
  }

  function setObjectConstructionState(instance, constructing) {
    try {
      if (api && typeof api.__setObjectConstructionState === 'function') {
        api.__setObjectConstructionState(instance, !!constructing);
      }
    } catch (_) {
    }
  }

  function constructNativeInstance(nativeClass, args, rememberInstance, markConstructing) {
    if (args.length === 1 &&
        args[0] &&
        typeof args[0] === 'object' &&
        (args[0].kind === 'pointer' || args[0].kind === 'reference') &&
        typeof nativeClass.construct === 'function') {
      return nativeClass.construct(args[0]);
    }

    var actualArgs = args;
    var initializer = null;
    if (args.length === 1 && isConstructorOptions(args[0])) {
      var optionSelectors = selectorCandidatesFromOptions(args[0]);
      if (!optionSelectors.length) {
        throw new Error('No initializer found that matches constructor invocation.');
      }
      var optionArgs = valuesFromOptions(args[0]);
      initializer = chooseInitializerBySelectors(
        nativeClass,
        optionArgs,
        optionSelectors
      );
      if (initializer) {
        actualArgs = optionArgs;
      }
    }
    if (!initializer) {
      initializer = chooseInitializer(nativeClass, actualArgs, null);
    }
    if (!initializer) {
      throw new Error('No initializer found that matches constructor invocation.');
    }
    if (typeof nativeClass.alloc !== 'function') {
      throw new Error('Native class cannot be allocated');
    }
    var instance = nativeClass.alloc();
    if (typeof rememberInstance === 'function') {
      instance = rememberInstance(instance);
    }
    if (markConstructing) {
      setObjectConstructionState(instance, true);
    }
    var initializedInstance;
    try {
      if (initializer.selectorName === 'init') {
        if (typeof instance.init !== 'function') {
          throw new Error('No initializer found that matches constructor invocation.');
        }
        initializedInstance = instance.init();
      } else if (initializer.name && typeof instance[initializer.name] === 'function') {
        initializedInstance = instance[initializer.name](...actualArgs);
      } else {
        var invokeArgs = [initializer.selectorName];
        for (var invokeArgIndex = 0; invokeArgIndex < actualArgs.length; invokeArgIndex++) {
          invokeArgs.push(actualArgs[invokeArgIndex]);
        }
        initializedInstance = instance.invoke(...invokeArgs);
      }
      return initializedInstance;
    } catch (error) {
      if (unavailableInitializerError(error)) {
        throw new Error('No initializer found that matches constructor invocation.');
      }
      throw error;
    } finally {
      if (markConstructing) {
        if (initializedInstance && initializedInstance !== instance) {
          setObjectConstructionState(initializedInstance, false);
        }
        setObjectConstructionState(instance, false);
      }
    }
  }

  function nativeClassForInstance(instance, classFallback, baseConstructor) {
    var constructor = instance && instance.constructor;
    if (constructor && constructor !== baseConstructor &&
        constructor !== classFallback) {
      return constructor;
    }
    return classFallback || baseConstructor;
  }

  // Gives extend()ed/TypeScript-native-subclass instances a `class`/
  // `superclass` identity that resolves to the ACTUAL (possibly further
  // JS-subclassed) constructor rather than always reporting the class the
  // extension was originally built against.
  function installInstanceClassIdentity(target, classFallback, baseConstructor) {
    if (!target || typeof Object.create !== 'function' ||
        typeof Object.setPrototypeOf !== 'function') {
      return;
    }
    var parent = null;
    try {
      parent = Object.getPrototypeOf(target);
    } catch (_) {
    }
    var identityPrototype = Object.create(parent || null);
    try {
      Object.defineProperty(identityPrototype, 'class', {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function() {
          return nativeClassForInstance(this, classFallback, baseConstructor);
        }
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(identityPrototype, 'superclass', {
        configurable: true,
        enumerable: false,
        get: function() {
          var constructor = nativeClassForInstance(
            this,
            classFallback,
            baseConstructor
          );
          if (!constructor) {
            return undefined;
          }
          var superclass = constructor.superclass;
          if (typeof superclass === 'function' && superclass.kind !== 'class') {
            return superclass.call(constructor);
          }
          return superclass;
        }
      });
    } catch (_) {
    }
    try {
      Object.setPrototypeOf(target, identityPrototype);
    } catch (_) {
    }
  }

	  function wrapNativeClass(nativeClass) {
	    if (!nativeClass || (typeof nativeClass !== 'object' && typeof nativeClass !== 'function')) {
	      return nativeClass;
	    }
    var nativeClassName = nativeClass.runtimeName || nativeClass.name || '';
    if (nativeClassName && classWrappersByName[nativeClassName]) {
      if (classWrappers) {
        try {
          classWrappers.set(nativeClass, classWrappersByName[nativeClassName]);
        } catch (_) {
        }
      }
      return classWrappersByName[nativeClassName];
    }
    if (classWrappers) {
      var cached = classWrappers.get(nativeClass);
      if (cached) {
        return cached;
      }
    }
    var constructable = function NativeScriptNativeClass() {
      var args = Array.prototype.slice.call(arguments);
      var redirectConstructor = this && this.constructor;
      if (redirectConstructor &&
          redirectConstructor !== constructable &&
          redirectConstructor !== wrapper &&
          typeof redirectConstructor.__nativeApiEnsureClass === 'function') {
        var redirectedWrapper = redirectConstructor.__nativeApiEnsureClass();
        if (redirectedWrapper &&
            redirectedWrapper !== constructable &&
            redirectedWrapper !== wrapper &&
            typeof redirectedWrapper === 'function') {
          return rememberClassOnInstance(
            redirectedWrapper.call(this, ...args),
            redirectConstructor
          );
        }
      }
      if (args.length > 0 ||
          shouldUseAllocInitConstructor(constructable, wrapper)) {
        return rememberInstanceClass(constructNativeInstance(
          nativeClass,
          args,
          rememberInstanceClass,
          shouldUseAllocInitConstructor(constructable, wrapper)
        ));
      }
      if (typeof nativeClass.new !== 'function') {
        throw new Error('Native class cannot be initialized');
      }
      return rememberInstanceClass(nativeClass.new());
	    };
		    function rememberInstanceClass(instance) {
		      return rememberClassOnInstance(instance, wrapper || constructable);
		    }
		    // Static allocators may be invoked with a TypeScript-derived class as
		    // the receiver (core does `_super.new.call(this)`); those must
		    // materialize and allocate the derived Objective-C class, not the base.
		    function derivedClassWrapper(target) {
		      if (target && target !== constructable && target !== wrapper &&
		          typeof target.__nativeApiEnsureClass === 'function') {
		        var derived = target.__nativeApiEnsureClass();
		        if (derived && derived !== constructable && derived !== wrapper) {
		          return derived;
		        }
		      }
		      return undefined;
		    }
	    try {
	      Object.defineProperty(constructable, 'name', {
	        configurable: true,
	        enumerable: false,
	        value: nativeClassName || nativeClass.name || 'NativeScriptNativeClass'
	      });
	    } catch (_) {
	    }
	    try {
	      Object.defineProperty(constructable, 'extend', {
        configurable: true,
        enumerable: false,
        writable: false,
        value: function(methods, options) {
          if (methods == null || typeof methods !== 'object') {
            throw new Error('extend() first parameter must be an object');
          }
          var extensionMethods = nativeExtensionMethodsWithIndexedCollectionAliases(methods);
          var extendOptions =
            nativeExtensionOptionsWithIterator(options, extensionMethods);
          var extendedNativeClass = api.__extendClass(nativeClass, extensionMethods, extendOptions);
          var extended = wrapNativeClass(extendedNativeClass);
          try {
            Object.defineProperty(extended, '__nativeApiUseAllocInitConstructor', {
              configurable: false,
              enumerable: false,
              writable: false,
              value: true
            });
          } catch (_) {
          }
          try {
            Object.setPrototypeOf(extended, wrapper || constructable);
          } catch (_) {
          }
          var extendedPrototype = Object.create(constructable.prototype || null);
          try {
            Object.defineProperties(extendedPrototype, Object.getOwnPropertyDescriptors(extensionMethods));
          } catch (_) {
            Object.keys(extensionMethods).forEach(function(key) {
              extendedPrototype[key] = extensionMethods[key];
            });
          }
          try {
            Object.defineProperty(extendedPrototype, 'constructor', {
              configurable: true,
              enumerable: false,
              writable: true,
              value: extended
            });
          } catch (_) {
          }
          installInstanceClassIdentity(extendedPrototype, extended, constructable);
          extended.prototype = extendedPrototype;
          try {
            api.__rememberClassWrapper(extendedNativeClass, extended, extendedPrototype);
          } catch (_) {
          }
          return extended;
        }
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(constructable, 'alloc', {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function() {
          if (arguments.length !== 0) {
            throw new Error('alloc does not take arguments; use invoke for an explicit Objective-C selector.');
          }
          var derived = derivedClassWrapper(this);
          if (derived && typeof derived.alloc === 'function') {
            return rememberClassOnInstance(derived.alloc.apply(derived, arguments), this);
          }
          return rememberInstanceClass(nativeClass.alloc.apply(nativeClass, arguments));
        }
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(constructable, 'new', {
        configurable: true,
        enumerable: false,
        writable: false,
        value: function() {
          if (arguments.length !== 0) {
            throw new Error('new does not take arguments; use invoke for an explicit Objective-C selector.');
          }
          var derived = derivedClassWrapper(this);
          if (derived && typeof derived.new === 'function') {
            return rememberClassOnInstance(derived.new(), this);
          }
          if (typeof nativeClass.new !== 'function') {
            throw new Error('Native class cannot be initialized');
          }
          return rememberInstanceClass(nativeClass.new());
        }
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(constructable, 'caller', {
        configurable: true,
        enumerable: false,
        writable: false,
        value: null
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(constructable, 'arguments', {
        configurable: true,
        enumerable: false,
        writable: false,
        value: null
      });
    } catch (_) {
    }
    var basePrototypeTarget = {};
    var classMembersInstalled = false;
    function selectorArgumentCount(selectorName) {
      var count = 0;
      if (typeof selectorName !== 'string') {
        return count;
      }
      for (var i = 0; i < selectorName.length; i++) {
        if (selectorName.charCodeAt(i) === 58) {
          count++;
        }
      }
      return count;
    }
    function selectorDescriptor(member, selectorName, signatureOffset, argumentCount, runtimeOnly) {
      return {
        name: member.name || '',
        selectorName: selectorName || '',
        setterSelectorName: member.setterSelectorName || '',
        signatureOffset: typeof signatureOffset === 'number' ? signatureOffset : 0,
        setterSignatureOffset: typeof member.setterSignatureOffset === 'number'
          ? member.setterSignatureOffset
          : 0,
        flags: typeof member.flags === 'number' ? member.flags : 0,
        property: !!member.property,
        readonly: !!member.readonly,
        argumentCount: typeof argumentCount === 'number'
          ? argumentCount
          : selectorArgumentCount(selectorName),
        runtimeOnly: !!runtimeOnly
      };
    }
    function addSelectorGroups(groups, members, runtimeOnly) {
      if (!groups || !members || typeof members.length !== 'number') {
        return;
      }
      for (var i = 0; i < members.length; i++) {
        var member = members[i];
        if (!member || member.property || !member.name || !member.selectorName) {
          continue;
        }
        // Skip methods that need special interceptor handling with kNonMasking.
        if (member.name === 'superclass' || member.name === 'class' ||
            member.name === 'constructor' || member.name === 'className') {
          continue;
        }
        var argumentCount = typeof member.argumentCount === 'number'
          ? member.argumentCount
          : 0;
        var group = groups[member.name];
        if (!group) {
          group = [];
          groups[member.name] = group;
        }
        if (group[argumentCount] === undefined) {
          group[argumentCount] = selectorDescriptor(
            member,
            member.selectorName,
            member.signatureOffset,
            argumentCount,
            runtimeOnly
          );
        }
        // Methods with a trailing NSError** out-parameter (selector ending in
        // "error:") may be called with the error argument omitted, so register
        // the error-omitted arity too.
        if (argumentCount > 0 &&
            /error:$/.test(member.selectorName) &&
            group[argumentCount - 1] === undefined) {
          group[argumentCount - 1] = selectorDescriptor(
            member,
            member.selectorName,
            member.signatureOffset,
            argumentCount - 1,
            runtimeOnly
          );
        }
      }
    }
    function installSelectorGroups(target, groups, receiverIsClass) {
      if (!target || !groups) {
        return;
      }
      for (var name in groups) {
        if (!Object.prototype.hasOwnProperty.call(groups, name) ||
            Object.prototype.hasOwnProperty.call(target, name)) {
          continue;
        }
        var selectors = groups[name];
        if (!selectors || !selectors.length) {
          continue;
        }
        var hasMetadataSelector = false;
        for (var selectorIndex = 0; selectorIndex < selectors.length; selectorIndex++) {
          if (selectors[selectorIndex] && !selectors[selectorIndex].runtimeOnly) {
            hasMetadataSelector = true;
            break;
          }
        }
        if (!hasMetadataSelector && receiverIsClass && name in target) {
          continue;
        }
        var selectorFunction =
            api.__makeSelectorGroupFunction(nativeClass, !!receiverIsClass, selectors);
          Object.defineProperty(target, name, {
            configurable: true,
            enumerable: false,
            writable: true,
            value: receiverIsClass
              ? (function(fn, memberName) {
                  return function() {
                    if (this && typeof this === 'object' && this.kind === 'object') {
                      var baseArgs = [nativeClass, this, memberName];
                      for (var baseArgIndex = 0; baseArgIndex < arguments.length; baseArgIndex++) {
                        baseArgs.push(arguments[baseArgIndex]);
                      }
                      return api.__invokeBase(...baseArgs);
                    }
                    var args = [];
                    for (var argIndex = 0; argIndex < arguments.length; argIndex++) {
                      args.push(arguments[argIndex]);
                    }
                    return rememberInstanceClass(fn(...args));
                  };
                })(selectorFunction, name)
              : selectorFunction
          });
      }
    }
    function installClassMembers(target, members, receiverIsClass, runtimeMembers) {
      var hasMetadataMembers = members && typeof members.length === 'number';
      var hasRuntimeMembers = runtimeMembers && typeof runtimeMembers.length === 'number';
      if (!target || (!hasMetadataMembers && !hasRuntimeMembers)) {
        return;
      }
      var selectorGroups = Object.create(null);
      addSelectorGroups(selectorGroups, members, false);
      for (var i = 0; hasMetadataMembers && i < members.length; i++) {
        var member = members[i];
        if (!member || !member.name) {
          continue;
        }
        if (member.property) {
            // Skip properties that need special interceptor handling (they
            // return wrapped class constructors, not raw native values).
            if (member.name === 'superclass' || member.name === 'class' ||
                member.name === 'constructor' || member.name === 'debugDescription' ||
                member.name === 'className') {
              continue;
            }
            var existingDescriptor = Object.getOwnPropertyDescriptor(target, member.name);
            if (existingDescriptor &&
                (typeof existingDescriptor.get === 'function' ||
                 typeof existingDescriptor.set === 'function')) {
              continue;
            }
            var getterFunction = member.selectorName
              ? api.__makeSelectorGroupFunction(
                  nativeClass,
                  !!receiverIsClass,
                  [selectorDescriptor(member, member.selectorName, member.signatureOffset, 0)]
                )
              : undefined;
            var setterFunction = !member.readonly && member.setterSelectorName
              ? api.__makeSelectorGroupFunction(
                  nativeClass,
                  !!receiverIsClass,
                  [
                    null,
                    selectorDescriptor(
                      member,
                      member.setterSelectorName,
                      member.setterSignatureOffset,
                      1
                    )
                  ]
                )
              : undefined;
            var descriptor = {
              configurable: true,
              enumerable: false,
              get: receiverIsClass
                ? (function(name, selectorName) {
                    return function() {
                      return selectorName
                        ? nativeClass.invoke(selectorName)
                        : nativeClass[name];
                    };
                  })(member.name, member.selectorName)
                : (getterFunction || (function(name) {
                    return function() {
                      return api.__invokeBase(nativeClass, this, name);
                    };
                  })(member.name))
            };
            if (!member.readonly) {
              descriptor.set = receiverIsClass
                ? (function(name, setterSelectorName) {
                    return function(value) {
                      if (setterSelectorName) {
                        return nativeClass.invoke(setterSelectorName, value);
                      }
                      nativeClass[name] = value;
                    };
                  })(member.name, member.setterSelectorName)
                : (setterFunction || (function(name) {
                    return function(value) {
                      return api.__invokeBase(nativeClass, this, name, value);
                    };
                  })(member.name));
            }
            Object.defineProperty(target, member.name, descriptor);
        } else {
          continue;
        }
      }
      installSelectorGroups(target, selectorGroups, receiverIsClass);
    }
    function installNativeClassMembersIfNeeded() {
      if (classMembersInstalled) {
        return;
      }
      classMembersInstalled = true;
      installClassMembers(
        constructable,
        nativeClass.__staticMembers,
        true,
        nativeClass.__runtimeStaticMembers
      );
      installClassMembers(
        basePrototypeTarget,
        nativeClass.__instanceMembers,
        false,
        nativeClass.__runtimeInstanceMembers
      );
      try {
        delete constructable.__nativeApiInstallMembers;
      } catch (_) {
      }
    }
    try {
      Object.defineProperty(constructable, '__nativeApiInstallMembers', {
        configurable: true,
        enumerable: false,
        writable: false,
        value: installNativeClassMembersIfNeeded
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(basePrototypeTarget, 'constructor', {
        configurable: true,
        enumerable: false,
        writable: true,
        value: constructable
      });
    } catch (_) {
    }
    try {
      Object.defineProperty(basePrototypeTarget, 'toString', {
        configurable: true,
        enumerable: false,
        writable: true,
        value: function() {
          return '[object NativeScriptObject]';
        }
      });
    } catch (_) {
    }
    try {
      if (typeof Symbol === 'function' && Symbol.iterator &&
          typeof api.__fastEnumeration === 'function') {
        Object.defineProperty(basePrototypeTarget, Symbol.iterator, {
          configurable: true,
          enumerable: false,
          writable: true,
          value: function() {
            return api.__fastEnumeration(this);
          }
        });
      }
    } catch (_) {
    }
    constructable.prototype = basePrototypeTarget;
    try {
      Object.defineProperty(constructable, Symbol.hasInstance, {
        configurable: true,
        enumerable: false,
        value: function(value) {
          if (!value || typeof value !== 'object') {
            return false;
          }
          // `this` is the constructor instanceof was invoked on. A
          // TypeScript-derived native class inherits this method through the
          // wrapper prototype chain until it materializes, so membership must
          // be answered for the DERIVED Objective-C class — and before
          // materialization no instance of it can exist.
          try {
            if (this && this !== constructable && this !== wrapper &&
                this.__nativeApiTypeScriptState) {
              var derivedWrapper = this.__nativeApiTypeScriptState.wrapper;
              if (!derivedWrapper) {
                return false;
              }
              if (derivedWrapper !== constructable && derivedWrapper !== wrapper) {
                return derivedWrapper[Symbol.hasInstance](value);
              }
            }
          } catch (_) {
          }
          var expectedName = nativeClass.runtimeName || nativeClass.name;
          try {
            // Pass the proxied wrapper: the raw constructable carries no
            // __nativeApiClass, so it does not marshal to the Objective-C
            // Class and isKindOfClass() misreports.
            if (typeof value.isKindOfClass === 'function' &&
                value.isKindOfClass(wrapper || constructable) === true) {
              return true;
            }
          } catch (_) {
          }
          try {
            var current = typeof value.class === 'function' ? value.class() : null;
            while (current) {
              if (current === wrapper || current === constructable) {
                return true;
              }
              var currentName = current.runtimeName || current.name;
              if (typeof expectedName === 'string' && currentName === expectedName) {
                return true;
              }
              var next = current.superclass || null;
              if (typeof next === 'function' && next.kind !== 'class') {
                next = next.call(current);
              }
              current = next || null;
            }
          } catch (_) {
          }
          return typeof expectedName === 'string' && value.className === expectedName;
        }
      });
    } catch (_) {
    }
    var cachedNativeFunctions = typeof Map === 'function' ? new Map() : null;
	    var wrapper = typeof Proxy === 'function'
	      ? new Proxy(constructable, {
          get: function(target, property, receiver) {
            if (property === '__nativeApiClass') {
              return nativeClass;
            }
            if (property === 'toString') {
              return function() {
                return String(nativeClass);
              };
            }
            if (property === 'prototype') {
              installNativeClassMembersIfNeeded();
              return Reflect.get(target, property, receiver);
            }
            if (property === 'hasOwnProperty') {
              return function(key) {
                installNativeClassMembersIfNeeded();
                return Object.prototype.hasOwnProperty.call(target, key);
              };
            }
            if (Object.prototype.hasOwnProperty.call(target, property) ||
                property === 'prototype' ||
                property === 'length' ||
                property === 'name') {
              return Reflect.get(target, property, receiver);
            }
            installNativeClassMembersIfNeeded();
            if (Object.prototype.hasOwnProperty.call(target, property)) {
              return Reflect.get(target, property, receiver);
            }
            if (cachedNativeFunctions && cachedNativeFunctions.has(property)) {
              return cachedNativeFunctions.get(property);
            }
            var nativeValue = nativeClass[property];
            if (nativeValue !== undefined) {
              if (typeof nativeValue === 'function') {
                if (cachedNativeFunctions) {
                  cachedNativeFunctions.set(property, nativeValue);
                }
                try {
                  Object.defineProperty(target, property, {
                    configurable: true,
                    enumerable: false,
                    writable: false,
                    value: nativeValue
                  });
                } catch (_) {
                }
              }
              return nativeValue;
            }
            var reflected = Reflect.get(target, property, receiver);
            if (reflected !== undefined || property in target) {
              return reflected;
            }
            installNativeClassMembersIfNeeded();
            reflected = Reflect.get(target, property, receiver);
            if (reflected !== undefined || property in target) {
              return reflected;
            }
            return reflected;
          },
          set: function(target, property, value, receiver) {
            if (property === 'prototype') {
              target[property] = value;
              return true;
            }
            installNativeClassMembersIfNeeded();
            if (setDescriptorValue(target, property, receiver, value)) {
              return true;
            }
            if (setInheritedNativeClassValue(target, property, value)) {
              return true;
            }
            try {
              nativeClass[property] = value;
              return true;
            } catch (_) {
            }
            if (receiver && receiver !== target) {
              Object.defineProperty(receiver, property, {
                configurable: true,
                enumerable: true,
                writable: true,
                value: value
              });
              return true;
            }
            return Reflect.set(target, property, value, receiver);
          },
          has: function(target, property) {
            installNativeClassMembersIfNeeded();
            return property in target || property in nativeClass;
          },
          ownKeys: function(target) {
            installNativeClassMembersIfNeeded();
            return Reflect.ownKeys(target).filter(function(key) {
              return key !== 'new' &&
                key !== 'hasOwnProperty' &&
                key !== '__nativeApiInstallMembers';
            });
          },
          getOwnPropertyDescriptor: function(target, property) {
            installNativeClassMembersIfNeeded();
            return Reflect.getOwnPropertyDescriptor(target, property);
          }
	        })
	      : constructable;
	    if (classWrappers) {
	      classWrappers.set(nativeClass, wrapper);
	    }
    try {
      var nativeSuperclass = nativeClass.__superclass;
      if (nativeSuperclass && nativeSuperclass !== nativeClass) {
        var superclassWrapper = wrapNativeClass(nativeSuperclass);
        if (superclassWrapper && superclassWrapper !== wrapper &&
            typeof Object.setPrototypeOf === 'function') {
          Object.setPrototypeOf(wrapper, superclassWrapper);
          if (superclassWrapper.prototype) {
            Object.setPrototypeOf(constructable.prototype, superclassWrapper.prototype);
          }
        }
      }
    } catch (_) {
    }
    try {
      api.__rememberClassWrapper(nativeClass, wrapper, constructable.prototype);
    } catch (_) {
    }
    if (nativeClassName) {
      classWrappersByName[nativeClassName] = wrapper;
      cacheGlobal(nativeClassName, wrapper);
      if (!Object.prototype.hasOwnProperty.call(globalThis, nativeClassName)) {
        try {
          Object.defineProperty(globalThis, nativeClassName, {
            configurable: true,
            enumerable: false,
            writable: false,
            value: wrapper
          });
        } catch (_) {
        }
      }
    }
    if (nativeClass.name && nativeClass.name !== nativeClassName) {
      classWrappersByName[nativeClass.name] = wrapper;
      cacheGlobal(nativeClass.name, wrapper);
    }
	    return wrapper;
	  }

	  function rememberClassOnInstance(instance, classWrapper) {
	    if (instance && typeof instance === 'object' && classWrapper) {
	      try {
	        if (typeof classWrapper.__nativeApiInstallMembers === 'function') {
	          classWrapper.__nativeApiInstallMembers();
	        }
	        if (typeof api.__rememberObjectClassWrapper === 'function') {
	          api.__rememberObjectClassWrapper(instance, classWrapper);
	        } else {
	          instance.__nativeApiClassWrapper = classWrapper;
	        }
	      } catch (_) {
	      }
	    }
	    return instance;
	  }

	  function isNativeClassLike(value) {
	    if (!value || (typeof value !== 'object' && typeof value !== 'function')) {
	      return false;
	    }
	    if (value.kind === 'class') {
	      return true;
	    }
	    try {
	      return !!value.__nativeApiClass;
	    } catch (_) {
	      return false;
	    }
	  }

	  function nativeClassLikeHandle(value) {
	    if (!value || (typeof value !== 'object' && typeof value !== 'function')) {
	      return value;
	    }
	    try {
	      if (typeof value.__nativeApiEnsureClass === 'function') {
	        value = value.__nativeApiEnsureClass();
	      }
	    } catch (_) {
	    }
	    try {
	      return value.__nativeApiClass || value;
	    } catch (_) {
	      return value;
	    }
	  }

	  function materializeTypeScriptNativeClass(constructor) {
	    if (!constructor || typeof constructor !== 'function') {
	      return undefined;
	    }
	    var state = constructor.__nativeApiTypeScriptState;
	    if (!state) {
	      return undefined;
	    }
	    if (state.wrapper) {
	      return state.wrapper;
	    }
	    if (state.materializing) {
	      return state.base;
	    }

	    state.materializing = true;
	    try {
	      var baseWrapper = state.base;
	      if (baseWrapper && typeof baseWrapper.__nativeApiEnsureClass === 'function') {
	        baseWrapper = baseWrapper.__nativeApiEnsureClass();
	      }

	      var options = {};
	      var className = constructor.ObjCClassName || constructor.name;
	      if (className) {
	        options.name = className;
	      }
	      if (constructor.ObjCProtocols) {
	        options.protocols = constructor.ObjCProtocols;
	      }
	      if (constructor.ObjCExposedMethods) {
	        options.exposedMethods = constructor.ObjCExposedMethods;
	      }

	      var nativeBase = nativeClassLikeHandle(baseWrapper);
	      var extensionMethods =
	        nativeExtensionMethodsWithIndexedCollectionAliases(constructor.prototype || {});
	      options = nativeExtensionOptionsWithIterator(options, extensionMethods);
	      var nativeClass = api.__extendClass(nativeBase, extensionMethods, options);
	      var wrapper = wrapNativeClass(nativeClass);
	      state.wrapper = wrapper;
	      try {
	        Object.defineProperty(wrapper, '__nativeApiUseAllocInitConstructor', {
	          configurable: false,
	          enumerable: false,
	          writable: false,
	          value: true
	        });
	      } catch (_) {
	      }

	      try {
	        Object.setPrototypeOf(constructor, wrapper);
	      } catch (_) {
	      }
	      try {
	        api.__rememberClassWrapper(nativeClass, constructor, extensionMethods);
	      } catch (_) {
	      }
	      return wrapper;
	    } finally {
	      state.materializing = false;
	    }
	  }

	  function defineTypeScriptStaticForwarder(constructor, name, isProperty, readonly) {
	    if (!name || name === 'length' || name === 'name' || name === 'prototype' ||
	        Object.prototype.hasOwnProperty.call(constructor, name)) {
	      return;
	    }

	    var descriptor = {
	      configurable: true,
	      enumerable: false
	    };

	    if (isProperty) {
	      descriptor.get = function() {
	        var wrapper = materializeTypeScriptNativeClass(constructor);
	        return wrapper ? wrapper[name] : undefined;
	      };
	      if (!readonly) {
	        descriptor.set = function(value) {
	          var wrapper = materializeTypeScriptNativeClass(constructor);
	          if (wrapper) {
	            wrapper[name] = value;
	          }
	        };
	      }
	    } else {
	      descriptor.writable = true;
	      descriptor.value = function() {
	        if (name === 'class') {
	          materializeTypeScriptNativeClass(constructor);
	          return constructor;
	        }
	        if (name === 'superclass') {
	          var state = constructor.__nativeApiTypeScriptState;
	          return state && state.base;
	        }
	        var wrapper = materializeTypeScriptNativeClass(constructor);
	        var member = wrapper && wrapper[name];
	        if (typeof member !== 'function') {
	          throw new TypeError(String(name) + ' is not a function');
	        }
	        var memberArgs = [];
	        for (var memberArgIndex = 0; memberArgIndex < arguments.length; memberArgIndex++) {
	          memberArgs.push(arguments[memberArgIndex]);
	        }
	        var result = wrapper[name](...memberArgs);
	        if (name === 'alloc' || name === 'new' || name === 'construct') {
	          return rememberClassOnInstance(result, constructor);
	        }
	        return result;
	      };
	    }

	    try {
	      Object.defineProperty(constructor, name, descriptor);
	    } catch (_) {
	    }
	  }

	  function installTypeScriptNativeClassSupport(constructor, base) {
	    if (!constructor || typeof constructor !== 'function' || !isNativeClassLike(base)) {
	      return false;
	    }
	    if (constructor.__nativeApiTypeScriptState) {
	      return true;
	    }

	    try {
	      Object.defineProperty(constructor, '__nativeApiTypeScriptState', {
	        configurable: false,
	        enumerable: false,
	        writable: false,
	        value: {
	          base: base,
	          wrapper: null,
	          materializing: false
	        }
	      });
	    } catch (_) {
	      constructor.__nativeApiTypeScriptState = {
	        base: base,
	        wrapper: null,
	        materializing: false
	      };
	    }

	    try {
	      Object.defineProperty(constructor, '__nativeApiEnsureClass', {
	        configurable: false,
	        enumerable: false,
	        writable: false,
	        value: function() {
	          return materializeTypeScriptNativeClass(constructor);
	        }
	      });
	    } catch (_) {
	    }

	    try {
	      Object.defineProperty(constructor, '__nativeApiClass', {
	        configurable: true,
	        enumerable: false,
	        get: function() {
	          var wrapper = materializeTypeScriptNativeClass(constructor);
	          return wrapper && wrapper.__nativeApiClass;
	        }
	      });
	    } catch (_) {
	    }

	    installInstanceClassIdentity(constructor.prototype || {}, constructor, null);

	    ['alloc', 'new', 'class', 'superclass', 'extend'].forEach(function(name) {
	      defineTypeScriptStaticForwarder(constructor, name, false, false);
	    });

	    try {
	      var members = base.__staticMembers || [];
	      for (var i = 0; i < members.length; i++) {
	        var member = members[i];
	        if (member && member.name) {
	          defineTypeScriptStaticForwarder(
	            constructor,
	            member.name,
	            !!member.property,
	            !!member.readonly
	          );
	        }
	      }
	    } catch (_) {
	    }

	    return true;
	  }

	  function installTypeScriptNativeHelpers() {
	    var extendStatics = Object.setPrototypeOf ||
	      ({ __proto__: [] } instanceof Array && function(d, b) { d.__proto__ = b; }) ||
	      function(d, b) {
	        for (var p in b) {
	          if (Object.prototype.hasOwnProperty.call(b, p)) {
	            d[p] = b[p];
	          }
	        }
	      };

	    globalThis.__extends = function(d, b) {
	      if (typeof b !== 'function' && b !== null) {
	        throw new TypeError('Class extends value ' + String(b) + ' is not a constructor or null');
	      }
	      extendStatics(d, b);
	      function __() { this.constructor = d; }
	      d.prototype = b === null ? Object.create(b) : (__.prototype = b.prototype, new __());
	      if (b !== null) {
	        installTypeScriptNativeClassSupport(d, b);
	      }
	    };

	    globalThis.NativeClass = function NativeClass(constructor) {
	      if (constructor && typeof constructor.__nativeApiEnsureClass === 'function') {
	        constructor.__nativeApiEnsureClass();
	      }
	      return constructor;
	    };

	    globalThis.ObjCClass = function ObjCClass() {
	      var protocols = Array.prototype.slice.call(arguments);
	      return function(constructor) {
	        if (constructor.ObjCProtocols) {
	          for (var protocolIndex = 0; protocolIndex < protocols.length; protocolIndex++) {
	            constructor.ObjCProtocols.push(protocols[protocolIndex]);
	          }
	        } else {
	          constructor.ObjCProtocols = protocols;
	        }
	        if (typeof constructor.__nativeApiEnsureClass === 'function') {
	          constructor.__nativeApiEnsureClass();
	        }
	        return constructor;
	      };
	    };
	  }

	  function wrapInteropFactory(nativeFactory, properties) {
	    if (typeof nativeFactory !== 'function' || nativeFactory.__nativeScriptConstructable) {
	      return nativeFactory;
	    }
    var constructable = function NativeScriptInteropValue() {
      var factoryArgs = [];
      for (var factoryArgIndex = 0; factoryArgIndex < arguments.length; factoryArgIndex++) {
        factoryArgs.push(arguments[factoryArgIndex]);
      }
      return nativeFactory(...factoryArgs);
    };
    try {
      if (nativeFactory.prototype) {
        constructable.prototype = nativeFactory.prototype;
      }
    } catch (_) {
    }
    try {
      Object.defineProperty(constructable, Symbol.hasInstance, {
        configurable: true,
        enumerable: false,
        value: function(value) {
          return !!value && typeof value === 'object' && value.kind === properties.kind;
        }
      });
    } catch (_) {
    }
    Object.keys(properties).forEach(function(key) {
      try {
        Object.defineProperty(constructable, key, {
          configurable: true,
          enumerable: false,
          writable: false,
          value: properties[key]
        });
      } catch (_) {
      }
    });
    Object.defineProperty(constructable, '__nativeScriptConstructable', {
      configurable: false,
      enumerable: false,
      writable: false,
      value: true
    });
    return constructable;
  }

  function installInteropConstructors() {
    var interop = globalThis.interop;
    if (!interop || typeof interop !== 'object') {
      return;
    }
    var pointerSize;
    try {
      if (typeof interop.sizeof === 'function' && interop.types && interop.types.pointer !== undefined) {
        pointerSize = interop.sizeof(interop.types.pointer);
      }
    } catch (_) {
      pointerSize = undefined;
    }
    interop.Pointer = wrapInteropFactory(interop.Pointer, { kind: 'pointer', sizeof: pointerSize });
    interop.Reference = wrapInteropFactory(interop.Reference, { kind: 'reference', sizeof: pointerSize });
    interop.Block = wrapInteropFactory(interop.Block, { kind: 'block', sizeof: pointerSize });
    interop.FunctionReference = wrapInteropFactory(
      interop.FunctionReference,
      { kind: 'functionReference', sizeof: pointerSize }
    );
    if (interop.types && typeof interop.types === 'object') {
      Object.keys(interop.types).forEach(function(name) {
        var value = interop.types[name];
        if (typeof value !== 'number') {
          return;
        }
        var boxed = {
          valueOf: function() { return value; },
          toString: function() { return String(value); }
        };
        Object.defineProperty(boxed, typeCodeKey, {
          configurable: false,
          enumerable: false,
          writable: false,
          value: value
        });
        interop.types[name] = boxed;
      });
    }
  }

  function installObjcHelpers() {
    var objc = globalThis.objc;
    if (!objc || (typeof objc !== 'object' && typeof objc !== 'function')) {
      objc = {};
      Object.defineProperty(globalThis, 'objc', {
        configurable: true,
        enumerable: true,
        writable: true,
        value: objc
      });
    }
    if (typeof objc.autoreleasepool !== 'function') {
      objc.autoreleasepool = api.autoreleasepool;
    }
  }

  function defineInlineFunction(name, value) {
    if (Object.prototype.hasOwnProperty.call(globalThis, name)) {
      return;
    }
    Object.defineProperty(globalThis, name, {
      configurable: true,
      enumerable: false,
      writable: true,
      value: value
    });
  }

  function installInlineFunctions() {
    var makePoint = function(x, y) { return { x: x, y: y }; };
    var makeSize = function(width, height) { return { width: width, height: height }; };
    var makeRect = function(x, y, width, height) {
      return { origin: { x: x, y: y }, size: { width: width, height: height } };
    };
    defineInlineFunction('CGPointMake', makePoint);
    defineInlineFunction('NSMakePoint', makePoint);
    defineInlineFunction('CGSizeMake', makeSize);
    defineInlineFunction('NSMakeSize', makeSize);
    defineInlineFunction('CGRectMake', makeRect);
    defineInlineFunction('NSMakeRect', makeRect);
    defineInlineFunction('NSMakeRange', function(location, length) {
      return { location: location, length: length };
    });
    defineInlineFunction('UIEdgeInsetsMake', function(top, left, bottom, right) {
      return { top: top, left: left, bottom: bottom, right: right };
    });
  }

  function names(kind) {
    var metadata = api.metadata;
    var fn = metadata && metadata[kind];
    return typeof fn === 'function' ? fn() : [];
  }

  function nameSet(values) {
    var result = Object.create(null);
    (values || []).forEach(function(value) {
      result[value] = true;
    });
    return result;
  }

  var classNameList = names('classNames');
  var functionNameList = names('functionNames');
  var constantNameList = names('constantNames');
  var protocolNameList = names('protocolNames');
  var enumNameList = names('enumNames');
  var functionNameSet = nameSet(functionNameList);
  var constantNameSet = nameSet(constantNameList);
  var classNameSet = nameSet(classNameList);
  var protocolNameSet = nameSet(protocolNameList);
  var enumNameSet = nameSet(enumNameList);

  function resolveNativeApiEnum(enumName) {
    return (api.getEnum && api.getEnum(enumName)) || api[enumName];
  }

  Object.defineProperty(globalThis, '__nativeScriptResolveNativeApiLazyGlobal', {
    configurable: false,
    enumerable: false,
    writable: false,
    value: function(name, kind) {
      var value;
      if (kind === 'class') {
        value = wrapNativeClass(api[name]);
      } else if (kind === 'function' || kind === 'constant') {
        value = api[name];
      } else if (kind === 'protocol') {
        value = (api.getProtocol && api.getProtocol(name)) || api[name];
      } else if (kind === 'enum') {
        value = resolveNativeApiEnum(name);
      } else if (kind === 'struct') {
        value = wrapAggregateConstructor((api.getStruct && api.getStruct(name)) || api[name]);
      } else if (kind === 'union') {
        value = wrapAggregateConstructor((api.getUnion && api.getUnion(name)) || api[name]);
      } else if (kind && kind.indexOf('enumMember:') === 0) {
        var enumValue = resolveNativeApiEnum(kind.slice('enumMember:'.length));
        value = enumValue && enumValue[name];
      } else {
        value = api[name];
      }
      cacheGlobal(name, value);
      return value;
    }
  });

  classNameList.forEach(function(name) {
    defineLazyGlobal(name, function(className) {
      return wrapNativeClass(api[className]);
    }, false, 'class');
  });
  functionNameList.forEach(function(name) {
    defineLazyGlobal(name, function(functionName) {
      return api[functionName];
    }, false, 'function');
  });
  constantNameList.forEach(function(name) {
    defineLazyGlobal(name, function(constantName) {
      return api[constantName];
    }, false, 'constant');
  });
  protocolNameList.forEach(function(name) {
    defineLazyGlobal(name, function(protocolName) {
      return (api.getProtocol && api.getProtocol(protocolName)) || api[protocolName];
    }, false, 'protocol');
  });
  enumNameList.forEach(function(name) {
    defineLazyGlobal(name, resolveNativeApiEnum, false, 'enum');
    var enumValue = resolveNativeApiEnum(name);
    if (!enumValue || typeof enumValue !== 'object') {
      return;
    }
    Object.keys(enumValue).forEach(function(memberName) {
      if (/^-?\d+$/.test(memberName)) {
        return;
      }
      defineLazyGlobal(memberName, function() {
        return enumValue[memberName];
      }, false, 'enumMember:' + name);
    });
  });
  names('structNames').forEach(function(name) {
    var conflictsWithValue =
      !!functionNameSet[name] || !!constantNameSet[name] || !!classNameSet[name] ||
      !!protocolNameSet[name] || !!enumNameSet[name];
    defineLazyGlobal(name, function(structName) {
      return wrapAggregateConstructor((api.getStruct && api.getStruct(structName)) || api[structName]);
    }, !conflictsWithValue, 'struct');
  });
  names('unionNames').forEach(function(name) {
    var conflictsWithValue =
      !!functionNameSet[name] || !!constantNameSet[name] || !!classNameSet[name] ||
      !!protocolNameSet[name] || !!enumNameSet[name];
    defineLazyGlobal(name, function(unionName) {
      return wrapAggregateConstructor((api.getUnion && api.getUnion(unionName)) || api[unionName]);
    }, !conflictsWithValue, 'union');
  });

  if (typeof globalThis.UIColor === 'undefined' &&
      typeof globalThis.NSColor !== 'undefined') {
    globalThis.UIColor = globalThis.NSColor;
    cacheGlobal('UIColor', globalThis.UIColor);
  }
  var colorCtor = globalThis.UIColor || globalThis.NSColor;
  if (colorCtor && colorCtor.prototype &&
      typeof colorCtor.prototype.initWithRedGreenBlueAlpha !== 'function') {
    colorCtor.prototype.initWithRedGreenBlueAlpha = function(red, green, blue, alpha) {
      if (typeof this.initWithSRGBRedGreenBlueAlpha === 'function') {
        return this.initWithSRGBRedGreenBlueAlpha(red, green, blue, alpha);
      }
      if (typeof this.initWithCalibratedRedGreenBlueAlpha === 'function') {
        return this.initWithCalibratedRedGreenBlueAlpha(red, green, blue, alpha);
      }
      if (typeof colorCtor.colorWithSRGBRedGreenBlueAlpha === 'function') {
        return colorCtor.colorWithSRGBRedGreenBlueAlpha(red, green, blue, alpha);
      }
      if (typeof colorCtor.colorWithCalibratedRedGreenBlueAlpha === 'function') {
        return colorCtor.colorWithCalibratedRedGreenBlueAlpha(red, green, blue, alpha);
      }
      return this;
    };
  }
  defineLazyGlobal('CC_SHA256', function() { return api.CC_SHA256; });

	  installInteropConstructors();
	  installObjcHelpers();
	  installTypeScriptNativeHelpers();
	  installInlineFunctions();

  try {
    Object.defineProperty(globalThis, installedFlagName, {
      configurable: false,
      enumerable: false,
      writable: false,
      value: true
    });
  } catch (_) {
  }
})
)Engine_GLOBALS";

  std::string script(GlobalInstaller);
  script += "(";
  script += jsStringLiteral(globalName);
  script += ");";
  runtime.evaluateJavaScript(std::make_shared<StringBuffer>(std::move(script)),
                             "NativeApiGlobals.js");
  NativeApiWriteSmokeStage("engine:globals:after-eval");
}

void InstallNativeApi(Runtime& runtime, const NativeApiConfig& config) {
  const char* globalName = config.globalName != nullptr && config.globalName[0] != '\0'
                               ? config.globalName
                               : "__nativeScriptNativeApi";
  NativeApiWriteSmokeStage("engine:create-api");
  Object api = CreateNativeApi(runtime, config);
  Object global = runtime.global();
  NativeApiWriteSmokeStage("engine:set-global");
  global.setProperty(runtime, globalName, api);

  NativeApiWriteSmokeStage("engine:set-interop");
  Value existingInterop = global.getProperty(runtime, "interop");
  if (existingInterop.isUndefined() || existingInterop.isNull()) {
    global.setProperty(runtime, "interop", api.getProperty(runtime, "interop"));
  }
  if (config.installGlobalSymbols) {
    NativeApiWriteSmokeStage("engine:install-globals");
    InstallNativeApiGlobalSymbols(runtime, globalName);
  } else {
    // RN doesn't install the aggregate global surface: unused, and building
    // it eagerly costs launch time.
    NativeApiWriteSmokeStage("engine:skip-globals");
  }
  NativeApiWriteSmokeStage("engine:installed");
}
