// UIAppearance proxy primitives.
//
// `[SomeView appearance]` (and the whenContainedIn:/appearanceWhenContainedIn:
// variants) hands back an opaque `_UIAppearance` proxy, not a real instance of
// the class — UIKit forwards whatever selectors it recognizes to an internal
// invocation-recording store instead of actually executing them. There is no
// public, introspectable way to ask one of these proxies "what class are you
// a proxy for" other than parsing its `-description`, which UIKit formats as
// `<Customizable class: ClassName>`. Everything below exists to (a) recover
// that class from the description once, tag it onto the wrapped object as an
// expando so we don't have to re-parse on every access, and (b) cache
// get/set values in a class-keyed expando store (keyed on the customizable
// class, not the proxy instance — UIAppearance state is effectively global
// per class/containment-chain, not per proxy object) so repeated reads see
// the value a set() just wrote instead of round-tripping back into UIKit's
// opaque recording machinery. Setters ALSO cache: an appearance proxy setter
// doesn't reliably support read-your-write, so we do it ourselves.

// Forward declaration: runtimeWritablePropertySetter is defined later in
// Object.mm (this file is included before it), but is needed here by
// makeAppearanceProxyPropertySetter's non-metadata-setter fallback.
std::optional<std::string> runtimeWritablePropertySetter(
    id object, const std::string& property);

constexpr const char* kNativeApiAppearanceClassNameExpando =
    "__nativeApiAppearanceClassName";

// Parses UIKit's `<Customizable class: ClassName>` description format.
Class appearanceProxyCustomizableClassFromExactDescription(id object) {
#if TARGET_OS_IPHONE
  if (object == nil) {
    return Nil;
  }

  NSString* description = [object description];
  NSString* prefix = @"<Customizable class: ";
  if (description.length <= prefix.length + 1 ||
      ![description hasPrefix:prefix] || ![description hasSuffix:@">"]) {
    return Nil;
  }

  NSRange classNameRange =
      NSMakeRange(prefix.length, description.length - prefix.length - 1);
  NSString* className = [description substringWithRange:classNameRange];
  return NSClassFromString(className);
#else
  return Nil;
#endif
}

// The customizable class an appearance proxy wraps. Appearance-family calls
// tag their result before returning it to JS, so property access only needs a
// cache lookup and never sends an arbitrary object's overridable
// `-description` method.
Class taggedAppearanceProxyClass(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id object) {
  if (object == nil || bridge == nullptr) {
    return Nil;
  }

  Value classNameValue = bridge->findObjectExpando(
      runtime, object, kNativeApiAppearanceClassNameExpando);
  if (!classNameValue.isString()) {
    return Nil;
  }

  std::string className =
      classNameValue.asString(runtime).utf8(runtime);
  return objc_lookUpClass(className.c_str());
}

std::string appearanceProxyExpandoPropertyKey(
    const std::string& property) {
  return "__nativeApiAppearance:" + property;
}

// Cached class-keyed (not proxy-instance-keyed — see file header) UIAppearance
// property value.
Value cachedAppearanceProxyPropertyValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id object, const std::string& property) {
  if (Class appearanceClass =
          taggedAppearanceProxyClass(runtime, bridge, object)) {
    return bridge->findObjectExpando(
        runtime, appearanceClass, appearanceProxyExpandoPropertyKey(property));
  }
  return Value::undefined();
}

void cacheAppearanceProxyPropertyValue(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    id object, const std::string& property, const Value& value) {
  if (Class appearanceClass =
          taggedAppearanceProxyClass(runtime, bridge, object)) {
    bridge->setObjectExpando(runtime, appearanceClass,
                             appearanceProxyExpandoPropertyKey(property),
                             value);
  }
}

// Picks the more capable of two candidate members exposing the same property
// name (prefers writable over readonly, a member with a known setter
// selector, a member with resolved signature metadata).
const NativeApiMember* betterAppearanceProxyAccessorMember(
    const NativeApiMember* current, const NativeApiMember& candidate) {
  if (current == nullptr) {
    return &candidate;
  }
  if (current->readonly != candidate.readonly) {
    return candidate.readonly ? current : &candidate;
  }
  if (current->setterSelectorName.empty() &&
      !candidate.setterSelectorName.empty()) {
    return &candidate;
  }
  if (current->signatureOffset == MD_SECTION_OFFSET_NULL &&
      candidate.signatureOffset != MD_SECTION_OFFSET_NULL) {
    return &candidate;
  }
  return current;
}

const NativeApiMember* selectAppearanceProxyPropertyMember(
    const std::vector<NativeApiMember>& members, const std::string& property) {
  const NativeApiMember* selected = nullptr;
  for (const auto& member : members) {
    if (!member.property || member.name != property) {
      continue;
    }
    bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
    if (memberIsStatic) {
      continue;
    }
    selected = betterAppearanceProxyAccessorMember(selected, member);
  }
  return selected;
}

// True for a `[SomeClass appearance...]` static-selector call — the class
// receiver + selector-name convention UIKit uses for all of the appearance
// proxy factory methods.
bool isStaticAppearanceSelector(bool receiverIsClass,
                                const std::string& selectorName) {
  return receiverIsClass && selectorName.rfind("appearance", 0) == 0;
}

// Recovers the customizable class from an appearance proxy's description and
// tags it onto the proxy as an expando (so future accesses don't need to
// re-parse the description).
Class tagStaticAppearanceNativeResult(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Class appearanceClass, id native) {
  if (bridge == nullptr || appearanceClass == Nil || native == nil) {
    return Nil;
  }
  Class customizableClass =
      appearanceProxyCustomizableClassFromExactDescription(native);
  if (customizableClass == Nil) {
    return Nil;
  }
  const char* className = class_getName(customizableClass);
  if (className == nullptr || className[0] == '\0') {
    return Nil;
  }
  bridge->setObjectExpando(runtime, native, kNativeApiAppearanceClassNameExpando,
                           makeString(runtime, className));
  return customizableClass;
}

std::shared_ptr<void> retainAppearanceProxyForAccessor(id native) {
  id retained = [native retain];
  return std::shared_ptr<void>(static_cast<void*>(retained), [](void* value) {
    [(id)value release];
  });
}

bool shouldInstallAppearanceProxyAccessor(const NativeApiMember& member) {
  if (!member.property || member.name.empty()) {
    return false;
  }
  bool memberIsStatic = (member.flags & metagen::mdMemberStatic) != 0;
  if (memberIsStatic) {
    return false;
  }
  return member.name != "superclass" && member.name != "class" &&
         member.name != "constructor" && member.name != "debugDescription" &&
         member.name != "className" && member.name != "description";
}

Function makeAppearanceProxyPropertyGetter(
    Runtime& runtime, std::shared_ptr<NativeApiBridge> bridge, id native,
    std::shared_ptr<void> retainedNative, std::string property) {
  return Function::createFromHostFunction(
      runtime, PropNameID::forAscii(runtime, property.c_str()), 0,
      [bridge = std::move(bridge), native, retainedNative = std::move(retainedNative),
       property = std::move(property)](Runtime& runtime, const Value&,
                                       const Value*, size_t) -> Value {
        return cachedAppearanceProxyPropertyValue(runtime, bridge, native,
                                                  property);
      });
}

Function makeAppearanceProxyPropertySetter(
    Runtime& runtime, std::shared_ptr<NativeApiBridge> bridge, id native,
    std::shared_ptr<void> retainedNative, NativeApiMember member) {
  std::string functionName = member.setterSelectorName.empty()
                                 ? member.name
                                 : member.setterSelectorName;
  return Function::createFromHostFunction(
      runtime, PropNameID::forAscii(runtime, functionName.c_str()), 1,
      [bridge = std::move(bridge), native, retainedNative = std::move(retainedNative),
       member = std::move(member)](Runtime& runtime, const Value&, const Value* args,
                                   size_t count) -> Value {
        if (count < 1) {
          throw JSError(runtime,
                        "UIAppearance property setter expects a value.");
        }
        Value setterArgs[] = {Value(runtime, args[0])};
        if (!member.setterSelectorName.empty()) {
          NativeApiMember setterMember = member;
          setterMember.selectorName = member.setterSelectorName;
          setterMember.signatureOffset = member.setterSignatureOffset;
          callObjCSelector(runtime, bridge, native, false,
                           setterMember.selectorName, &setterMember,
                           setterArgs, 1);
        } else if (auto setterSelectorName =
                       runtimeWritablePropertySetter(native, member.name)) {
          callObjCSelector(runtime, bridge, native, false, *setterSelectorName,
                           nullptr, setterArgs, 1);
        } else {
          throw JSError(runtime,
                        "UIAppearance property setter is unavailable.");
        }
        cacheAppearanceProxyPropertyValue(runtime, bridge, native, member.name,
                                          args[0]);
        return Value::undefined();
      });
}

// Installs get/set accessor descriptors for every writable metadata property
// of `customizableClass` onto `resultObject` (the JS wrapper for the
// appearance proxy) — this is what makes `View.appearance().tintColor = ...`
// resolve as a real property assignment instead of requiring `.invoke(...)`.
void installAppearanceProxyPropertyAccessors(
    Runtime& runtime, const std::shared_ptr<NativeApiBridge>& bridge,
    Class customizableClass, id native, Object& resultObject) {
  if (bridge == nullptr || customizableClass == Nil) {
    return;
  }
  const NativeApiSymbol* symbol =
      bridge->findClassForRuntimeClass(customizableClass);
  if (symbol == nullptr) {
    return;
  }

  Object objectConstructor =
      runtime.global().getPropertyAsObject(runtime, "Object");
  Function defineProperty =
      objectConstructor.getPropertyAsFunction(runtime, "defineProperty");
  std::shared_ptr<void> retainedNative =
      retainAppearanceProxyForAccessor(native);
  const auto& members = bridge->membersForClass(*symbol);
  std::unordered_map<std::string, const NativeApiMember*> accessors;
  for (const auto& member : members) {
    if (!shouldInstallAppearanceProxyAccessor(member)) {
      continue;
    }
    accessors[member.name] =
        betterAppearanceProxyAccessorMember(accessors[member.name], member);
  }

  for (const auto& accessor : accessors) {
    const NativeApiMember& member = *accessor.second;

    try {
      Object descriptor(runtime);
      descriptor.setProperty(runtime, "configurable", true);
      descriptor.setProperty(runtime, "enumerable", false);
      descriptor.setProperty(
          runtime, "get",
          makeAppearanceProxyPropertyGetter(runtime, bridge, native,
                                            retainedNative, member.name));
      if (!member.readonly) {
        descriptor.setProperty(
            runtime, "set",
            makeAppearanceProxyPropertySetter(runtime, bridge, native,
                                              retainedNative, member));
      }
      defineProperty.call(runtime, resultObject, makeString(runtime, member.name),
                          descriptor);
    } catch (const std::exception&) {
    }
  }
}
