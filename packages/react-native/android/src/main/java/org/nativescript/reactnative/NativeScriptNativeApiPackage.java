package org.nativescript.reactnative;

import androidx.annotation.Nullable;

import com.facebook.react.BaseReactPackage;
import com.facebook.react.bridge.NativeModule;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.model.ReactModuleInfo;
import com.facebook.react.module.model.ReactModuleInfoProvider;

import java.util.HashMap;
import java.util.Map;

/** Autolinking entry point for @nativescript/react-native. */
public class NativeScriptNativeApiPackage extends BaseReactPackage {

    @Nullable
    @Override
    public NativeModule getModule(String name, ReactApplicationContext reactContext) {
        if (NativeScriptNativeApiModule.NAME.equals(name)) {
            return new NativeScriptNativeApiModule(reactContext);
        }
        return null;
    }

    @Override
    public ReactModuleInfoProvider getReactModuleInfoProvider() {
        return () -> {
            Map<String, ReactModuleInfo> infos = new HashMap<>();
            infos.put(
                    NativeScriptNativeApiModule.NAME,
                    new ReactModuleInfo(
                            NativeScriptNativeApiModule.NAME,
                            NativeScriptNativeApiModule.NAME,
                            /* canOverrideExistingModule */ false,
                            /* needsEagerInit */ false,
                            /* isCxxModule */ false,
                            /* isTurboModule */ true));
            return infos;
        };
    }
}
