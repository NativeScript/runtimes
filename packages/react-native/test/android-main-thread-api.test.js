const assert = require("assert");
const fs = require("fs");
const path = require("path");
const { stripTypeScriptTypes } = require("node:module");

const packageRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(packageRoot, "../..");

function read(relativePath, root = packageRoot) {
  return fs.readFileSync(path.join(root, relativePath), "utf8");
}

const index = read("src/index.ts");

function extract(name) {
  const match = new RegExp(
    `(?:export )?function ${name}(?:<[^>]*>)?\\([\\s\\S]*?\\n\\}\\n`,
  ).exec(index);
  assert(match, `index.ts should define ${name}`);
  return stripTypeScriptTypes(match[0].replace(/^export /, ""));
}

const load = (names, globals) =>
  new Function(
    "globalThis",
    `${names.map(extract).join("\n")}; return { ${names.join(", ")} };`,
  )(globals);

(async () => {
  // runOnUI on Android: the callback gets its arguments, and a throw rejects.
  {
    const calls = [];
    const globals = {
      __nativeScriptRunOnMainThread(fn) {
        calls.push("main");
        return fn();
      },
    };
    const { runOnAndroidMainThread } = load(["runOnAndroidMainThread"], globals);
    assert.strictEqual(await runOnAndroidMainThread((a, b) => a + b, [2, 3]), 5);
    await assert.rejects(
      runOnAndroidMainThread(() => {
        throw new Error("boom");
      }, []),
      /boom/,
    );
    assert.deepStrictEqual(calls, ["main", "main"]);
  }

  // A build without the native half rejects rather than running on the JS thread.
  {
    const { runOnAndroidMainThread } = load(["runOnAndroidMainThread"], {});
    let ran = false;
    await assert.rejects(
      runOnAndroidMainThread(() => {
        ran = true;
      }, []),
      /rebuild the app/,
    );
    assert.strictEqual(ran, false);
  }

  assert(
    /ensureNativeScriptInstalled\(\);\s*if \(Platform\.OS === "android"\) \{\s*return runOnAndroidMainThread\(callback, args\);/.test(
      index,
    ),
    "runOnUI should take the Android path before requiring Worklets",
  );

  // nativeViewHandle and isMainThread prefer the Android globals, else iOS's.
  {
    const android = load(["nativeHandleForUIKitView", "nativeViewHandle", "isMainThread"], {
      __nativeScriptAndroidViewHandle: (view) => `android:${view.id}`,
      __nativeScriptIsMainThread: () => true,
    });
    assert.strictEqual(android.nativeViewHandle({ id: 7 }), "android:7");
    assert.strictEqual(android.isMainThread(), true);

    const ios = load(["nativeHandleForUIKitView", "nativeViewHandle", "isMainThread"], {
      interop: { handleof: () => ({ toHexString: () => "0x10" }) },
      NSThread: { isMainThread: false },
    });
    assert.strictEqual(ios.nativeViewHandle({}), "0x10");
    assert.strictEqual(ios.isMainThread(), false);
  }

  {
    const context = {};
    assert.strictEqual(
      load(["androidContext"], { __nativeScriptAndroidContext: () => context }).androidContext(),
      context,
    );
    assert.strictEqual(load(["androidContext"], {}).androidContext(), null);
  }

  const declarations = read("src/index.d.ts");
  for (const name of ["nativeViewHandle", "androidContext"]) {
    assert(
      declarations.includes(`export function ${name}(`) &&
        declarations.includes(`${name}: typeof ${name};`),
      `index.d.ts should declare ${name} and put it on the default export`,
    );
  }

  // The globals index.ts reads are the ones the guest installs.
  const install = read("android/src/main/cpp/NativeScriptRnInstall.cpp");
  for (const name of [
    "__nativeScriptRunOnMainThread",
    "__nativeScriptIsMainThread",
    "__nativeScriptAndroidViewHandle",
    "__nativeScriptAndroidContext",
  ]) {
    assert(install.includes(`"${name}"`), `NativeScriptRnInstall.cpp should install ${name}`);
    assert(index.includes(name), `index.ts should read ${name}`);
  }

  // Each guest-only native com.tns.Runtime declares has its JNI symbol here.
  const runtimeJava = read(
    "platforms/android/test-app/runtime/src/main/java/com/tns/Runtime.java",
    repoRoot,
  );
  for (const name of [
    "runMainThreadTaskNative",
    "releaseEngineLockNative",
    "reacquireEngineLockNative",
    "drainMicrotasksNative",
  ]) {
    assert(runtimeJava.includes(`private native void ${name}(`) ||
        runtimeJava.includes(`private native int ${name}(`),
      `Runtime.java should declare ${name}`);
    assert(install.includes(`Java_com_tns_Runtime_${name}`), `${name} should be implemented`);
  }
  assert(
    /private void runOnMainThread\(long task\)/.test(runtimeJava) &&
      install.includes('"runOnMainThread", "(J)V"'),
    "the guest should call Runtime.runOnMainThread(long)",
  );

  // NativeScriptUIView has an Android view manager, registered by the package.
  const packageJava = read(
    "android/src/main/java/org/nativescript/reactnative/NativeScriptNativeApiPackage.java",
  );
  assert(packageJava.includes("new NativeScriptUIViewManager()"));
  const manager = read(
    "android/src/main/java/org/nativescript/reactnative/NativeScriptUIViewManager.java",
  );
  assert(manager.includes('NAME = "NativeScriptUIView"'));
  const registry = read(
    "android/src/main/java/org/nativescript/reactnative/NativeScriptViewRegistry.java",
  );
  assert(
    install.includes('"handleFor"') && registry.includes("public static synchronized String handleFor(Object object)"),
  );
  assert(
    install.includes('"viewContext"') && registry.includes("public static synchronized Context viewContext()"),
  );

  console.log("android-main-thread-api tests passed");
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
