const assert = require("assert");
const fs = require("fs");
const path = require("path");

const packageRoot = path.resolve(__dirname, "..");

const moduleSource = fs.readFileSync(
  path.join(packageRoot, "ios/NativeScriptNativeApiModule.mm"),
  "utf8",
);
assert(
  moduleSource.includes("__nativeScriptLoadReactImage"),
  "worklet runtime should expose a generic RN image loading host function",
);
assert(
  moduleSource.includes("[RCTConvert RCTImageSource:jsonSource]") &&
    moduleSource.includes("currentReactImageLoader()") &&
    moduleSource.includes("loadImageWithURLRequest:imageSource.request"),
  "image loading should use React Native RCTImageLoader/RCTImageSource parity path",
);
assert(
  moduleSource.includes("nativeScriptHandleFromNSObject(retainedImage)") &&
    moduleSource.includes("runtimeStrong->schedule("),
  "loaded UIImages should be retained and delivered back through the worklet runtime",
);

const publicApi = fs.readFileSync(path.join(packageRoot, "src/index.ts"), "utf8");
const dispatcherApi = fs.readFileSync(
  path.join(packageRoot, "src/ui/dispatcher.ts"),
  "utf8",
);
assert(
  publicApi.includes("export function loadImage(") &&
    publicApi.includes(".__nativeScriptLoadReactImage") &&
    publicApi.includes("interop?.object?.(interop.Pointer(handle))") &&
    dispatcherApi.includes("loadImage: (source, options, callback)"),
  "public TS API should wrap image handles into NativeScript objects and expose ctx.loadImage",
);

const declarations = fs.readFileSync(
  path.join(packageRoot, "src/index.d.ts"),
  "utf8",
);
assert(
  declarations.includes("NativeScriptImageLoadOptions") &&
    declarations.includes("loadImage("),
  "type declarations should expose generic image loading",
);

console.log("React Native image loader API tests passed");
