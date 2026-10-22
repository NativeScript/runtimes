package org.nativescript.reactnative;

import android.app.Activity;
import android.content.Context;
import android.view.View;

import com.facebook.react.bridge.ReactContext;

import java.lang.ref.WeakReference;
import java.util.HashMap;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * Names Android views for {@link NativeScriptUIViewManager}: the string a
 * NativeScriptUIView takes as its {@code nativeViewHandle}.
 *
 * <p>Holds views weakly, as the pointer iOS uses for a handle does not retain
 * its UIView: whoever made the view keeps it alive until a host shows it.
 */
public final class NativeScriptViewRegistry {
    private static final Map<View, String> handles = new WeakHashMap<>();
    private static final Map<String, WeakReference<View>> views = new HashMap<>();
    private static long nextId;
    private static WeakReference<ReactContext> reactContext = new WeakReference<>(null);

    private NativeScriptViewRegistry() {
    }

    static synchronized void setReactContext(ReactContext context) {
        reactContext = new WeakReference<>(context);
    }

    /**
     * Called from JavaScript through __nativeScriptAndroidContext: the Context
     * to build a view with. The current Activity, whose theme the view takes;
     * the application before one exists.
     */
    public static synchronized Context viewContext() {
        ReactContext context = reactContext.get();
        if (context == null) {
            return null;
        }
        Activity activity = context.getCurrentActivity();
        return activity != null ? activity : context.getApplicationContext();
    }

    /** Called from JavaScript through __nativeScriptAndroidViewHandle. Null for a non-View. */
    public static synchronized String handleFor(Object object) {
        if (!(object instanceof View)) {
            return null;
        }
        View view = (View) object;
        String handle = handles.get(view);
        if (handle == null) {
            if (views.size() >= 64 && views.size() > 2 * handles.size()) {
                views.values().removeIf(ref -> ref.get() == null);
            }
            handle = "android:" + (++nextId);
            handles.put(view, handle);
            views.put(handle, new WeakReference<>(view));
        }
        return handle;
    }

    public static synchronized View resolve(String handle) {
        WeakReference<View> ref = handle != null ? views.get(handle) : null;
        View view = ref != null ? ref.get() : null;
        if (ref != null && view == null) {
            views.remove(handle);
        }
        return view;
    }
}
