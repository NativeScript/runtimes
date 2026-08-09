package org.nativescript.reactnative;

import android.content.Context;

import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.module.annotations.ReactModule;

import com.tns.Logger;
import com.tns.Runtime;

import org.nativescript.nativeapi.NativeScriptNativeApiSpec;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Installs NativeScript into React Native's JavaScript runtime.
 *
 * <p>The interesting call is {@link #install(String)}, which must run on the JS
 * thread -- it is a synchronous TurboModule method, so React Native already
 * guarantees that. Everything it does happens against the runtime React Native
 * created; nothing here starts a JavaScript engine.
 */
@ReactModule(name = NativeScriptNativeApiModule.NAME)
public class NativeScriptNativeApiModule extends NativeScriptNativeApiSpec {
    public static final String NAME = "NativeScriptNativeApi";

    /** Written into the APK by nativescript.gradle. */
    private static final String ASSET_METADATA_DIR = "nativescript-metadata";

    /** Ships with this library; see src/main/assets. */
    private static final String ASSET_TS_HELPERS = "nativescript/internal/ts_helpers.js";

    private static final String[] METADATA_FILES = {
            "treeNodeStream.dat", "treeStringsStream.dat", "treeValueStream.dat",
    };

    static {
        System.loadLibrary("nativescript_rn");
    }

    private final ReactApplicationContext reactContext;

    public NativeScriptNativeApiModule(ReactApplicationContext reactContext) {
        super(reactContext);
        this.reactContext = reactContext;
    }

    @Override
    public String getName() {
        return NAME;
    }

    private native boolean nativeIsInstalled();

    private native String nativeRuntimeBackend();

    @Override
    public boolean install(String metadataPath) {
        if (nativeIsInstalled()) {
            return true;
        }

        Context context = reactContext.getApplicationContext();
        String resolvedPath = (metadataPath == null || metadataPath.isEmpty())
                ? defaultMetadataPath()
                : metadataPath;

        try {
            stageMetadata(context, new File(resolvedPath));
        } catch (IOException e) {
            throw new RuntimeException(
                    "NativeScript could not stage its metadata. Check that nativescript.gradle is "
                            + "applied in android/app/build.gradle so the metadata is generated "
                            + "into the APK.", e);
        }

        // The runtime reads a jsi::Runtime pointer, not a bridge. This holder is
        // how React Native hands one out, and it stays valid for the life of the
        // JS context -- a reload replaces the context and re-runs install().
        long runtimePtr = reactContext.getJavaScriptContextHolder().get();
        if (runtimePtr == 0) {
            throw new RuntimeException(
                    "React Native did not expose a JavaScript runtime. NativeScript requires the "
                            + "JSI runtime and cannot run over the legacy bridge.");
        }

        attachRuntime(context, runtimePtr, resolvedPath, readAsset(context, ASSET_TS_HELPERS));
        return true;
    }

    @Override
    public boolean installWorkletRuntime(com.facebook.react.bridge.ReadableMap runtimeHolder,
                                         String metadataPath) {
        // Worklet runtimes are separate jsi::Runtimes on separate threads. Each
        // would need its own attach and its own object manager, which the object
        // graph does not yet model on Android; iOS reaches this through
        // NativeApiJsi.mm. Reported honestly rather than silently no-oped.
        return false;
    }

    @Override
    public boolean isInstalled() {
        return nativeIsInstalled();
    }

    /**
     * The directory the runtime is pointed at.
     *
     * <p>Note this is the <em>parent</em>: MetadataBuilder appends "/metadata"
     * to whatever it is handed, so the .dat files are staged one level below.
     */
    @Override
    public String defaultMetadataPath() {
        return new File(reactContext.getApplicationContext().getFilesDir(), "nativescript")
                .getAbsolutePath();
    }

    @Override
    public String getRuntimeBackend() {
        return nativeRuntimeBackend();
    }

    @Override
    public boolean __writeTestMarker(String content) {
        try {
            File marker = new File(reactContext.getApplicationContext().getFilesDir(),
                    "nativescript-test-marker.txt");
            try (OutputStream out = new FileOutputStream(marker)) {
                out.write(content.getBytes("UTF-8"));
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static void attachRuntime(Context context, long runtimePtr, String metadataPath,
                                      String bootstrapScript) {
        File filesDir = context.getFilesDir();
        File dexDir = new File(filesDir, "nativescript-dex");
        if (!dexDir.exists() && !dexDir.mkdirs()) {
            throw new RuntimeException("Could not create " + dexDir.getAbsolutePath());
        }

        Logger logger = new NativeScriptLogger(isDebuggable(context));

        Runtime.attachToHostRuntime(
                logger,
                context.getPackageName(),
                context.getApplicationInfo().nativeLibraryDir,
                filesDir,
                context.getClassLoader(),
                dexDir,
                /* dexThumb */ buildStamp(context),
                isDebuggable(context),
                runtimePtr,
                metadataPath,
                bootstrapScript);
    }

    /**
     * Reads one of this library's own assets.
     *
     * <p>Kept in memory rather than staged to disk like the metadata: it is a
     * single small script that is evaluated once, and the runtime takes it as
     * source rather than as a path.
     */
    private static String readAsset(Context context, String name) {
        try (InputStream in = context.getAssets().open(name)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            return out.toString("UTF-8");
        } catch (IOException e) {
            throw new RuntimeException("NativeScript could not read its own asset: " + name, e);
        }
    }

    /**
     * Identifies this build of the app.
     *
     * <p>Used both as the dex cache key and as the metadata staging stamp, so it
     * has to change whenever the app is rebuilt or upgraded -- otherwise a new
     * build reads the previous one's generated proxies and metadata.
     */
    private static String buildStamp(Context context) {
        try {
            android.content.pm.PackageInfo info = context.getPackageManager()
                    .getPackageInfo(context.getPackageName(), 0);
            return info.versionName + "-" + info.lastUpdateTime;
        } catch (android.content.pm.PackageManager.NameNotFoundException e) {
            throw new RuntimeException("Could not read this app's own package info", e);
        }
    }

    private static boolean isDebuggable(Context context) {
        return (context.getApplicationInfo().flags
                & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    /**
     * Copies the generated metadata out of the APK once.
     *
     * <p>The reader takes a directory of files, and assets inside an APK are not
     * files. Extraction is keyed on the build stamp so a rebuild that regenerates
     * metadata does not keep serving the previous build's copy.
     */
    private static void stageMetadata(Context context, File root) throws IOException {
        File stamp = new File(root, ".version");
        String version = buildStamp(context);

        if (stamp.exists() && version.equals(readAll(stamp))) {
            return;
        }

        // MetadataBuilder::BuildMetadata appends "/metadata" to the path it is
        // given, so the files go one level below the directory we report.
        File target = new File(root, "metadata");
        if (!target.exists() && !target.mkdirs()) {
            throw new IOException("Could not create " + target.getAbsolutePath());
        }

        for (String name : METADATA_FILES) {
            try (InputStream in = context.getAssets().open(ASSET_METADATA_DIR + "/" + name);
                 OutputStream out = new FileOutputStream(new File(target, name))) {
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = in.read(buffer)) != -1) {
                    out.write(buffer, 0, read);
                }
            }
        }

        try (OutputStream out = new FileOutputStream(stamp)) {
            out.write(version.getBytes("UTF-8"));
        }
    }

    private static String readAll(File file) throws IOException {
        byte[] data = new byte[(int) file.length()];
        try (InputStream in = new java.io.FileInputStream(file)) {
            int read = 0;
            while (read < data.length) {
                int n = in.read(data, read, data.length - read);
                if (n < 0) break;
                read += n;
            }
        }
        return new String(data, "UTF-8");
    }
}
