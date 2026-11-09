// Included by NativeApiJSC.mm inside the NativeScript anonymous namespace.

std::shared_ptr<Runtime> retainNativeApiRuntime(Runtime& runtime) {
  return std::make_shared<Runtime>(runtime.state());
}

void SetNativeApiObjectPrototype(Runtime& runtime, Object& object,
                                       const Object& prototype) {
  JSObjectSetPrototype(runtime.context(), object.local(runtime),
                       prototype.local(runtime));
}

