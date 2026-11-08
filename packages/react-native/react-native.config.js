module.exports = {
  dependency: {
    platforms: {
      android: {
        sourceDir: "android",
        packageImportPath:
          "import org.nativescript.reactnative.NativeScriptNativeApiPackage;",
        packageInstance: "new NativeScriptNativeApiPackage()",
      },
    },
  },
};
