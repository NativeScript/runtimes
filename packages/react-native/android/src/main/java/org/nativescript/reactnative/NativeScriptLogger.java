package org.nativescript.reactnative;

import android.util.Log;

import com.tns.Logger;

/**
 * Routes the runtime's diagnostics to logcat.
 *
 * <p>The standalone runtime reads its verbosity from the app's package.json. A
 * guest has no such file, so it is passed in at construction and defaults off:
 * the runtime logs every marshalled call when enabled, which is far too much
 * for an app that is only using NativeScript for a few APIs.
 */
public class NativeScriptLogger implements Logger {
    private static final String TAG = "TNS.Java";

    private boolean enabled;

    public NativeScriptLogger(boolean enabled) {
        this.enabled = enabled;
    }

    @Override
    public boolean isEnabled() {
        return enabled;
    }

    @Override
    public void setEnabled(boolean isEnabled) {
        this.enabled = isEnabled;
    }

    @Override
    public void write(String msg) {
        write(TAG, msg);
    }

    @Override
    public void write(String tag, String msg) {
        if (enabled) {
            Log.d(tag, msg);
        }
    }
}
