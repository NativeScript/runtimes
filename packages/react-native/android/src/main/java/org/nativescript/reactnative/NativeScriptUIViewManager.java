package org.nativescript.reactnative;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.facebook.react.module.annotations.ReactModule;
import com.facebook.react.uimanager.SimpleViewManager;
import com.facebook.react.uimanager.ThemedReactContext;
import com.facebook.react.uimanager.ViewManagerDelegate;
import com.facebook.react.viewmanagers.NativeScriptUIViewManagerDelegate;
import com.facebook.react.viewmanagers.NativeScriptUIViewManagerInterface;

/**
 * NativeScriptUIView on Android: shows the android.view.View its
 * {@code nativeViewHandle} names (see {@link NativeScriptViewRegistry}).
 *
 * <p>The other props drive iOS's UIKit host (controllers, children views, host
 * ready events) and have no Android counterpart.
 */
@ReactModule(name = NativeScriptUIViewManager.NAME)
public class NativeScriptUIViewManager extends SimpleViewManager<NativeScriptUIHostView>
        implements NativeScriptUIViewManagerInterface<NativeScriptUIHostView> {
    public static final String NAME = "NativeScriptUIView";

    private final ViewManagerDelegate<NativeScriptUIHostView> delegate =
            new NativeScriptUIViewManagerDelegate<>(this);

    @NonNull
    @Override
    public String getName() {
        return NAME;
    }

    @Override
    protected ViewManagerDelegate<NativeScriptUIHostView> getDelegate() {
        return delegate;
    }

    @NonNull
    @Override
    protected NativeScriptUIHostView createViewInstance(@NonNull ThemedReactContext context) {
        return new NativeScriptUIHostView(context);
    }

    @Override
    public void onDropViewInstance(@NonNull NativeScriptUIHostView view) {
        super.onDropViewInstance(view);
        view.setNativeViewHandle(null);
    }

    @Override
    public void setNativeViewHandle(NativeScriptUIHostView view, @Nullable String value) {
        view.setNativeViewHandle(value);
    }

    @Override
    public void setHostId(NativeScriptUIHostView view, @Nullable String value) {
    }

    @Override
    public void setHostReadyId(NativeScriptUIHostView view, @Nullable String value) {
    }

    @Override
    public void setChildrenViewHandle(NativeScriptUIHostView view, @Nullable String value) {
    }

    @Override
    public void setControllerHandle(NativeScriptUIHostView view, @Nullable String value) {
    }

    @Override
    public void setDetachControllerView(NativeScriptUIHostView view, boolean value) {
    }

    @Override
    public void setDebugName(NativeScriptUIHostView view, @Nullable String value) {
    }

    @Override
    public void setUpdateRevision(NativeScriptUIHostView view, int value) {
    }

    @Override
    public void setMountedRevision(NativeScriptUIHostView view, int value) {
    }
}
